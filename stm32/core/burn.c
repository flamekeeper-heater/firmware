/* The burn sequencer: see burn.h. */
#include "burn.h"

#include <string.h>

enum { R_OK = 0x00, R_NOT_SUPPORTED = 0x13, R_NOT_OFF = 0x20, R_REFUSED = 0x2F };
enum { MODE_OFF = 0, MODE_HEAT = 1, MODE_FAN = 2, MODE_PRIME = 3 };

#define BUCKET_MS 60000u
#define BUCKETS 10u

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
}

static uint32_t since(uint32_t now, uint32_t then) { return (uint32_t) (now - then); }

static void enter(burn *b, uint8_t step, uint32_t now) {
  b->step = step;
  b->step_ms = now;
}

static int temp_known(int16_t t) { return t != BURN_UNKNOWN_TEMP; }

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

void burn_init(burn *b, const burn_params *p, uint32_t now, const burn_inputs *in) {
  memset(b, 0, sizeof(*b));
  b->p = *p;
  b->last_ms = now;
  b->glow_bucket_start = now;
  enter(b, BURN_STEP_OFF, now);
  /* A reset in the middle of a burn: the heat exchanger may be hot and the chamber may hold fuel.
   * Cool it down before anything else (the original spec, section 6.2). */
  if (in != NULL && temp_known(in->hx_c10) && in->hx_c10 >= p->hot_start_c10) {
    b->fan_on_ms = now;
    enter(b, BURN_STEP_COOLDOWN, now);
  }
}

/* Whatever stands in the way of any start; sets fault and why. */
static int start_blocked(burn *b, const burn_inputs *in) {
  if (b->fault_latched)
    return 1;
  if (in->overheat_latch) {
    b->fault = BURN_FAULT_OVERHEAT, b->why = BURN_WHY_OVERHEAT_LATCH, b->fault_latched = 1;
    return 1;
  }
  if (in->co_alarm) {
    b->fault = BURN_FAULT_CO, b->why = BURN_WHY_CO, b->fault_latched = 1;
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
    if (glow_window_ms(b) + (uint32_t) b->p.preheat_s * 1000u > (uint32_t) b->p.glow_window_max_s * 1000u)
      return R_REFUSED;
  }
  b->mode = mode;
  b->power = power > 100 ? 100 : power;
  b->attempt = 1;
  b->flame = 0;
  b->fan_low = 0;
  b->supply_bad = 0;
  b->glow_attempt_ms = 0;
  b->fan_on_ms = now;
  if (mode == MODE_HEAT)
    enter(b, BURN_STEP_SELF_TEST, now);
  else if (mode == MODE_FAN)
    enter(b, BURN_STEP_FAN_ONLY, now);
  else
    enter(b, BURN_STEP_PRIMING, now);
  return R_OK;
}

/* Fuel has gone in since the start: stopping needs a burn-off and a cooldown. */
static int fuel_delivered(const burn *b) {
  return b->step == BURN_STEP_IGNITION || b->step == BURN_STEP_PURGE ||
         b->step == BURN_STEP_WARM_UP || b->step == BURN_STEP_RUNNING;
}

void burn_stop(burn *b, uint8_t why, uint32_t now) {
  if (why != BURN_WHY_ASKED && b->why == BURN_WHY_NONE)
    b->why = why;
  if (fuel_delivered(b)) {
    enter(b, BURN_STEP_BURN_OFF, now);
    return;
  }
  switch (b->step) {
    case BURN_STEP_SELF_TEST:
    case BURN_STEP_PREHEAT:
    case BURN_STEP_FAN_ONLY:
    case BURN_STEP_PRIMING:
      enter(b, BURN_STEP_OFF, now);
      break;
    default:
      break; /* off, or already stopping */
  }
}

/* A fault that ends the run: fuel off at once, the fan cools the heater if it still can. */
static void fault_stop(burn *b, uint8_t fault, uint8_t why, uint32_t now) {
  b->fault = fault;
  b->why = why;
  if (fuel_delivered(b))
    enter(b, BURN_STEP_BURN_OFF, now);
  else if (b->step != BURN_STEP_BURN_OFF && b->step != BURN_STEP_COOLDOWN)
    enter(b, BURN_STEP_OFF, now);
}

