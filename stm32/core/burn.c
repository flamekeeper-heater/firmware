/* The burn sequencer: see burn.h. */
#include "burn.h"

#include <string.h>

enum { R_OK = 0x00, R_NOT_SUPPORTED = 0x13, R_NOT_OFF = 0x20, R_REFUSED = 0x2F };
enum { MODE_OFF = 0, MODE_HEAT = 1, MODE_FAN = 2, MODE_PRIME = 3 };

/* Glow on-time in one-minute buckets: the current one and the ten before it, so a minute's glow
 * leaves the count only once all of it is more than 10 minutes old (GLW-1 holds in any 10 minutes,
 * not only on the buckets' boundaries). */
#define BUCKET_MS 60000u
#define BUCKETS 11u

/* What each step is (indexed by BURN_STEP_*). A fan retry (step 12) is all 0 here: it stands for the
 * step it goes on with (effective_step) for the state and for being active. */
enum {
  ACTIVE = 1u << 0,    /* a run asked for and not yet stopping (the dead-man's business) */
  FUEL = 1u << 1,      /* fuel has gone in since the start: a stop needs a burn-off and a cooldown */
  HEARTBEAT = 1u << 2, /* the high sides' heartbeat runs */
};
static const struct {
  uint8_t state, flags;
} STEPS[] = {
    {BURN_STATE_OFF, 0},                              /* OFF */
    {BURN_STATE_STARTING, ACTIVE | HEARTBEAT},        /* SELF_TEST */
    {BURN_STATE_STARTING, ACTIVE | HEARTBEAT},        /* PREHEAT */
    {BURN_STATE_STARTING, ACTIVE | FUEL | HEARTBEAT}, /* IGNITION */
    {BURN_STATE_STARTING, ACTIVE | FUEL | HEARTBEAT}, /* PURGE */
    {BURN_STATE_STARTING, ACTIVE | FUEL | HEARTBEAT}, /* WARM_UP */
    {BURN_STATE_HEATING, ACTIVE | FUEL | HEARTBEAT},  /* RUNNING */
    {BURN_STATE_STOPPING, HEARTBEAT},                 /* BURN_OFF */
    {BURN_STATE_COOLING, 0},                          /* COOLDOWN */
    {BURN_STATE_FAN_ONLY, ACTIVE},                    /* FAN_ONLY */
    {BURN_STATE_PRIMING, ACTIVE | HEARTBEAT},         /* PRIMING */
    {BURN_STATE_STARTING, ACTIVE},                    /* PRE_CHECK */
    {0, 0},                                           /* FAN_RETRY */
};
typedef char steps_cover_every_step[sizeof(STEPS) / sizeof(STEPS[0]) == BURN_STEP_FAN_RETRY + 1u ? 1 : -1];

void burn_params_default(burn_params *p) {
  memset(p, 0, sizeof(*p));
  p->fan_start_rpm = 1500;
  p->fan_min_rpm = 1800;
  p->fan_max_rpm = 4500;
  p->fan_burn_off_rpm = 3500;
  p->glow_min_fan_rpm = 1000;
  p->fan_stall_rpm = 200;
  p->fan_spin_up_ms = 10000;
  p->fan_stall_ms = 1000;
  p->fan_retries = 2;
  p->fan_retry_wait_s = 5;
  p->fan_retry_forget_s = 600;
  p->reset_fan_s = 30;
  p->reset_fan_within_s = 240;
  p->pump_start_cHz = 160;
  p->pump_min_cHz = 160;
  p->pump_max_cHz = 550;
  p->pump_prime_cHz = 500;
  p->prime_s = 60;
  p->glow_w = 90;
  p->preheat_s = 30;
  p->ignition_s = 90;
  p->glow_after_flame_s = 20;
  p->glow_attempt_max_s = 120;
  p->glow_window_max_s = 240;
  p->attempts = 2;
  p->purge_s = 60;
  p->flame_rise_c10 = 150;
  p->flame_lost_c10 = 400;
  p->warm_up_s = 60;
  p->overheat_derate_c10 = 2450;
  p->overheat_stop_c10 = 2550;
  p->cool_c10 = 500;
  p->cooldown_min_s = 60;
  p->cooldown_max_s = 1200;
  p->burn_off_s = 30;
  p->hot_start_c10 = 800;
  p->supply_low_cv = 1050;
  p->supply_high_cv = 3000;
  p->supply_fault_ms = 5000;
  p->precheck_max_ms = 2000;
  p->derate_start_c10 = 850;
  p->derate_full_c10 = 1000;
  p->level1_fan_ma = 600;
  p->level6_fan_ma = 1400;
  p->level10_fan_ma = 2800;
  p->board_stop_c10 = 1050;
  p->board_stop_ms = 10000;
  p->fuel_up_percent_s = 5;
  p->air_down_percent_s = 5;
  p->plug_v = 12;
  p->co_fitted = 1; /* the safe side: a board without the sensor shows a sensor fault until set */
}

