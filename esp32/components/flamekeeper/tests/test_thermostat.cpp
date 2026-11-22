// Host unit test for the thermostat. Build and run with tests/run.sh.
#include "../thermostat.h"

#include <cmath>
#include <cstdio>
#include <vector>

static int checks = 0, failures = 0;

#define CHECK(cond) \
  do { \
    checks++; \
    if (!(cond)) { \
      failures++; \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
  } while (0)

#define CHECK_NEAR(a, b, tol) \
  do { \
    checks++; \
    double va_ = (a), vb_ = (b); \
    if (!(std::fabs(va_ - vb_) <= (tol))) { \
      failures++; \
      printf("FAIL %s:%d: %s = %g, expected %s = %g (+-%g)\n", __FILE__, __LINE__, #a, va_, #b, vb_, double(tol)); \
    } \
  } while (0)

using namespace esphome::flamekeeper;

static ThermostatSettings plain() {
  ThermostatSettings s;
  s.ramp_up = 0;  // most tests look at the static mapping; ramps are tested on their own
  s.ramp_down = 0;
  return s;
}

static void test_sanitise() {
  Thermostat t;
  ThermostatSettings s;
  s.band = 0.0f;
  s.integral_min = -3;
  s.min_power = -5;
  s.max_power = 250;
  s.ramp_up = -1;
  s.ramp_down = -1;
  s.stop_above = -2;
  s.start_below = -2;
  s.fallback_power = 500;
  t.set(s);
  const ThermostatSettings &g = t.settings();
  CHECK_NEAR(g.band, 0.1, 1e-6);
  CHECK(g.integral_min == 0);
  CHECK(g.min_power == 0);
  CHECK(g.max_power == 100);
  CHECK(g.ramp_up == 0 && g.ramp_down == 0);
  CHECK(g.stop_above == 0 && g.start_below == 0);
  CHECK(g.fallback_power == 100);

  s = ThermostatSettings();
  s.min_power = 60;
  s.max_power = 30;  // max below min -> max follows min
  t.set(s);
  CHECK(t.settings().min_power == 60 && t.settings().max_power == 60);
}

static void test_proportional() {
  Thermostat t;
  t.set(plain());  // band 2
  t.enable(0, 20.0f, 20.0f, 0.0f);
  uint32_t now = 0;
  struct { float room, want; } cases[] = {{22.0f, 0}, {21.0f, 0}, {20.0f, 0}, {19.0f, 50}, {18.0f, 100}, {15.0f, 100}};
  for (auto &c : cases) {
    now += 1000;
    ThermostatOutput o = t.update(now, c.room, 20.0f, true);
    CHECK(o.want_heat);
    CHECK_NEAR(o.demand, c.want, 1e-3);
  }
  // Quarter of the band below target.
  CHECK_NEAR(t.update(now + 1000, 19.5f, 20.0f, true).demand, 25.0, 1e-3);
}

static void test_limits() {
  Thermostat t;
  ThermostatSettings s = plain();
  s.min_power = 20;
  s.max_power = 80;
  t.set(s);
  t.enable(0, 20.0f, 20.0f, 0.0f);
  CHECK_NEAR(t.demand(), 20, 1e-6);  // initial demand held to the limits
  CHECK_NEAR(t.update(1000, 25.0f, 20.0f, true).demand, 20, 1e-3);
  CHECK_NEAR(t.update(2000, 10.0f, 20.0f, true).demand, 80, 1e-3);
  CHECK_NEAR(t.update(3000, 19.0f, 20.0f, true).demand, 50, 1e-3);  // halfway between min and max
  t.enable(0, 20.0f, 20.0f, 95.0f);
  CHECK_NEAR(t.demand(), 80, 1e-6);
}

static void test_ramps() {
  Thermostat t;
  ThermostatSettings s;
  s.ramp_up = 30;    // 0.5 % per second
  s.ramp_down = 12;  // 0.2 % per second
  t.set(s);
  t.enable(0, 10.0f, 20.0f, 0.0f);
  uint32_t now = 0;
  for (int i = 0; i < 60; i++)
    t.update(now += 1000, 10.0f, 20.0f, true);
  CHECK_NEAR(t.demand(), 30.0, 0.01);  // one minute at 30 %/min
  for (int i = 0; i < 60; i++)
    t.update(now += 1000, 10.0f, 20.0f, true);
  CHECK_NEAR(t.demand(), 60.0, 0.01);
  for (int i = 0; i < 100; i++)
    t.update(now += 1000, 30.0f, 20.0f, true);  // wants 0
  CHECK_NEAR(t.demand(), 60.0 - 20.0, 0.01);
  for (int i = 0; i < 400; i++)
    t.update(now += 1000, 30.0f, 20.0f, true);
  CHECK_NEAR(t.demand(), 0.0, 1e-4);  // reaches the target exactly, no undershoot
  for (int i = 0; i < 400; i++)
    t.update(now += 1000, 10.0f, 20.0f, true);
  CHECK_NEAR(t.demand(), 100.0, 1e-4);  // and no overshoot going up

  // Unlimited ramps jump at once.
  Thermostat u;
  u.set(plain());
  u.enable(0, 10.0f, 20.0f, 0.0f);
  CHECK_NEAR(u.update(1000, 10.0f, 20.0f, true).demand, 100.0, 1e-4);
  CHECK_NEAR(u.update(2000, 30.0f, 20.0f, true).demand, 0.0, 1e-4);
}

static void test_hold_not_burning() {
  Thermostat t;
  ThermostatSettings s;
  s.ramp_up = 60;
  s.integral_min = 1;
  t.set(s);
  t.enable(0, 10.0f, 20.0f, 40.0f);
  uint32_t now = 0;
  for (int i = 0; i < 30; i++) {
    ThermostatOutput o = t.update(now += 1000, 10.0f, 20.0f, false);
    CHECK(o.want_heat);
    CHECK_NEAR(o.demand, 40.0, 1e-6);
  }
  // Integral was frozen while holding: after the hold the first burning step is slew-limited only.
  ThermostatOutput o = t.update(now += 1000, 10.0f, 20.0f, true);
  CHECK_NEAR(o.demand, 41.0, 1e-3);
  // Still clamped to the limits while held.
  s.max_power = 30;
  t.set(s);
  CHECK_NEAR(t.update(now += 1000, 10.0f, 20.0f, false).demand, 30.0, 1e-6);
}

static void test_integral() {
  Thermostat t;
  ThermostatSettings s = plain();
  s.integral_min = 2;  // 120 s
  t.set(s);
  t.enable(0, 19.5f, 20.0f, 0.0f);
  // Steady offset of 0.5 C -> P = 12.5 %. The integral must lift the output above that over time.
  uint32_t now = 0;
  float first = t.update(now += 1000, 19.5f, 20.0f, true).demand;
  CHECK_NEAR(first, 25.0, 0.5);
  for (int i = 0; i < 119; i++)
    t.update(now += 1000, 19.5f, 20.0f, true);
  // After one integral time the I term equals the P term again: 25 % + 25 %.
  CHECK_NEAR(t.demand(), 50.0, 1.0);

  // The room settles at the target: P = 0, the integral alone holds the output (no steady offset).
  float held = t.update(now += 1000, 20.0f, 20.0f, true).demand;
  CHECK_NEAR(held, 25.0, 1.0);
  // Overshoot above target brings the integral back down.
  for (int i = 0; i < 600; i++)
    t.update(now += 1000, 21.0f, 20.0f, true);
  CHECK_NEAR(t.demand(), 0.0, 1e-3);

  // Anti-windup: long saturation at full power, then the room overshoots by 1 C: must drop promptly.
  Thermostat w;
  w.set(s);
  w.enable(0, 10.0f, 20.0f, 0.0f);
  now = 0;
  for (int i = 0; i < 3600; i++)
    w.update(now += 1000, 10.0f, 20.0f, true);
  CHECK_NEAR(w.demand(), 100.0, 1e-4);
  int steps = 0;
  while (w.update(now += 1000, 20.5f, 20.0f, true).demand > 1.0f && steps < 1000)
    steps++;
  CHECK(steps < 10);  // without anti-windup this would take over a minute

  // The integral does not run while not burning.
  Thermostat n;
  n.set(s);
  n.enable(0, 19.0f, 20.0f, 0.0f);
  now = 0;
  for (int i = 0; i < 600; i++)
    n.update(now += 1000, 19.0f, 20.0f, false);
  CHECK_NEAR(n.update(now += 1000, 19.0f, 20.0f, true).demand, 50.0, 1.0);
}

static void test_nan_room() {
  Thermostat t;
  ThermostatSettings s = plain();
  s.cycle = true;
  s.min_run_s = 0;
  s.min_off_s = 0;
  s.fallback_power = 35;
  s.integral_min = 1;
  t.set(s);
  t.enable(0, 20.0f, 20.0f, 0.0f);
  uint32_t now = 0;
  for (int i = 0; i < 100; i++) {
    ThermostatOutput o = t.update(now += 1000, NAN, 20.0f, true);
    CHECK(o.want_heat);
    CHECK_NEAR(o.demand, 35.0, 1e-4);
  }
  // Integral was frozen: when the sensor returns at the target, the output follows P alone.
  CHECK_NEAR(t.update(now += 1000, 20.0f, 20.0f, true).demand, 0.0, 1e-4);

  // A stopped heater is not restarted by a missing reading.
  t.update(now += 1000, 25.0f, 20.0f, true);
  CHECK(!t.want_heat());
  for (int i = 0; i < 50; i++)
    CHECK(!t.update(now += 1000, NAN, 20.0f, true).want_heat);

  // enable() with no reading starts heating.
  t.enable(now, NAN, 20.0f, 0.0f);
  CHECK(t.want_heat());
}

static void test_cycle() {
  Thermostat t;
  ThermostatSettings s = plain();
  s.cycle = true;
  s.stop_above = 1.5f;
  s.start_below = 1.0f;
  s.min_run_s = 600;
  s.min_off_s = 300;
  t.set(s);
  t.enable(0, 20.0f, 20.0f, 0.0f);
  uint32_t now = 0;
  // Too warm, but the minimum run time has not passed.
  for (int i = 0; i < 599; i++)
    CHECK(t.update(now += 1000, 22.0f, 20.0f, true).want_heat);
  CHECK(t.update(now += 1000, 21.4f, 20.0f, true).want_heat);  // 600 s but below the stop threshold
  ThermostatOutput o = t.update(now += 1000, 21.5f, 20.0f, true);
  CHECK(!o.want_heat);
  uint32_t stopped_at = now;
  CHECK(!t.want_heat());
  // Cold again, but the minimum off time has not passed (299 s).
  while (uint32_t(now - stopped_at) < 299000)
    CHECK(!t.update(now += 1000, 18.0f, 20.0f, true).want_heat);
  // Above the start threshold: stays off even after the time.
  CHECK(!t.update(now += 1000, 19.5f, 20.0f, true).want_heat);
  CHECK(!t.update(now += 1000, 19.5f, 20.0f, true).want_heat);
  o = t.update(now += 1000, 19.0f, 20.0f, true);
  CHECK(o.want_heat);
  CHECK_NEAR(o.demand, 50.0, 1e-3);  // P = 50 %, ramps are unlimited here
}

static void test_cycle_restart_resets() {
  Thermostat t;
  ThermostatSettings s;
  s.cycle = true;
  s.min_run_s = 0;
  s.min_off_s = 0;
  s.min_power = 10;
  s.ramp_up = 6;  // 0.1 %/s
  s.integral_min = 1;
  t.set(s);
  t.enable(0, 15.0f, 20.0f, 10.0f);
  uint32_t now = 0;
  for (int i = 0; i < 600; i++)
    t.update(now += 1000, 15.0f, 20.0f, true);
  CHECK(t.demand() > 60.0f);
  t.update(now += 1000, 22.0f, 20.0f, true);
  CHECK(!t.want_heat());
  ThermostatOutput o = t.update(now += 1000, 18.0f, 20.0f, true);
  CHECK(o.want_heat);
  CHECK_NEAR(o.demand, 10.1, 1e-3);  // restarted from min_power, then one ramp step
  // Cycle off: never stops.
  Thermostat u;
  u.set(plain());
  u.enable(0, 20.0f, 20.0f, 0.0f);
  CHECK(u.update(1000000, 40.0f, 20.0f, true).want_heat);
  // Disabled thermostat.
  u.disable();
  CHECK(!u.enabled() && !u.want_heat());
  ThermostatOutput d = u.update(2000000, 10.0f, 20.0f, true);
  CHECK(!d.want_heat && d.demand == 0);
}

static void test_enable_hot() {
  Thermostat t;
  ThermostatSettings s = plain();
  s.cycle = true;
  s.min_off_s = 300;
  s.min_run_s = 600;
  t.set(s);
  t.enable(5000, 21.5f, 20.0f, 0.0f);
  CHECK(t.enabled() && !t.want_heat());
  CHECK(!t.update(6000, 21.0f, 20.0f, true).want_heat);
  // Min-off timer already expired: starts at once when the room is cold enough.
  CHECK(!t.update(7000, 19.5f, 20.0f, true).want_heat);
  CHECK(t.update(8000, 19.0f, 20.0f, true).want_heat);
  // Just below the stop threshold: heating.
  t.enable(0, 21.4f, 20.0f, 0.0f);
  CHECK(t.want_heat());
  // Cycle off: always heating.
  s.cycle = false;
  t.set(s);
  t.enable(0, 30.0f, 20.0f, 0.0f);
  CHECK(t.want_heat());
}

// A scripted run: room falls then rises, returns the demand and want_heat sequence.
static std::vector<float> scenario(uint32_t start) {
  Thermostat t;
  ThermostatSettings s;
  s.integral_min = 3;
  s.cycle = true;
  s.min_run_s = 60;
  s.min_off_s = 30;
  s.ramp_up = 30;
  t.set(s);
  uint32_t now = start;
  t.enable(now, 18.0f, 20.0f, 5.0f);
  std::vector<float> out;
  for (int i = 0; i < 1500; i++) {
    float room = 18.0f + 5.0f * std::sin(float(i) / 120.0f);
    ThermostatOutput o = t.update(now += 1000, room, 20.0f, (i % 50) > 5);
    out.push_back(o.demand);
    out.push_back(o.want_heat ? 1.0f : 0.0f);
  }
  return out;
}

static void test_wrap() {
  std::vector<float> a = scenario(0);
  std::vector<float> b = scenario(0xFFFFF000u);
  CHECK(a.size() == b.size());
  bool same = a.size() == b.size();
  for (size_t i = 0; same && i < a.size(); i++)
    if (a[i] != b[i])
      same = false;
  CHECK(same);
  // The scenario really exercises both states.
  bool saw_off = false, saw_on = false;
  for (size_t i = 1; i < a.size(); i += 2)
    (a[i] > 0.5f ? saw_on : saw_off) = true;
  CHECK(saw_on && saw_off);
}

static void test_long_gap() {
  ThermostatSettings s;
  s.ramp_up = 60;  // 1 %/s
  Thermostat a, b;
  a.set(s);
  b.set(s);
  a.enable(0, 10.0f, 20.0f, 0.0f);
  b.enable(0, 10.0f, 20.0f, 0.0f);
  // A 10-minute gap counts as 10 s: 10 % of slew, not 100 %.
  CHECK_NEAR(a.update(600000, 10.0f, 20.0f, true).demand, 10.0, 1e-3);
  CHECK_NEAR(b.update(10000, 10.0f, 20.0f, true).demand, 10.0, 1e-3);
  // Same for the integral.
  s.integral_min = 1;
  s.ramp_up = 0;
  Thermostat c, d;
  c.set(s);
  d.set(s);
  c.enable(0, 19.0f, 20.0f, 0.0f);
  d.enable(0, 19.0f, 20.0f, 0.0f);
  CHECK_NEAR(c.update(500000, 19.0f, 20.0f, true).demand, d.update(10000, 19.0f, 20.0f, true).demand, 1e-4);
}

int main() {
  test_sanitise();
  test_proportional();
  test_limits();
  test_ramps();
  test_hold_not_burning();
  test_integral();
  test_nan_room();
  test_cycle();
  test_cycle_restart_resets();
  test_enable_hot();
  test_wrap();
  test_long_gap();
  printf("%d checks passed, %d failed\n", checks - failures, failures);
  return failures == 0 ? 0 : 1;
}
