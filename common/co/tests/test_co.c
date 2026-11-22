/* The CO alarm (co_alarm.c): warm-up, heater check, learning R0, the two alarm levels with their
 * time filters, latching and reset, the open-sensor fault, a reading at the rail, the cold flag. */
#include <stdio.h>
#include <string.h>

#include "../co_alarm.h"
#include "../../test/check.h"

#define SEC 1000u
#define MIN (60u * SEC)

/* The ADC count the divider gives for a sensing-layer resistance. */
static uint16_t adc_for(uint32_t rs) {
  return (uint16_t) ((uint64_t) CO_ADC_FULL * CO_LOAD_OHM / ((uint64_t) CO_LOAD_OHM + rs));
}

/* A sensor in air: clean at 300 k (heated), cooler (heater off) reads 5 x higher. */
typedef struct {
  co_alarm c;
  uint32_t now;
  uint32_t r0;
  uint32_t ratio_x1000; /* the air: Rs / R0 */
  int16_t temp;
  int stuck_adc;        /* -1: follows the air */
} sim;

static void sim_init(sim *s, int fitted) {
  co_config cfg;
  co_default_config(&cfg);
  cfg.fitted = (uint8_t) fitted;
  co_init(&s->c, &cfg);
  s->now = 1000;
  s->r0 = 300000;
  s->ratio_x1000 = 1000;
  s->temp = 200;
  s->stuck_adc = -1;
}

static void run(sim *s, uint32_t ms) {
  uint32_t end = s->now + ms;
  while ((int32_t) (end - s->now) > 0) {
    uint32_t rs = (uint32_t) ((uint64_t) s->r0 * s->ratio_x1000 / 1000u);
    if (!co_heater_on(&s->c))
      rs *= 5; /* the layer cools */
    co_step(&s->c, s->now, s->stuck_adc >= 0 ? (uint16_t) s->stuck_adc : adc_for(rs), s->temp);
    s->now += SEC;
  }
}

static void test_not_fitted(void) {
  sim s;
  sim_init(&s, 0);
  run(&s, 5 * MIN);
  CHECK_EQ(s.c.state, CO_NOT_FITTED);
  CHECK_EQ(co_heater_on(&s.c), 0);
}

static void test_start_up(void) {
  sim s;
  sim_init(&s, 1);
  run(&s, 2 * MIN);
  CHECK_EQ(s.c.state, CO_WARMING_UP);
  CHECK_EQ(co_heater_on(&s.c), 1);
  run(&s, 61 * SEC);                 /* warm-up over: the heater check switches it off */
  CHECK_EQ(co_heater_on(&s.c), 0);
  run(&s, 6 * SEC);
  CHECK_EQ(co_heater_on(&s.c), 1);
  CHECK(s.c.checked);
  run(&s, 2 * SEC);
  CHECK_EQ(s.c.state, CO_LEARNING);
  run(&s, 10 * MIN);
  CHECK_EQ(s.c.state, CO_OK);
  CHECK_EQ(s.c.r0, co_rs_ohm(adc_for(300000)));
  CHECK_EQ(s.c.ratio, 1000);
  CHECK_EQ(s.c.ppm, CO_UNKNOWN_PPM); /* clean air: nothing on the curve */
}

static void ok_sensor(sim *s) {
  sim_init(s, 1);
  run(s, 14 * MIN);
  CHECK_EQ(s->c.state, CO_OK);
}

static void test_alarm_levels(void) {
  sim s;
  ok_sensor(&s);
  /* About 400 ppm for 50 s: no alarm; 60 s: alarm, latched. */
  s.ratio_x1000 = 25;
  run(&s, 50 * SEC);
  CHECK_EQ(s.c.state, CO_OK);
  CHECK(s.c.ppm > 300 && s.c.ppm < 500);
  s.ratio_x1000 = 1000;
  run(&s, 5 * SEC);
  s.ratio_x1000 = 25;
  run(&s, 61 * SEC);
  CHECK_EQ(s.c.state, CO_ALARM);
  CHECK(s.c.alarm);
  /* Reset refused while it is still there, and until the air has been clean for a minute. */
  CHECK_EQ(co_reset(&s.c, s.now), 0);
  s.ratio_x1000 = 1000;
  run(&s, 30 * SEC);
  CHECK_EQ(co_reset(&s.c, s.now), 0);
  CHECK_EQ(s.c.state, CO_ALARM);
  run(&s, 31 * SEC);
  CHECK_EQ(co_reset(&s.c, s.now), 1);
  run(&s, 2 * SEC);
  CHECK_EQ(s.c.state, CO_OK);
  /* About 1200 ppm: alarm after 10 s. */
  s.ratio_x1000 = 8;
  run(&s, 9 * SEC);
  CHECK_EQ(s.c.state, CO_OK);
  run(&s, 2 * SEC);
  CHECK_EQ(s.c.state, CO_ALARM);
}