void burn_set_power(burn *b, uint8_t power) { b->power = power > 100 ? 100 : power; }

int burn_fault_reset(burn *b, const burn_inputs *in) {
  if (!b->fault_latched)
    return R_REFUSED;
  if (in->overheat_latch || in->co_alarm || in->glow_fault || in->fan_fault)
    return R_REFUSED; /* the cause is still there */
  b->fault_latched = 0;
  b->fault = BURN_FAULT_NONE;
  b->why = BURN_WHY_NONE;
  return R_OK;
}

/* ---------------------------------------------------------------- the loop */

uint8_t burn_state(const burn *b) {
  switch (b->step) {
    case BURN_STEP_SELF_TEST:
    case BURN_STEP_PREHEAT:
    case BURN_STEP_IGNITION:
    case BURN_STEP_PURGE:
    case BURN_STEP_WARM_UP:
      return BURN_STATE_STARTING;
    case BURN_STEP_RUNNING:
      return BURN_STATE_HEATING;
    case BURN_STEP_BURN_OFF:
      return BURN_STATE_STOPPING;
    case BURN_STEP_COOLDOWN:
      return BURN_STATE_COOLING;
    case BURN_STEP_FAN_ONLY:
      return BURN_STATE_FAN_ONLY;
    case BURN_STEP_PRIMING:
      return BURN_STATE_PRIMING;
    default:
      return BURN_STATE_OFF;
  }
}

int burn_active(const burn *b) {
  return b->step != BURN_STEP_OFF && b->step != BURN_STEP_BURN_OFF && b->step != BURN_STEP_COOLDOWN;
}

int burn_fuel_steps(const burn *b) { return fuel_delivered(b) || b->step == BURN_STEP_PRIMING; }

uint16_t burn_prime_left_s(const burn *b, uint32_t now) {
  if (b->step != BURN_STEP_PRIMING)
    return 0;
  uint32_t done = since(now, b->step_ms) / 1000u;
  return done >= b->p.prime_s ? 0 : (uint16_t) (b->p.prime_s - done);
}

