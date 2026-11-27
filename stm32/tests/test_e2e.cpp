// End to end: the ESP32's real link client (esp32/components/flamekeeper/link_client.*) talks over
// a byte wire to this safety core, which runs the heater model on a healthy board (core_rig.h). A
// whole heat cycle, the dead-man when the ESP32 stops talking, an MCU reset in the middle of a run,
// the CO alarm, and the input-current budget's glow derate.
#include <cstdio>
#include <cstring>

#include "../../common/test/check.h"
#include "core_rig.h"

using esphome::flamekeeper::Mode;

// The heater bench (core_rig.h), healthy, with the ESP32's link client polling over the wire.
struct Bench : HeaterBench<Bench> {
  uint32_t stroke_ms_total = 0;
  uint32_t stroke_ms_unguarded = 0;  // PUMP_CMD driven without the overcurrent watchdog armed
  int max_heartbeat_gap_in_burn = 0;
  uint16_t fan_extra_ma = 0;  // more fan current than the model's (a stiffer fan)

  Bench() { mcu_reset(LINK_RESET_POWER_ON); }
  void ms_out(safety_io io, uint8_t step_before) {
    HeaterBench::ms_out(io, step_before);
    if (io.pump_duty)
      stroke_ms_total++;
    if (io.pump_duty && !io.pump_oc_armed)
      stroke_ms_unguarded++;
    // While fuel may flow the heartbeat must run (the pump's high side needs it).
    if (s.burn.out.pump_cHz > 0 && io.heartbeat != CHECKS_HB_RUNNING)
      max_heartbeat_gap_in_burn++;
  }
  fan_meas fan_meas_now(const burn_inputs &in) {
    fan_meas fm = HeaterBench::fan_meas_now(in);
    if (m.fan_rpm >= 1000)
      fm.isense_ma = uint16_t(fm.isense_ma + fan_extra_ma);
    return fm;
  }
  bool run_until_state(uint8_t state, uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += TICK) {
      if (client.up() && client.raw_status().state == state)
        return true;
      pass();
    }
    return client.up() && client.raw_status().state == state;
  }
};

static void test_glow_waits_after_reset() {
  // After power-on a heat start goes ahead at once, but the fan runs 30 s longer before the first
  // glow (GLW-21): the glow switches cool from a run the reset may have cut short.
  Bench b;
  b.run(2 * SEC);
  b.client.set_power_demand(60);
  CHECK(b.client.set_mode(Mode::HEAT).accepted());
  CHECK(b.run_until_state(BURN_STATE_STARTING, 2 * SEC));
  uint32_t asked = b.now, first_glow = 0, air_from = 0;
  while (b.now - asked < 2 * MIN && first_glow == 0) {
    b.pass();
    if (air_from == 0 && b.s.in.fan_rpm >= b.s.burn.p.glow_min_fan_rpm)
      air_from = b.now;
    if (b.s.burn.out.glow_w > 0)
      first_glow = b.now;
  }
  CHECK(air_from > 0 && first_glow > 0);
  CHECK(first_glow - air_from >= 30 * SEC);
  CHECK(first_glow - air_from < 31 * SEC);
}

