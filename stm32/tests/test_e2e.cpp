// End to end: the ESP32's real link client (esp32/components/flamekeeper/link_client.*) talks over
// a byte wire to this safety core, which runs the heater model. A whole heat cycle, the dead-man
// when the ESP32 stops talking, an MCU reset in the middle of a run, and the CO alarm.
#include <cstdio>
#include <cstring>
#include <deque>

#include "../../esp32/components/flamekeeper/link_client.h"

extern "C" {
#include "../core/safety.h"
#include "heater_model.h"
}

static int n_checks = 0, failures = 0;
#define CHECK(c)                                                \
  do {                                                          \
    n_checks++;                                                   \
    if (!(c)) {                                                 \
      failures++;                                               \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);  \
    }                                                           \
  } while (0)

using esphome::flamekeeper::LinkClient;
using esphome::flamekeeper::LinkTransport;
using esphome::flamekeeper::Mode;

static const uint32_t TICK = 10;  // the safety loop's pass
static const uint32_t SEC = 1000;
static const uint32_t MIN = 60 * SEC;

struct Bench : LinkTransport {
  safety s;
  heater_model m;
  uint32_t now = 0;
  bool esp_alive = true;  // false: the ESP32 is hung (its loop stops)
  // The glow plug: 0.2 Ohm cold to 1.0 Ohm hot, on a 12.6 V supply.
  double plug_heat = 0, last_i = 0, glow_w = 0;
  uint16_t fan_duty = 0;
  // A healthy board's interlocks, for the self-tests: the heartbeat detector, the overheat latch.
  int hb_running_ms = 0;
  int latch_armed = 0;
  uint16_t test_pulse = 0;
  int glow_en = 0;
  double pump_i = 0;  // the pump coil's current, A (6 Ohm, 2 ms)
  uint32_t stroke_ms_total = 0;
  uint32_t stroke_ms_unguarded = 0;  // PUMP_CMD driven without the overcurrent watchdog armed
  int max_heartbeat_gap_in_burn = 0;
  std::deque<uint8_t> to_esp;
  LinkClient client{this};
  // The CO sensor: R0 300 k in clean air; the air as Rs / R0 (x 1000); unheated it reads 5 x higher.
  // co_adc >= 0 holds the reading there (an open sensor, or none fitted).
  uint32_t co_ratio = 1000;
  int co_adc = -1;
  int16_t board_c10 = 350;

