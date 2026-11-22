/* The glow plug's pulse planner: see glow.h. */
#include "glow.h"

#include <string.h>

void glow_params_default(glow_params *p) {
  memset(p, 0, sizeof(*p));
  p->period_us = 5000;
  p->min_pulse_us = 150;
  p->stale_ms = 50;
  p->duty_max_permille = 990;
  p->i_rms_max_ca = 1500;
  p->i_high_ca = 2200;
  p->w_high_us = 1500;
  p->i_very_high_ca = 10000;
  p->w_very_high_us = 300;
  p->margin_percent = 10;
  p->min_reading_ca = 50;
  p->p_max_w = 90;
  p->p_low_supply_w = 75;
  p->low_supply_cv = 1100;
  p->start_v_cv = 250;
  p->ramp_ms = 5000;
  p->bucket_cap_a2s = 9720;
  p->bucket_drain_ma2s_per_s = 40500;
  p->gap_fault_ca = 200;
  p->trip_hold_ms = 500;
  p->trips_max = 3;
  p->trips_forget_ms = 2000;
  p->board_max_c10 = 850;
}

static uint32_t since(uint32_t now, uint32_t then) { return (uint32_t) (now - then); }

static uint64_t cap_a2us(const glow *g) { return (uint64_t) g->p.bucket_cap_a2s * 1000000u; }

/* Heat added by a pulse of current i (0.01 A) for w us, in A2 x us. */
static uint64_t pulse_a2us(uint32_t i_ca, uint32_t w_us) {
  return (uint64_t) i_ca * i_ca * w_us / 10000u;
}

void glow_init(glow *g, const glow_params *p, uint32_t now_ms) {
  memset(g, 0, sizeof(*g));
  g->p = *p;
  g->last_ms = now_ms;
  /* After a reset the budget starts empty: the burn runs the fan longer before the first glow
   * instead (GLW-21; burn_params.reset_fan_s). */
}

uint32_t glow_bucket_a2s(const glow *g) { return (uint32_t) (g->bucket_a2us / 1000000u); }

static glow_cmd off(glow *g) {
  glow_cmd c = {0, 0};
  g->last_width_us = 0;
  g->credit_us = 0;
  return c;
}

