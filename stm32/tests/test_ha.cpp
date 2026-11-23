// End to end from the safety core to what Home Assistant and the apps see: this safety core on the
// heater bench (core_rig.h), the ESP32's link client, controller and automation on top of it, and
// the app protocol's State and Telemetry records. A fan stall the core retries, the lockout past
// the retries, the CO sensor fault apart from the CO alarm, and the firmware the MCU reports.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "../../common/test/check.h"
#include "../../esp32/components/flamekeeper/automation.h"
#include "../../esp32/components/flamekeeper/ble_protocol.h"
#include "core_rig.h"

namespace fk = esphome::flamekeeper;
using fk::Fault;
using fk::HeaterState;
using fk::Mode;
using fk::Source;

struct Bench : HeaterBench<Bench> {
  fk::Controller c{&client};
  fk::Automation a{c};
  std::vector<std::pair<int, std::string>> alerts;  // event 08: bit, text

  Bench() {
    mcu_reset(LINK_RESET_POWER_ON);
    c.on_event([this](fk::Event e, fk::Result, uint16_t id, const char *text) {
      if (e == fk::Event::ALERT)
        alerts.push_back({int(id), text});
    });
  }
  void pass_out(const burn_inputs &in) {
    HeaterBench::pass_out(in);
    c.tick(now, true);
    a.tick(now);
  }
  const fk::Status &st() const { return c.status(); }
  int count(int bit) const {
    int n = 0;
    for (auto &x : alerts)
      n += x.first == bit ? 1 : 0;
    return n;
  }
  // Runs until the condition holds (at most ms); true when it did.
  template <class F> bool until(F cond, uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += TICK) {
      if (cond())
        return true;
      pass();
    }
    return cond();
  }
  std::string reason() const {
    char t[128];
    fk::fault_reason_text(c.status(), t, sizeof(t));
    return t;
  }
};

static void test_fan_stall_retry_and_lockout() {
  Bench b;
  b.run(5 * SEC);
  CHECK(b.c.set_mode(Mode::FAN, Source::PHONE).accepted());
  CHECK(b.until([&] { return b.st().state == HeaterState::FAN_ONLY && b.m.fan_rpm > 1000; }, 30 * SEC));
  b.run(5 * SEC);
  const size_t none = b.alerts.size();

  // The fan jams: everything stops, and the core waits 5 s before it tries again. Still fan only.
  b.m.fan_stalled = 1;
  CHECK(b.until([&] { return b.st().fault == Fault::FAN; }, 10 * SEC));
  CHECK_EQ(b.client.raw_status().detail, BURN_STEP_FAN_RETRY);
  CHECK(!(b.client.raw_status().flags & LINK_STATUS_FAULT_LATCHED));
  CHECK(b.st().state == HeaterState::FAN_ONLY);
  CHECK_EQ(b.st().panel_fault, BURN_WHY_FAN_STALL);
  CHECK(fk::fault_retrying(b.st()));
  CHECK(b.reason() == "The fan stalled: trying again");
  CHECK(std::string(fk::fault_name(b.st().fault)) == "Fan");  // "Error"
  // The apps: State byte 9 the heater state, byte 11 the fault; Telemetry byte 19 the reason.
  uint8_t rec[64];
  fk::ble::encode_state(b.st(), rec);
  CHECK_EQ(rec[9], uint8_t(HeaterState::FAN_ONLY));
  CHECK_EQ(rec[11], uint8_t(Fault::FAN));
  fk::ble::encode_telemetry(b.st(), rec);
  CHECK_EQ(rec[19], BURN_WHY_FAN_STALL);
  // Freed within the wait: the run carries on, the fault clears, nothing was alerted.
  b.run(2 * SEC);
  b.m.fan_stalled = 0;
  CHECK(b.until([&] { return b.st().fault == Fault::NONE; }, 10 * SEC));
  CHECK(b.st().state == HeaterState::FAN_ONLY);
  CHECK(b.st().mode == Mode::FAN);
  b.run(10 * SEC);
  CHECK(b.m.fan_rpm > 1000);
  CHECK_EQ(b.alerts.size(), none);
  CHECK(b.reason() == "None");

  // Jammed for good: the retries run out (two since the first), the core locks the fan out and the
  // heater is off. One alert, which says so; the run is over.
  b.m.fan_stalled = 1;
  CHECK(b.until([&] { return b.client.raw_status().flags & LINK_STATUS_FAULT_LATCHED; }, 60 * SEC));
  b.run(2 * SEC);
  CHECK(b.st().state == HeaterState::OFF);
  CHECK(b.st().fault == Fault::FAN);
  CHECK(!fk::fault_retrying(b.st()));
  CHECK(b.st().mode == Mode::OFF);
  CHECK(b.st().run_source == Source::NONE);
  CHECK_EQ(b.count(2), 1);
  CHECK_EQ(b.alerts.size(), none + 1);
  CHECK(!b.alerts.empty() && b.alerts.back().second.find("locked out") != std::string::npos);
  CHECK(b.st().alerts & fk::alert::BURN);
  // Freed, and the fault reset: it can start again, and the alert stays until cleared.
  b.m.fan_stalled = 0;
  b.run(2 * SEC);
  CHECK(b.c.reset_fault(Source::HOME_ASSISTANT).accepted());
  CHECK(b.until([&] { return b.st().fault == Fault::NONE; }, 5 * SEC));
  CHECK(b.c.set_mode(Mode::FAN, Source::PHONE).accepted());
  CHECK(b.until([&] { return b.st().state == HeaterState::FAN_ONLY; }, 5 * SEC));
  CHECK_EQ(b.count(2), 1);
}

