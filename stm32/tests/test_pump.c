/* The pump stroke driver against a coil model: stroke timing and length, the 12 V equivalent duty,
 * the rate limits, no stroke before the high side is ready, and every fault it watches for. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../core/pump.h"
#include "../../common/test/check.h"

/* A dosing pump's coil: 6 Ohm, 2 ms time constant (PMP-8 needs the current gone within 10 ms of a
 * stroke); the board's high side. */
typedef struct {
  pump pu;
  uint32_t now;
  double vin, i;
  int hb_running_ms;
  int hs_ok, hs_flt_once_at; /* > 0: the high side reports a fault at that millisecond */
  int open_coil, shorted_coil, low_side_stuck;
  /* Findings. */
  uint32_t strokes, stroke_ms_total, first_stroke_ms, last_start_ms, min_gap_ms;
  uint16_t duty_seen;
  int duty_while_not_ready;
  int hb_held;
  uint32_t ms_run;
  int in_stroke;
  pump_out last;
} rig;

static void rig_init(rig *r, double vin) {
  memset(r, 0, sizeof(*r));
  pump_params p;
  pump_params_default(&p);
  pump_init(&r->pu, &p);
  r->vin = vin;
  r->hs_ok = 1;
  r->min_gap_ms = 1000000;
}

static void ms(rig *r, uint16_t rate, int priming) {
  pump_meas m;
  m.vin_cv = (uint16_t) (r->vin * 100);
  m.i_ma = (uint16_t) (r->i * 1000);
  m.overcurrent = (uint8_t) (r->shorted_coil && r->i > 3.3);
  m.hs_ok = (uint8_t) r->hs_ok;
  if (r->hs_flt_once_at > 0 && (int) r->ms_run == r->hs_flt_once_at)
    m.hs_ok = 0, r->hs_ok = 0;
  m.hb_running_ms = (uint16_t) r->hb_running_ms;
  pump_out o = pump_ms(&r->pu, r->now, rate, priming, &m);
  r->last = o;

  /* The heartbeat: held off when the driver asks (a re-arm then clears the high side's fault). */
  if (o.hb_hold_off) {
    r->hb_held = 1;
    r->hb_running_ms = 0;
    r->hs_ok = 1; /* stopping the heartbeat clears the high side's latch (PMP-16) */
  } else {
    r->hb_running_ms++;
  }
  if (o.duty > 0) {
    if (!r->in_stroke) {
      if (r->strokes > 0 && r->now - r->last_start_ms < r->min_gap_ms)
        r->min_gap_ms = r->now - r->last_start_ms;
      if (r->strokes == 0)
        r->first_stroke_ms = r->now;
      r->strokes++;
      r->last_start_ms = r->now;
    }
    r->stroke_ms_total++;
    r->duty_seen = o.duty;
    if (r->hb_running_ms < 20)
      r->duty_while_not_ready++;
  }
  r->in_stroke = o.duty > 0;

  /* The coil current. */
  double v = r->low_side_stuck ? r->vin : o.duty / 1000.0 * r->vin;
  double target = r->open_coil ? 0 : r->shorted_coil ? v / 0.5 : v / 6.0;
  r->i += (target - r->i) * (1 - exp(-1.0 / 2.0));
  if (r->open_coil)
    r->i = 0;
  r->now++;
  r->ms_run++;
}

static void run(rig *r, uint32_t n, uint16_t rate, int priming) {
  for (uint32_t k = 0; k < n; k++)
    ms(r, rate, priming);
}

static void test_strokes(void) {
  rig r;
  rig_init(&r, 12.6);
  run(&r, 10000, 160, 0); /* 1.6 Hz for 10 s */
  CHECK(r.pu.fault == PUMP_FAULT_NONE);
  CHECK(r.strokes == 16);
  CHECK(r.first_stroke_ms >= 20); /* PMP-2: the heartbeat first */
  CHECK(r.min_gap_ms == 625);
  CHECK(r.stroke_ms_total == 16 * 38);
  CHECK(r.duty_seen == 952); /* 12 V of 12.6 */
  CHECK(r.duty_while_not_ready == 0);

  rig r2;
  rig_init(&r2, 26.0);
  run(&r2, 3000, 300, 0);
  CHECK(r2.duty_seen == 461); /* 12 V of 26 */
  CHECK(r2.pu.fault == PUMP_FAULT_NONE);
}

static void test_rate_limits(void) {
  rig r;
  rig_init(&r, 12.6);
  run(&r, 5000, 900, 0); /* asks for 9 Hz while burning: 6 Hz at most */
  CHECK(r.min_gap_ms == 166);
  CHECK(r.pu.fault == PUMP_FAULT_NONE);
  rig r2;
  rig_init(&r2, 12.6);
  run(&r2, 5000, 1500, 1); /* priming: up to 10 Hz */
  CHECK(r2.min_gap_ms == 100);
  /* Stroke duty at 10 Hz: at most 0.38 (PMP-5). */
  CHECK((double) r2.stroke_ms_total / 5000.0 <= 0.385);
  CHECK(r2.pu.fault == PUMP_FAULT_NONE);
}

