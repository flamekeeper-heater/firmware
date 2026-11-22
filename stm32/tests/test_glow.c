/* The glow pulse planner against a plug model: every pulse inside its current caps and the rms
 * cap, the power held as the plug heats, the gentle start, probes, the heat budget, and the
 * faults that end glowing at once. 12 V and 24 V plugs on 12 V and 24 V supplies. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../core/glow.h"

static int checks = 0, failures = 0;
#define CHECK(c)                                                  \
  do {                                                            \
    checks++;                                                     \
    if (!(c)) {                                                   \
      failures++;                                                 \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);         \
    }                                                             \
  } while (0)

#define PERIOD_MS 5u

/* A glow plug: resistance from cold to hot as it heats; 0.02 Ohm of wiring in series. */
typedef struct {
  double r_cold, r_hot, heat; /* heat 0 cold .. 1 at full power */
  double vin;
} plug;

static double plug_r(const plug *pl) { return pl->r_cold + (pl->r_hot - pl->r_cold) * pl->heat; }
static double plug_i(const plug *pl) { return pl->vin / (plug_r(pl) + 0.02); }

typedef struct {
  glow g;
  plug pl;
  uint32_t now;
  uint16_t last_width;
  double last_i;
  int airflow, trip;
  double gap_a;
  int16_t board_c10;
  /* Findings. */
  int cap_violations, rms_violations;
  double energy_j, window_j; /* plug energy: total, and since the window was reset */
  double max_bucket;
  uint32_t pulses, probes;
  uint32_t last_pulse_ms;
} rig;

static void rig_init(rig *r, double r_cold, double r_hot, double vin) {
  memset(r, 0, sizeof(*r));
  glow_params p;
  glow_params_default(&p);
  glow_init(&r->g, &p, 0);
  r->g.bucket_a2us = 0; /* tests start with an empty budget unless they say otherwise */
  r->pl.r_cold = r_cold, r->pl.r_hot = r_hot, r->pl.vin = vin;
  r->airflow = 1;
  r->board_c10 = 300;
}

static void period(rig *r, uint16_t power_w) {
  glow_meas m;
  m.vin_cv = (uint16_t) (r->pl.vin * 100 + 0.5);
  m.pulse_ca = (uint16_t) (r->last_i * 100 + 0.5);
  m.gap_ca = (uint16_t) (r->gap_a * 100 + 0.5);
  m.airflow_ok = (uint8_t) r->airflow;
  m.board_c10 = r->board_c10;
  m.high_side_trip = (uint8_t) r->trip;
  glow_cmd c = glow_period(&r->g, r->now, power_w, &m);
  double i = plug_i(&r->pl);
  if (c.width_us > 0) {
    r->pulses++;
    if (c.width_us == r->g.p.min_pulse_us && !r->g.have_reading)
      r->probes++;
    else {
      /* The current caps hold for the real current, not only the predicted one. */
      if ((i > 100 && c.width_us > 300) || (i > 22 && c.width_us > 1500) || c.width_us > 4950)
        r->cap_violations++;
      /* rms over this pulse's share of time: since the previous pulse. */
      double span = (double) (r->now - r->last_pulse_ms) * 1000.0;
      if (span < 5000)
        span = 5000;
      if (sqrt(c.width_us / span) * i > 15.0 * 1.001)
        r->rms_violations++;
    }
  }
  if (c.width_us > 0)
    r->last_pulse_ms = r->now;
  r->last_width = c.width_us;
  r->last_i = c.width_us > 0 ? i : 0;
  double e = i * i * plug_r(&r->pl) * c.width_us / 1e6;
  r->energy_j += e;
  r->window_j += e;
  /* The plug heats with its average power (90 W = full heat), over about 8 s. */
  double p_avg = e / (PERIOD_MS / 1000.0);
  r->pl.heat += (p_avg / 90.0 - r->pl.heat) * (PERIOD_MS / 8000.0);
  if (r->pl.heat < 0)
    r->pl.heat = 0;
  if (glow_bucket_a2s(&r->g) > r->max_bucket)
    r->max_bucket = glow_bucket_a2s(&r->g);
  r->now += PERIOD_MS;
}

static void run(rig *r, uint32_t ms, uint16_t power_w) {
  for (uint32_t t = 0; t < ms; t += PERIOD_MS)
    period(r, power_w);
}

/* Average plug power over the next ms. */
static double avg_power(rig *r, uint32_t ms, uint16_t power_w) {
  r->window_j = 0;
  run(r, ms, power_w);
  return r->window_j / (ms / 1000.0);
}

