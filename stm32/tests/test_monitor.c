/* The safety MCU's checks of itself: VDDA's window (PWR-5) with its filter and the power-up rise,
 * and the tick against the second clock (GLW-1). */
#include <stdio.h>

#include "../core/monitor.h"
#include "../../common/test/check.h"

#define HZ 250u      /* the second clock as the chip runs it: the LSI / 128 */
#define WRAP 32768u

static void setup(monitor *m, uint32_t now) {
  monitor_params p;
  monitor_params_default(&p, HZ, WRAP);
  monitor_init(m, &p, now);
}

/* ms readings of mv from t on; returns the time after them. */
static uint32_t feed(monitor *m, uint32_t t, uint32_t ms, uint16_t mv) {
  for (uint32_t i = 0; i < ms; i++)
    monitor_vdda(m, t + i, mv);
  return t + ms;
}

static void test_vdda_window(void) {
  monitor m;
  setup(&m, 0);
  /* Before any reading nothing is held (the board feeds one at once). */
  CHECK(!monitor_holding(&m));
  uint32_t t = feed(&m, 0, 100, 3300);
  CHECK(m.vdda_up && !monitor_holding(&m) && m.fault == MONITOR_OK);
  /* Inside the window at both ends: no fault. */
  t = feed(&m, t, 100, 3000);
  t = feed(&m, t, 100, 3600);
  CHECK(m.fault == MONITOR_OK);
  /* One stray conversion, and a few in a row (under 10 ms): no fault. */
  t = feed(&m, t, 1, 3700);
  t = feed(&m, t, 1, 3300);
  t = feed(&m, t, 9, 2900);
  t = feed(&m, t, 1, 3300);
  t = feed(&m, t, 9, 3650);
  t = feed(&m, t, 1, 3300);
  CHECK(m.fault == MONITOR_OK);
  /* Above 3.6 V for 10 ms: a fault, held. */
  t = feed(&m, t, 10, 3610);
  CHECK(m.fault == MONITOR_OK);
  t = feed(&m, t, 1, 3610);
  CHECK(m.fault == MONITOR_VDDA_HIGH && monitor_holding(&m));
  /* It latches: back in the window, still held until a fault reset clears it. */
  t = feed(&m, t, 100, 3300);
  CHECK(m.fault == MONITOR_VDDA_HIGH && monitor_holding(&m));
  CHECK(monitor_clear(&m) == 1);
  CHECK(m.fault == MONITOR_OK && !monitor_holding(&m));
  /* Below 3.0 V for 10 ms: a fault; a reset while it lasts does not clear it. */
  t = feed(&m, t, 11, 2990);
  CHECK(m.fault == MONITOR_VDDA_LOW && monitor_holding(&m));
  CHECK(monitor_clear(&m) == 0);
  CHECK(m.fault == MONITOR_VDDA_LOW);
  t = feed(&m, t, 1, 3300);
  CHECK(monitor_clear(&m) == 1);
  /* Mixed high and low readings: out of the window all the same. */
  for (int i = 0; i < 6; i++) {
    t = feed(&m, t, 1, 3700);
    t = feed(&m, t, 1, 2800);
  }
  CHECK(m.fault == MONITOR_VDDA_HIGH || m.fault == MONITOR_VDDA_LOW);
  /* A dead reading (0 counts reads as the highest VDDA) is out of the window too. */
  monitor_clear(&m);
  t = feed(&m, t, 1, 3300);
  CHECK(monitor_clear(&m) == 1);
  t = feed(&m, t, 11, 0xFFFFu);
  CHECK(m.fault == MONITOR_VDDA_HIGH);
}

static void test_vdda_power_up(void) {
  monitor m;
  /* A slow rise from the power-on reset: held, never latched. */
  setup(&m, 0);
  uint32_t t = 0;
  for (uint16_t mv = 1700; mv < 3300; mv += 5)
    t = feed(&m, t, 1, mv);
  CHECK(m.fault == MONITOR_OK);
  t = feed(&m, t, 50, 3300);
  CHECK(m.vdda_up && !monitor_holding(&m) && m.fault == MONITOR_OK);
  /* Held while it rises, from the first low reading. */
  setup(&m, 0);
  t = feed(&m, 0, 1, 2000);
  CHECK(monitor_holding(&m) && m.fault == MONITOR_OK);
  /* Just at 3.0 V is not up yet (the margin): no flapping at the edge. */
  t = feed(&m, t, 100, 3010);
  CHECK(!m.vdda_up && monitor_holding(&m) && m.fault == MONITOR_OK);
  t = feed(&m, t, 1, 3100);
  CHECK(m.vdda_up && !monitor_holding(&m));
  /* Up, then down again: a fault as usual. */
  t = feed(&m, t, 11, 2950);
  CHECK(m.fault == MONITOR_VDDA_LOW);

  /* A rail that never comes up: a fault once the rise time is over. */
  setup(&m, 1000);
  t = feed(&m, 1000, 499, 2500);
  CHECK(m.fault == MONITOR_OK && monitor_holding(&m));
  t = feed(&m, t, 2, 2500);
  CHECK(m.fault == MONITOR_VDDA_LOW && monitor_holding(&m));
  CHECK(monitor_clear(&m) == 0);
  t = feed(&m, t, 1, 3300);
  CHECK(monitor_clear(&m) == 1 && !monitor_holding(&m));

  /* Over-voltage while rising counts at once. */
  setup(&m, 0);
  t = feed(&m, 0, 5, 2500);
  t = feed(&m, t, 11, 3800);
  CHECK(m.fault == MONITOR_VDDA_HIGH);
}

