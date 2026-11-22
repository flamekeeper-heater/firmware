/* The fan stage controller against a DC fan motor model: spin-up on the ramp, the 10 V cap, the
 * start current limit (a frozen fan must never reach the 8 A fuse), the speed loop, and every
 * fault. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../core/fan.h"

static int checks = 0, failures = 0;
#define CHECK(c)                                                  \
  do {                                                            \
    checks++;                                                     \
    if (!(c)) {                                                   \
      failures++;                                                 \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);         \
    }                                                             \
  } while (0)

#define TICK 10u /* the safety loop's pass, ms */

/* A brushed DC fan: 0.9 Ohm, full speed 5000 rpm at 10 V drawing 4.3 A. */
typedef struct {
  double r, ke, kt, load, inertia;
  double rpm, i;
  double vin;
  int frozen;       /* the rotor does not turn (ice, debris) */
  int switch_stuck; /* the low-side switch is shorted: the motor sees the full supply */
  double pushed_rpm; /* > 0: something else turns the fan at this speed */
} motor;

static void motor_init(motor *mo, double r, double vin) {
  memset(mo, 0, sizeof(*mo));
  mo->r = r;
  mo->ke = (10.0 - 4.3 * 0.9) / 5000.0;
  mo->kt = 1.0;
  mo->load = 4.3 / (5000.0 * 5000.0);
  mo->inertia = 0.003;
  mo->vin = vin;
}

/* One step at this duty (permille); returns the average switch current the shunt reads, mA. */
static double motor_step(motor *mo, uint16_t duty, double dt_s) {
  double d = mo->switch_stuck ? 1.0 : duty / 1000.0;
  double v = d * mo->vin;
  double e = mo->ke * mo->rpm;
  mo->i = (v - e) / mo->r;
  if (mo->i < 0)
    mo->i = 0; /* the freewheel diode: no current back into the supply */
  if (mo->pushed_rpm > 0)
    mo->rpm = mo->pushed_rpm;
  else if (mo->frozen)
    mo->rpm = 0;
  else {
    /* Torque: the motor's, less the fan's air load and the bearings' friction (0.2 A worth). */
    double friction = mo->rpm > 0 ? 0.2 : 0;
    mo->rpm += (mo->kt * mo->i - mo->load * mo->rpm * mo->rpm - friction) / mo->inertia * dt_s;
    if (mo->rpm < 0)
      mo->rpm = 0;
  }
  /* The shunt sees the motor current only while the switch conducts; one ADC step is 54 mA. */
  double sw = mo->i * d * 1000.0;
  return floor(sw / 54.0) * 54.0;
}

typedef struct {
  fan f;
  motor mo;
  uint32_t now;
  uint16_t duty;
  double isense;
  double max_i;            /* the true motor current: the fuse's view */
  double max_i_limited;    /* ... once the start limit can act (duty at or above 10 %) */
  uint16_t max_duty;
  int slew_violations;
} rig;

static void rig_init(rig *r, double ohms, double vin) {
  memset(r, 0, sizeof(*r));
  fan_params p;
  fan_params_default(&p);
  fan_init(&r->f, &p, 0);
  motor_init(&r->mo, ohms, vin);
}

static void tick(rig *r, uint16_t target) {
  fan_meas m;
  m.rpm = (uint16_t) (r->mo.rpm + 0.5);
  m.vin_cv = (uint16_t) (r->mo.vin * 100 + 0.5);
  m.isense_ma = (uint16_t) r->isense;
  uint16_t before = r->duty;
  r->duty = fan_step(&r->f, r->now, target, &m);
  uint16_t cap = fan_duty_cap(&r->f, m.vin_cv);
  if (r->duty > before && r->duty - before > cap * TICK / 1000 + 1)
    r->slew_violations++;
  if (r->duty > r->max_duty)
    r->max_duty = r->duty;
  r->isense = motor_step(&r->mo, r->duty, TICK / 1000.0);
  if (r->mo.i > r->max_i)
    r->max_i = r->mo.i;
  /* The start limit's domain: duty high enough to estimate, fan below half its command. */
  if (r->duty >= 100 && r->mo.rpm * 2 < target && r->mo.i > r->max_i_limited)
    r->max_i_limited = r->mo.i;
  r->now += TICK;
}

static void run(rig *r, uint32_t ms, uint16_t target) {
  for (uint32_t t = 0; t < ms; t += TICK)
    tick(r, target);
}

