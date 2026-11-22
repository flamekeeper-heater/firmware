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

static int checks = 0, failures = 0;
#define CHECK(c)                                                \
  do {                                                          \
    checks++;                                                   \
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
  std::deque<uint8_t> to_esp;
  LinkClient client{this};

  Bench() {
    heater_model_init(&m, 10);
    burn_params p;
    burn_params_default(&p);
    burn_inputs in = inputs();
    safety_init(&s, &p, now, &in, LINK_RESET_POWER_ON, 0);
  }
  burn_inputs inputs() const {
    burn_inputs in;
    in.hx_c10 = heater_model_hx(&m);
    in.fan_rpm = (uint16_t) (m.fan_rpm + 0.5);
    in.supply_cv = m.supply_cv;
    in.overheat_latch = 0;
    in.co_alarm = 0;
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
  void tick() {
    const burn_outputs *o = safety_outputs(&s);
    heater_model_step(&m, TICK, o->fan_rpm, o->pump_cHz, o->glow_w);
    now += TICK;
    burn_inputs in = inputs();
    safety_step(&s, now, &in);
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

static void test_heat_cycle() {
  Bench b;
  b.run(2 * SEC);
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
}

static void test_dead_man() {
  Bench b;
  b.run(2 * SEC);
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
  b.run(2 * SEC);
  b.client.set_power_demand(100);
  CHECK(b.client.set_mode(Mode::HEAT).accepted());
  CHECK(b.run_until_state(BURN_STATE_HEATING, 5 * MIN));
  b.run(5 * MIN);
  // The safety MCU resets (watchdog): hot heat exchanger, the ESP32 still asking for heat.
  burn_params p;
  burn_params_default(&p);
  burn_inputs in = b.inputs();
  safety_init(&b.s, &p, b.now, &in, LINK_RESET_WATCHDOG, 0);
  CHECK(b.s.burn.step == BURN_STEP_COOLDOWN);
  CHECK(b.run_until_state(BURN_STATE_COOLING, 5 * SEC));
  CHECK(b.run_until_state(BURN_STATE_OFF, 25 * MIN));
  b.run(1 * MIN);
  CHECK(b.client.raw_status().state == BURN_STATE_OFF);  // an MCU reset never starts the heater
}

int main() {
  test_heat_cycle();
  test_dead_man();
  test_mcu_reset_mid_run();
  std::printf("e2e: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