// PWR-1, PWR-2: on a low supply with a fan drawing 4 A the glow is held to what the 11 A budget
// leaves (11 A less 0.2 A headroom, the fan, the pump and 7.5 W of logic), and the app sees the
// derated power; the run still lights.
static void test_glow_derated_by_budget() {
  Bench b;
  b.m.supply_cv = 1150;
  b.fan_extra_ma = 4000;
  b.run(2 * SEC);
  b.s.burn.reset_fan_due = 0;
  b.client.set_power_demand(60);
  CHECK(b.client.set_mode(Mode::HEAT).accepted());
  uint32_t asked = b.now;
  while (b.now - asked < 60 * SEC && b.s.burn.step != BURN_STEP_PREHEAT)
    b.pass();
  b.run(10 * SEC);
  CHECK(b.s.burn.step == BURN_STEP_PREHEAT && b.s.burn.out.glow_w == 90);
  // 11 - 0.2 - 4.0 (and the model's few mA) - 0.65 = 6.1 A at 11.5 V: about 70 W.
  CHECK(b.s.glow_limit_w >= 66 && b.s.glow_limit_w <= 71);
  CHECK(b.client.raw_status().glow_w == b.s.glow_limit_w);
  b.run(8 * SEC); /* a whole 10 s of preheat in the estimate's window: held at the budget */
  CHECK(budget_avg_ma(&b.s.budget) <= 11000 && budget_avg_ma(&b.s.budget) >= 10600);
  CHECK(b.run_until_state(BURN_STATE_HEATING, 5 * MIN));
  CHECK(b.client.raw_status().fault == 0);
}

static void test_heat_cycle() {
  Bench b;
  b.run(125 * SEC);
  CHECK(b.client.up());
  CHECK(b.client.info().kind == LINK_KIND_STM32);
  CHECK(b.client.raw_status().state == BURN_STATE_OFF);
  b.client.set_power_demand(60);
  CHECK(b.client.set_mode(Mode::HEAT).accepted());
  CHECK(b.run_until_state(BURN_STATE_STARTING, 2 * SEC));
  CHECK(b.run_until_state(BURN_STATE_HEATING, 5 * MIN));
  b.run(2 * MIN);
  CHECK(b.client.raw_status().power == 60);
  CHECK(b.client.raw_status().fault == 0);
  CHECK(b.m.lit);
  CHECK(b.client.set_mode(Mode::OFF).accepted());
  CHECK(b.run_until_state(BURN_STATE_STOPPING, 2 * SEC));
  CHECK(b.run_until_state(BURN_STATE_COOLING, 1 * MIN));
  CHECK(b.run_until_state(BURN_STATE_OFF, 20 * MIN));
  CHECK(b.client.raw_status().run_time_s > 120);
  b.run(1 * MIN);
  CHECK(b.client.raw_status().state == BURN_STATE_OFF);  // and it stays off
  CHECK(b.s.fan.fault == FAN_FAULT_NONE);  // no false fan fault through spin-up, changes, coasting
  CHECK(b.s.power_up_failed == 0);
  CHECK(b.latch_armed);
  CHECK(b.max_heartbeat_gap_in_burn == 0);
  CHECK(b.s.pump.fault == PUMP_FAULT_NONE);
  // Started two minutes after power-on: not cold, nothing remembered, a 12 V supply: a warning.
  CHECK(b.s.plug_judged && b.s.plug_refused == 0 && b.s.plug_warning);
  esphome::flamekeeper::HeaterStatus hs;
  b.client.read_status(hs);
  CHECK(hs.plug_warning);  // the flag reached the ESP32
  CHECK(b.s.pump.strokes > 100);  // real strokes through the run
  CHECK(b.stroke_ms_total == b.s.pump.strokes * 38);
  CHECK(b.stroke_ms_unguarded == 0);  // PMP-7: armed for every stroke
  CHECK(b.s.glow.fault == GLOW_FAULT_NONE);
}