  Bench() {
    heater_model_init(&m, 10);
    burn_params p;
    burn_params_default(&p);
    glow_params gp;
    glow_params_default(&gp);
    fan_params fp;
    fan_params_default(&fp);
    checks_params cp;
    checks_params_default(&cp);
    pump_params pp;
    pump_params_default(&pp);
    burn_inputs in = inputs();
    safety_init(&s, &p, &gp, &fp, &cp, &pp, now, &in, LINK_RESET_POWER_ON, 0);
  }
  burn_inputs inputs() const {
    burn_inputs in;
    in.hx_c10 = heater_model_hx(&m);
    in.fan_rpm = (uint16_t) (m.fan_rpm + 0.5);
    in.supply_cv = m.supply_cv;
    in.overheat_latch = 0;
    in.co_alarm = 0;
    in.co_fault = 0;
    in.glow_fault = 0;
    in.fan_fault = 0;
    in.precheck = 0;
    in.pump_fault = 0;
    in.board_c10 = board_c10;  // a cool board: no derate
    in.plug_refused = 0;
    return in;
  }
  // ESP32 -> MCU: the MCU answers each complete request at once (well inside its 20 ms).
  void send(const uint8_t *data, size_t len) override {
    uint8_t reply[LINK_FRAME_MAX];
    for (size_t i = 0; i < len; i++) {
      size_t n = safety_receive(&s, data[i], now, reply);
      for (size_t j = 0; j < n; j++)
        to_esp.push_back(reply[j]);
    }
  }
  int read() override {
    if (to_esp.empty())
      return -1;
    int b = to_esp.front();
    to_esp.pop_front();
    return b;
  }
  // One 5 ms glow period: the planner's pulse into the plug model.
  void glow_period(uint32_t t) {
    double r = 0.2 + 0.8 * plug_heat;
    double i = 12.6 / (r + 0.02);
    glow_meas gm;
    gm.vin_cv = 1260;
    gm.pulse_ca = (uint16_t) (last_i * 100 + 0.5);
    gm.gap_ca = 0;
    gm.airflow_ok = 1;
    gm.board_c10 = 250;
    gm.high_side_trip = 0;
    glow_cmd c = safety_glow_period(&s, t, &gm);
    last_i = c.width_us ? i : 0;
    double w = i * i * r * c.width_us / 5000.0;
    glow_w = w;
    plug_heat += (w / 90.0 - plug_heat) * (5.0 / 8000.0);
  }
  // One millisecond of the self-tests and the heartbeat.
  void ms(uint32_t t) {
    bool hb_ok = hb_running_ms >= 1;
    bool pump_on = hb_ok && latch_armed;
    checks_obs co;
    co.vin_cv = 1260;
    co.pump_v_cv = pump_on ? 1260 : 30;
    co.glow_v_cv = (glow_en && hb_ok) ? 1260 : 0;  // the plug's + side follows its enable
    co.glow_ca = 0;
    co.pump_peak_ma = test_pulse ? (pump_on ? 400 : 3) : 0;
    co.latch_armed = (uint8_t) latch_armed;
    co.pump_hs_ok = 1;
    co.hx_c10 = heater_model_hx(&m);
    pump_meas pm;
    pm.vin_cv = 1260;
    pm.i_ma = (uint16_t) (pump_i * 1000);
    pm.overcurrent = 0;
    pm.hs_ok = 1;
    pm.hb_running_ms = 0;
    safety_io out = safety_ms(&s, t, &co, &pm);
    double v = (pump_on && out.pump_duty) ? out.pump_duty / 1000.0 * 12.6 : 0;
    pump_i += (v / 6.0 - pump_i) * 0.393;
    if (out.pump_duty)
      stroke_ms_total++;
    if (out.pump_duty && !out.pump_oc_armed)
      stroke_ms_unguarded++;
    hb_running_ms = out.heartbeat == CHECKS_HB_RUNNING ? hb_running_ms + 1 : 0;
    if (out.latch_reset)
      latch_armed = 1;
    test_pulse = out.pump_pulse_us;
    glow_en = out.glow_en;
    // While fuel may flow the heartbeat must run (the pump's high side needs it).
    if (s.burn.out.pump_cHz > 0 && out.heartbeat != CHECKS_HB_RUNNING)
      max_heartbeat_gap_in_burn++;
  }
  void tick() {
    const burn_outputs *o = safety_outputs(&s);
    for (uint32_t i = 0; i < TICK; i++)
      ms(now + i);
    glow_period(now);
    glow_period(now + 5);
    heater_model_step(&m, TICK, o->fan_rpm, o->pump_cHz, (uint16_t) (plug_heat > 0.5 ? 90 : 0));
    now += TICK;
    burn_inputs in = inputs();
    if (now % SEC == 0)
      safety_co_step(&s, now, co_reading(), board_c10);
    safety_step(&s, now, &in);
    // The fan stage: the model's fan follows its target; the shunt reads about 4.3 A at full
    // speed, times the duty.
    fan_meas fm;
    fm.rpm = in.fan_rpm;
    fm.vin_cv = 1260;
    fm.isense_ma = (uint16_t) (4300.0 * m.fan_rpm / 5000.0 * fan_duty / 1000.0);
    fan_duty = safety_fan_step(&s, now, &fm);
    if (esp_alive)
      client.loop(now);
  }
  uint16_t co_reading() const {
    if (co_adc >= 0)
      return (uint16_t) co_adc;
    uint64_t rs = 300000ull * co_ratio / 1000u * (safety_co_heater(&s) ? 1u : 5u);
    return (uint16_t) ((uint64_t) CO_ADC_FULL * CO_LOAD_OHM / (CO_LOAD_OHM + rs));
  }
  void run(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += TICK)
      tick();
  }
  bool run_until_state(uint8_t state, uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += TICK) {
      if (client.up() && client.raw_status().state == state)
        return true;
      tick();
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
    b.tick();
    if (air_from == 0 && b.s.in.fan_rpm >= b.s.burn.p.glow_min_fan_rpm)
      air_from = b.now;
    if (b.s.burn.out.glow_w > 0)
      first_glow = b.now;
  }
  CHECK(air_from > 0 && first_glow > 0);
  CHECK(first_glow - air_from >= 30 * SEC);
  CHECK(first_glow - air_from < 31 * SEC);
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
}

