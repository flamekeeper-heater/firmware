#include "heater_model.h"

#include <string.h>

void heater_model_init(heater_model *m, double ambient_c) {
  memset(m, 0, sizeof(*m));
  m->ambient_c = ambient_c;
  m->fan_max_rpm = 6000;
  m->heat_gain = 0.0213;
  m->supply_cv = 1260;
  m->hx_c10 = ambient_c * 10;
}

void heater_model_step(heater_model *m, uint32_t dt_ms, uint16_t fan_target_rpm, uint16_t pump_cHz,
                       uint16_t glow_w) {
  double dt = dt_ms / 1000.0;

  /* Fan: follows its target with about 1.5 s of inertia, up to what the motor can do. */
  double target = m->fan_stalled ? 0 : fan_target_rpm;
  if (target > m->fan_max_rpm)
    target = m->fan_max_rpm;
  m->fan_rpm += (target - m->fan_rpm) * (dt / 1.5 > 1 ? 1 : dt / 1.5);

  /* Glow plug: about 15 s to get hot, as long to cool. */
  double g = glow_w > 0 ? 1.0 : 0.0;
  m->glow_heat += (g - m->glow_heat) * (dt / 15.0 > 1 ? 1 : dt / 15.0);
  if (glow_w > 0)
    m->glow_on_s += dt;

  /* Flame: lights once there is fuel, air and a hot plug; goes out without fuel. */
  if (pump_cHz > 0)
    m->fuel_s += dt;
  if (!m->lit && !m->no_ignition && !m->kill_flame && pump_cHz > 0 && m->fan_rpm > 500 &&
      m->glow_heat > 0.6 && m->fuel_s > 3)
    m->lit = 1;
  if (m->lit) {
    m->no_fuel_s = pump_cHz > 0 ? 0 : m->no_fuel_s + dt;
    if (m->no_fuel_s > 5 || m->kill_flame || m->fan_rpm < 300)
      m->lit = 0, m->fuel_s = 0;
  }

  /* Heat exchanger: warmed by the flame, cooled towards ambient by the airflow. */
  double amb = m->ambient_c * 10;
  double heat = m->lit ? m->heat_gain * pump_cHz : 0;
  double loss = (m->hx_c10 - amb) * (0.002 + 0.000001 * m->fan_rpm);
  m->hx_c10 += (heat - loss) * dt;
}

int16_t heater_model_hx(const heater_model *m) {
  if (m->sensor_open)
    return (int16_t) 0x8000;
  return (int16_t) (m->hx_c10 + (m->hx_c10 >= 0 ? 0.5 : -0.5));
}