static void test_dead_man() {
  Bench b;
  b.run(125 * SEC);
  b.client.set_power_demand(50);
  CHECK(b.client.set_mode(Mode::HEAT).accepted());
  CHECK(b.run_until_state(BURN_STATE_HEATING, 5 * MIN));
  // The ESP32 hangs: no more polls. Within about 2 s the MCU stops the burn by itself.
  b.esp_alive = false;
  b.run(1900);
  CHECK(b.s.burn.step == BURN_STEP_RUNNING);
  b.run(300);
  CHECK(b.s.burn.step == BURN_STEP_BURN_OFF);
  CHECK(safety_outputs(&b.s)->pump_cHz == 0);
  // The ESP32 comes back still asking for heat with the same start number: no restart.
  b.esp_alive = true;
  CHECK(b.run_until_state(BURN_STATE_OFF, 25 * MIN));
  CHECK(b.client.raw_status().flags & LINK_STATUS_DEADMAN_STOP);
  b.run(1 * MIN);
  CHECK(b.client.raw_status().state == BURN_STATE_OFF);
  CHECK(!b.m.lit);
  // The stop is in the MCU's fault log, and the ESP32 has read it.
  esphome::flamekeeper::HeaterLogEntry e[4];
  CHECK_EQ(b.client.fault_log(0, e, 4), 1);
  CHECK(e[0].kind == LINK_LOG_FAULT && e[0].code == 0 && e[0].why == BURN_WHY_DEADMAN);
  CHECK(e[0].step == BURN_STEP_RUNNING && e[0].hx_c10 > 300 && e[0].uptime_s > 125);
}

static void test_mcu_reset_mid_run() {
  Bench b;
  b.run(125 * SEC);
  b.client.set_power_demand(100);
  CHECK(b.client.set_mode(Mode::HEAT).accepted());
  CHECK(b.run_until_state(BURN_STATE_HEATING, 5 * MIN));
  b.run(5 * MIN);
  // The safety MCU resets (watchdog): hot heat exchanger, the ESP32 still asking for heat.
  b.mcu_reset(LINK_RESET_WATCHDOG);
  safety_log_reset(&b.s);  // as board_init does once the saved log is loaded (none here)
  CHECK(b.s.burn.step == BURN_STEP_COOLDOWN);
  CHECK(b.run_until_state(BURN_STATE_COOLING, 5 * SEC));
  CHECK(b.run_until_state(BURN_STATE_OFF, 25 * MIN));
  b.run(1 * MIN);
  CHECK(b.client.raw_status().state == BURN_STATE_OFF);  // an MCU reset never starts the heater
  // The reset is in the MCU's fault log, with the cooldown it started; the ESP32 has read it.
  esphome::flamekeeper::HeaterLogEntry e[4];
  CHECK_EQ(b.client.fault_log(0, e, 4), 1);
  CHECK(e[0].kind == LINK_LOG_RESET && e[0].code == LINK_RESET_WATCHDOG && e[0].step == BURN_STEP_COOLDOWN);
  CHECK(e[0].hx_c10 > 1000);
}

static void test_parameters() {
  Bench b;
  b.run(10 * SEC);  // the client describes the table, then fetches each help text
  CHECK(b.client.param_count() == PARAM_COUNT);
  esphome::flamekeeper::ParamInfo info;
  CHECK(b.client.param_info(0, info));
  CHECK(std::strcmp(info.name, "Pump min") == 0);
  CHECK(info.id == 0x01 && info.decimals == 2 && info.max == 400);
  CHECK(std::strlen(b.client.param_help(0x11)) > 10);
  esphome::flamekeeper::ParamValue v;
  CHECK(b.client.param_get(0x11, v) && v.known && v.value == 4500);

  // Staged, then applied with the heater off: the sequencer runs with it.
  CHECK(b.client.param_set(0x11, 5000).accepted());
  b.run(1 * SEC);
  CHECK(b.s.burn.p.fan_max_rpm == 4500);
  CHECK(b.client.raw_status().flags & LINK_STATUS_PARAMS_STAGED);
  CHECK(b.client.param_apply().accepted());
  b.run(1 * SEC);
  CHECK(b.s.burn.p.fan_max_rpm == 5000);
  CHECK(safety_params_save_pending(&b.s));
  uint8_t img[PARAMS_IMAGE_MAX];
  size_t n = params_pack(&b.s.params, img, sizeof(img));
  safety_params_saved(&b.s);
  CHECK(n > 0);

  // Out of range from the app: refused by the board.
  b.client.param_set(0x02, 900);  // 9 Hz
  b.run(1 * SEC);
  CHECK(b.s.params.staged_mask == 0);

  // While the heater runs, an apply is refused and the change stays staged.
  b.run(120 * SEC);  // the glow budget after power-on
  b.client.set_power_demand(50);
  b.client.set_mode(Mode::HEAT);
  CHECK(b.run_until_state(BURN_STATE_STARTING, 2 * SEC));
  b.client.param_set(0x10, 2000);
  b.run(500);
  b.client.param_apply();
  b.run(1 * SEC);
  CHECK(b.s.burn.p.fan_min_rpm == 1800);
  CHECK(b.s.params.staged_mask != 0);

  // After a reset the saved image comes back.
  Bench c;
  safety_load_params(&c.s, img, n);
  CHECK(c.s.burn.p.fan_max_rpm == 5000);
}

