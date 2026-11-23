/* The input-current budget (PWR-1, PWR-2): each stage's share, the glow's power limit, the fan's
 * allowance, and the 10 s estimate held at the budget by the glow limit alone. */
#include <stdio.h>

#include "../core/budget.h"
#include "../../common/test/check.h"

static void setup(budget *b) {
  budget_params p;
  budget_params_default(&p);
  budget_init(b, &p, 0);
}

static void test_shares(void) {
  budget b;
  setup(&b);
  /* The logic: 7.5 W at the supply. */
  CHECK_EQ(budget_logic_ma(&b, 1200), 625);
  CHECK_EQ(budget_logic_ma(&b, 900), 833);
  CHECK_EQ(budget_logic_ma(&b, 2400), 312);
  CHECK_EQ(budget_logic_ma(&b, 0), 0);   /* no supply (USB only): no input current */
  CHECK_EQ(budget_logic_ma(&b, 500), 0);
  /* The pump: 1 A strokes of 38 ms at 6 Hz average about 230 mA over a second. */
  for (uint32_t t = 0; t < 5000; t++)
    budget_pump_ms(&b, t % 167 < 38 ? 1000 : 0);
  CHECK(budget_pump_ma(&b) >= 200 && budget_pump_ma(&b) <= 260);
  /* ... and it forgets in a few seconds. */
  for (uint32_t t = 0; t < 8000; t++)
    budget_pump_ms(&b, 0);
  CHECK(budget_pump_ma(&b) <= 5);
}

static void test_glow_limit(void) {
  budget b;
  setup(&b);
  /* At 9 V with the fan at 2.8 A: 11 A less 0.2 headroom, 2.8 fan, 0.83 logic = 7.17 A, 64 W. */
  budget_pass(&b, 10, 2800, 900);
  CHECK_EQ(budget_glow_limit_w(&b, 900), 64);
  /* At 12 V with the fan at 1 A: 9.18 A, 110 W (the planner's own 90 W binds then). */
  budget_pass(&b, 20, 1000, 1200);
  CHECK_EQ(budget_glow_limit_w(&b, 1200), 110);
  /* The pump's share comes off too. */
  for (uint32_t t = 0; t < 5000; t++)
    budget_pump_ms(&b, 1500);
  CHECK(budget_glow_limit_w(&b, 1200) >= 91 && budget_glow_limit_w(&b, 1200) <= 93);
  /* Nothing left, or less than a useful glow: the plug off. */
  budget_pass(&b, 30, 9000, 900);
  CHECK_EQ(budget_glow_limit_w(&b, 900), 0);
  budget_pass(&b, 40, 8500, 900); /* 0.97 A of room x 9 V = 8.7 W, under 10 W */
  CHECK_EQ(budget_glow_limit_w(&b, 900), 0);
  /* The fan's allowance with the glow off: the budget less the pump, logic and headroom. */
  setup(&b);
  budget_pass(&b, 10, 2800, 1200);
  CHECK_EQ(budget_fan_allowed_ma(&b), 11000 - 200 - 625);
  budget_pass(&b, 20, 2800, 0);
  CHECK_EQ(budget_fan_allowed_ma(&b), 11000 - 200);
}

static void test_estimate(void) {
  budget b;
  setup(&b);
  /* Fan 2 A at 12 V, and a 50 A pulse of 1 ms every 5 ms (10 A): 12.6 A over 10 s. */
  for (uint32_t t = 10; t <= 20000; t += 10) {
    budget_glow_pulse(&b, 5000, 1000);
    budget_glow_pulse(&b, 5000, 1000);
    budget_pass(&b, t, 2000, 1200);
  }
  CHECK(budget_avg_ma(&b) >= 12550 && budget_avg_ma(&b) <= 12650);
  /* The glow stops: the average falls over the next 10 s, not at once. */
  for (uint32_t t = 20010; t <= 25000; t += 10)
    budget_pass(&b, t, 2000, 1200);
  CHECK(budget_avg_ma(&b) >= 7500 && budget_avg_ma(&b) <= 7700);
  for (uint32_t t = 25010; t <= 31000; t += 10)
    budget_pass(&b, t, 2000, 1200);
  CHECK(budget_avg_ma(&b) >= 2600 && budget_avg_ma(&b) <= 2650);
  /* A pause in the passes (a long flash erase) does not lose the time. */
  budget_pass(&b, 31500, 2000, 1200);
  for (uint32_t t = 31510; t <= 45000; t += 10)
    budget_pass(&b, t, 2000, 1200);
  CHECK(budget_avg_ma(&b) >= 2600 && budget_avg_ma(&b) <= 2650);
}

/* The glow held at its limit, as the planner would pulse it, while the fan and the pump rise: the
 * estimate stays at or below 11 A, and the limit binds (the budget is used, not wasted). */
static void test_closed_loop(void) {
  budget b;
  setup(&b);
  const uint16_t vin = 950, plug_ca = 4300; /* a warm 12 V plug at 9.5 V: 43 A */
  uint16_t max_avg = 0;
  for (uint32_t t = 10; t <= 60000; t += 10) {
    uint16_t fan = (uint16_t) (t < 20000 ? 500 : t < 40000 ? 500 + (t - 20000) / 8 : 3000);
    for (uint32_t ms = 0; ms < 10; ms++)
      budget_pump_ms(&b, t > 30000 && (t + ms) % 182 < 38 ? 1200 : 0);
    for (int period = 0; period < 2; period++) {
      uint32_t p = budget_glow_limit_w(&b, vin);
      if (p > 90)
        p = 90;
      uint32_t width = p * 100000u / ((uint32_t) vin * plug_ca / 100u) * 5000u / 1000u; /* duty x 5 ms */
      budget_glow_pulse(&b, plug_ca, (uint16_t) width);
    }
    budget_pass(&b, t, fan, vin);
    if (t > 10000 && budget_avg_ma(&b) > max_avg)
      max_avg = budget_avg_ma(&b);
  }
  CHECK(max_avg <= 11000);
  CHECK(max_avg >= 10600);
}

int main(void) {
  test_shares();
  test_glow_limit();
  test_estimate();
  test_closed_loop();
  return check_report("budget");
}