static void test_rail_and_open(void) {
  sim s;
  ok_sensor(&s);
  /* Shorted sensor / open load: the rail reads as very high CO: the alarm. */
  s.stuck_adc = CO_ADC_FULL;
  run(&s, 11 * SEC);
  CHECK_EQ(s.c.state, CO_ALARM);

  ok_sensor(&s);
  /* Open sensor or heater off: below 0.5 % of full scale for 5 s: a sensor fault. */
  s.stuck_adc = 10;
  run(&s, 4 * SEC);
  CHECK(!s.c.fault);
  run(&s, 2 * SEC);
  CHECK(s.c.fault);
  CHECK_EQ(s.c.state, CO_SENSOR_FAULT);
  CHECK_EQ(co_reset(&s.c, s.now), 0); /* still open */
  s.stuck_adc = -1;
  run(&s, 2 * SEC);
  CHECK_EQ(s.c.state, CO_SENSOR_FAULT); /* latched */
  CHECK_EQ(co_reset(&s.c, s.now), 1);
  run(&s, 2 * SEC);
  CHECK_EQ(s.c.state, CO_OK);
}

static void test_heater_check_fails(void) {
  sim s;
  sim_init(&s, 1);
  s.stuck_adc = adc_for(300000); /* the reading does not move with the heater: broken heater */
  run(&s, 3 * MIN + 10 * SEC);
  CHECK(s.c.fault);
  CHECK_EQ(s.c.state, CO_SENSOR_FAULT);
  run(&s, 30 * SEC);
  CHECK_EQ(s.c.state, CO_SENSOR_FAULT);
  /* Repaired: a reset checks again. */
  s.stuck_adc = -1;
  CHECK_EQ(co_reset(&s.c, s.now), 1);
  run(&s, 8 * SEC);
  CHECK(s.c.checked);
  CHECK(!s.c.fault);
}

static void test_cold_and_r0(void) {
  sim s;
  ok_sensor(&s);
  s.temp = -350;
  run(&s, 2 * SEC);
  CHECK_EQ(s.c.state, CO_TOO_COLD);
  s.ratio_x1000 = 8; /* still alarms when cold: the safe side */
  run(&s, 11 * SEC);
  CHECK_EQ(s.c.state, CO_ALARM);

  /* R0 from flash: no learning time. A high spike ages out slowly. */
  co_config cfg;
  co_default_config(&cfg);
  co_init(&s.c, &cfg);
  co_load_r0(&s.c, co_rs_ohm(adc_for(300000)));
  s.temp = 200;
  s.ratio_x1000 = 1000;
  s.now = 5000;
  run(&s, 3 * MIN + 10 * SEC);
  CHECK_EQ(s.c.state, CO_OK);
  uint32_t r0 = s.c.r0;
  s.ratio_x1000 = 3000; /* a dry, cold gust: higher than clean air */
  run(&s, 2 * SEC);
  CHECK(s.c.r0 > r0 * 2);
  s.ratio_x1000 = 1000;
  run(&s, 120 * MIN);
  CHECK(s.c.r0 < r0 * 3); /* coming down, 1/64 per 10 min */
  CHECK(s.c.r0 > r0);
}

static void test_curve(void) {
  CHECK_EQ(co_ppm_from_ratio(30), 300);
  CHECK_EQ(co_ppm_from_ratio(10), 1000);
  CHECK_EQ(co_ppm_from_ratio(3), 2140);
  CHECK_EQ(co_ppm_from_ratio(400), CO_UNKNOWN_PPM);
  CHECK(co_ppm_from_ratio(20) > co_ppm_from_ratio(25));
  CHECK_EQ(co_rs_ohm(0), UINT32_MAX);
  CHECK_EQ(co_rs_ohm(CO_ADC_FULL), 0);
  CHECK(co_rs_ohm(adc_for(1000000)) > 990000 && co_rs_ohm(adc_for(1000000)) < 1010000);
}

static void test_set_fitted(void) {
  /* A learnt sensor set "not fitted": no heater, no state, its alarm dropped; fitted again: a new
   * warm-up and heater check, with the R0 it had learnt (no learning again). */
  sim s;
  ok_sensor(&s);
  uint32_t r0 = s.c.r0;
  co_set_fitted(&s.c, 1); /* unchanged: nothing happens */
  CHECK_EQ(s.c.state, CO_OK);
  s.c.alarm = 1;
  co_set_fitted(&s.c, 0);
  CHECK_EQ(s.c.state, CO_NOT_FITTED);
  CHECK_EQ(co_heater_on(&s.c), 0);
  CHECK(!s.c.alarm && !s.c.cfg.fitted);
  run(&s, MIN);
  CHECK_EQ(s.c.state, CO_NOT_FITTED);
  co_set_fitted(&s.c, 1);
  CHECK_EQ(s.c.state, CO_WARMING_UP);
  CHECK_EQ(co_heater_on(&s.c), 1);
  CHECK_EQ(s.c.r0, r0);
  CHECK_EQ(s.c.cfg.warmup_ms, 3u * MIN); /* the configuration is kept */
  run(&s, 3 * MIN + 10 * SEC);
  CHECK(s.c.checked);
  run(&s, 2 * SEC);
  CHECK_EQ(s.c.state, CO_OK);
}

int main(void) {
  test_not_fitted();
  test_start_up();
  test_alarm_levels();
  test_rail_and_open();
  test_heater_check_fails();
  test_cold_and_r0();
  test_curve();
  test_set_fitted();
  return check_report("co");
}