static void enter(burn *b, uint8_t step, uint32_t now) {
  b->step = step;
  b->step_ms = now;
}

/* The heat exchanger too hot for a start: it cools down first. */
static int hot(const burn *b, const burn_inputs *in) {
  return temp_known(in->hx_c10) && in->hx_c10 >= b->p.hot_start_c10;
}

static uint16_t lerp16(uint16_t lo, uint16_t hi, uint32_t num, uint32_t den) {
  if (den == 0 || num >= den)
    return hi;
  return (uint16_t) (lo + (int32_t) ((int32_t) hi - (int32_t) lo) * (int32_t) num / (int32_t) den);
}

static uint16_t fan_for(const burn_params *p, uint8_t power) {
  return lerp16(p->fan_min_rpm, p->fan_max_rpm, power, 100);
}

static uint16_t pump_for(const burn_params *p, uint8_t power) {
  return lerp16(p->pump_min_cHz, p->pump_max_cHz, power, 100);
}

/* ---------------------------------------------------------------- board temperature */

/* The fan current recorded at a power level (1..10), between the three recorded points. */
static uint32_t level_fan_ma(const burn_params *p, uint32_t level) {
  if (level <= 6)
    return p->level1_fan_ma + (uint32_t) (p->level6_fan_ma - p->level1_fan_ma) * (level - 1u) / 5u;
  return p->level6_fan_ma + (uint32_t) (p->level10_fan_ma - p->level6_fan_ma) * (level - 6u) / 4u;
}

/* The power asked, under the board's cap (FAN-4). */
static uint8_t capped_power(const burn *b, const burn_inputs *in) {
  uint8_t cap = burn_power_cap(b, in->board_c10);
  return b->power < cap ? b->power : cap;
}

/* ... and held at the minimum while the heat exchanger is above the derate limit. */
static uint8_t heat_power(const burn *b, const burn_inputs *in) {
  return in->hx_c10 >= b->p.overheat_derate_c10 ? 0 : capped_power(b, in);
}

uint8_t burn_power_cap(const burn *b, int16_t board_c10) {
  const burn_params *p = &b->p;
  int board_hot = temp_known(board_c10) && board_c10 > p->derate_start_c10;
  if (!board_hot && b->fan_budget_ma == 0xFFFFu)
    return 100;
  uint32_t allowed = 0xFFFFFFFFu;
  if (board_hot) {
    allowed = p->level1_fan_ma;
    if (board_c10 < p->derate_full_c10)
      allowed = p->level10_fan_ma - (uint32_t) (p->level10_fan_ma - p->level1_fan_ma) *
                                        (uint32_t) (board_c10 - p->derate_start_c10) /
                                        (uint32_t) (p->derate_full_c10 - p->derate_start_c10);
  }
  if (b->fan_budget_ma < allowed) /* PWR-2: then the fan */
    allowed = b->fan_budget_ma;
  uint32_t level = 1;
  for (uint32_t n = 10; n >= 1; n--)
    if (b->level_ma[n - 1u] <= allowed) {
      level = n;
      break;
    }
  return (uint8_t) ((level - 1u) * 100u / 9u);
}

/* ---------------------------------------------------------------- glow bookkeeping */

static void glow_buckets_advance(burn *b, uint32_t now) {
  uint32_t gap = since(now, b->glow_bucket_start);
  if (gap >= BUCKET_MS * BUCKETS) {
    memset(b->glow_bucket, 0, sizeof(b->glow_bucket));
    b->glow_bucket_start = now;
    return;
  }
  while (since(now, b->glow_bucket_start) >= BUCKET_MS) {
    b->glow_bucket_i = (uint8_t) ((b->glow_bucket_i + 1) % BUCKETS);
    b->glow_bucket[b->glow_bucket_i] = 0;
    b->glow_bucket_start += BUCKET_MS;
  }
}

