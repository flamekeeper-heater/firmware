// End to end: the ESP32's real link client (esp32/components/flamekeeper/link_client.*) talks over
// a byte wire to this safety core, which runs the heater model. A whole heat cycle, the dead-man
// when the ESP32 stops talking, and an MCU reset in the middle of a run.
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
  int max_heartbeat_gap_in_burn = 0;
  std::deque<uint8_t> to_esp;
  LinkClient client{this};

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
    burn_inputs in = inputs();
    safety_init(&s, &p, &gp, &fp, &cp, now, &in, LINK_RESET_POWER_ON, 0);
  }
  burn_inputs inputs() const {
    burn_inputs in;
    in.hx_c10 = heater_model_hx(&m);
    in.fan_rpm = (uint16_t) (m.fan_rpm + 0.5);
    in.supply_cv = m.supply_cv;
    in.overheat_latch = 0;
    in.co_alarm = 0;
    in.glow_fault = 0;
    in.fan_fault = 0;
    in.precheck = 0;
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
    checks_out out = safety_ms(&s, t, &co);
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

static void test_glow_rests_after_reset() {
  Bench b;
  b.run(2 * SEC);
  // After power-on the glow stage's heat budget starts full: a heat start waits until half of it
  // has drained (two minutes).
  b.client.set_power_demand(60);
  b.client.set_mode(Mode::HEAT);
  b.run(1 * SEC);
  CHECK(b.client.raw_status().state == BURN_STATE_OFF);
  CHECK(b.client.raw_status().start_result == LINK_BUSY);
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
  burn_inputs in = b.inputs();
  safety_init(&b.s, &p, &gp, &fp, &cp, b.now, &in, LINK_RESET_WATCHDOG, 0);
  b.latch_armed = 0;  // a reset leaves the latch tripped until the power-up test re-arms it
  CHECK(b.s.burn.step == BURN_STEP_COOLDOWN);
  CHECK(b.run_until_state(BURN_STATE_COOLING, 5 * SEC));
  CHECK(b.run_until_state(BURN_STATE_OFF, 25 * MIN));
  b.run(1 * MIN);
  CHECK(b.client.raw_status().state == BURN_STATE_OFF);  // an MCU reset never starts the heater
}

int main() {
  test_glow_rests_after_reset();
  test_heat_cycle();
  test_dead_man();
  test_mcu_reset_mid_run();
  std::printf("e2e: %d checks, %d failures\n", n_checks, failures);
  return failures ? 1 : 0;
}
