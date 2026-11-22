/* The fan stage controller: see fan.h. */
#include "fan.h"

#include <string.h>

void fan_params_default(fan_params *p) {
  memset(p, 0, sizeof(*p));
  p->v_max_cv = 1000;
  p->rpm_at_v_max = 5000;
  p->kp_permille_per_krpm = 150;
  p->ki_permille_per_krpm_s = 150;
  p->ramp_ms = 1000;
  p->start_limit_ma = 6000;
  p->start_limit_ms = 50;
  p->back_off_permille = 20;
  p->estimate_min_permille = 100;
  p->turning_rpm = 100;
  p->stall_ma = 100;
  p->stall_ms = 1000;
  p->stuck_on_ma = 200;
  p->stuck_on_ms = 100;
  p->spin_down_ms = 30000;
  p->overspeed_percent = 20;
}

static uint32_t since(uint32_t now, uint32_t then) { return (uint32_t) (now - then); }

void fan_init(fan *f, const fan_params *p, uint32_t now_ms) {
  fan_params keep = *p; /* p may point into f: a restart with its own parameters */
  memset(f, 0, sizeof(*f));
  f->p = keep;
  f->last_ms = now_ms;
  f->target_dropped_ms = now_ms; /* a fan still coasting after a reset gets its spin-down time */
}

uint16_t fan_duty_cap(const fan *f, uint16_t vin_cv) {
  if (vin_cv == 0)
    return 0;
  uint32_t cap = (uint32_t) f->p.v_max_cv * 1000u / vin_cv;
  return (uint16_t) (cap > 1000u ? 1000u : cap);
}

static uint16_t set(fan *f, uint16_t duty) {
  f->duty = duty;
  return duty;
}

static uint16_t fail(fan *f, uint8_t fault) {
  f->fault = fault;
  f->integ = 0;
  return set(f, 0);
}

/* One pass. open_permille > 0: open loop at that duty (fan_step_open), else the speed loop. */
static uint16_t step(fan *f, uint32_t now_ms, uint16_t target_rpm, uint16_t open_permille, const fan_meas *m) {
  const fan_params *p = &f->p;
  uint32_t dt = since(now_ms, f->last_ms);
  f->last_ms = now_ms;
  if (f->fault != FAN_FAULT_NONE)
    return set(f, 0);

  /* Current while the switch was off: it is stuck on (FAN-6). */
  if (f->duty == 0 && m->isense_ma > p->stuck_on_ma) {
    if (!f->stuck_timing)
      f->stuck_timing = 1, f->stuck_ms = now_ms;
    else if (since(now_ms, f->stuck_ms) >= p->stuck_on_ms)
      return fail(f, FAN_FAULT_STUCK_ON);
  } else {
    f->stuck_timing = 0;
  }

  if (open_permille > 0) {
    /* No speed to compare with: the start limit always applies, and the speed checks wait for
     * the next command, which counts as a drop (its spin-down time starts then). */
    target_rpm = 0xFFFFu;
    f->target = 0xFFFFu;
    f->integ = 0;
  } else {
    if (target_rpm < f->target)
      f->target_dropped_ms = now_ms;
    f->target = target_rpm;
    int spun_down = since(now_ms, f->target_dropped_ms) >= p->spin_down_ms;

    /* Speed without a command, or well above it, once it had time to slow down (FAN-3). */
    if (target_rpm == 0 && m->rpm >= p->turning_rpm && spun_down)
      return fail(f, FAN_FAULT_UNCOMMANDED);
    if (target_rpm > 0 && spun_down &&
        (uint32_t) m->rpm * 100u > (uint32_t) target_rpm * (100u + p->overspeed_percent))
      return fail(f, FAN_FAULT_OVERSPEED);

    if (target_rpm == 0) {
      f->integ = 0;
      f->over = 0;
      f->stall_timing = 0;
      return set(f, 0);
    }
  }

  /* Stalled: current flows but the Hall shows no turning (FAN-3: off within 1 s). */
  if (f->duty > 0 && m->isense_ma > p->stall_ma && m->rpm < p->turning_rpm) {
    if (!f->stall_timing)
      f->stall_timing = 1, f->stall_ms = now_ms;
    else if (since(now_ms, f->stall_ms) >= p->stall_ms)
      return fail(f, FAN_FAULT_STALL);
  } else {
    f->stall_timing = 0;
  }

  /* The speed loop: feed-forward from the 10 V point, plus PI on the Hall error. */
  int32_t cap = fan_duty_cap(f, m->vin_cv);
  int32_t err = 0, integ = 0, want = open_permille;
  if (open_permille == 0) {
    int32_t ff = (int32_t) ((uint64_t) target_rpm * p->v_max_cv * 1000u /
                            ((uint64_t) p->rpm_at_v_max * (m->vin_cv ? m->vin_cv : 1u)));
    err = (int32_t) target_rpm - (int32_t) m->rpm;
    integ = f->integ + (int32_t) p->ki_permille_per_krpm_s * err * (int32_t) dt / 1000;
    if (integ > cap * 1000)
      integ = cap * 1000;
    if (integ < -cap * 1000)
      integ = -cap * 1000;
    want = ff + (int32_t) p->kp_permille_per_krpm * err / 1000 + integ / 1000;
  }
  int32_t free_want = want;
  if (want > cap)
    want = cap;
  if (want < 0)
    want = 0;

  /* The ramp: from 0 to the cap in no less than ramp_ms (FAN-9). */
  int32_t up = cap * (int32_t) dt / (int32_t) (p->ramp_ms ? p->ramp_ms : 1);
  if (up < 1)
    up = 1;
  if (want > (int32_t) f->duty + up)
    want = (int32_t) f->duty + up;

  /* The start limit: while the fan turns below half its command, the motor current (average
   * switch current / duty) stays at or below the limit (FAN-9). */
  if ((uint32_t) m->rpm * 2u < target_rpm && f->duty >= p->estimate_min_permille) {
    uint32_t motor_ma = (uint32_t) m->isense_ma * 1000u / f->duty;
    if (motor_ma > p->start_limit_ma) {
      if (want > (int32_t) f->duty)
        want = f->duty; /* no rise while above */
      if (!f->over)
        f->over = 1, f->over_ms = now_ms;
      else if (since(now_ms, f->over_ms) >= p->start_limit_ms) {
        want = (int32_t) f->duty - (int32_t) p->back_off_permille;
        if (want < 0)
          want = 0;
        f->over_ms = now_ms; /* back off again after another while above */
      }
    } else {
      f->over = 0;
    }
  } else {
    f->over = 0;
  }

  /* No integral wind-up while the ramp, a limit or zero holds the duty away from what the loop
   * wants. */
  if (open_permille == 0 && !(err > 0 && want < free_want) && !(err < 0 && want > free_want))
    f->integ = integ;
  return set(f, (uint16_t) want);
}

uint16_t fan_step(fan *f, uint32_t now_ms, uint16_t target_rpm, const fan_meas *m) {
  return step(f, now_ms, target_rpm, 0, m);
}

uint16_t fan_step_open(fan *f, uint32_t now_ms, uint16_t duty_permille, const fan_meas *m) {
  if (duty_permille == 0)
    return step(f, now_ms, 0, 0, m);
  return step(f, now_ms, 0, duty_permille > 1000u ? 1000u : duty_permille, m);
}