static void test_fan_stall_after_fuel() {
  // A stall in a heat run: the core cools the heater down after its wait: the run is over at once,
  // alerted at once, and only once when it is off.
  Bench b;
  b.run(125 * SEC);
  b.c.set_power(60, Source::PHONE);
  CHECK(b.c.set_mode(Mode::HEAT, Source::PHONE).accepted());
  CHECK(b.until([&] { return b.st().state == HeaterState::HEATING; }, 5 * MIN));
  b.run(30 * SEC);
  const size_t mark = b.alerts.size();
  b.m.fan_stalled = 1;
  CHECK(b.until([&] { return b.st().fault == Fault::FAN; }, 10 * SEC));
  CHECK(b.st().state == HeaterState::STOPPING);
  CHECK(!fk::fault_retrying(b.st()));
  b.run(SEC);
  CHECK_EQ(b.count(2), 1);
  CHECK(b.alerts.size() > mark && b.alerts.back().second == "heater fault: Fan (the fan stalled)");
  b.run(3 * SEC);
  b.m.fan_stalled = 0;  // it turns again: the cooldown runs
  CHECK(b.until([&] { return b.st().state == HeaterState::OFF; }, 25 * MIN));
  b.run(2 * SEC);
  CHECK_EQ(b.count(2), 1);
}

static void test_co_sensor_fault_and_states() {
  Bench b;
  b.run(2 * MIN);
  CHECK(std::string(fk::co_state_name(b.st())) == "Warming up");
  CHECK(std::isnan(b.st().co_ppm));
  b.run(12 * MIN);
  CHECK(std::string(fk::co_state_name(b.st())) == "OK");
  CHECK(b.st().co_ppm == 0.0f);
  // "CO sensor fitted" (0x52): in the table the apps and Home Assistant read, on by default.
  fk::ParamValue v;
  CHECK(b.c.param_get(0x52, v) && v.known && v.value == 1);
  b.c.set_power(50, Source::PHONE);
  CHECK(b.c.set_mode(Mode::HEAT, Source::PHONE).accepted());
  CHECK(b.until([&] { return b.st().state == HeaterState::HEATING; }, 5 * MIN));
  const size_t mark = b.alerts.size();
  b.co_adc = 10;  // the sensor goes open
  CHECK(b.until([&] { return b.st().fault == Fault::CO_SENSOR; }, 10 * SEC));
  b.run(2 * SEC);
  CHECK_EQ(b.st().panel_fault, BURN_WHY_CO_SENSOR);
  CHECK(std::string(fk::co_state_name(b.st())) == "Sensor fault");
  CHECK(b.reason() == "The CO sensor reads open or failed its check");
  CHECK(std::isnan(b.st().co_ppm));
  uint8_t rec[64];
  fk::ble::encode_telemetry(b.st(), rec);
  CHECK_EQ(rec[19], BURN_WHY_CO_SENSOR);
  CHECK_EQ(rec[30], uint8_t(fk::CoState::SENSOR_FAULT));
  CHECK(b.until([&] { return b.st().state == HeaterState::OFF; }, 25 * MIN));
  b.run(2 * SEC);
  // Its own alert (11), not the CO alarm (10), and no burn problem (2) on top.
  CHECK(b.st().alerts & fk::alert::CO_SENSOR);
  CHECK(!(b.st().alerts & (fk::alert::CO_ALARM | fk::alert::BURN)));
  int sensor = 0, others = 0;
  for (size_t i = mark; i < b.alerts.size(); i++)
    (b.alerts[i].first == 11 ? sensor : others)++;
  CHECK_EQ(sensor, 1);
  CHECK_EQ(others, 0);
  CHECK(b.c.set_mode(Mode::HEAT, Source::PHONE).result == fk::Result::REFUSED);
}

static void test_firmware_shown() {
  Bench b;
  b.run(2 * SEC);
  const std::string fw = fk::heater_firmware_text(b.client);
  CHECK(fw == "Flamekeeper safety " SAFETY_VERSION_TEXT);
  // This bench's ESP32 carries no image: the update is not available.
  CHECK(std::string(fk::firmware_update_status(b.st(), b.client)) == "Not available");
}

int main() {
  test_fan_stall_retry_and_lockout();
  test_fan_stall_after_fuel();
  test_co_sensor_fault_and_states();
  test_firmware_shown();
  return check_report("ha");
}
