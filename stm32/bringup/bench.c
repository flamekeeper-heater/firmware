/* Bench actions of the bring-up build: see bench.h. */
#include "bench.h"

#include <string.h>

/* Out-slot phases. */
enum { PH_PRECHECK = 1, PH_ARM, PH_RUN, PH_TAIL, PH_ASK, PH_READ };

static uint32_t since(uint32_t now, uint32_t then) { return (uint32_t) (now - then); }
static int reached(uint32_t now, uint32_t when) { return (int32_t) (now - when) >= 0; }

void bench_init(bench *bn) { memset(bn, 0, sizeof(*bn)); }

int bench_interlocks_ok(const safety *s) {
  return s->chk.power_up_done && s->power_up_failed == 0 && s->checks == 0 && !s->no_starts;
}

static int burn_off(const safety *s) { return s->burn.step == BURN_STEP_OFF; }

static void push(bench *bn, uint8_t action, uint8_t end) {
  bench_event e;
  memset(&e, 0, sizeof(e));
  e.action = action;
  e.end = end;
  if (bn->ev_n == BENCH_EVENTS) { /* full: the oldest goes */
    memmove(bn->ev, bn->ev + 1, sizeof(bn->ev[0]) * (BENCH_EVENTS - 1));
    bn->ev_n--;
  }
  bn->ev[bn->ev_n++] = e;
}
static bench_event *last(bench *bn) { return &bn->ev[bn->ev_n - 1]; }

int bench_next_event(bench *bn, bench_event *ev) {
  if (bn->ev_n == 0)
    return 0;
  *ev = bn->ev[0];
  memmove(bn->ev, bn->ev + 1, sizeof(bn->ev[0]) * (BENCH_EVENTS - 1));
  bn->ev_n--;
  return 1;
}

/* ---------------------------------------------------------------- requests */

int bench_fan(bench *bn, safety *s, uint32_t now, uint8_t action, uint16_t value, uint16_t seconds) {
  if (bn->fan)
    return BENCH_E_BUSY;
  if (!burn_off(s))
    return BENCH_E_NOT_OFF;
  if (seconds == 0 || seconds > BENCH_FAN_MAX_S)
    return BENCH_E_RANGE;
  if (action == BENCH_ACT_FAN_DUTY) {
    if (value == 0 || value > 1000u)
      return BENCH_E_RANGE; /* the core caps it to 10 V at the motor */
  } else if (action == BENCH_ACT_FAN_RPM) {
    if (value < BENCH_FAN_RPM_MIN || value > BENCH_FAN_RPM_MAX)
      return BENCH_E_RANGE;
  } else {
    return BENCH_E_RANGE;
  }
  if (s->fan.fault != FAN_FAULT_NONE || (s->burn.fault_latched && s->burn.fault == BURN_FAULT_FAN))
    return BENCH_E_FAULT;
  bn->fan = action;
  bn->fan_value = value;
  bn->fan_end_ms = now + (uint32_t) seconds * 1000u;
  return BENCH_OK;
}

static int out_free(const bench *bn, const safety *s) {
  if (bn->out)
    return BENCH_E_BUSY;
  if (!burn_off(s))
    return BENCH_E_NOT_OFF;
  if (checks_busy(&s->chk) || !s->chk.power_up_done)
    return BENCH_E_BUSY;
  return BENCH_OK;
}

static int start_out(bench *bn, safety *s, uint32_t now, uint8_t action, uint8_t phase, uint32_t limit_ms) {
  bn->out = action;
  bn->phase = phase;
  bn->phase_ms = now;
  bn->out_end_ms = now + limit_ms;
  bn->stopping = 0;
  if (phase == PH_PRECHECK)
    checks_begin_pre_start(&s->chk, now);
  return BENCH_OK;
}

int bench_hb(bench *bn, safety *s, uint32_t now, uint16_t seconds) {
  int r = out_free(bn, s);
  if (r)
    return r;
  if (seconds == 0 || seconds > BENCH_HB_MAX_S)
    return BENCH_E_RANGE;
  if (s->pump.fault != PUMP_FAULT_NONE)
    return BENCH_E_FAULT; /* the pump's fault holds the heartbeat off */
  return start_out(bn, s, now, BENCH_ACT_HB, PH_RUN, (uint32_t) seconds * 1000u);
}

static int stage_faults(const safety *s) {
  return s->burn.fault_latched || s->pump.fault != PUMP_FAULT_NONE || s->glow.fault != GLOW_FAULT_NONE;
}