static uint32_t glow_window_ms(const burn *b) {
  uint32_t sum = 0;
  for (unsigned i = 0; i < BUCKETS; i++)
    sum += b->glow_bucket[i];
  return sum;
}

uint16_t burn_glow_window_s(const burn *b) { return (uint16_t) (glow_window_ms(b) / 1000u); }

/* ---------------------------------------------------------------- requests */

/* Off with the heat exchanger hot: it cools down first. */
static void cool_if_hot(burn *b, const burn_inputs *in, uint32_t now) {
  if (b->step == BURN_STEP_OFF && hot(b, in)) {
    b->fan_on_ms = now;
    enter(b, BURN_STEP_COOLDOWN, now);
  }
}

void burn_set_params(burn *b, const burn_params *p) {
  b->p = *p;
  for (uint32_t n = 1; n <= 10; n++)
    b->level_ma[n - 1u] = level_fan_ma(p, n);
}

void burn_init(burn *b, const burn_params *p, uint32_t now, const burn_inputs *in) {
  memset(b, 0, sizeof(*b));
  burn_set_params(b, p);
  b->last_ms = now;
  b->glow_bucket_start = now;
  b->reset_ms = now;
  b->reset_fan_due = 1;
  b->fan_budget_ma = 0xFFFFu;
  enter(b, BURN_STEP_OFF, now);
  /* A reset in the middle of a burn: the heat exchanger may be hot and the chamber may hold fuel.
   * Cool it down before anything else (the original spec, section 6.2). */
  if (in != NULL)
    cool_if_hot(b, in, now);
}

/* Whatever stands in the way of any start; sets fault and why. */
static int start_blocked(burn *b, const burn_inputs *in) {
  if (b->fault_latched)
    return 1;
  if (in->mcu_fault) {
    b->fault = BURN_FAULT_MCU, b->why = in->mcu_fault, b->fault_latched = 1;
    return 1;
  }
  if (in->overheat_latch) {
    b->fault = BURN_FAULT_OVERHEAT, b->why = BURN_WHY_OVERHEAT_LATCH, b->fault_latched = 1;
    return 1;
  }
  if (in->co_alarm) {
    b->fault = BURN_FAULT_CO, b->why = BURN_WHY_CO, b->fault_latched = 1;
    return 1;
  }
  if (in->co_fault) {
    b->fault = BURN_FAULT_CO_SENSOR, b->why = BURN_WHY_CO_SENSOR, b->fault_latched = 1;
    return 1;
  }
  if (in->supply_cv < b->p.supply_low_cv) {
    b->fault = BURN_FAULT_SUPPLY_LOW, b->why = BURN_WHY_SUPPLY;
    return 1;
  }
  if (in->supply_cv > b->p.supply_high_cv) {
    b->fault = BURN_FAULT_SUPPLY_HIGH, b->why = BURN_WHY_SUPPLY;
    return 1;
  }
  return 0;
}

int burn_start(burn *b, uint8_t mode, uint8_t power, uint32_t now, const burn_inputs *in) {
  if (mode != MODE_HEAT && mode != MODE_FAN && mode != MODE_PRIME)
    return R_NOT_SUPPORTED;
  if (b->step != BURN_STEP_OFF)
    return R_NOT_OFF;
  if (!b->fault_latched)
    b->fault = BURN_FAULT_NONE, b->why = BURN_WHY_NONE;
  if (start_blocked(b, in))
    return R_REFUSED;
  if (mode == MODE_HEAT) {
    if (!temp_known(in->hx_c10)) {
      b->fault = BURN_FAULT_SENSOR, b->why = BURN_WHY_SENSOR_OPEN;
      return R_REFUSED;
    }
    if (in->hx_c10 >= b->p.hot_start_c10)
      return R_NOT_OFF; /* still hot: it is not off yet as far as a start goes */
    /* The glow plug needs at least a full preheat left in its 10-minute allowance. */
    glow_buckets_advance(b, now);
    if (glow_window_ms(b) + MS(b->p.preheat_s) > MS(b->p.glow_window_max_s))
      return R_REFUSED;
  }
  b->mode = mode;
  b->power = power > 100 ? 100 : power;
  b->attempt = 1;
  b->fan_low.on = 0;
  b->supply_bad.on = 0;
  b->glow_attempt_ms = 0;
  b->fan_on_ms = now;
  b->air_ok = 0;
  /* Heat and priming may stroke the pump or heat the plug: the interlocks are tested first. */
  enter(b, mode == MODE_FAN ? BURN_STEP_FAN_ONLY : BURN_STEP_PRE_CHECK, now);
  return R_OK;
}