static void test_plug_cold_check() {
  // Eleven minutes after power-on with a cool heater: the first probe verifies the 12 V plug.
  Bench b;
  b.run(11 * MIN);
  b.client.set_power_demand(50);
  CHECK(b.client.set_mode(Mode::HEAT).accepted());
  CHECK(b.run_until_state(BURN_STATE_HEATING, 5 * MIN));
  CHECK(b.s.plug.last_cold);
  CHECK(b.s.plug.rec.verified_v == 12);  // the model's 0.2 Ohm plug, under 0.8 Ohm
  CHECK(safety_plug_save_pending(&b.s));
  CHECK(!b.s.plug_warning);
  // "New glow plug fitted" from the ESP32: the remembered check is gone.
  CHECK(b.client.glow_plug_fitted().accepted());
  b.run(1 * SEC);
  CHECK(b.s.plug.rec.verified_v == 0);
}

// ---------------------------------------------------------------- the CO alarm

static uint8_t co_state(const Bench &b) { return b.client.raw_detail().co_state; }

static void test_co_alarm_stops_and_latches() {
  Bench b;
  b.run(2 * MIN);
  CHECK(co_state(b) == CO_WARMING_UP);
  CHECK(safety_co_heater(&b.s));                   // heated from power-up (CO-5)
  b.run(12 * MIN);                                 // warm-up, heater check, learning
  CHECK(co_state(b) == CO_OK);
  CHECK(b.client.raw_detail().co_ppm == 0);        // clean air
  b.client.set_power_demand(50);
  CHECK(b.client.set_mode(Mode::HEAT).accepted());
  CHECK(b.run_until_state(BURN_STATE_HEATING, 5 * MIN));
  // About 1200 ppm: the alarm after 10 s, the run burns off and cools down.
  b.co_ratio = 8;
  b.run(12 * SEC);
  CHECK(b.s.burn.fault == BURN_FAULT_CO && b.s.burn.fault_latched);
  CHECK(b.s.burn.step == BURN_STEP_BURN_OFF);
  CHECK(safety_outputs(&b.s)->pump_cHz == 0);
  b.run(1 * SEC);
  CHECK(co_state(b) == CO_ALARM);
  CHECK(b.client.raw_detail().co_ppm > 1000 && b.client.raw_detail().co_ppm <= 2140);  // about 1200
  CHECK(b.client.raw_status().fault == BURN_FAULT_CO);
  CHECK(b.client.raw_status().flags & LINK_STATUS_FAULT_LATCHED);
  esphome::flamekeeper::HeaterStatus hs;
  b.client.read_status(hs);
  CHECK(hs.fault == esphome::flamekeeper::Fault::CO);
  CHECK(b.run_until_state(BURN_STATE_OFF, 25 * MIN));
  // Clean air again: still latched; a reset is refused until the air has been clean a minute.
  b.co_ratio = 1000;
  b.run(30 * SEC);
  CHECK(b.client.reset_fault().accepted());  // sent: the MCU decides
  b.run(1 * SEC);
  CHECK(b.s.burn.fault_latched && co_state(b) == CO_ALARM);
  b.run(35 * SEC);
  b.client.reset_fault();
  b.run(2 * SEC);
  CHECK(!b.s.burn.fault_latched && b.s.burn.fault == BURN_FAULT_NONE);
  CHECK(co_state(b) == CO_OK);
  b.client.set_mode(Mode::OFF);
  b.run(1 * SEC);
  CHECK(b.client.set_mode(Mode::HEAT).accepted());
  CHECK(b.run_until_state(BURN_STATE_STARTING, 2 * SEC));
}