static void test_spin_up(void) {
  rig r;
  rig_init(&r, 0.9, 12.6);
  run(&r, 500, 3000);
  CHECK(r.duty <= fan_duty_cap(&r.f, 1260) / 2 + 2); /* half a second: at most half way up */
  run(&r, 7500, 3000);
  CHECK(fabs(r.mo.rpm - 3000) < 150);
  run(&r, 5000, 3000);
  CHECK(fabs(r.mo.rpm - 3000) < 60);
  CHECK(r.slew_violations == 0);
  CHECK(r.max_duty <= fan_duty_cap(&r.f, 1260));
  CHECK(r.max_i_limited < 6.6); /* the start limit, while below half speed */
  CHECK(r.f.fault == FAN_FAULT_NONE);
  /* Full speed: above half speed the start limit is off, the fuse still never sees 8 A. */
  run(&r, 10000, 4800);
  CHECK(fabs(r.mo.rpm - 4800) < 150);
  CHECK(r.max_i < 8.0);
  /* The feed-forward carries it there, the integral only trims (above 4294 rpm the feed-forward
   * once overflowed 32 bits and the integral carried it all). */
  CHECK(r.f.integ < 100 * 1000 && r.f.integ > -100 * 1000);
}

static void test_24v_cap(void) {
  rig r;
  rig_init(&r, 0.9, 26.0);
  run(&r, 15000, 4500);
  CHECK(r.max_duty <= 1000 * 10.0 / 26.0 + 1); /* 10 V at the motor at most */
  CHECK(fabs(r.mo.rpm - 4500) < 150);
  CHECK(r.max_i_limited < 6.6);
  CHECK(r.max_i < 8.0);
  CHECK(r.f.fault == FAN_FAULT_NONE);
  if (failures)
    printf("  24 V spin-up: start %.2f A, max %.2f A\n", r.max_i_limited, r.max_i);
}

static void test_frozen(double ohms, double vin) {
  rig r;
  rig_init(&r, ohms, vin);
  r.mo.frozen = 1;
  uint32_t t = 0;
  while (r.f.fault == FAN_FAULT_NONE && t < 10000) {
    tick(&r, 4500);
    t += TICK;
  }
  CHECK(r.f.fault == FAN_FAULT_STALL);
  CHECK(t < 2500); /* the stall rule ends it soon after current flows */
  CHECK(r.max_i < 8.0); /* never the fuse */
  CHECK(r.max_i_limited < 6.6);
  run(&r, 1000, 4500);
  CHECK(r.duty == 0); /* and it stays off */
  if (failures)
    printf("  frozen %.1f Ohm at %.1f V: max %.2f A, limited %.2f A, fault after %u ms\n", ohms, vin, r.max_i,
           r.max_i_limited, t);
}

static void test_spin_down_is_not_a_fault(void) {
  rig r;
  rig_init(&r, 0.9, 12.6);
  run(&r, 12000, 4800);
  run(&r, 15000, 1500); /* slowing from 4800 to 1500 */
  CHECK(r.f.fault == FAN_FAULT_NONE);
  CHECK(fabs(r.mo.rpm - 1500) < 100);
  run(&r, 35000, 0); /* off: it coasts down */
  CHECK(r.f.fault == FAN_FAULT_NONE);
  CHECK(r.duty == 0);
}

static void test_stuck_on(void) {
  rig r;
  rig_init(&r, 0.9, 12.6);
  run(&r, 5000, 2000);
  r.mo.switch_stuck = 1;
  run(&r, 2000, 0); /* asked to stop: duty 0, yet current flows */
  CHECK(r.f.fault == FAN_FAULT_STUCK_ON);
}

static void test_uncommanded_and_overspeed(void) {
  rig r;
  rig_init(&r, 0.9, 12.6);
  r.mo.pushed_rpm = 2000; /* turned from outside with no command */
  run(&r, 29000, 0);
  CHECK(r.f.fault == FAN_FAULT_NONE); /* still inside the spin-down allowance */
  run(&r, 2000, 0);
  CHECK(r.f.fault == FAN_FAULT_UNCOMMANDED);

  rig r2;
  rig_init(&r2, 0.9, 12.6);
  run(&r2, 10000, 2000);
  r2.mo.pushed_rpm = 3000; /* 50 % over the command */
  run(&r2, 35000, 2000);
  CHECK(r2.f.fault == FAN_FAULT_OVERSPEED);
}

static void test_off_is_off(void) {
  rig r;
  rig_init(&r, 0.9, 12.6);
  run(&r, 5000, 0);
  CHECK(r.max_duty == 0);
  CHECK(r.f.fault == FAN_FAULT_NONE);
}

int main(void) {
  test_spin_up();
  test_24v_cap();
  test_frozen(0.9, 12.6);
  test_frozen(0.9, 26.0);
  test_frozen(0.6, 26.0); /* a low-resistance winding on 24 V: the case the fuse fears */
  test_spin_down_is_not_a_fault();
  test_stuck_on();
  test_uncommanded_and_overspeed();
  test_off_is_off();
  printf("fan: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