/* The step a fan retry stands for: the one it goes on with after its wait. */
static uint8_t effective_step(const burn *b) {
  return b->step == BURN_STEP_FAN_RETRY ? b->retry_step : b->step;
}

static int fuel_delivered(const burn *b) { return (STEPS[b->step].flags & FUEL) != 0; }

/* Where a stop goes from here: a burn-off once fuel went in, off before that. Off, or already
 * stopping (a retry on its way to a cooldown too), stays as it is. */
static uint8_t stop_route(const burn *b) {
  uint8_t s = effective_step(b);
  if (fuel_delivered(b))
    return BURN_STEP_BURN_OFF;
  if (s == BURN_STEP_OFF || s == BURN_STEP_BURN_OFF || s == BURN_STEP_COOLDOWN)
    return b->step;
  return BURN_STEP_OFF;
}

static void stop_now(burn *b, uint32_t now) {
  uint8_t to = stop_route(b);
  if (to != b->step)
    enter(b, to, now);
}

void burn_stop(burn *b, uint8_t why, uint32_t now) {
  if (why != BURN_WHY_ASKED && b->why == BURN_WHY_NONE)
    b->why = why;
  /* A stop from the link comes between passes: fuel and glow end now, not at the next pass (the
   * pump and the glow planner run every millisecond and every 5 ms from these outputs). */
  b->out.pump_cHz = 0;
  b->out.glow_w = 0;
  stop_now(b, now);
}

/* A fault that ends the run: fuel off at once, the fan cools the heater if it still can. Off, it is
 * only recorded. A latched fault stays the one shown until a fault reset: a later stop only acts. */
static void fault_stop(burn *b, uint8_t fault, uint8_t why, uint32_t now) {
  if (!b->fault_latched)
    b->fault = fault, b->why = why;
  stop_now(b, now);
}

/* A fault that needs a fault reset, when cond holds: 1 when it latched now. With a fault latched
 * already (this one or another), the first stays the one shown until a fault reset: a further cause
 * keeps the heater stopping and blocks the reset (burn_fault_reset), but does not take its place
 * (two causes used to swap places every pass). */
static int latch(burn *b, int cond, uint8_t fault, uint8_t why, uint32_t now) {
  if (!cond)
    return 0;
  if (b->fault_latched) {
    stop_now(b, now);
    return 0;
  }
  fault_stop(b, fault, why, now);
  b->fault_latched = 1;
  return 1;
}

/* The fan stalled or never turned (FAN-3, FAN-5): fuel, glow and fan off at once; the fan tries
 * again after a wait, up to fan_retries times; one stall more locks it out until a fault reset. */
static void fan_stalled(burn *b, uint8_t why, uint32_t now, const burn_inputs *in) {
  if (!b->fault_latched) /* a latched fault stays the one shown (latch) */
    b->fault = BURN_FAULT_FAN, b->why = why;
  b->fan_ok_ms = 0;
  b->fan_low.on = 0;
  if (++b->fan_stalls > b->p.fan_retries) {
    b->fault_latched = 1;
    enter(b, BURN_STEP_OFF, now);
    return;
  }
  if (b->step == BURN_STEP_OFF)
    return;
  if (fuel_delivered(b) || b->step == BURN_STEP_BURN_OFF || b->step == BURN_STEP_COOLDOWN || hot(b, in))
    b->retry_step = BURN_STEP_COOLDOWN;
  else if (b->step == BURN_STEP_FAN_ONLY)
    b->retry_step = BURN_STEP_FAN_ONLY;
  else
    b->retry_step = BURN_STEP_SELF_TEST;
  enter(b, BURN_STEP_FAN_RETRY, now);
}

void burn_set_power(burn *b, uint8_t power) { b->power = power > 100 ? 100 : power; }