glow_cmd glow_period(glow *g, uint32_t now_ms, uint16_t power_w, const glow_meas *m) {
  const glow_params *p = &g->p;
  uint32_t dt = since(now_ms, g->last_ms);
  uint8_t was_enabled = g->on && !g->holding;
  g->last_ms = now_ms;

  /* The pulse that went out last period: its reading, and the heat it added. */
  if (g->last_width_us > 0) {
    if (g->last_width_us >= p->min_pulse_us) {
      g->i_ca = m->pulse_ca;
      /* A pulse with (almost) no current is no reading: a dead sense line or a missed sample
       * would otherwise plan long pulses for a plug that may draw 100 A. Only probes follow. */
      g->have_reading = m->pulse_ca >= p->min_reading_ca;
    }
    uint64_t add = pulse_a2us(g->i_ca, g->last_width_us);
    g->bucket_a2us += add;
  }
  g->last_width_us = 0;

  if (g->fault != GLOW_FAULT_NONE)
    return off(g);

  /* Current between pulses with the high side on: the low side is stuck on (GLW-6). */
  if (was_enabled && m->gap_ca > p->gap_fault_ca) {
    g->fault = GLOW_FAULT_STUCK_LOW_SIDE;
    g->on = 0;
    return off(g);
  }

  /* The high side latched an over-current (GLW-9): enable low to clear it; three in a row is a
   * fault. */
  if (g->trips > 0 && !g->holding && since(now_ms, g->trip_ms) >= p->trips_forget_ms)
    g->trips = 0;
  if (was_enabled && m->high_side_trip) {
    g->trips++;
    g->trip_ms = now_ms;
    g->holding = 1;
    if (g->trips >= p->trips_max) {
      g->fault = GLOW_FAULT_HIGH_SIDE_TRIPS;
      g->on = 0;
      return off(g);
    }
  }
  if (g->holding) {
    if (since(now_ms, g->trip_ms) < p->trip_hold_ms)
      return off(g);
    g->holding = 0;
    g->have_reading = 0; /* start again from a probe */
  }

  /* Off: the plug cools, and so does the heat budget (GLW-21: it drains only while off). */
  if (power_w == 0) {
    uint64_t drain = (uint64_t) p->bucket_drain_ma2s_per_s * dt; /* milli-A2s/s x ms = A2 x us */
    g->bucket_a2us = g->bucket_a2us > drain ? g->bucket_a2us - drain : 0;
    g->on = 0;
    return off(g);
  }

  /* No glow without measured airflow (GLW-14) or on a hot board (GLW-15). */
  if (!m->airflow_ok || m->board_c10 > p->board_max_c10) {
    g->on = 0;
    g->have_reading = 0;
    return off(g);
  }

  glow_cmd cmd = {1, 0};
  if (!g->on) {
    g->on = 1;
    g->on_since_ms = now_ms;
    g->credit_us = 0;
    g->have_reading = 0;
  }
  uint64_t room = g->bucket_a2us >= cap_a2us(g) ? 0 : cap_a2us(g) - g->bucket_a2us;
  uint32_t vin = m->vin_cv ? m->vin_cv : 1;

  /* First pulse of a run, or after a gap: a probe, allowed at any current (GLW-5). */
  if (!g->have_reading || since(now_ms, g->last_pulse_ms) > p->stale_ms) {
    if (room == 0)
      return cmd;
    cmd.width_us = p->min_pulse_us;
    g->last_width_us = cmd.width_us;
    g->last_pulse_ms = now_ms;
    g->vin_then_cv = (uint16_t) vin;
    g->i_ca = 0; /* the probe's heat is counted from its reading */
    g->have_reading = 0;
    return cmd;
  }

  /* The current to expect: the last reading scaled by the supply since (i_now) for the power,
   * plus the margin (ip) for every limit. */
  uint64_t i_now = (uint64_t) g->i_ca * vin / (g->vin_then_cv ? g->vin_then_cv : 1);
  if (i_now == 0)
    i_now = 1;
  uint64_t ip = i_now * (100u + p->margin_percent) / 100u;
  if (ip == 0)
    ip = 1;

  /* Duty in parts per million: the power asked, with a gentle start, under the rms cap. */
  uint32_t p_set = power_w;
  uint32_t p_lim = vin < p->low_supply_cv ? p->p_low_supply_w : p->p_max_w;
  if (p_set > p_lim)
    p_set = p_lim;
  uint64_t d_power = (uint64_t) p_set * 10000000000ull / ((uint64_t) vin * i_now);
  uint64_t d_start = (uint64_t) 1000000u * p->start_v_cv * p->start_v_cv / ((uint64_t) vin * vin);
  uint64_t d = d_power;
  uint32_t t_on = since(now_ms, g->on_since_ms);
  if (t_on < p->ramp_ms && d_start < d_power)
    d = d_start + (d_power - d_start) * t_on / p->ramp_ms;
  uint64_t d_rms = (uint64_t) 1000000u * p->i_rms_max_ca * p->i_rms_max_ca / (ip * ip);
  if (d > d_rms)
    d = d_rms;
  if (d > (uint64_t) p->duty_max_permille * 1000u)
    d = (uint64_t) p->duty_max_permille * 1000u;

  /* Owed on-time; below a readable pulse it waits for the next period (a lower pulse rate). */
  g->credit_us += (uint32_t) (d * p->period_us / 1000000u);
  if (g->credit_us > p->period_us)
    g->credit_us = p->period_us;
  if (g->credit_us < p->min_pulse_us)
    return cmd;
  uint32_t w = g->credit_us;

  /* Per-pulse caps (GLW-4, GLW-5). The rms cap counts over the time since the last pulse, so a
   * low duty paid out as fewer, longer pulses stays inside it too. */
  uint64_t interval_us = (uint64_t) since(now_ms, g->last_pulse_ms) * 1000u;
  if (interval_us < p->period_us)
    interval_us = p->period_us;
  uint64_t w_rms = d_rms * interval_us / 1000000u;
  if (w < p->min_pulse_us || w_rms < p->min_pulse_us)
    return cmd;
  if (w > w_rms)
    w = (uint32_t) w_rms;
  if (ip > p->i_very_high_ca && w > p->w_very_high_us)
    w = p->w_very_high_us;
  else if (ip > p->i_high_ca && w > p->w_high_us)
    w = p->w_high_us;
  if (w > (uint32_t) p->period_us * p->duty_max_permille / 1000u)
    w = (uint32_t) p->period_us * p->duty_max_permille / 1000u;

  /* No pulse may overfill the heat budget (GLW-21). */
  uint64_t add = pulse_a2us((uint32_t) ip, w);
  if (add > room) {
    w = (uint32_t) (room * 10000u / (ip * ip));
    if (w < p->min_pulse_us)
      return cmd;
  }

  g->credit_us -= w;
  cmd.width_us = (uint16_t) w;
  g->last_width_us = cmd.width_us;
  g->last_pulse_ms = now_ms;
  g->vin_then_cv = (uint16_t) vin;
  return cmd;
}