static void test_stop_mid_stroke(void) {
  rig r;
  rig_init(&r, 12.6);
  run(&r, 40, 160, 0); /* inside the first stroke */
  CHECK(r.in_stroke);
  run(&r, 1, 0, 0); /* no fuel wanted: PUMP_CMD low at once */
  CHECK(!r.in_stroke);
  run(&r, 2000, 0, 0);
  CHECK(r.strokes == 1);
  CHECK(r.pu.fault == PUMP_FAULT_NONE); /* the current decays in time */
}

static void test_faults(void) {
  rig r;
  rig_init(&r, 12.6);
  r.open_coil = 1;
  run(&r, 1000, 160, 0);
  CHECK(r.pu.fault == PUMP_FAULT_OPEN_COIL);
  CHECK(r.strokes == 1);
  CHECK(r.hb_held);

  rig r2;
  rig_init(&r2, 12.6);
  r2.shorted_coil = 1;
  run(&r2, 200, 160, 0);
  CHECK(r2.pu.fault == PUMP_FAULT_OVERCURRENT);
  CHECK(!r2.in_stroke);

  rig r3;
  rig_init(&r3, 12.6);
  run(&r3, 1000, 160, 0);
  r3.low_side_stuck = 1; /* current between strokes */
  uint32_t t = r3.now;
  for (int k = 0; k < 2000 && r3.pu.fault == PUMP_FAULT_NONE; k++)
    ms(&r3, 160, 0);
  CHECK(r3.pu.fault == PUMP_FAULT_LOW_SIDE);
  CHECK(r3.now - t <= 650); /* caught by the next gap at the latest */
}

static void test_high_side_rearm(void) {
  rig r;
  rig_init(&r, 12.6);
  r.hs_flt_once_at = 2000;
  run(&r, 6000, 160, 0);
  CHECK(r.pu.fault == PUMP_FAULT_NONE); /* one re-arm per run */
  CHECK(r.pu.rearms == 1);
  CHECK(r.strokes >= 9);
  r.hs_flt_once_at = 7000;
  run(&r, 3000, 160, 0);
  CHECK(r.pu.fault == PUMP_FAULT_HIGH_SIDE); /* the second in the same run locks out */

  rig r2;
  rig_init(&r2, 12.6);
  r2.hs_flt_once_at = 2000;
  run(&r2, 3000, 160, 0);
  pump_new_run(&r2.pu); /* a new start gets its re-arm back */
  r2.hs_flt_once_at = 4000;
  run(&r2, 3000, 160, 0);
  CHECK(r2.pu.fault == PUMP_FAULT_NONE);
}

static void test_not_ready(void) {
  rig r;
  rig_init(&r, 12.6);
  r.hs_ok = 0; /* the high side reports a fault from the start */
  run(&r, 15, 160, 0);
  CHECK(r.strokes == 0); /* before the heartbeat has run 20 ms: nothing */
}

/* PMP-7: the overcurrent watchdog is armed from a stroke's first PWM pulse until decay_ms after
 * its last, and only then. */
static void test_overcurrent_window(void) {
  rig r;
  rig_init(&r, 12.6);
  CHECK(r.pu.p.oc_ma == 3300 && r.pu.p.oc_hold_us == 50);
  int armed_idle = 0, armed_in_stroke = 0, unarmed_in_stroke = 0, armed_after = 0;
  int strokes_seen = 0, was_stroke = 0;
  for (int k = 0; k < 1000; k++) { /* 1.6 Hz: two strokes */
    ms(&r, 160, 0);
    if (r.last.duty > 0) {
      armed_in_stroke += r.last.oc_armed;
      unarmed_in_stroke += !r.last.oc_armed;
      if (!was_stroke)
        strokes_seen++;
    } else if (r.last.oc_armed) {
      armed_after++;
    }
    if (strokes_seen == 0)
      armed_idle += r.last.oc_armed; /* before the first stroke: never */
    was_stroke = r.last.duty > 0;
  }
  CHECK(strokes_seen == 2);
  CHECK(armed_idle == 0);
  CHECK(unarmed_in_stroke == 0);
  CHECK(armed_in_stroke == 2 * 38);
  CHECK(armed_after == 2 * 10); /* PUMP_DECAY_WAIT_MS after each stroke */

  /* A stroke stopped early (no fuel wanted): the window still runs decay_ms past its last pulse. */
  rig r2;
  rig_init(&r2, 12.6);
  run(&r2, 40, 160, 0);
  CHECK(r2.in_stroke && r2.last.oc_armed);
  int after = 0;
  for (int k = 0; k < 30; k++) {
    ms(&r2, 0, 0);
    after += r2.last.oc_armed;
  }
  CHECK(after == 10);

  /* The watchdog's cut: a fault, the heartbeat held off, and nothing armed any more. */
  rig r3;
  rig_init(&r3, 12.6);
  r3.shorted_coil = 1;
  run(&r3, 200, 160, 0);
  CHECK(r3.pu.fault == PUMP_FAULT_OVERCURRENT);
  CHECK(r3.last.hb_hold_off && !r3.last.oc_armed && r3.last.duty == 0);
}

int main(void) {
  test_strokes();
  test_overcurrent_window();
  test_rate_limits();
  test_stop_mid_stroke();
  test_faults();
  test_high_side_rearm();
  test_not_ready();
  return check_report("pump");
}