int burn_latch_causes(const burn_inputs *in, burn_cause out[BURN_CAUSES_MAX]) {
  /* As checks() latches them (a CO alarm hides the sensor fault); a fan stall (fan_fault 1) is retried. */
  const struct {
    int on;
    uint8_t fault, why;
  } c[] = {
      {in->mcu_fault != 0, BURN_FAULT_MCU, in->mcu_fault},
      {in->overheat_latch, BURN_FAULT_OVERHEAT, BURN_WHY_OVERHEAT_LATCH},
      {in->glow_fault, BURN_FAULT_GLOW, BURN_WHY_GLOW_STAGE},
      {in->fan_fault > 1, BURN_FAULT_FAN, BURN_WHY_FAN_STAGE},
      {in->pump_fault, BURN_FAULT_PUMP, BURN_WHY_PUMP_STAGE},
      {in->co_alarm, BURN_FAULT_CO, BURN_WHY_CO},
      {in->co_fault && !in->co_alarm, BURN_FAULT_CO_SENSOR, BURN_WHY_CO_SENSOR},
  };
  int n = 0;
  for (unsigned i = 0; i < sizeof(c) / sizeof(c[0]) && n < BURN_CAUSES_MAX; i++)
    if (c[i].on)
      out[n].fault = c[i].fault, out[n].why = c[i].why, n++;
  return n;
}

int burn_fault_reset(burn *b, const burn_inputs *in) {
  if (!b->fault_latched)
    return R_REFUSED;
  if (in->overheat_latch || in->co_alarm || in->co_fault || in->glow_fault || in->fan_fault || in->pump_fault ||
      in->mcu_fault)
    return R_REFUSED; /* the cause is still there */
  b->fault_latched = 0;
  b->fault = BURN_FAULT_NONE;
  b->why = BURN_WHY_NONE;
  b->fan_stalls = 0;
  /* Off with the heat exchanger still hot (a fan lockout stops everything at once): it cools down
   * first, as after an MCU reset. A fan still jammed stalls again and locks out again. */
  cool_if_hot(b, in, b->last_ms);
  return R_OK;
}

/* ---------------------------------------------------------------- the loop */

uint8_t burn_state(const burn *b) {
  if (b->step == BURN_STEP_FAN_RETRY && b->retry_step == BURN_STEP_COOLDOWN)
    return BURN_STATE_STOPPING; /* a fan retry on its way to the cooldown */
  return STEPS[effective_step(b)].state;
}

int burn_active(const burn *b) { return (STEPS[effective_step(b)].flags & ACTIVE) != 0; }

int burn_burning(const burn *b) { return fuel_delivered(b); }

int burn_heartbeat_wanted(const burn *b) { return (STEPS[b->step].flags & HEARTBEAT) != 0; }

uint16_t burn_prime_left_s(const burn *b, uint32_t now) {
  if (b->step != BURN_STEP_PRIMING)
    return 0;
  uint32_t done = since(now, b->step_ms) / 1000u;
  return done >= b->p.prime_s ? 0 : (uint16_t) (b->p.prime_s - done);
}