static void test_plug(double r_cold, double r_hot, double vin, double want_w) {
  rig r;
  rig_init(&r, r_cold, r_hot, vin);
  /* The first pulse is a probe. */
  period(&r, 90);
  CHECK(r.last_width == 150);
  /* Gentle start: the first half second stays near the starting voltage. */
  double early = avg_power(&r, 500, 90);
  double v_rms_early = sqrt(early * plug_r(&r.pl));
  CHECK(v_rms_early < 4.5);
  /* After the ramp the power is what was asked, and it holds while the plug heats. */
  run(&r, 6000, 90);
  double mid = avg_power(&r, 2000, 90);
  run(&r, 20000, 90);
  double late = avg_power(&r, 2000, 90);
  if (want_w > 0) {
    CHECK(mid > want_w * 0.85 && mid < want_w * 1.05);
    CHECK(late > want_w * 0.85 && late < want_w * 1.05);
  } else {
    CHECK(mid < 90 * 1.05 && late < 90 * 1.05); /* cannot reach it, never above it */
  }
  CHECK(r.cap_violations == 0);
  CHECK(r.rms_violations == 0);
  CHECK(r.probes == 1);
  if (failures)
    printf("  plug %.2f-%.2f Ohm at %.1f V: early %.1f V rms, mid %.1f W, late %.1f W\n", r_cold, r_hot,
           vin, v_rms_early, mid, late);
}

static void test_rms_limited(void) {
  /* A 12 V plug at 12 V: 90 W would need over 15 A rms while cold, so the rms cap rules. */
  rig r;
  rig_init(&r, 0.15, 0.9, 12.6);
  run(&r, 6000, 90);
  double pw = avg_power(&r, 1000, 90);
  double i_rms = sqrt(pw / plug_r(&r.pl));
  CHECK(i_rms <= 15.2);
  CHECK(r.rms_violations == 0);
}

static void test_low_supply(void) {
  rig r;
  rig_init(&r, 1.5, 4.0, 10.5); /* a 24 V plug on a weak 12 V supply cannot reach 75 W anyway */
  run(&r, 6000, 90);
  rig r2;
  rig_init(&r2, 0.6, 1.2, 10.8);
  run(&r2, 6000, 90);
  double pw = avg_power(&r2, 2000, 90);
  CHECK(pw < 75 * 1.05);
}

static void test_probe_after_gap(void) {
  rig r;
  rig_init(&r, 0.2, 1.0, 12.6);
  run(&r, 3000, 90);
  uint32_t probes = r.probes;
  run(&r, 100, 0); /* off for 100 ms */
  period(&r, 90);
  CHECK(r.probes == probes + 1);
  CHECK(r.last_width == 150);
}

static void test_supply_jump(void) {
  /* The supply rises between pulses: the next pulse is planned from the new voltage, so the
   * caps still hold for the real current. */
  rig r;
  rig_init(&r, 0.2, 1.0, 12.0);
  run(&r, 6000, 90);
  r.pl.vin = 28.0;
  run(&r, 2000, 90);
  CHECK(r.cap_violations == 0);
  CHECK(r.rms_violations == 0);
}

static void test_stuck_low_side(void) {
  rig r;
  rig_init(&r, 0.2, 1.0, 12.6);
  run(&r, 1000, 90);
  r.gap_a = 30;
  period(&r, 90);
  CHECK(r.g.fault == GLOW_FAULT_STUCK_LOW_SIDE);
  CHECK(r.last_width == 0);
  r.gap_a = 0;
  run(&r, 1000, 90);
  CHECK(r.last_width == 0); /* never again until a reset */
  /* The user's fault reset: pulses again, from a probe, and the heat budget is kept (GLW-21). */
  uint32_t used = glow_bucket_a2s(&r.g);
  CHECK(used > 0);
  glow_clear_fault(&r.g);
  CHECK(r.g.fault == GLOW_FAULT_NONE && glow_bucket_a2s(&r.g) == used);
  period(&r, 90);
  CHECK(r.last_width == r.g.p.min_pulse_us);
  run(&r, 1000, 90);
  CHECK(r.g.fault == GLOW_FAULT_NONE && r.last_width > 0);
}