/* Safety checks that hold in every step; they may end the run. */
static void checks(burn *b, uint32_t now, const burn_inputs *in) {
  const burn_params *p = &b->p;
  int burning = fuel_delivered(b);

  if (in->overheat_latch && !(b->fault == BURN_FAULT_OVERHEAT && b->fault_latched)) {
    b->fault_latched = 1;
    if (b->step == BURN_STEP_OFF)
      b->fault = BURN_FAULT_OVERHEAT, b->why = BURN_WHY_OVERHEAT_LATCH;
    else
      fault_stop(b, BURN_FAULT_OVERHEAT, BURN_WHY_OVERHEAT_LATCH, now); /* OVH-4: fuel off, fan on */
    return;
  }
  if (in->glow_fault && !(b->fault == BURN_FAULT_GLOW && b->fault_latched)) {
    /* The board needs looking at: no restart until a reset (GLW-6, GLW-9). */
    b->fault_latched = 1;
    if (b->step == BURN_STEP_OFF)
      b->fault = BURN_FAULT_GLOW, b->why = BURN_WHY_GLOW_STAGE;
    else
      fault_stop(b, BURN_FAULT_GLOW, BURN_WHY_GLOW_STAGE, now);
    return;
  }
  if (in->fan_fault && !(b->fault == BURN_FAULT_FAN && b->fault_latched)) {
    /* FAN-3: fuel and plug off at once, an alarm, no restart until a reset. A stalled fan gives no
     * air at all; otherwise the fan keeps turning by itself (the safe side) while it burns off. */
    uint8_t why = in->fan_fault == 1 ? BURN_WHY_FAN_STALL : BURN_WHY_FAN_STAGE;
    b->fault_latched = 1;
    if (b->step == BURN_STEP_OFF || in->fan_fault == 1)
      b->fault = BURN_FAULT_FAN, b->why = why, enter(b, BURN_STEP_OFF, now);
    else
      fault_stop(b, BURN_FAULT_FAN, why, now);
    return;
  }
  if (in->co_alarm && !(b->fault == BURN_FAULT_CO && b->fault_latched)) {
    b->fault_latched = 1;
    if (b->step == BURN_STEP_OFF)
      b->fault = BURN_FAULT_CO, b->why = BURN_WHY_CO;
    else
      fault_stop(b, BURN_FAULT_CO, BURN_WHY_CO, now);
    return;
  }
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

  /* Supply out of range for a while. */
  int bad = in->supply_cv < p->supply_low_cv || in->supply_cv > p->supply_high_cv;
  if (bad && !b->supply_bad)
    b->supply_bad = 1, b->supply_bad_ms = now;
  else if (!bad)
    b->supply_bad = 0;
  if (b->supply_bad && since(now, b->supply_bad_ms) >= p->supply_fault_ms && burn_active(b)) {
    fault_stop(b, in->supply_cv < p->supply_low_cv ? BURN_FAULT_SUPPLY_LOW : BURN_FAULT_SUPPLY_HIGH,
               BURN_WHY_SUPPLY, now);
    return;
  }

  /* Fan stall (FAN-3): asked to turn, past its spin-up time, and still not turning. */
  if (b->out.fan_rpm > 0 && since(now, b->fan_on_ms) >= p->fan_spin_up_ms) {
    if (in->fan_rpm < p->fan_stall_rpm) {
      if (!b->fan_low)
        b->fan_low = 1, b->fan_low_ms = now;
      else if (since(now, b->fan_low_ms) >= p->fan_stall_ms) {
        /* No air: fuel and glow off at once, and the fan too (it does not turn). */
        b->fault = BURN_FAULT_FAN;
        b->why = BURN_WHY_FAN_STALL;
        enter(b, BURN_STEP_OFF, now);
        return;
      }
    } else {
      b->fan_low = 0;
    }
  } else {
    b->fan_low = 0;
  }
}