/* Safety checks that hold in every step; they may end the run. */
static void checks(burn *b, uint32_t now, uint32_t dt, const burn_inputs *in) {
  const burn_params *p = &b->p;
  int burning = fuel_delivered(b);

  /* The latched faults: no restart until a fault reset. The MCU's own supply or clock first: every
   * other reading and timing rests on them (PWR-5, GLW-1). */
  if (latch(b, in->mcu_fault != 0, BURN_FAULT_MCU, in->mcu_fault, now))
    return;
  if (in->mcu_fault && b->fault == BURN_FAULT_MCU)
    b->why = in->mcu_fault; /* a VDDA fault in place of a tick fault: the reason follows */
  if (latch(b, in->overheat_latch, BURN_FAULT_OVERHEAT, BURN_WHY_OVERHEAT_LATCH, now))
    return; /* OVH-4: fuel off, fan on */
  if (latch(b, in->glow_fault, BURN_FAULT_GLOW, BURN_WHY_GLOW_STAGE, now))
    return; /* the board needs looking at (GLW-6, GLW-9) */
  if (in->fan_fault == 1 && !b->fault_latched && b->step != BURN_STEP_FAN_RETRY) {
    /* The fan stage saw current with no turning (FAN-3): a stall, retried (FAN-5). */
    fan_stalled(b, BURN_WHY_FAN_STALL, now, in);
    return;
  }
  /* FAN-3, FAN-6: the switch stuck on, or the fan turning against its command. Fuel and plug off at
   * once, an alarm; the fan keeps turning by itself (the safe side) while it burns off. */
  if (latch(b, in->fan_fault > 1, BURN_FAULT_FAN, BURN_WHY_FAN_STAGE, now))
    return;
  /* The pump stage stopped itself (and the heartbeat): burn off and cool. */
  if (latch(b, in->pump_fault, BURN_FAULT_PUMP, BURN_WHY_PUMP_STAGE, now))
    return;
  if (latch(b, in->co_alarm, BURN_FAULT_CO, BURN_WHY_CO, now))
    return;
  /* CO-2: a sensor fault is an alarm too: the same heater-safe action. */
  if (latch(b, in->co_fault && !in->co_alarm, BURN_FAULT_CO_SENSOR, BURN_WHY_CO_SENSOR, now))
    return;
  if (b->step == BURN_STEP_OFF)
    return;

  /* The heat exchanger sensor: open while fuel may burn. */
  if (burning && !temp_known(in->hx_c10)) {
    fault_stop(b, BURN_FAULT_SENSOR, BURN_WHY_SENSOR_OPEN, now);
    return;
  }
  if (burning && in->hx_c10 >= p->overheat_stop_c10) {
    fault_stop(b, BURN_FAULT_OVERHEAT, BURN_WHY_OVERHEAT_LIMIT, now);
    return;
  }

  /* Board too hot even at the lowest power (FAN-4). */
  int board_hot = temp_known(in->board_c10) && in->board_c10 >= p->board_stop_c10 && burn_active(b);
  if (held_for(&b->board_hot, board_hot, now, p->board_stop_ms)) {
    fault_stop(b, BURN_FAULT_OVERHEAT, BURN_WHY_BOARD_HOT, now);
    return;
  }

  /* Supply out of range for a while (timed whether or not a run is active). */
  int bad = in->supply_cv < p->supply_low_cv || in->supply_cv > p->supply_high_cv;
  if (held_for(&b->supply_bad, bad, now, p->supply_fault_ms) && burn_active(b)) {
    fault_stop(b, in->supply_cv < p->supply_low_cv ? BURN_FAULT_SUPPLY_LOW : BURN_FAULT_SUPPLY_HIGH,
               BURN_WHY_SUPPLY, now);
    return;
  }

  /* Fan stall (FAN-3): asked to turn, past its spin-up time, and still not turning. */
  int asked = b->out.fan_rpm > 0 && since(now, b->fan_on_ms) >= p->fan_spin_up_ms;
  if (held_for(&b->fan_low, asked && in->fan_rpm < p->fan_stall_rpm, now, p->fan_stall_ms)) {
    fan_stalled(b, BURN_WHY_FAN_STALL, now, in); /* no air: everything off at once */
    return;
  }
  if (asked && in->fan_rpm >= p->fan_stall_rpm && b->fan_stalls > 0) {
    /* Turning as asked: the stall count forgets in time (FAN-5). */
    b->fan_ok_ms += dt;
    if (b->fan_ok_ms >= MS(p->fan_retry_forget_s))
      b->fan_stalls = 0, b->fan_ok_ms = 0;
  }
}

/* The flame's peak, and its loss: a fall below the peak by flame_lost_c10 stops the run (1). */
static int flame_lost(burn *b, uint32_t now, const burn_inputs *in) {
  if (in->hx_c10 > b->peak_c10)
    b->peak_c10 = in->hx_c10;
  if (in->hx_c10 < b->peak_c10 - b->p.flame_lost_c10) {
    fault_stop(b, BURN_FAULT_FLAME_OUT, BURN_WHY_FLAME_LOST, now);
    return 1;
  }
  return 0;
}