int bench_pump(bench *bn, safety *s, uint32_t now, uint16_t strokes, uint16_t cHz) {
  int r = out_free(bn, s);
  if (r)
    return r;
  if (strokes == 0 || strokes > BENCH_PUMP_MAX_STROKES || cHz < BENCH_PUMP_MIN_CHZ || cHz > s->pump.p.max_cHz)
    return BENCH_E_RANGE;
  if (!bench_interlocks_ok(s))
    return BENCH_E_INTERLOCKS;
  if (stage_faults(s))
    return BENCH_E_FAULT;
  bn->pump_n = strokes;
  bn->pump_cHz = cHz;
  bn->pump_done = 0;
  bn->peak_min = 0xFFFFu;
  bn->peak_max = 0;
  bn->was_stroking = 0;
  uint32_t limit = BENCH_PRECHECK_MAX_MS + (uint32_t) strokes * (100000u / cHz) + 1000u;
  return start_out(bn, s, now, BENCH_ACT_PUMP, PH_PRECHECK, limit);
}

int bench_glow(bench *bn, safety *s, uint32_t now) {
  int r = out_free(bn, s);
  if (r)
    return r;
  if (!bench_interlocks_ok(s))
    return BENCH_E_INTERLOCKS;
  if (stage_faults(s))
    return BENCH_E_FAULT;
  /* GLW-14: the plug only with measured airflow, here from a fan action of the bench. */
  if (!bn->fan || s->in.fan_rpm < s->burn.p.glow_min_fan_rpm)
    return BENCH_E_NO_AIR;
  return start_out(bn, s, now, BENCH_ACT_GLOW, PH_PRECHECK, BENCH_PRECHECK_MAX_MS + 1000u);
}

int bench_precheck(bench *bn, safety *s, uint32_t now) {
  int r = out_free(bn, s);
  if (r)
    return r;
  return start_out(bn, s, now, BENCH_ACT_PRECHECK, PH_PRECHECK, BENCH_PRECHECK_MAX_MS);
}

/* ---------------------------------------------------------------- the millisecond step */

static void end_fan(bench *bn, uint8_t end, uint8_t fault) {
  push(bn, bn->fan, end);
  last(bn)->fault = fault;
  bn->fan = 0;
}

static void end_out(bench *bn, safety *s, uint8_t end) {
  push(bn, bn->out, end);
  bench_event *e = last(bn);
  if (bn->out == BENCH_ACT_PUMP) {
    e->a = bn->pump_done;
    e->b = bn->pump_done ? bn->peak_min : 0;
    e->c = bn->peak_max;
  }
  if (bn->out == BENCH_ACT_PUMP || bn->out == BENCH_ACT_HB)
    e->fault = s->pump.fault;
  else if (bn->out == BENCH_ACT_GLOW)
    e->fault = s->glow.fault;
  bn->out = 0;
  bn->phase = 0;
  s->bench.glow_probe = 0;
}

/* The pump's stop: PUMP_CMD low first, the heartbeat a decay wait later (PMP-10). */
static void pump_tail(bench *bn, uint32_t now, int stopped) {
  bn->phase = PH_TAIL;
  bn->phase_ms = now;
  bn->stopping = (uint8_t) stopped;
}

void bench_stop(bench *bn, safety *s, uint32_t now) {
  if (bn->fan)
    end_fan(bn, BENCH_END_STOPPED, 0);
  if (bn->out == BENCH_ACT_PUMP && (bn->phase == PH_RUN || bn->phase == PH_TAIL))
    pump_tail(bn, now, 1);
  else if (bn->out)
    end_out(bn, s, BENCH_END_STOPPED);
  s->bench.fan_duty = 0;
  s->bench.fan_rpm = 0;
  s->bench.pump_cHz = 0;
  s->bench.glow_probe = 0;
  if (!bn->out)
    s->bench.heartbeat = 0;
}

static uint8_t cold_type(const safety *s, uint16_t r_mohm) {
  const plug_params *p = &s->plug.p;
  if (r_mohm < p->r12_max_mohm)
    return 12;
  if (r_mohm >= p->r24_min_mohm && r_mohm <= p->r24_max_mohm)
    return 24;
  return 0;
}