static void test_co_sensor_fault() {
  Bench b;
  b.run(14 * MIN);
  CHECK(co_state(b) == CO_OK);
  b.client.set_power_demand(50);
  CHECK(b.client.set_mode(Mode::HEAT).accepted());
  CHECK(b.run_until_state(BURN_STATE_HEATING, 5 * MIN));
  // The sensor goes open (CO-3): a sensor fault after 5 s, with the heater-safe action (CO-2).
  b.co_adc = 10;
  b.run(7 * SEC);
  CHECK(b.s.burn.fault == BURN_FAULT_CO_SENSOR && b.s.burn.fault_latched);
  CHECK(b.s.burn.step == BURN_STEP_BURN_OFF);
  b.run(1 * SEC);
  CHECK(co_state(b) == CO_SENSOR_FAULT);
  CHECK(b.client.raw_detail().co_ppm == LINK_UNKNOWN_U16);
  esphome::flamekeeper::HeaterStatus hs;
  b.client.read_status(hs);
  CHECK(hs.fault == esphome::flamekeeper::Fault::CO_SENSOR);
  CHECK(b.run_until_state(BURN_STATE_OFF, 25 * MIN));
  b.client.reset_fault();
  b.run(2 * SEC);
  CHECK(b.s.burn.fault_latched);  // still open
  b.co_adc = -1;
  b.run(3 * SEC);
  b.client.reset_fault();
  b.run(2 * SEC);
  CHECK(!b.s.burn.fault_latched);
  // The reset runs the heater check again (heater off 5 s), then the sensor reads on.
  b.run(10 * SEC);
  CHECK(b.s.co.checked && co_state(b) == CO_OK);
}

static void test_glow_fault_reset_keeps_budget() {
  // A glow stage fault in preheat, then the user's fault reset: the fault clears, the plug's heat
  // budget stays as used (GLW-21: it starts empty only after an MCU reset).
  Bench b;
  b.run(125 * SEC);
  b.client.set_power_demand(50);
  CHECK(b.client.set_mode(Mode::HEAT).accepted());
  for (uint32_t t = 0; t < 2 * MIN && b.s.burn.step != BURN_STEP_PREHEAT; t += TICK)
    b.pass();
  b.run(10 * SEC);
  CHECK(glow_bucket_a2s(&b.s.glow) > 100);
  b.glow_ls = true;  // the low side sticks on (GLW-6)
  b.run(1 * SEC);
  b.glow_ls = false;
  CHECK(b.s.glow.fault == GLOW_FAULT_STUCK_LOW_SIDE);
  CHECK(b.s.burn.fault == BURN_FAULT_GLOW && b.s.burn.fault_latched);
  CHECK(b.s.burn.step == BURN_STEP_OFF);
  b.client.set_mode(Mode::OFF);
  b.run(1 * SEC);
  uint32_t used = glow_bucket_a2s(&b.s.glow);
  b.client.reset_fault();
  b.run(200);
  CHECK(!b.s.burn.fault_latched && b.s.glow.fault == GLOW_FAULT_NONE);
  CHECK(glow_bucket_a2s(&b.s.glow) + 10 >= used);  // only drained since, by the plug's rest
  glow_params gp;
  glow_params_default(&gp);
  CHECK(std::memcmp(&b.s.glow.p, &gp, sizeof(gp)) == 0);  // and its parameters
}