/* The second clock's count at tick t when it runs at ratio permille of the tick's rate (1000:
 * agree), from count c0 at tick 0. */
static uint32_t clock_at(uint32_t t, uint32_t ratio, uint32_t c0) {
  return (uint32_t) ((c0 + (uint64_t) t * HZ * ratio / 1000000u) % WRAP);
}

/* Passes of 10 ms from t to end with the clock at ratio. */
static uint32_t run_clock(monitor *m, uint32_t t, uint32_t end, uint32_t ratio, uint32_t c0) {
  for (; t < end; t += 10)
    monitor_clock(m, t, clock_at(t, ratio, c0));
  return t;
}

static void test_tick(void) {
  monitor m;
  setup(&m, 0);
  feed(&m, 0, 1, 3300);
  /* Agreeing clocks for a minute, across the count's wrap (131 s): no fault. */
  uint32_t t = run_clock(&m, 0, 200000, 1000, 30000);
  CHECK(m.fault == MONITOR_OK && m.tick_checks >= 199 && !m.tick_bad);
  CHECK(m.tick_permille >= 996 && m.tick_permille <= 1004);
  /* Off by up to 19 % either way: no fault (the LSI's own spread is within it). */
  setup(&m, 0);
  t = run_clock(&m, 0, 20000, 810, 7);
  CHECK(m.fault == MONITOR_OK);
  setup(&m, 0);
  t = run_clock(&m, 0, 20000, 1190, 7);
  CHECK(m.fault == MONITOR_OK);
  /* A tick 25 % fast (the second clock reads 800 per 1000) or slow: a fault within 2 s. */
  setup(&m, 0);
  t = run_clock(&m, 0, 2010, 750, 7);
  CHECK(m.fault == MONITOR_TICK && m.tick_bad);
  CHECK(!monitor_holding(&m)); /* the burn's fault stop handles it; no hold of every output */
  CHECK(monitor_clear(&m) == 0);
  setup(&m, 0);
  t = run_clock(&m, 0, 2010, 1300, 7);
  CHECK(m.fault == MONITOR_TICK);
  /* A dead second clock (no count), and one many times too fast or too slow: faults. */
  setup(&m, 0);
  for (t = 0; t < 2010; t += 10)
    monitor_clock(&m, t, 1234);
  CHECK(m.fault == MONITOR_TICK && m.tick_permille == 0);
  setup(&m, 0);
  t = run_clock(&m, 0, 2010, 3000, 7);
  CHECK(m.fault == MONITOR_TICK);
  /* A fault clears once a check passes again (the first one after the change straddles it). */
  t = run_clock(&m, t, t + 2500, 1000, 0);
  CHECK(!m.tick_bad && monitor_clear(&m) == 1 && m.fault == MONITOR_OK);
  /* The loop stalled (a flash erase): 60 ms of the second clock with no tick in a second is 6 %. */
  setup(&m, 0);
  uint32_t real = 0;
  for (t = 0; t < 3000; t += 10) {
    real += t == 1500 ? 70u : 10u;
    monitor_clock(&m, t, real * HZ / 1000u % WRAP);
  }
  CHECK(m.fault == MONITOR_OK);
  /* A tick whose loop passes come unevenly still checks over its own elapsed time. */
  setup(&m, 0);
  for (t = 0; t < 5000; t += (t / 10) % 3 == 0 ? 30u : 10u)
    monitor_clock(&m, t, clock_at(t, 1000, 0));
  CHECK(m.fault == MONITOR_OK);
}

int main(void) {
  test_vdda_window();
  test_vdda_power_up();
  test_tick();
  return check_report("monitor");
}