static void step_out(bench *bn, safety *s, uint32_t now) {
  uint32_t in_phase = since(now, bn->phase_ms);

  if (bn->phase == PH_PRECHECK) {
    if (checks_busy(&s->chk)) {
      if (in_phase >= BENCH_PRECHECK_MAX_MS)
        end_out(bn, s, BENCH_END_TIMEOUT);
      return;
    }
    if (s->chk.failed) {
      uint16_t f = s->chk.failed;
      end_out(bn, s, BENCH_END_PRECHECK_FAILED);
      last(bn)->fail = f;
      return;
    }
    if (bn->out == BENCH_ACT_PRECHECK) {
      end_out(bn, s, BENCH_END_DONE);
      return;
    }
    bn->phase = bn->out == BENCH_ACT_PUMP ? PH_RUN : PH_ARM;
    bn->phase_ms = now;
    bn->strokes0 = s->pump.strokes;
    return;
  }

  if (bn->out == BENCH_ACT_HB) {
    if (reached(now, bn->out_end_ms))
      end_out(bn, s, BENCH_END_DONE);
    return;
  }

  if (bn->out == BENCH_ACT_PUMP) {
    if (bn->phase == PH_RUN) {
      /* A stroke that just ended: its peak. The rate drops once the last one is over, never inside
       * a stroke (that would cut it short). */
      if (bn->was_stroking && !s->pump.stroking) {
        bn->pump_done++;
        uint16_t pk = s->pump.stroke_peak_ma;
        if (pk < bn->peak_min)
          bn->peak_min = pk;
        if (pk > bn->peak_max)
          bn->peak_max = pk;
      }
      bn->was_stroking = s->pump.stroking;
      if (bn->pump_done >= bn->pump_n)
        pump_tail(bn, now, 0);
      else if (reached(now, bn->out_end_ms))
        pump_tail(bn, now, 2);
      return;
    }
    if (bn->phase == PH_TAIL && in_phase >= (uint32_t) s->pump.p.decay_ms + 2u)
      end_out(bn, s, bn->stopping == 1 ? BENCH_END_STOPPED : bn->stopping == 2 ? BENCH_END_TIMEOUT : BENCH_END_DONE);
    return;
  }

  if (bn->out == BENCH_ACT_GLOW) {
    if (bn->phase == PH_ARM) {
      if (s->hb_running_ms >= BENCH_GLOW_ARM_MS) {
        bn->phase = PH_ASK;
        bn->phase_ms = now;
        s->bench.glow_probe = 1; /* the core clears it once the pulse is out */
      } else if (in_phase >= 500u) {
        end_out(bn, s, BENCH_END_TIMEOUT); /* the heartbeat never ran (a pump fault holds it) */
      }
      return;
    }
    if (bn->phase == PH_ASK) {
      if (!s->bench.glow_probe) {
        bn->phase = PH_READ;
        bn->phase_ms = now;
      } else if (in_phase >= BENCH_GLOW_PROBE_MAX_MS) {
        end_out(bn, s, BENCH_END_NO_PROBE);
      }
      return;
    }
    if (bn->phase == PH_READ && in_phase >= BENCH_GLOW_READ_MS) {
      uint16_t i = s->glow.i_ca, v = s->glow.vin_then_cv;
      uint32_t r = i ? (uint32_t) v * 1000u / i : 0xFFFFu;
      if (r > 0xFFFFu)
        r = 0xFFFFu;
      end_out(bn, s, BENCH_END_DONE);
      bench_event *e = last(bn);
      e->a = i, e->b = v, e->c = (uint16_t) r, e->d = i ? cold_type(s, (uint16_t) r) : 0;
    }
  }
}

void bench_ms(bench *bn, safety *s, uint32_t now) {
  /* A start over the link (or anything that takes the burn out of off) ends every bench action:
   * the core ignores the bench requests from then on anyway. */
  if (!burn_off(s) && bench_busy(bn)) {
    if (bn->fan)
      end_fan(bn, BENCH_END_BURN, 0);
    if (bn->out)
      end_out(bn, s, BENCH_END_BURN);
  }

  if (bn->fan) {
    if (s->fan.fault != FAN_FAULT_NONE)
      end_fan(bn, BENCH_END_FAULT, s->fan.fault);
    else if (reached(now, bn->fan_end_ms))
      end_fan(bn, BENCH_END_DONE, 0);
  }

  if (bn->out) {
    if (s->pump.fault != PUMP_FAULT_NONE && (bn->out == BENCH_ACT_PUMP || bn->out == BENCH_ACT_HB) &&
        bn->phase != PH_PRECHECK) {
      end_out(bn, s, BENCH_END_FAULT);
    } else if (s->glow.fault != GLOW_FAULT_NONE && bn->out == BENCH_ACT_GLOW) {
      end_out(bn, s, BENCH_END_FAULT);
    } else if (bn->out == BENCH_ACT_GLOW && !bn->fan && bn->phase != PH_READ) {
      end_out(bn, s, BENCH_END_NO_AIR);
    } else if (bn->out != BENCH_ACT_HB && reached(now, bn->out_end_ms) &&
               !(bn->out == BENCH_ACT_PUMP && bn->phase == PH_TAIL)) {
      if (bn->out == BENCH_ACT_PUMP && bn->phase == PH_RUN)
        pump_tail(bn, now, 2);
      else
        end_out(bn, s, BENCH_END_TIMEOUT);
    } else {
      step_out(bn, s, now);
    }
  }

  /* The requests for the next millisecond. */
  s->bench.fan_duty = bn->fan == BENCH_ACT_FAN_DUTY ? bn->fan_value : 0;
  s->bench.fan_rpm = bn->fan == BENCH_ACT_FAN_RPM ? bn->fan_value : 0;
  int hb = bn->out == BENCH_ACT_HB ||
           ((bn->out == BENCH_ACT_PUMP || bn->out == BENCH_ACT_GLOW) && bn->phase != PH_PRECHECK && bn->phase != 0);
  s->bench.heartbeat = (uint8_t) hb;
  s->bench.pump_cHz = bn->out == BENCH_ACT_PUMP && bn->phase == PH_RUN ? bn->pump_cHz : 0;
  if (bn->out != BENCH_ACT_GLOW)
    s->bench.glow_probe = 0;
}
