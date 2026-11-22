/* The safety MCU's self-tests: see checks.h. */
#include "checks.h"

#include <string.h>

#define UNKNOWN_TEMP ((int16_t) 0x8000)

enum {
  /* Power-up. */
  PU_LATCH_HOLD = 1,
  PU_WAIT_REARM,
  PU_LATCH_PULSE,
  PU_LATCH_CONFIRM,
  PU_STATIC_HIGH,
  PU_STATIC_LOW,
  PU_RUNNING,
  /* Pre-start. */
  PS_STOPPED,
  PS_GLOW_NO_HB,
  PS_HB,
  PS_GLOW_ON,
};

void checks_params_default(checks_params *p) {
  memset(p, 0, sizeof(*p));
  p->settle_ms = 10;
  p->hs_arm_ms = 20;
  p->stopped_ms = 50;
  p->pulse_us = 150;
  p->no_current_ma = 100;
  p->rise_ma = 200;
  p->pump_off_percent = 80;
  p->pump_on_percent = 10;
  p->glow_off_percent = 20;
  p->glow_on_percent = 80;
  p->glow_ls_ca = 200;
  p->glow_test_ms = 20;
  p->rearm_after_ms = 500;
  p->latch_pulse_ms = 2;
  p->rearm_max_c10 = 2400;
  p->timeout_ms = 2000;
}

static uint32_t since(uint32_t now, uint32_t then) { return (uint32_t) (now - then); }

void checks_init(checks *c, const checks_params *p, uint32_t boot_ms) {
  memset(c, 0, sizeof(*c));
  c->p = *p;
  c->boot_ms = boot_ms;
}

static void stage(checks *c, uint8_t s, uint32_t now) {
  c->stage = s;
  c->stage_ms = now;
  c->pulse_pending = 0;
}

void checks_begin_power_up(checks *c, uint32_t now_ms, const checks_obs *o) {
  c->kind = CHECKS_POWER_UP;
  c->failed = 0;
  c->begin_ms = now_ms;
  c->latch_skipped = o->latch_armed; /* already armed: step 1 cannot run (OVH-3) */
  stage(c, o->latch_armed ? PU_WAIT_REARM : PU_LATCH_HOLD, now_ms);
}

void checks_begin_pre_start(checks *c, uint32_t now_ms) {
  c->kind = CHECKS_PRE_START;
  c->failed = 0;
  c->begin_ms = now_ms;
  stage(c, PS_STOPPED, now_ms);
}

static const checks_out OFF = {CHECKS_HB_STOPPED, 0, 0, 0};

static checks_out fail(checks *c, uint16_t what) {
  c->failed |= what;
  if (c->kind == CHECKS_POWER_UP)
    c->power_up_done = 1;
  c->kind = CHECKS_IDLE;
  return OFF;
}

static checks_out done(checks *c) {
  if (c->kind == CHECKS_POWER_UP)
    c->power_up_done = 1;
  c->kind = CHECKS_IDLE;
  return OFF;
}

/* v below pct % of the supply. */
static int below(uint16_t v, uint16_t vin, uint8_t pct) { return (uint32_t) v * 100u < (uint32_t) vin * pct; }

/* A test pulse in this stage: ask once the stage has settled, then read its peak the step after.
 * Returns 1 when the peak is in (*peak), 0 while waiting; fills out when the pulse goes. */
static int pulse(checks *c, uint32_t now, uint16_t settle, const checks_obs *o, checks_out *out,
                 uint16_t *peak) {
  if (c->pulse_pending) {
    *peak = o->pump_peak_ma;
    c->pulse_pending = 0;
    return 1;
  }
  if (since(now, c->stage_ms) >= settle) {
    out->pump_pulse_us = c->p.pulse_us;
    c->pulse_pending = 1;
  }
  return 0;
}

