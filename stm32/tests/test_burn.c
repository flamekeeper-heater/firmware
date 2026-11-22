/* The burn sequencer against the heater model: whole cycles, every way a run can end, and the
 * board's rules (glow only with airflow, glow on-time caps, nothing restarts by itself). */
#include <stdio.h>
#include <string.h>

#include "../core/burn.h"
#include "heater_model.h"

static int checks = 0, failures = 0;
#define CHECK(c)                                                  \
  do {                                                            \
    checks++;                                                     \
    if (!(c)) {                                                   \
      failures++;                                                 \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);         \
    }                                                             \
  } while (0)
#define CHECK_EQ(a, b)                                                                              \
  do {                                                                                              \
    checks++;                                                                                       \
    long long va_ = (long long) (a), vb_ = (long long) (b);                                         \
    if (va_ != vb_) {                                                                               \
      failures++;                                                                                   \
      printf("FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_);         \
    }                                                                                               \
  } while (0)

enum { HEAT = 1, FAN = 2, PRIME = 3 };
#define SEC 1000u
#define MIN (60u * SEC)
#define TICK 100u

typedef struct {
  burn b;
  heater_model m;
  uint32_t now;
  uint8_t latch, co, co_fault, glow_fault, fan_fault, pump_fault, plug_refused;
  uint8_t precheck; /* what the pre-start self-test reports (passes by default) */
  int16_t board_c10; /* the board thermistor */
  int air_below_fuel; /* passes where the fan's level fell below the fuel's */
  /* What the run did. */
  uint32_t glow_ms, glow_no_air_ms, pump_ms, max_glow_attempt_ms;
  uint8_t max_step_seen;
} rig;

static burn_inputs inputs(const rig *r) {
  burn_inputs in;
  in.hx_c10 = heater_model_hx(&r->m);
  in.fan_rpm = (uint16_t) (r->m.fan_rpm + 0.5);
  in.supply_cv = r->m.supply_cv;
  in.overheat_latch = r->latch;
  in.co_alarm = r->co;
  in.co_fault = r->co_fault;
  in.glow_fault = r->glow_fault;
  in.fan_fault = r->fan_fault;
  in.precheck = r->precheck;
  in.pump_fault = r->pump_fault;
  in.board_c10 = r->board_c10;
  in.plug_refused = r->plug_refused;
  return in;
}

static void rig_init(rig *r, uint32_t start, double ambient_c) {
  memset(r, 0, sizeof(*r));
  heater_model_init(&r->m, ambient_c);
  r->now = start;
  r->precheck = BURN_PRECHECK_PASSED;
  r->board_c10 = 400;
  burn_params p;
  burn_params_default(&p);
  burn_inputs in = inputs(r);
  burn_init(&r->b, &p, r->now, &in);
  r->b.reset_fan_due = 0; /* most tests start long after the reset; test_reset_fan says otherwise */
}

static void tick(rig *r) {
  const burn_outputs *o = &r->b.out;
  if (o->glow_w > 0) {
    r->glow_ms += TICK;
    if (r->m.fan_rpm < r->b.p.glow_min_fan_rpm)
      r->glow_no_air_ms += TICK;
  }
  if (o->pump_cHz > 0)
    r->pump_ms += TICK;
  if (r->b.glow_attempt_ms > r->max_glow_attempt_ms)
    r->max_glow_attempt_ms = r->b.glow_attempt_ms;
  heater_model_step(&r->m, TICK, o->fan_rpm, o->pump_cHz, o->glow_w);
  r->now += TICK;
  burn_inputs in = inputs(r);
  burn_step(&r->b, r->now, &in);
  if (r->fan_fault == 1 && r->b.out.fan_rpm == 0) /* the fan stage's stall clears at command 0 */
    r->fan_fault = 0;
  if (r->b.step > r->max_step_seen)
    r->max_step_seen = r->b.step;
  if (r->b.step == BURN_STEP_RUNNING && r->b.air_mp < r->b.fuel_mp)
    r->air_below_fuel++;
}

static void run(rig *r, uint32_t ms) {
  for (uint32_t t = 0; t < ms; t += TICK)
    tick(r);
}

/* Run until the step is `step` (1) or ms passes (0). */
static int run_until_step(rig *r, uint8_t step, uint32_t ms) {
  for (uint32_t t = 0; t < ms; t += TICK) {
    if (r->b.step == step)
      return 1;
    tick(r);
  }
  return r->b.step == step;
}

static int start(rig *r, uint8_t mode, uint8_t power) {
  burn_inputs in = inputs(r);
  return burn_start(&r->b, mode, power, r->now, &in);
}

/* ---------------------------------------------------------------- tests */

static void test_full_cycle(uint32_t t0) {
  rig r;
  rig_init(&r, t0, 5);
  CHECK_EQ(burn_state(&r.b), BURN_STATE_OFF);
  CHECK_EQ(start(&r, HEAT, 50), 0);
  CHECK_EQ(burn_state(&r.b), BURN_STATE_STARTING);
  CHECK_EQ(r.b.step, BURN_STEP_PRE_CHECK);
  run(&r, TICK);
  CHECK_EQ(r.b.step, BURN_STEP_SELF_TEST);
  CHECK(run_until_step(&r, BURN_STEP_PREHEAT, 10 * SEC));
  CHECK(run_until_step(&r, BURN_STEP_IGNITION, 40 * SEC));
  CHECK(run_until_step(&r, BURN_STEP_WARM_UP, 100 * SEC));
  CHECK(r.m.lit);
  CHECK(run_until_step(&r, BURN_STEP_RUNNING, 70 * SEC));
  CHECK_EQ(burn_state(&r.b), BURN_STATE_HEATING);
  run(&r, 10 * MIN);
  CHECK_EQ(r.b.step, BURN_STEP_RUNNING);
  CHECK_EQ(r.b.fault, BURN_FAULT_NONE);
  CHECK_EQ(r.b.out.glow_w, 0);
  CHECK(r.glow_ms <= 120 * SEC);   /* GLW-1: one attempt */
  CHECK_EQ(r.glow_no_air_ms, 0);   /* GLW-14 */
  CHECK_EQ(r.b.running_power, 50);
  CHECK(r.m.hx_c10 > 900 && r.m.hx_c10 < 2400);

  /* More power: the fan first, at once; the fuel follows over seconds. */
  burn_set_power(&r.b, 100);
  run(&r, 1 * SEC);
  CHECK_EQ(r.b.out.fan_rpm, r.b.p.fan_max_rpm);
  CHECK(r.b.out.pump_cHz < r.b.p.pump_max_cHz);
  run(&r, 10 * SEC);
  CHECK_EQ(r.b.out.pump_cHz, r.b.p.pump_max_cHz);
  /* Less power: the fuel first, at once; the fan follows. */
  burn_set_power(&r.b, 20);
  run(&r, TICK);
  CHECK_EQ(r.b.running_power, 20);
  CHECK(r.b.out.fan_rpm > 1800 + (4500 - 1800) * 20 / 100);
  run(&r, 20 * SEC);
  CHECK_EQ(r.b.out.fan_rpm, 1800 + (4500 - 1800) * 20 / 100);
  CHECK_EQ(r.air_below_fuel, 0);
  burn_set_power(&r.b, 100);
  run(&r, 15 * SEC);

  /* A second start while it runs is not a start. */
  CHECK_EQ(start(&r, HEAT, 30), 0x20);

  burn_stop(&r.b, BURN_WHY_ASKED, r.now);
  CHECK_EQ(burn_state(&r.b), BURN_STATE_STOPPING);
  run(&r, TICK);
  CHECK_EQ(r.b.out.pump_cHz, 0);
  CHECK(r.b.out.fan_rpm > 0);
  CHECK(run_until_step(&r, BURN_STEP_COOLDOWN, 40 * SEC));
  CHECK_EQ(burn_state(&r.b), BURN_STATE_COOLING);
  CHECK(run_until_step(&r, BURN_STEP_OFF, 20 * MIN));
  CHECK(r.m.hx_c10 < r.b.p.cool_c10);
  CHECK_EQ(r.b.fault, BURN_FAULT_NONE);
  run(&r, TICK);
  CHECK_EQ(r.b.out.fan_rpm, 0);

  /* Off stays off: nothing starts by itself. */
  run(&r, 5 * MIN);
  CHECK_EQ(r.b.step, BURN_STEP_OFF);
  CHECK_EQ(r.b.out.pump_cHz, 0);
}

static void test_glow_needs_airflow(void) {
  rig r;
  rig_init(&r, 0, 10);
  r.m.fan_max_rpm = 800; /* below the glow's minimum speed */
  CHECK_EQ(start(&r, HEAT, 50), 0);
  CHECK(run_until_step(&r, BURN_STEP_FAN_RETRY, 15 * SEC)); /* tried twice more (FAN-5) ... */
  CHECK(run_until_step(&r, BURN_STEP_OFF, 60 * SEC));       /* ... then locked out */
  CHECK_EQ(r.b.fan_stalls, 3);
  CHECK(r.b.fault_latched);
  CHECK_EQ(r.b.fault, BURN_FAULT_FAN);
  CHECK_EQ(r.b.why, BURN_WHY_FAN_SPIN_UP);
  CHECK_EQ(r.glow_ms, 0);
  CHECK_EQ(r.pump_ms, 0);
}

static void test_no_ignition(void) {
  rig r;
  rig_init(&r, 0, 10);
  r.m.no_ignition = 1;
  CHECK_EQ(start(&r, HEAT, 50), 0);
  CHECK(run_until_step(&r, BURN_STEP_PURGE, 3 * MIN));
  CHECK_EQ(r.b.out.pump_cHz, 0);
  CHECK_EQ(r.b.out.glow_w, 0);
  CHECK(run_until_step(&r, BURN_STEP_PREHEAT, 2 * MIN));
  CHECK_EQ(r.b.attempt, 2);
  CHECK(run_until_step(&r, BURN_STEP_BURN_OFF, 3 * MIN));
  CHECK_EQ(r.b.fault, BURN_FAULT_START);
  CHECK_EQ(r.b.why, BURN_WHY_NO_FLAME);
  CHECK(r.max_glow_attempt_ms <= 120 * SEC);
  CHECK(r.glow_ms <= 240 * SEC);
  CHECK(run_until_step(&r, BURN_STEP_OFF, 5 * MIN));
  CHECK_EQ(r.b.fault, BURN_FAULT_START); /* reported until the next start */

  /* The plug has used its 10-minute allowance: a new start waits. */
  CHECK_EQ(start(&r, HEAT, 50), 0x2F);
  run(&r, 10 * MIN);
  CHECK_EQ(start(&r, HEAT, 50), 0);
  CHECK_EQ(r.b.fault, BURN_FAULT_NONE);
}

static void test_flame_out(void) {
  rig r;
  rig_init(&r, 0, 10);
  CHECK_EQ(start(&r, HEAT, 80), 0);
  CHECK(run_until_step(&r, BURN_STEP_RUNNING, 5 * MIN));
  run(&r, 5 * MIN);
  r.m.kill_flame = 1;
  CHECK(run_until_step(&r, BURN_STEP_BURN_OFF, 3 * MIN));
  CHECK_EQ(r.b.fault, BURN_FAULT_FLAME_OUT);
  CHECK_EQ(r.b.out.pump_cHz, 0);
  CHECK(run_until_step(&r, BURN_STEP_OFF, 20 * MIN));
  CHECK_EQ(r.b.fault, BURN_FAULT_FLAME_OUT);
}

static void test_overheat_derate_holds(void) {
  /* A heater that runs too hot at full power: the derate holds it near 245 C, short of the stop. */
  rig r;
  rig_init(&r, 0, 30);
  r.m.heat_gain = 0.06;
  CHECK_EQ(start(&r, HEAT, 100), 0);
  CHECK(run_until_step(&r, BURN_STEP_RUNNING, 5 * MIN));
  double peak = 0;
  for (int i = 0; i < 20000; i++) {
    tick(&r);
    if (r.m.hx_c10 > peak)
      peak = r.m.hx_c10;
  }
  CHECK_EQ(r.b.step, BURN_STEP_RUNNING);
  CHECK(peak < 2550);
  CHECK_EQ(r.air_below_fuel, 0);
}

static void test_overheat_limit(void) {
  rig r;
  rig_init(&r, 0, 30);
  r.m.heat_gain = 0.12; /* a heater that overheats even at the lowest fuel with full air */
  CHECK_EQ(start(&r, HEAT, 100), 0);
  CHECK(run_until_step(&r, BURN_STEP_RUNNING, 5 * MIN));
  int derated = 0;
  for (int i = 0; i < 30000 && r.b.step == BURN_STEP_RUNNING; i++) {
    tick(&r);
    if (r.m.hx_c10 >= 2450 && r.b.step == BURN_STEP_RUNNING && r.b.out.pump_cHz == r.b.p.pump_min_cHz)
      derated = 1;
  }
  CHECK(derated);
  CHECK_EQ(r.b.step, BURN_STEP_BURN_OFF);
  CHECK_EQ(r.b.fault, BURN_FAULT_OVERHEAT);
  CHECK_EQ(r.b.why, BURN_WHY_OVERHEAT_LIMIT);
  CHECK(r.m.hx_c10 < 2700); /* stopped well before the hardware latch */
}

static void test_overheat_latch(void) {
  rig r;
  rig_init(&r, 0, 10);
  CHECK_EQ(start(&r, HEAT, 50), 0);
  CHECK(run_until_step(&r, BURN_STEP_RUNNING, 5 * MIN));
  r.latch = 1;
  run(&r, TICK);
  /* OVH-4: fuel off at once, the fan keeps going to cool it. */
  CHECK_EQ(r.b.out.pump_cHz, 0);
  CHECK(r.b.out.fan_rpm > 0);
  CHECK_EQ(r.b.fault, BURN_FAULT_OVERHEAT);
  CHECK(r.b.fault_latched);
  CHECK(run_until_step(&r, BURN_STEP_OFF, 25 * MIN));
  CHECK_EQ(start(&r, HEAT, 50), 0x2F);
  burn_inputs in = inputs(&r);
  CHECK_EQ(burn_fault_reset(&r.b, &in), 0x2F); /* still tripped */
  r.latch = 0;
  in = inputs(&r);
  CHECK_EQ(burn_fault_reset(&r.b, &in), 0);
  CHECK_EQ(start(&r, HEAT, 50), 0);
}

static void test_co_alarm(void) {
  rig r;
  rig_init(&r, 0, 10);
  CHECK_EQ(start(&r, HEAT, 50), 0);
  CHECK(run_until_step(&r, BURN_STEP_RUNNING, 5 * MIN));
  r.co = 1;
  run(&r, TICK);
  CHECK_EQ(r.b.fault, BURN_FAULT_CO);
  CHECK_EQ(r.b.step, BURN_STEP_BURN_OFF);
  CHECK_EQ(r.b.out.pump_cHz, 0);
  r.co = 0;
  CHECK(run_until_step(&r, BURN_STEP_OFF, 25 * MIN));
  CHECK_EQ(start(&r, HEAT, 50), 0x2F); /* latched until a user reset */
}

/* CO-2: a CO sensor fault is an alarm with the heater-safe action: the run ends as for CO, and the
 * fault stays until the sensor reads again and the user resets it. */
static void test_co_sensor_fault(void) {
  rig r;
  rig_init(&r, 0, 10);
  CHECK_EQ(start(&r, HEAT, 50), 0);
  CHECK(run_until_step(&r, BURN_STEP_RUNNING, 5 * MIN));
  r.co_fault = 1;
  run(&r, TICK);
  CHECK_EQ(r.b.fault, BURN_FAULT_CO_SENSOR);
  CHECK_EQ(r.b.why, BURN_WHY_CO_SENSOR);
  CHECK(r.b.fault_latched);
  CHECK_EQ(r.b.step, BURN_STEP_BURN_OFF);
  CHECK_EQ(r.b.out.pump_cHz, 0);
  CHECK(run_until_step(&r, BURN_STEP_OFF, 25 * MIN));
  burn_inputs in = inputs(&r);
  CHECK_EQ(burn_fault_reset(&r.b, &in), 0x2F); /* still faulty */
  CHECK_EQ(start(&r, FAN, 50), 0x2F);
  r.co_fault = 0;
  CHECK_EQ(start(&r, HEAT, 50), 0x2F); /* latched until a user reset */
  in = inputs(&r);
  CHECK_EQ(burn_fault_reset(&r.b, &in), 0);
  CHECK_EQ(start(&r, HEAT, 50), 0);

  /* With the heater off: latched at once, and no start. */
  rig_init(&r, 0, 10);
  r.co_fault = 1;
  run(&r, TICK);
  CHECK_EQ(r.b.fault, BURN_FAULT_CO_SENSOR);
  CHECK(r.b.fault_latched);
  CHECK_EQ(r.b.step, BURN_STEP_OFF);
  CHECK_EQ(start(&r, HEAT, 50), 0x2F);
}

static void test_glow_stage_fault(void) {
  rig r;
  rig_init(&r, 0, 10);
  CHECK_EQ(start(&r, HEAT, 50), 0);
  CHECK(run_until_step(&r, BURN_STEP_IGNITION, 1 * MIN));
  r.glow_fault = 1; /* e.g. current between pulses: the low side is stuck on */
  run(&r, TICK);
  CHECK_EQ(r.b.fault, BURN_FAULT_GLOW);
  CHECK_EQ(r.b.step, BURN_STEP_BURN_OFF);
  CHECK_EQ(r.b.out.pump_cHz, 0);
  CHECK_EQ(r.b.out.glow_w, 0);
  CHECK(run_until_step(&r, BURN_STEP_OFF, 25 * MIN));
  CHECK_EQ(start(&r, HEAT, 50), 0x2F); /* no restart: the board needs looking at */
}

static void test_fan_stage_faults(void) {
  /* The fan's switch stuck on while running: fuel off, burn off and cool (the fan runs anyway). */
  rig r;
  rig_init(&r, 0, 10);
  CHECK_EQ(start(&r, HEAT, 50), 0);
  CHECK(run_until_step(&r, BURN_STEP_RUNNING, 5 * MIN));
  r.fan_fault = 2;
  run(&r, TICK);
  CHECK_EQ(r.b.fault, BURN_FAULT_FAN);
  CHECK_EQ(r.b.why, BURN_WHY_FAN_STAGE);
  CHECK_EQ(r.b.step, BURN_STEP_BURN_OFF);
  CHECK_EQ(r.b.out.pump_cHz, 0);
  CHECK(r.b.fault_latched);

  /* A stall reported by the fan stage: everything off at once (no air), the fan stage cleared,
   * then the fan tries again and cools the heater down (FAN-5). */
  rig r2;
  rig_init(&r2, 0, 10);
  CHECK_EQ(start(&r2, HEAT, 50), 0);
  CHECK(run_until_step(&r2, BURN_STEP_RUNNING, 5 * MIN));
  r2.fan_fault = 1;
  run(&r2, TICK);
  CHECK_EQ(r2.b.step, BURN_STEP_FAN_RETRY);
  CHECK_EQ(burn_state(&r2.b), BURN_STATE_STOPPING);
  CHECK_EQ(r2.b.why, BURN_WHY_FAN_STALL);
  CHECK_EQ(r2.b.out.fan_rpm, 0);
  CHECK_EQ(r2.b.out.pump_cHz, 0);
  CHECK_EQ(r2.b.out.glow_w, 0);
  CHECK_EQ(r2.fan_fault, 0);
  CHECK(!r2.b.fault_latched);
  CHECK(run_until_step(&r2, BURN_STEP_COOLDOWN, 6 * SEC));
  CHECK(run_until_step(&r2, BURN_STEP_OFF, 20 * MIN));
  CHECK_EQ(r2.b.why, BURN_WHY_FAN_STALL);
  CHECK(!r2.b.fault_latched);
  CHECK_EQ(start(&r2, HEAT, 50), 0); /* the user may start again */
}

static void test_precheck(void) {
  /* Nothing runs until the self-test passes. */
  rig r;
  rig_init(&r, 0, 10);
  r.precheck = BURN_PRECHECK_RUNNING;
  CHECK_EQ(start(&r, HEAT, 50), 0);
  run(&r, 1 * SEC);
  CHECK_EQ(r.b.step, BURN_STEP_PRE_CHECK);
  CHECK_EQ(r.b.out.fan_rpm, 0);
  CHECK_EQ(r.b.out.pump_cHz, 0);
  CHECK_EQ(r.b.out.glow_w, 0);
  r.precheck = BURN_PRECHECK_PASSED;
  run(&r, TICK);
  CHECK_EQ(r.b.step, BURN_STEP_SELF_TEST);

  /* A failed self-test: a pump fault, no restart until a reset. */
  rig r2;
  rig_init(&r2, 0, 10);
  r2.precheck = BURN_PRECHECK_FAILED_PUMP;
  CHECK_EQ(start(&r2, HEAT, 50), 0);
  run(&r2, TICK);
  CHECK_EQ(r2.b.step, BURN_STEP_OFF);
  CHECK_EQ(r2.b.fault, BURN_FAULT_PUMP);
  CHECK_EQ(r2.b.why, BURN_WHY_SELF_TEST);
  CHECK_EQ(start(&r2, HEAT, 50), 0x2F);
  CHECK_EQ(r2.pump_ms, 0);

  /* One that never ends: a start fault after its time. */
  rig r3;
  rig_init(&r3, 0, 10);
  r3.precheck = BURN_PRECHECK_RUNNING;
  CHECK_EQ(start(&r3, PRIME, 0), 0);
  run(&r3, 3 * SEC);
  CHECK_EQ(r3.b.step, BURN_STEP_OFF);
  CHECK_EQ(r3.b.fault, BURN_FAULT_START);
  CHECK_EQ(r3.pump_ms, 0);
}

static void test_pump_stage_fault(void) {
  rig r;
  rig_init(&r, 0, 10);
  CHECK_EQ(start(&r, HEAT, 50), 0);
  CHECK(run_until_step(&r, BURN_STEP_RUNNING, 5 * MIN));
  r.pump_fault = 2; /* current between strokes: the low side conducts */
  run(&r, TICK);
  CHECK_EQ(r.b.fault, BURN_FAULT_PUMP);
  CHECK_EQ(r.b.why, BURN_WHY_PUMP_STAGE);
  CHECK_EQ(r.b.step, BURN_STEP_BURN_OFF);
  CHECK_EQ(r.b.out.pump_cHz, 0);
  CHECK(r.b.out.fan_rpm > 0); /* the fan still cools it */
  r.pump_fault = 0;
  CHECK(run_until_step(&r, BURN_STEP_OFF, 25 * MIN));
  CHECK_EQ(start(&r, HEAT, 50), 0x2F); /* latched until a reset */
}

static void test_board_derate(void) {
  rig r;
  rig_init(&r, 0, 10);
  /* The cap: no derate up to 85 C; then the highest level whose fan current fits. */
  CHECK_EQ(burn_power_cap(&r.b, 850), 100);
  CHECK_EQ(burn_power_cap(&r.b, BURN_UNKNOWN_TEMP), 100);
  CHECK_EQ(burn_power_cap(&r.b, 900), 66);  /* 2.07 A allowed: level 7 (1.75 A) fits, 8 (2.1) not */
  CHECK_EQ(burn_power_cap(&r.b, 950), 44);  /* 1.33 A: level 5 (1.24 A) */
  CHECK_EQ(burn_power_cap(&r.b, 1000), 0);  /* the lowest level */
  CHECK_EQ(burn_power_cap(&r.b, 1200), 0);

  CHECK_EQ(start(&r, HEAT, 100), 0);
  CHECK(run_until_step(&r, BURN_STEP_RUNNING, 5 * MIN));
  run(&r, 20 * SEC);
  CHECK_EQ(r.b.running_power, 100);
  r.board_c10 = 900;
  run(&r, TICK);
  CHECK_EQ(r.b.running_power, 66); /* the fuel steps down at once */
  CHECK(r.b.out.fan_rpm > 1800 + (4500 - 1800) * 66 / 100); /* the air follows */
  run(&r, 15 * SEC);
  CHECK_EQ(r.b.out.fan_rpm, 1800 + (4500 - 1800) * 66 / 100);
  r.board_c10 = 600; /* cooled again: back up, air first */
  run(&r, 30 * SEC);
  CHECK_EQ(r.b.running_power, 100);
  CHECK_EQ(r.air_below_fuel, 0);

  /* Fan only is held down the same way. */
  rig r2;
  rig_init(&r2, 0, 10);
  r2.board_c10 = 950;
  CHECK_EQ(start(&r2, FAN, 100), 0);
  run(&r2, TICK);
  CHECK_EQ(r2.b.out.fan_rpm, 1800 + (4500 - 1800) * 44 / 100);
}

static void test_board_too_hot(void) {
  rig r;
  rig_init(&r, 0, 10);
  CHECK_EQ(start(&r, HEAT, 60), 0);
  CHECK(run_until_step(&r, BURN_STEP_RUNNING, 5 * MIN));
  r.board_c10 = 1060;
  run(&r, 9 * SEC);
  CHECK_EQ(r.b.step, BURN_STEP_RUNNING); /* at the lowest power, for a while */
  CHECK_EQ(r.b.running_power, 0);
  run(&r, 2 * SEC);
  CHECK_EQ(r.b.step, BURN_STEP_BURN_OFF);
  CHECK_EQ(r.b.fault, BURN_FAULT_OVERHEAT);
  CHECK_EQ(r.b.why, BURN_WHY_BOARD_HOT);
  CHECK(run_until_step(&r, BURN_STEP_OFF, 25 * MIN)); /* the cooldown keeps its full fan */
}

static void test_plug_refused(void) {
  rig r;
  rig_init(&r, 0, 10);
  CHECK_EQ(start(&r, HEAT, 50), 0);
  CHECK(run_until_step(&r, BURN_STEP_PREHEAT, 10 * SEC));
  r.plug_refused = 3; /* the probe says a 12 V plug, set as 24 V, on a 24 V supply */
  run(&r, TICK);
  CHECK_EQ(r.b.step, BURN_STEP_OFF);
  CHECK_EQ(r.b.fault, BURN_FAULT_GLOW);
  CHECK_EQ(r.b.why, BURN_WHY_PLUG_TYPE);
  CHECK_EQ(r.pump_ms, 0); /* no fuel went in */
  CHECK(!r.b.fault_latched); /* fix the setting and start again */
  r.plug_refused = 0;
  CHECK_EQ(start(&r, HEAT, 50), 0);
}

static void test_fan_stall(void) {
  rig r;
  rig_init(&r, 0, 10);
  CHECK_EQ(start(&r, HEAT, 50), 0);
  CHECK(run_until_step(&r, BURN_STEP_RUNNING, 5 * MIN));
  r.m.fan_stalled = 1;
  uint32_t t = r.now;
  CHECK(run_until_step(&r, BURN_STEP_FAN_RETRY, 10 * SEC));
  CHECK((uint32_t) (r.now - t) <= 6 * SEC); /* fan slows (inertia), then 1 s below the stall speed */
  CHECK_EQ(r.b.fault, BURN_FAULT_FAN);
  CHECK_EQ(r.b.why, BURN_WHY_FAN_STALL);
  run(&r, TICK);
  CHECK_EQ(r.b.out.pump_cHz, 0);
  CHECK_EQ(r.b.out.glow_w, 0);
  CHECK_EQ(r.b.out.fan_rpm, 0);
  /* It never frees: two retries (each a cooldown that does not turn), then locked out. */
  uint32_t pump = r.pump_ms, glow = r.glow_ms;
  CHECK(run_until_step(&r, BURN_STEP_OFF, 60 * SEC));
  CHECK_EQ(r.b.fan_stalls, 3);
  CHECK(r.b.fault_latched);
  CHECK_EQ(r.pump_ms, pump);
  CHECK_EQ(r.glow_ms, glow);
  CHECK_EQ(start(&r, HEAT, 50), 0x2F);
  run(&r, 5 * MIN);
  CHECK_EQ(r.b.step, BURN_STEP_OFF); /* no more tries */
  CHECK_EQ(r.b.out.fan_rpm, 0);
  /* A fault reset clears it. */
  r.m.fan_stalled = 0;
  burn_inputs in = inputs(&r);
  CHECK_EQ(burn_fault_reset(&r.b, &in), 0);
  CHECK_EQ(r.b.fan_stalls, 0);
}

static void test_fan_stall_frees(void) {
  /* A stall before any fuel: the start carries on once the fan turns again. */
  rig r;
  rig_init(&r, 0, 10);
  CHECK_EQ(start(&r, HEAT, 50), 0);
  CHECK(run_until_step(&r, BURN_STEP_PREHEAT, 10 * SEC));
  r.m.fan_stalled = 1;
  CHECK(run_until_step(&r, BURN_STEP_FAN_RETRY, 10 * SEC));
  CHECK_EQ(burn_state(&r.b), BURN_STATE_STARTING);
  CHECK_EQ(r.b.out.glow_w, 0);
  r.m.fan_stalled = 0;
  CHECK(run_until_step(&r, BURN_STEP_SELF_TEST, 6 * SEC));
  CHECK_EQ(r.b.fault, BURN_FAULT_NONE);
  CHECK(run_until_step(&r, BURN_STEP_RUNNING, 5 * MIN));
  CHECK_EQ(r.b.fan_stalls, 1);
  /* Ten minutes of normal turning and the count forgets. */
  run(&r, 10 * MIN + SEC);
  CHECK_EQ(r.b.fan_stalls, 0);

  /* A fan-only run carries on too; a stop during the wait ends it at once. */
  rig f;
  rig_init(&f, 0, 10);
  CHECK_EQ(start(&f, FAN, 50), 0);
  run(&f, 20 * SEC);
  f.m.fan_stalled = 1;
  CHECK(run_until_step(&f, BURN_STEP_FAN_RETRY, 10 * SEC));
  CHECK_EQ(burn_state(&f.b), BURN_STATE_FAN_ONLY);
  f.m.fan_stalled = 0;
  CHECK(run_until_step(&f, BURN_STEP_FAN_ONLY, 6 * SEC));
  f.m.fan_stalled = 1;
  CHECK(run_until_step(&f, BURN_STEP_FAN_RETRY, 20 * SEC));
  burn_stop(&f.b, BURN_WHY_ASKED, f.now);
  CHECK_EQ(f.b.step, BURN_STEP_OFF);
}

static void test_reset_fan(void) {
  /* The first heat start soon after a reset runs the fan 30 s longer before the glow (GLW-21). */
  rig r;
  rig_init(&r, 0, 10);
  r.b.reset_fan_due = 1;
  CHECK_EQ(start(&r, HEAT, 50), 0);
  CHECK(run_until_step(&r, BURN_STEP_SELF_TEST, SEC));
  uint32_t t = r.now;
  CHECK(run_until_step(&r, BURN_STEP_PREHEAT, 45 * SEC));
  CHECK((uint32_t) (r.now - t) >= 30 * SEC);
  CHECK((uint32_t) (r.now - t) <= 35 * SEC);
  CHECK_EQ(r.glow_ms, 0);
  /* The next start does not owe it. */
  burn_stop(&r.b, BURN_WHY_ASKED, r.now);
  run(&r, TICK);
  CHECK_EQ(start(&r, HEAT, 50), 0);
  t = r.now; /* the fan still turns: the self-test may pass in the first pass */
  CHECK(run_until_step(&r, BURN_STEP_PREHEAT, 10 * SEC));
  CHECK((uint32_t) (r.now - t) < 10 * SEC);

  /* Nor does a first start long after the reset. */
  rig q;
  rig_init(&q, 0, 10);
  q.b.reset_fan_due = 1;
  run(&q, 5 * MIN);
  CHECK_EQ(start(&q, HEAT, 50), 0);
  CHECK(run_until_step(&q, BURN_STEP_SELF_TEST, SEC));
  t = q.now;
  CHECK(run_until_step(&q, BURN_STEP_PREHEAT, 10 * SEC));
  CHECK((uint32_t) (q.now - t) < 10 * SEC);
}

static void test_sensor_open(void) {
  rig r;
  rig_init(&r, 0, 10);
  r.m.sensor_open = 1;
  CHECK_EQ(start(&r, HEAT, 50), 0x2F);
  CHECK_EQ(r.b.fault, BURN_FAULT_SENSOR);
  r.m.sensor_open = 0;
  CHECK_EQ(start(&r, HEAT, 50), 0);
  CHECK(run_until_step(&r, BURN_STEP_RUNNING, 5 * MIN));
  r.m.sensor_open = 1;
  run(&r, TICK);
  CHECK_EQ(r.b.step, BURN_STEP_BURN_OFF);
  CHECK_EQ(r.b.fault, BURN_FAULT_SENSOR);
  /* Without a reading the cooldown runs its full length, then stops. */
  CHECK(run_until_step(&r, BURN_STEP_OFF, 21 * MIN));
}

static void test_reset_while_hot(void) {
  rig r;
  rig_init(&r, 0, 10);
  CHECK_EQ(start(&r, HEAT, 100), 0);
  CHECK(run_until_step(&r, BURN_STEP_RUNNING, 5 * MIN));
  run(&r, 5 * MIN);
  /* The MCU resets: the heater is hot and nobody asked for anything. */
  burn_params p;
  burn_params_default(&p);
  burn_inputs in = inputs(&r);
  burn_init(&r.b, &p, r.now, &in);
  CHECK_EQ(r.b.step, BURN_STEP_COOLDOWN);
  CHECK_EQ(start(&r, HEAT, 50), 0x20);
  run(&r, TICK);
  CHECK_EQ(r.b.out.pump_cHz, 0);
  CHECK(r.b.out.fan_rpm > 0);
  CHECK(run_until_step(&r, BURN_STEP_OFF, 25 * MIN));
  CHECK_EQ(r.pump_ms > 0, 1);
}

static void test_fan_only_and_priming(void) {
  rig r;
  rig_init(&r, 0, 10);
  CHECK_EQ(start(&r, FAN, 40), 0);
  CHECK_EQ(burn_state(&r.b), BURN_STATE_FAN_ONLY);
  run(&r, 30 * SEC);
  CHECK_EQ(r.b.out.fan_rpm, 1800 + (4500 - 1800) * 40 / 100);
  CHECK_EQ(r.b.out.pump_cHz, 0);
  CHECK_EQ(r.glow_ms, 0);
  burn_stop(&r.b, BURN_WHY_ASKED, r.now);
  CHECK_EQ(r.b.step, BURN_STEP_OFF);

  CHECK_EQ(start(&r, PRIME, 0), 0);
  CHECK_EQ(r.b.step, BURN_STEP_PRE_CHECK); /* priming strokes the pump: the interlocks first */
  run(&r, TICK);
  CHECK_EQ(burn_state(&r.b), BURN_STATE_PRIMING);
  run(&r, 10 * SEC);
  CHECK_EQ(burn_prime_left_s(&r.b, r.now), 50);
  CHECK_EQ(r.b.out.pump_cHz, r.b.p.pump_prime_cHz);
  CHECK_EQ(r.b.out.fan_rpm, 0);
  CHECK(run_until_step(&r, BURN_STEP_OFF, 60 * SEC));
  CHECK_EQ(burn_prime_left_s(&r.b, r.now), 0);
  CHECK_EQ(start(&r, 7, 0), 0x13);
}

static void test_supply(void) {
  rig r;
  rig_init(&r, 0, 10);
  r.m.supply_cv = 1000;
  CHECK_EQ(start(&r, HEAT, 50), 0x2F);
  CHECK_EQ(r.b.fault, BURN_FAULT_SUPPLY_LOW);
  r.m.supply_cv = 1260;
  CHECK_EQ(start(&r, HEAT, 50), 0);
  CHECK(run_until_step(&r, BURN_STEP_RUNNING, 5 * MIN));
  r.m.supply_cv = 1000;
  run(&r, 4 * SEC);
  CHECK_EQ(r.b.step, BURN_STEP_RUNNING); /* a dip is tolerated */
  r.m.supply_cv = 1260;
  run(&r, 2 * SEC);
  r.m.supply_cv = 3100;
  CHECK(run_until_step(&r, BURN_STEP_BURN_OFF, 6 * SEC));
  CHECK_EQ(r.b.fault, BURN_FAULT_SUPPLY_HIGH);
}

static void test_stop_before_fuel(void) {
  rig r;
  rig_init(&r, 0, 10);
  CHECK_EQ(start(&r, HEAT, 50), 0);
  CHECK(run_until_step(&r, BURN_STEP_PREHEAT, 10 * SEC));
  run(&r, 5 * SEC);
  burn_stop(&r.b, BURN_WHY_ASKED, r.now);
  CHECK_EQ(r.b.step, BURN_STEP_OFF); /* no fuel went in: nothing to burn off */
  run(&r, TICK);
  CHECK_EQ(r.b.out.glow_w, 0);
  CHECK_EQ(r.b.out.fan_rpm, 0);
  CHECK_EQ(r.pump_ms, 0);
}

static void test_dead_man_stop(void) {
  rig r;
  rig_init(&r, 0, 10);
  CHECK_EQ(start(&r, HEAT, 50), 0);
  CHECK(run_until_step(&r, BURN_STEP_RUNNING, 5 * MIN));
  CHECK(burn_active(&r.b));
  burn_stop(&r.b, BURN_WHY_DEADMAN, r.now);
  CHECK(!burn_active(&r.b));
  CHECK_EQ(r.b.why, BURN_WHY_DEADMAN);
  CHECK(run_until_step(&r, BURN_STEP_OFF, 25 * MIN)); /* a full burn-off and cooldown */
}

int main(void) {
  test_full_cycle(0);
  test_full_cycle(0xFFFF0000u); /* across the 49-day wrap of the millisecond clock */
  test_glow_needs_airflow();
  test_no_ignition();
  test_flame_out();
  test_overheat_derate_holds();
  test_overheat_limit();
  test_overheat_latch();
  test_co_alarm();
  test_co_sensor_fault();
  test_glow_stage_fault();
  test_fan_stage_faults();
  test_precheck();
  test_pump_stage_fault();
  test_board_derate();
  test_board_too_hot();
  test_plug_refused();
  test_fan_stall();
  test_fan_stall_frees();
  test_reset_fan();
  test_sensor_open();
  test_reset_while_hot();
  test_fan_only_and_priming();
  test_supply();
  test_stop_before_fuel();
  test_dead_man_stop();
  printf("burn: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