static void sequence(burn *b, uint32_t now, uint32_t dt, const burn_inputs *in) {
  const burn_params *p = &b->p;
  uint32_t in_step = since(now, b->step_ms);
  burn_outputs *o = &b->out;
  int want_glow = 0;

  o->fan_rpm = 0, o->pump_cHz = 0, o->glow_w = 0;
  b->running_power = 0;

  switch (b->step) {
    case BURN_STEP_OFF:
      break;

    case BURN_STEP_PRE_CHECK: /* nothing runs while the interlocks are tested */
      if (in->precheck == BURN_PRECHECK_PASSED) {
        enter(b, b->mode == MODE_PRIME ? BURN_STEP_PRIMING : BURN_STEP_SELF_TEST, now);
      } else if (in->precheck != BURN_PRECHECK_RUNNING || in_step >= p->precheck_max_ms) {
        b->fault = in->precheck == BURN_PRECHECK_FAILED_PUMP   ? BURN_FAULT_PUMP
                   : in->precheck == BURN_PRECHECK_FAILED_GLOW ? BURN_FAULT_GLOW
                                                               : BURN_FAULT_START;
        b->why = BURN_WHY_SELF_TEST;
        b->fault_latched = 1;
        enter(b, BURN_STEP_OFF, now);
      }
      break;

    case BURN_STEP_SELF_TEST: {
      o->fan_rpm = p->fan_start_rpm;
      /* The first heat start soon after a reset runs the fan longer before the glow (GLW-21). */
      int longer = b->reset_fan_due && since(now, b->reset_ms) < MS(p->reset_fan_within_s);
      if (in->fan_rpm >= p->glow_min_fan_rpm) {
        if (!b->air_ok)
          b->air_ok = 1, b->air_ok_ms = now;
        if (!longer || since(now, b->air_ok_ms) >= MS(p->reset_fan_s)) {
          b->reset_fan_due = 0;
          b->glow_attempt_ms = 0;
          enter(b, BURN_STEP_PREHEAT, now);
        }
      } else if (in_step >= p->fan_spin_up_ms) {
        fan_stalled(b, BURN_WHY_FAN_SPIN_UP, now, in);
        o->fan_rpm = 0;
      }
      break;
    }

    case BURN_STEP_FAN_RETRY:
      if (in_step >= MS(p->fan_retry_wait_s)) {
        if (b->retry_step != BURN_STEP_COOLDOWN)
          b->fault = BURN_FAULT_NONE, b->why = BURN_WHY_NONE; /* the run carries on */
        b->air_ok = 0;
        enter(b, b->retry_step, now);
      }
      break;

    case BURN_STEP_PREHEAT:
      o->fan_rpm = p->fan_start_rpm;
      want_glow = 1;
      if (in->plug_refused) { /* GLW-10 to GLW-13: not this plug, or not checked */
        b->fault = BURN_FAULT_GLOW, b->why = BURN_WHY_PLUG_TYPE;
        enter(b, BURN_STEP_OFF, now);
        break;
      }
      if (in_step >= MS(p->preheat_s)) {
        b->start_c10 = in->hx_c10;
        b->peak_c10 = in->hx_c10;
        enter(b, BURN_STEP_IGNITION, now);
      }
      break;

    case BURN_STEP_IGNITION:
      o->fan_rpm = p->fan_start_rpm;
      o->pump_cHz = p->pump_start_cHz;
      want_glow = 1;
      if (in->hx_c10 >= b->start_c10 + p->flame_rise_c10) {
        b->flame_ms = now;
        b->peak_c10 = in->hx_c10;
        enter(b, BURN_STEP_WARM_UP, now);
      } else if (in_step >= MS(p->ignition_s)) {
        if (b->attempt < p->attempts)
          enter(b, BURN_STEP_PURGE, now);
        else
          fault_stop(b, BURN_FAULT_START, BURN_WHY_NO_FLAME, now);
      }
      break;

    case BURN_STEP_PURGE:
      o->fan_rpm = p->fan_burn_off_rpm;
      if (in_step >= MS(p->purge_s)) {
        b->attempt++;
        b->glow_attempt_ms = 0;
        enter(b, BURN_STEP_PREHEAT, now);
      }
      break;

    case BURN_STEP_WARM_UP: {
      uint32_t ramp = MS(p->warm_up_s);
      uint8_t target = heat_power(b, in);
      o->fan_rpm = lerp16(p->fan_start_rpm, fan_for(p, target), in_step, ramp);
      o->pump_cHz = lerp16(p->pump_start_cHz, pump_for(p, target), in_step, ramp);
      b->running_power = (uint8_t) (in_step >= ramp ? target : (uint32_t) target * in_step / ramp);
      want_glow = since(now, b->flame_ms) < MS(p->glow_after_flame_s);
      if (!flame_lost(b, now, in) && in_step >= ramp) {
        b->fuel_mp = b->air_mp = (uint32_t) target * 1000u;
        enter(b, BURN_STEP_RUNNING, now);
      }
      break;
    }

    case BURN_STEP_RUNNING: {
      uint32_t want = (uint32_t) heat_power(b, in) * 1000u;
      /* Fuel follows air (FAN-4): the pump falls at once and rises slowly; the fan rises at once
       * and falls slowly; the fan is never below the fuel. */
      uint32_t up = (uint32_t) p->fuel_up_percent_s * dt;
      uint32_t down = (uint32_t) p->air_down_percent_s * dt;
      if (want < b->fuel_mp)
        b->fuel_mp = want;
      else
        b->fuel_mp = b->fuel_mp + up < want ? b->fuel_mp + up : want;
      if (want > b->air_mp)
        b->air_mp = want;
      else
        b->air_mp = b->air_mp > want + down ? b->air_mp - down : want;
      if (b->air_mp < b->fuel_mp)
        b->air_mp = b->fuel_mp;
      o->fan_rpm = fan_for(p, (uint8_t) ((b->air_mp + 999u) / 1000u));
      o->pump_cHz = pump_for(p, (uint8_t) (b->fuel_mp / 1000u));
      b->running_power = (uint8_t) (b->fuel_mp / 1000u);
      (void) flame_lost(b, now, in);
      break;
    }

    case BURN_STEP_BURN_OFF:
      o->fan_rpm = p->fan_burn_off_rpm;
      if (in_step >= MS(p->burn_off_s))
        enter(b, BURN_STEP_COOLDOWN, now);
      break;

    case BURN_STEP_COOLDOWN:
      o->fan_rpm = p->fan_burn_off_rpm;
      if (in_step >= MS(p->cooldown_min_s) && temp_known(in->hx_c10) &&
          in->hx_c10 < p->cool_c10) {
        enter(b, BURN_STEP_OFF, now);
      } else if (in_step >= MS(p->cooldown_max_s)) {
        if (temp_known(in->hx_c10) && b->fault == BURN_FAULT_NONE)
          b->fault = BURN_FAULT_SENSOR, b->why = BURN_WHY_COOLDOWN_LIMIT;
        enter(b, BURN_STEP_OFF, now);
      }
      break;

    case BURN_STEP_FAN_ONLY:
      o->fan_rpm = fan_for(p, capped_power(b, in));
      break;

    case BURN_STEP_PRIMING:
      o->pump_cHz = p->pump_prime_cHz;
      if (in_step >= MS(p->prime_s))
        enter(b, BURN_STEP_OFF, now);
      break;
  }

  /* A step change above may have ended the run: outputs follow the new step on the next pass,
   * except that nothing may start burning in the pass that stopped it. */
  if (b->step == BURN_STEP_OFF || b->step == BURN_STEP_FAN_RETRY)
    o->fan_rpm = 0, o->pump_cHz = 0, want_glow = 0;
  if (b->step == BURN_STEP_BURN_OFF || b->step == BURN_STEP_COOLDOWN) {
    o->pump_cHz = 0;
    want_glow = 0;
    if (o->fan_rpm == 0)
      o->fan_rpm = p->fan_burn_off_rpm;
  }

  /* Glow: only with measured airflow (GLW-14) and inside both on-time caps (GLW-1). */
  int glow = want_glow && in->fan_rpm >= p->glow_min_fan_rpm &&
             b->glow_attempt_ms < MS(p->glow_attempt_max_s) && glow_window_ms(b) < MS(p->glow_window_max_s);
  o->glow_w = glow ? p->glow_w : 0;
  b->glow_on = (uint8_t) glow;
}

void burn_step(burn *b, uint32_t now, const burn_inputs *in) {
  uint32_t dt = since(now, b->last_ms);
  b->last_ms = now;
  glow_buckets_advance(b, now);
  if (b->glow_on) {
    b->glow_attempt_ms += dt;
    b->glow_bucket[b->glow_bucket_i] += dt;
  }
  uint16_t fan_before = b->out.fan_rpm;
  checks(b, now, dt, in);
  /* A step that ends in this pass hands over at once: the outputs are always the new step's. */
  for (int i = 0; i < 4; i++) {
    uint8_t step = b->step;
    sequence(b, now, dt, in);
    if (b->step == step)
      break;
  }
  if (fan_before == 0 && b->out.fan_rpm > 0)
    b->fan_on_ms = now;
}
