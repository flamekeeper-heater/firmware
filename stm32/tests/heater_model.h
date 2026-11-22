/* A simple diesel air heater for the host tests: fan inertia, a glow plug that heats up, a flame
 * that lights from hot glow plug, fuel and air, and a heat exchanger that warms with the flame and
 * cools with the airflow. Rough on purpose: it is there to run whole cycles, not to tune them. */
#ifndef FLAMEKEEPER_HEATER_MODEL_H
#define FLAMEKEEPER_HEATER_MODEL_H

#include <stdint.h>

typedef struct {
  /* Set by the test. */
  double ambient_c;
  double fan_max_rpm;   /* the motor's top speed (lower it to make a weak or stuck fan) */
  int fan_stalled;      /* 1: the fan does not turn at all */
  int no_ignition;      /* 1: the flame never lights */
  int kill_flame;       /* 1: the flame goes out (and stays out) */
  double heat_gain;     /* 0.1 C per s per 0.01 Hz of fuel while lit */
  int sensor_open;      /* 1: the heat-exchanger sensor reads open */
  uint16_t supply_cv;

  /* State. */
  double hx_c10;        /* heat exchanger, 0.1 C */
  double fan_rpm;
  double glow_heat;     /* 0 cold .. 1 hot */
  double fuel_s;        /* seconds of fuel delivered since the flame was last out */
  int lit;
  double no_fuel_s;     /* seconds with the pump stopped while lit */
  double glow_on_s;     /* total glow on-time */
} heater_model;

void heater_model_init(heater_model *m, double ambient_c);
/* Advance by dt_ms with the stages driven as given. */
void heater_model_step(heater_model *m, uint32_t dt_ms, uint16_t fan_target_rpm, uint16_t pump_cHz,
                       uint16_t glow_w);

int16_t heater_model_hx(const heater_model *m); /* what the sensor reads (0x8000 when open) */

#endif