static void test_co_not_fitted() {
  // CO_FITTED is a parameter (CO-1): off, nothing reads the sensor, whatever the pin says.
  Bench b;
  b.co_adc = 0;  // no sensor on the board: R_CO_LOAD pulls CO_ADC to ground
  b.run(10 * SEC);
  CHECK(b.client.param_set(0x52, 0).accepted());
  b.run(1 * SEC);
  CHECK(b.client.param_apply().accepted());
  b.run(1 * SEC);
  CHECK(b.s.burn.p.co_fitted == 0);
  CHECK(!safety_co_heater(&b.s));
  b.run(15 * MIN);
  CHECK(co_state(b) == CO_NOT_FITTED);
  CHECK(b.s.burn.fault == BURN_FAULT_NONE);
  b.client.set_power_demand(50);
  CHECK(b.client.set_mode(Mode::HEAT).accepted());
  CHECK(b.run_until_state(BURN_STATE_HEATING, 5 * MIN));
  // Fitted again (only with the heater off): a board without the sensor is a sensor fault, never
  // "absent" (CO-2).
  b.client.set_mode(Mode::OFF);
  CHECK(b.run_until_state(BURN_STATE_OFF, 25 * MIN));
  b.client.param_set(0x52, 1);
  b.run(1 * SEC);
  b.client.param_apply();
  b.run(1 * SEC);
  CHECK(safety_co_heater(&b.s));
  b.run(4 * MIN);
  CHECK(co_state(b) == CO_SENSOR_FAULT);
  CHECK(b.s.burn.fault == BURN_FAULT_CO_SENSOR);
}

static void test_co_warm_up_and_cold() {
  Bench b;
  b.co_ratio = 8;  // CO from the start: readings in the warm-up are ignored (CO-5)
  b.run(170 * SEC);
  CHECK(co_state(b) == CO_WARMING_UP);
  CHECK(b.s.burn.fault == BURN_FAULT_NONE);
  b.co_ratio = 1000;
  b.run(11 * MIN);
  CHECK(co_state(b) == CO_OK);
  // CO-8: below -30 C it shows "too cold", and the alarm still acts.
  b.board_c10 = -350;
  b.run(2 * SEC);
  CHECK(co_state(b) == CO_TOO_COLD);
  b.co_ratio = 8;
  b.run(12 * SEC);
  CHECK(b.s.burn.fault == BURN_FAULT_CO && b.s.burn.fault_latched);
}

static void test_co_r0_kept() {
  // CO-7: R0 learnt in clean air is offered for flash once, then only after a real change.
  Bench b;
  b.run(13 * MIN);
  CHECK(co_state(b) == CO_LEARNING);
  CHECK(safety_co_r0_to_save(&b.s) == 0);
  b.run(1 * MIN);
  CHECK(co_state(b) == CO_OK);
  uint32_t r0 = safety_co_r0_to_save(&b.s);
  CHECK(r0 > 290000 && r0 < 310000);
  safety_co_r0_saved(&b.s, r0);
  CHECK(safety_co_r0_to_save(&b.s) == 0);
  b.co_ratio = 1200;  // drier air: R0 rises 20 %, saved only an hour after the last save
  b.run(5 * MIN);
  CHECK(safety_co_r0_to_save(&b.s) == 0);
  b.run(60 * MIN);
  CHECK(safety_co_r0_to_save(&b.s) > r0);
  // After a reset with R0 from flash: no learning time, OK right after the warm-up and check.
  Bench c;
  safety_load_co_r0(&c.s, r0);
  c.run(3 * MIN + 10 * SEC);
  CHECK(co_state(c) == CO_OK);
  CHECK(c.s.co.r0 >= r0);
}

int main() {
  test_glow_derated_by_budget();
  test_plug_cold_check();
  test_parameters();
  test_glow_waits_after_reset();
  test_heat_cycle();
  test_dead_man();
  test_mcu_reset_mid_run();
  test_co_alarm_stops_and_latches();
  test_co_sensor_fault();
  test_glow_fault_reset_keeps_budget();
  test_co_not_fitted();
  test_co_warm_up_and_cold();
  test_co_r0_kept();
  return check_report("e2e");
}
