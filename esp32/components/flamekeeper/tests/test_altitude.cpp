// Host unit test for the altitude fuel trim. Build and run with tests/run.sh.
#include "../altitude_trim.h"

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

static AltitudeSettings on() {
  AltitudeSettings s;
  s.enabled = true;
  s.smoothing_s = 0;
  return s;
}

static void test_sanitise() {
  AltitudeTrim a;
  AltitudeSettings s;
  s.pct_per_1000m = 99;
  s.max_reduction = -4;
  s.max_increase = 80;
  s.smoothing_s = 99999;
  s.deadband_pct = 0.0f;
  a.set(s);
  CHECK(a.settings().pct_per_1000m == 30);
  CHECK(a.settings().max_reduction == 0);
  CHECK(a.settings().max_increase == 30);
  CHECK(a.settings().smoothing_s == 3600);
  CHECK(a.settings().deadband_pct == 0.5f);
  s.smoothing_s = -1;
  s.deadband_pct = 50;
  a.set(s);
  CHECK(a.settings().smoothing_s == 0);
  CHECK(a.settings().deadband_pct == 5);
}

static void test_invalid_samples() {
  AltitudeTrim a;
  a.set(on());
  CHECK(!a.valid());
  CHECK(std::isnan(a.smoothed_m()));
  a.sample(0, NAN);
  a.sample(0, -501.0f);
  a.sample(0, 9001.0f);
  CHECK(!a.valid());
  a.sample(1000, 1000.0f);
  CHECK(a.valid());
  a.sample(2000, NAN);
  a.sample(3000, 12000.0f);
  CHECK_NEAR(a.smoothed_m(), 1000.0, 1e-3);
  a.sample(4000, -500.0f);  // limits are inclusive
  CHECK_NEAR(a.smoothed_m(), -500.0, 1e-3);
}

static void test_filter() {
  AltitudeTrim a;
  AltitudeSettings s = on();
  s.smoothing_s = 120;
  a.set(s);
  uint32_t now = 0;
  a.sample(now, 0.0f);
  CHECK_NEAR(a.smoothed_m(), 0.0, 1e-6);  // first sample initialises directly
  for (int i = 0; i < 120; i++)
    a.sample(now += 1000, 1000.0f);
  CHECK_NEAR(a.smoothed_m(), 632.0, 5.0);  // one tau: 63 %
  for (int i = 0; i < 360; i++)
    a.sample(now += 1000, 1000.0f);
  CHECK_NEAR(a.smoothed_m(), 1000.0, 25.0);  // four tau: 98 %

  // A very long gap is capped at 10 tau (alpha ~ 1), never overshoots.
  AltitudeTrim b;
  b.set(s);
  b.sample(0, 0.0f);
  b.sample(100000000u, 500.0f);
  CHECK_NEAR(b.smoothed_m(), 500.0, 0.1);
}

static void test_no_smoothing() {
  AltitudeTrim a;
  a.set(on());
  a.sample(0, 100.0f);
  a.sample(1000, 900.0f);
  CHECK_NEAR(a.smoothed_m(), 900.0, 1e-3);
}

static void test_trim_scaling() {
  AltitudeTrim a;
  AltitudeSettings s = on();
  a.set(s);
  a.sample(0, 2000.0f);
  CHECK_NEAR(a.ideal_trim(), -20.0, 1e-4);
  CHECK(a.trim() == -20);
  s.pct_per_1000m = 8;
  a.set(s);
  CHECK_NEAR(a.ideal_trim(), -16.0, 1e-4);  // counted from sea level
  CHECK(a.trim() == -16);
}

static void test_clamps() {
  AltitudeTrim a;
  AltitudeSettings s = on();
  s.max_reduction = 15;
  s.max_increase = 5;
  a.set(s);
  a.sample(0, 5000.0f);  // wants -50
  CHECK(a.trim() == -15);
  a.sample(1000, -400.0f);  // wants +4: within the clamp
  CHECK_NEAR(a.ideal_trim(), 4.0, 1e-4);
  a.sample(2000, -500.0f);  // wants +5
  CHECK_NEAR(a.ideal_trim(), 5.0, 1e-4);
  CHECK(a.trim() == 5);
  // Default max_increase 0: never adds fuel.
  s.max_increase = 0;
  a.set(s);
  CHECK_NEAR(a.ideal_trim(), 0.0, 1e-6);
  CHECK(a.trim() == 0);
}

static void test_deadband() {
  AltitudeTrim a;
  AltitudeSettings s = on();
  s.pct_per_1000m = 10;
  s.deadband_pct = 1.0f;
  a.set(s);
  a.sample(0, 0.0f);
  CHECK(a.trim() == 0);
  // Ideal -0.5 -> -0.9 oscillation around the .5 rounding boundary: never changes from 0.
  for (int i = 0; i < 40; i++) {
    a.sample(1000 * i, 50.0f + ((i & 1) ? 40.0f : -40.0f));  // ideal -0.1 .. -0.9
    CHECK(a.trim() == 0);
  }
  a.sample(50000, 150.0f);  // ideal -1.5: moves, rounded (away from zero at .5)
  CHECK(a.trim() == -2);
  // Wobble +-0.4 around the -1.5 boundary now: held at -2 (distance 0.1..0.9 < 1).
  for (int i = 0; i < 40; i++) {
    a.sample(60000 + 1000 * i, 150.0f + ((i & 1) ? 40.0f : -40.0f));
    CHECK(a.trim() == -2);
  }
  a.sample(200000, 300.0f);  // ideal -3: 1 away from -2 -> changes
  CHECK(a.trim() == -3);
}

static void test_disabled() {
  AltitudeTrim a;
  AltitudeSettings s = on();
  a.set(s);
  CHECK(a.trim() == 0);  // not valid yet
  a.sample(0, 2000.0f);
  CHECK(a.trim() == -20);
  s.enabled = false;
  a.set(s);
  CHECK(a.ideal_trim() == 0.0f);
  CHECK(a.trim() == 0);
  s.enabled = true;
  a.set(s);
  CHECK(a.trim() == -20);  // held value was reset while disabled, recomputed fresh
}

static std::vector<float> run(uint32_t start) {
  AltitudeTrim a;
  AltitudeSettings s = on();
  s.smoothing_s = 60;
  a.set(s);
  std::vector<float> out;
  uint32_t now = start;
  for (int i = 0; i < 600; i++) {
    a.sample(now += 1000, 1500.0f + (i < 300 ? 0.0f : 800.0f));
    out.push_back(a.smoothed_m());
    out.push_back(float(a.trim()));
  }
  return out;
}

static void test_wrap() {
  std::vector<float> x = run(0), y = run(0xFFFFF000u);
  bool same = x.size() == y.size();
  for (size_t i = 0; same && i < x.size(); i++)
    if (x[i] != y[i])
      same = false;
  CHECK(same);
  CHECK(x.back() < -15.0f && x.back() > -25.0f);  // trim moved to ~-23 after the step
}

int main() {
  test_sanitise();
  test_invalid_samples();
  test_filter();
  test_no_smoothing();
  test_trim_scaling();
  test_clamps();
  test_deadband();
  test_disabled();
  test_wrap();
  printf("%d checks passed, %d failed\n", checks - failures, failures);
  return failures == 0 ? 0 : 1;
}