checks_out checks_step(checks *c, uint32_t now, const checks_obs *o) {
  const checks_params *p = &c->p;
  checks_out out = OFF;
  uint16_t peak = 0;
  uint32_t in_stage = since(now, c->stage_ms);

  if (c->kind == CHECKS_IDLE)
    return OFF;
  /* A hot heat exchanger after a reset may take minutes to cool before the latch can be re-armed:
   * that wait is not a timeout. */
  if (c->stage == PU_WAIT_REARM)
    c->begin_ms = now;
  else if (since(now, c->begin_ms) > p->timeout_ms)
    return fail(c, CHECK_FAIL_TIMEOUT);

  switch (c->stage) {
    /* ------------------------------------------------ power-up */
    case PU_LATCH_HOLD: /* PMP-12: the tripped latch holds the pump off whatever the heartbeat */
      out.heartbeat = CHECKS_HB_RUNNING;
      if (pulse(c, now, p->hs_arm_ms, o, &out, &peak)) {
        if (peak >= p->no_current_ma)
          return fail(c, CHECK_FAIL_LATCH_HOLD);
        stage(c, PU_WAIT_REARM, now);
        out.heartbeat = CHECKS_HB_STOPPED;
      }
      return out;

    case PU_WAIT_REARM: /* OVH-2: only with a plausible, cool sensor, and not too soon */
      if (since(now, c->boot_ms) < p->rearm_after_ms)
        return out;
      if (o->hx_c10 == UNKNOWN_TEMP)
        return fail(c, CHECK_FAIL_LATCH_REARM);
      if (o->hx_c10 >= p->rearm_max_c10)
        return out; /* still too hot: wait (the cooldown runs meanwhile) */
      stage(c, PU_LATCH_PULSE, now);
      out.latch_reset = 1;
      return out;

    case PU_LATCH_PULSE:
      out.latch_reset = in_stage < p->latch_pulse_ms;
      if (in_stage >= p->latch_pulse_ms)
        stage(c, PU_LATCH_CONFIRM, now);
      return out;

    case PU_LATCH_CONFIRM:
      if (in_stage < 5)
        return out;
      if (!o->latch_armed)
        return fail(c, CHECK_FAIL_LATCH_REARM);
      stage(c, PU_STATIC_HIGH, now);
      out.heartbeat = CHECKS_HB_STATIC_HIGH;
      return out;

    case PU_STATIC_HIGH: /* PMP-11: a static level must not pass for a heartbeat */
    case PU_STATIC_LOW:
      out.heartbeat = c->stage == PU_STATIC_HIGH ? CHECKS_HB_STATIC_HIGH : CHECKS_HB_STATIC_LOW;
      if (pulse(c, now, p->settle_ms, o, &out, &peak)) {
        if (peak >= p->no_current_ma)
          return fail(c, CHECK_FAIL_STATIC_HEARTBEAT);
        if (c->stage == PU_STATIC_HIGH)
          stage(c, PU_STATIC_LOW, now), out.heartbeat = CHECKS_HB_STATIC_LOW;
        else
          stage(c, PU_RUNNING, now), out.heartbeat = CHECKS_HB_RUNNING;
      }
      return out;

    case PU_RUNNING: /* PMP-13: everything right, and the pump does draw current */
      out.heartbeat = CHECKS_HB_RUNNING;
      if (in_stage >= p->hs_arm_ms && !c->pulse_pending && !o->pump_hs_ok)
        return fail(c, CHECK_FAIL_PUMP_HS_FAULT);
      if (pulse(c, now, p->hs_arm_ms, o, &out, &peak)) {
        if (peak < p->rise_ma)
          return fail(c, CHECK_FAIL_PUMP_NO_CURRENT);
        return done(c);
      }
      return out;

    /* ------------------------------------------------ pre-start */
    case PS_STOPPED: /* PMP-14 and GLW-8 (1): with no heartbeat both + sides are off */
      if (in_stage < p->stopped_ms)
        return out;
      if (!below(o->pump_v_cv, o->vin_cv, p->pump_off_percent))
        return fail(c, CHECK_FAIL_PUMP_HS_SHORT);
      if (!below(o->glow_v_cv, o->vin_cv, p->glow_off_percent))
        return fail(c, CHECK_FAIL_GLOW_HS_SHORT);
      stage(c, PS_GLOW_NO_HB, now);
      out.glow_en = 1;
      return out;

    case PS_GLOW_NO_HB: /* GLW-22: the enable alone must not switch the plug on */
      if (in_stage >= 1 && !below(o->glow_v_cv, o->vin_cv, p->glow_off_percent))
        return fail(c, CHECK_FAIL_GLOW_HB_HOLD);
      if (in_stage >= p->glow_test_ms) {
        stage(c, PS_HB, now);
        out.heartbeat = CHECKS_HB_RUNNING;
        return out;
      }
      out.glow_en = 1;
      return out;

    case PS_HB: /* PMP-2, PMP-14: the pump's high side comes up and reports no fault */
      out.heartbeat = CHECKS_HB_RUNNING;
      if (in_stage < p->hs_arm_ms)
        return out;
      if (!o->pump_hs_ok)
        return fail(c, CHECK_FAIL_PUMP_HS_FAULT);
      if ((uint32_t) o->pump_v_cv * 100u < (uint32_t) o->vin_cv * (100u - p->pump_on_percent))
        return fail(c, CHECK_FAIL_PUMP_HS_NO_OUTPUT);
      stage(c, PS_GLOW_ON, now);
      out.glow_en = 1;
      return out;

    case PS_GLOW_ON: /* GLW-8 (2): enable and heartbeat, no pulses: supply on the plug, no current */
      out.heartbeat = CHECKS_HB_RUNNING;
      if (in_stage >= 1 && o->glow_ca > p->glow_ls_ca)
        return fail(c, CHECK_FAIL_GLOW_LS_SHORT); /* the enable drops in this very step */
      if (in_stage >= p->glow_test_ms) {
        if (below(o->glow_v_cv, o->vin_cv, p->glow_on_percent))
          return fail(c, CHECK_FAIL_GLOW_HS_NO_OUTPUT);
        return done(c);
      }
      out.glow_en = 1;
      return out;
  }
  return fail(c, CHECK_FAIL_TIMEOUT);
}