static void test_mcu_reset_mid_run() {
  Bench b;
  b.run(125 * SEC);
  b.client.set_power_demand(100);
  CHECK(b.client.set_mode(Mode::HEAT).accepted());
  CHECK(b.run_until_state(BURN_STATE_HEATING, 5 * MIN));
  b.run(5 * MIN);
  // The safety MCU resets (watchdog): hot heat exchanger, the ESP32 still asking for heat.
  burn_params p;
  burn_params_default(&p);
  glow_params gp;
  glow_params_default(&gp);
  fan_params fp;
  fan_params_default(&fp);
  checks_params cp;
  checks_params_default(&cp);
  pump_params pp;
  pump_params_default(&pp);
  burn_inputs in = b.inputs();
  safety_init(&b.s, &p, &gp, &fp, &cp, &pp, b.now, &in, LINK_RESET_WATCHDOG, 0);
  b.latch_armed = 0;  // a reset leaves the latch tripped until the power-up test re-arms it
  CHECK(b.s.burn.step == BURN_STEP_COOLDOWN);
  CHECK(b.run_until_state(BURN_STATE_COOLING, 5 * SEC));
  CHECK(b.run_until_state(BURN_STATE_OFF, 25 * MIN));
  b.run(1 * MIN);
  CHECK(b.client.raw_status().state == BURN_STATE_OFF);  // an MCU reset never starts the heater
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
  CHECK(b.client.param_apply(true).accepted());
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
  b.client.param_apply(false);
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
  CHECK(b.s.plug.rec.verified_type == 12 && b.s.plug.rec.verified_setting == 12);
  CHECK(safety_plug_save_pending(&b.s));
  CHECK(!b.s.plug_warning);
  CHECK(b.s.plug.last_r_mohm > 150 && b.s.plug.last_r_mohm < 400);  // the model's 0.2 Ohm plug
  // "New glow plug fitted" from the ESP32: the remembered check is gone.
  CHECK(b.client.glow_plug_fitted().accepted());
  b.run(1 * SEC);
  CHECK(b.s.plug.rec.verified_type == 0);
}

// ---------------------------------------------------------------- the CO alarm

static uint8_t co_state(const Bench &b) { return b.client.raw_detail().co_state; }

static void test_co_alarm_stops_and_latches() {
  Bench b;
  b.run(2 * MIN);
  CHECK(co_state(b) == CO_WARMING_UP);
  CHECK(safety_co_heater(&b.s));                   // heated from power-up (CO-5)
  CHECK(b.client.info().capabilities & LINK_CAP_CO);
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

static void test_co_not_fitted() {
  // CO_FITTED is a parameter (CO-1): off, nothing reads the sensor, whatever the pin says.
  Bench b;
  b.co_adc = 0;  // no sensor on the board: R_CO_LOAD pulls CO_ADC to ground
  b.run(10 * SEC);
  CHECK(b.client.param_set(0x52, 0).accepted());
  b.run(1 * SEC);
  CHECK(b.client.param_apply(true).accepted());
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
  b.client.param_apply(true);
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
  test_plug_cold_check();
  test_parameters();
  test_glow_waits_after_reset();
  test_heat_cycle();
  test_dead_man();
  test_mcu_reset_mid_run();
  test_co_alarm_stops_and_latches();
  test_co_sensor_fault();
  test_co_not_fitted();
  test_co_warm_up_and_cold();
  test_co_r0_kept();
  std::printf("e2e: %d checks, %d failures\n", n_checks, failures);
  return failures ? 1 : 0;
}