static void sequence(burn *b, uint32_t now, const burn_inputs *in) {
  const burn_params *p = &b->p;
  uint32_t in_step = since(now, b->step_ms);
  burn_outputs *o = &b->out;
  int want_glow = 0;

  o->fan_rpm = 0, o->pump_cHz = 0, o->glow_w = 0;
  b->running_power = 0;

  switch (b->step) {
    case BURN_STEP_OFF:
      break;

    case BURN_STEP_SELF_TEST:
      o->fan_rpm = p->fan_start_rpm;
      if (in->fan_rpm >= p->glow_min_fan_rpm) {
        b->glow_attempt_ms = 0;
        enter(b, BURN_STEP_PREHEAT, now);
      } else if (in_step >= p->fan_spin_up_ms) {
        b->fault = BURN_FAULT_FAN, b->why = BURN_WHY_FAN_SPIN_UP;
        enter(b, BURN_STEP_OFF, now);
        o->fan_rpm = 0;
      }
      break;

    case BURN_STEP_PREHEAT:
      o->fan_rpm = p->fan_start_rpm;
      want_glow = 1;
      if (in_step >= (uint32_t) p->preheat_s * 1000u) {
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
        b->flame = 1;
        b->flame_ms = now;
        b->peak_c10 = in->hx_c10;
        enter(b, BURN_STEP_WARM_UP, now);
      } else if (in_step >= (uint32_t) p->ignition_s * 1000u) {
        if (b->attempt < p->attempts)
          enter(b, BURN_STEP_PURGE, now);
        else
          fault_stop(b, BURN_FAULT_START, BURN_WHY_NO_FLAME, now);
      }
      break;

    case BURN_STEP_PURGE:
      o->fan_rpm = p->fan_burn_off_rpm;
      if (in_step >= (uint32_t) p->purge_s * 1000u) {
        b->attempt++;
        b->glow_attempt_ms = 0;
        enter(b, BURN_STEP_PREHEAT, now);
      }
      break;

    case BURN_STEP_WARM_UP: {
      uint32_t ramp = (uint32_t) p->warm_up_s * 1000u;
      uint8_t target = in->hx_c10 >= p->overheat_derate_c10 ? 0 : b->power;
      o->fan_rpm = lerp16(p->fan_start_rpm, fan_for(p, target), in_step, ramp);
      o->pump_cHz = lerp16(p->pump_start_cHz, pump_for(p, target), in_step, ramp);
      b->running_power = (uint8_t) (in_step >= ramp ? target : (uint32_t) target * in_step / ramp);
      want_glow = since(now, b->flame_ms) < (uint32_t) p->glow_after_flame_s * 1000u;
      if (in->hx_c10 > b->peak_c10)
        b->peak_c10 = in->hx_c10;
      if (in->hx_c10 < b->peak_c10 - p->flame_lost_c10)
        fault_stop(b, BURN_FAULT_FLAME_OUT, BURN_WHY_FLAME_LOST, now);
      else if (in_step >= ramp)
        enter(b, BURN_STEP_RUNNING, now);
      break;
    }

    case BURN_STEP_RUNNING: {
      uint8_t power = in->hx_c10 >= p->overheat_derate_c10 ? 0 : b->power;
      o->fan_rpm = fan_for(p, power);
      o->pump_cHz = pump_for(p, power);
      b->running_power = power;
      if (in->hx_c10 > b->peak_c10)
        b->peak_c10 = in->hx_c10;
      if (in->hx_c10 < b->peak_c10 - p->flame_lost_c10)
        fault_stop(b, BURN_FAULT_FLAME_OUT, BURN_WHY_FLAME_LOST, now);
      break;
    }

    case BURN_STEP_BURN_OFF:
      o->fan_rpm = p->fan_burn_off_rpm;
      if (in_step >= (uint32_t) p->burn_off_s * 1000u)
        enter(b, BURN_STEP_COOLDOWN, now);
      break;

    case BURN_STEP_COOLDOWN:
      o->fan_rpm = p->fan_burn_off_rpm;
      if (in_step >= (uint32_t) p->cooldown_min_s * 1000u && temp_known(in->hx_c10) &&
          in->hx_c10 < p->cool_c10) {
        enter(b, BURN_STEP_OFF, now);
      } else if (in_step >= (uint32_t) p->cooldown_max_s * 1000u) {
        if (temp_known(in->hx_c10) && b->fault == BURN_FAULT_NONE)
          b->fault = BURN_FAULT_SENSOR, b->why = BURN_WHY_COOLDOWN_LIMIT;
        enter(b, BURN_STEP_OFF, now);
      }
      break;

    case BURN_STEP_FAN_ONLY:
      o->fan_rpm = fan_for(p, b->power);
      break;

    case BURN_STEP_PRIMING:
      o->pump_cHz = p->pump_prime_cHz;
      if (in_step >= (uint32_t) p->prime_s * 1000u)
        enter(b, BURN_STEP_OFF, now);
      break;
  }

  /* A step change above may have ended the run: outputs follow the new step on the next pass,
   * except that nothing may start burning in the pass that stopped it. */
  if (b->step == BURN_STEP_OFF)
    o->fan_rpm = 0, o->pump_cHz = 0, want_glow = 0;
  if (b->step == BURN_STEP_BURN_OFF || b->step == BURN_STEP_COOLDOWN) {
    o->pump_cHz = 0;
    want_glow = 0;
    if (o->fan_rpm == 0)
      o->fan_rpm = p->fan_burn_off_rpm;
  }

  /* Glow: only with measured airflow (GLW-14) and inside both on-time caps (GLW-1). */
  int glow = want_glow && in->fan_rpm >= p->glow_min_fan_rpm &&
             b->glow_attempt_ms < (uint32_t) p->glow_attempt_max_s * 1000u &&
             glow_window_ms(b) < (uint32_t) p->glow_window_max_s * 1000u;
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
  checks(b, now, in);
  /* A step that ends in this pass hands over at once: the outputs are always the new step's. */
  for (int i = 0; i < 4; i++) {
    uint8_t step = b->step;
    sequence(b, now, in);
    if (b->step == step)
      break;
  }
  if (fan_before == 0 && b->out.fan_rpm > 0)
    b->fan_on_ms = now;
}