static void test_high_side_trips(void) {
  rig r;
  rig_init(&r, 0.2, 1.0, 12.6);
  run(&r, 1000, 90);
  /* One trip: enable low for 0.5 s, then a probe and on again. */
  r.trip = 1;
  period(&r, 90);
  r.trip = 0;
  CHECK(r.last_width == 0);
  run(&r, 400, 90);
  CHECK(r.last_width == 0);
  run(&r, 200, 90);
  CHECK(r.pulses > 0 && r.g.fault == 0);
  run(&r, 3000, 90); /* well past the count's memory */
  /* Three in a row: a fault. */
  for (int k = 0; k < 3; k++) {
    r.trip = 1;
    period(&r, 90);
    r.trip = 0;
    run(&r, 600, 90);
  }
  CHECK(r.g.fault == GLOW_FAULT_HIGH_SIDE_TRIPS);
  glow_clear_fault(&r.g); /* the user's reset: the trip count starts again */
  CHECK(r.g.trips == 0 && !r.g.holding);
  run(&r, 1000, 90);
  CHECK(r.g.fault == GLOW_FAULT_NONE && r.last_width > 0);
}

static void test_airflow_and_board(void) {
  rig r;
  rig_init(&r, 0.2, 1.0, 12.6);
  run(&r, 2000, 90);
  r.airflow = 0;
  glow_meas m = {1260, 0, 0, 0, 300, 0};
  glow_cmd c = glow_period(&r.g, r.now, 90, &m);
  CHECK(c.enable == 0 && c.width_us == 0); /* in the same period */
  r.airflow = 1;
  r.now += 5;
  run(&r, 1000, 90);
  r.board_c10 = 900;
  period(&r, 90);
  CHECK(r.last_width == 0);
}

static void test_heat_budget(void) {
  /* A 0.4 Ohm plug at 12.6 V: 90 W comes at about 15 A rms, so the budget lasts under a minute,
   * then glowing stops. */
  rig r;
  rig_init(&r, 0.4, 0.4, 12.6);
  uint32_t t = 0;
  while (t < 120000 && !(r.pulses > 0 && glow_bucket_a2s(&r.g) >= 9700)) {
    period(&r, 400); /* ask for more than the plug can take */
    t += PERIOD_MS;
  }
  CHECK(t > 30000 && t < 60000);
  uint32_t pulses = r.pulses;
  run(&r, 5000, 400);
  CHECK(r.pulses - pulses <= 40); /* the last few A2s, then nothing fits */
  pulses = r.pulses;
  run(&r, 5000, 400);
  CHECK(r.pulses == pulses);
  CHECK(r.max_bucket <= 9720);
  /* Off for 240 s: empty again. */
  run(&r, 240000, 0);
  CHECK(glow_bucket_a2s(&r.g) == 0);
}

static void test_no_reading_no_long_pulses(void) {
  /* The current sense reads nothing (a dead line, a missed sample): only 150 us probes, ever. */
  glow g;
  glow_params p;
  glow_params_default(&p);
  glow_init(&g, &p, 0);
  g.bucket_a2us = 0;
  uint16_t longest = 0;
  for (uint32_t t = 0; t < 5000; t += 5) {
    glow_meas m = {2600, 0, 0, 1, 300, 0}; /* 26 V, a reading of 0 A */
    glow_cmd c = glow_period(&g, t, 90, &m);
    if (c.width_us > longest)
      longest = c.width_us;
  }
  CHECK(longest == 150);
}

static void test_reset_starts_empty(void) {
  /* The burn runs the fan longer after a reset instead (GLW-21). */
  glow g;
  glow_params p;
  glow_params_default(&p);
  glow_init(&g, &p, 1000);
  CHECK(glow_bucket_a2s(&g) == 0);
  glow_meas m = {1260, 0, 0, 1, 300, 0};
  glow_cmd c = glow_period(&g, 1005, 90, &m);
  CHECK(c.width_us == 150); /* a probe at once */
}

int main(void) {
  test_plug(0.2, 1.0, 12.6, 90);   /* 12 V plug, 12 V supply */
  test_plug(0.2, 1.0, 26.0, 90);   /* 12 V plug, 24 V supply: short pulses at high current */
  test_plug(1.5, 4.5, 26.0, 90);   /* 24 V plug, 24 V supply */
  test_plug(1.5, 4.5, 12.6, 0);    /* 24 V plug, 12 V supply: under-heats, never over */
  test_rms_limited();
  test_low_supply();
  test_probe_after_gap();
  test_supply_jump();
  test_stuck_low_side();
  test_high_side_trips();
  test_airflow_and_board();
  test_heat_budget();
  test_no_reading_no_long_pulses();
  test_reset_starts_empty();
  printf("glow: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
