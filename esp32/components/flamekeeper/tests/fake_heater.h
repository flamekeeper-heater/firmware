// A heater backend for the host tests: records requests and refuses like the real backends do
// (starts only with control on and from off, priming only from off).
#pragma once

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../controller.h"

static int g_checks = 0;
static int g_failures = 0;
#define CHECK(cond)                                                      \
  do {                                                                   \
    g_checks++;                                                          \
    if (!(cond)) {                                                       \
      g_failures++;                                                      \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
    }                                                                    \
  } while (0)
#define CHECK_EQ(a, b)                                                                                    \
  do {                                                                                                    \
    g_checks++;                                                                                           \
    long long va_ = (long long) (a), vb_ = (long long) (b);                                               \
    if (va_ != vb_) {                                                                                     \
      g_failures++;                                                                                       \
      std::printf("FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_);         \
    }                                                                                                     \
  } while (0)
static int report(const char *name) {
  std::printf("%s: %d checks, %d failures\n", name, g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}

namespace fk = esphome::flamekeeper;

class FakeHeater : public fk::Heater {
 public:
  uint32_t caps{fk::cap::FAN_ONLY | fk::cap::TEMPERATURE | fk::cap::PRIME | fk::cap::PRIME_STOP |
                fk::cap::PARAMETERS};
  fk::HeaterStatus st;
  float room{NAN};
  float altitude{NAN};
  std::vector<std::string> calls;

  struct Param {
    fk::ParamInfo info;
    fk::ParamValue value;
    std::string help{};
  };
  std::vector<Param> params;

  FakeHeater() {
    st.state = fk::HeaterState::OFF;
    st.answering = true;
    st.control = fk::ControlStatus::IDLE;
  }

  uint32_t capabilities() const override { return caps; }
  void read_status(fk::HeaterStatus &out) const override { out = st; }

  fk::Outcome set_mode(fk::Mode m) override {
    calls.push_back("mode " + std::to_string(int(m)));
    if (m == fk::Mode::OFF) {
      st.mode = fk::Mode::OFF;
      return fk::Outcome::ok();
    }
    if (!st.answering)
      return fk::Outcome::refuse(fk::Result::NO_REPLY, "the heater does not answer");
    if (m == fk::Mode::PRIME && st.mode != fk::Mode::OFF)
      return fk::Outcome::refuse(fk::Result::NOT_OFF, "the heater must be off");
    st.mode = m;
    return fk::Outcome::ok();
  }
  fk::Outcome set_heat_mode(fk::HeatMode m) override {
    st.heat_mode = m;
    return fk::Outcome::ok();
  }
  fk::Outcome set_power_demand(uint8_t percent) override {
    st.power = percent;
    return fk::Outcome::ok();
  }
  fk::Outcome set_target(float c) override {
    st.target_c = c;
    return fk::Outcome::ok();
  }
  fk::Outcome stop_prime() override {
    if (st.mode == fk::Mode::PRIME)
      st.mode = fk::Mode::OFF;
    return fk::Outcome::ok();
  }
  void set_room_temperature(float c) override { room = c; }
  void set_altitude(float m) override { altitude = m; }
  int fuel_trim = 0, fan_trim = 0, trims_calls = 0, fault_resets = 0;
  void set_trims(int fuel, int fan) override {
    fuel_trim = fuel;
    fan_trim = fan;
    trims_calls++;
  }
  fk::Outcome reset_fault() override {
    fault_resets++;
    st.fault = fk::Fault::NONE;
    return fk::Outcome::ok();
  }

  size_t param_count() const override { return params.size(); }
  bool param_info(size_t i, fk::ParamInfo &out) const override {
    if (i >= params.size())
      return false;
    out = params[i].info;
    return true;
  }
  bool param_get(uint16_t id, fk::ParamValue &out) const override {
    for (auto &p : params)
      if (p.info.id == id) {
        out = p.value;
        return true;
      }
    return false;
  }
  const char *param_help(uint16_t id) const override {
    for (auto &p : params)
      if (p.info.id == id)
        return p.help.c_str();
    return "";
  }
  fk::Outcome param_set(uint16_t id, int32_t v) override {
    for (auto &p : params)
      if (p.info.id == id) {
        if (v < p.info.min || v > p.info.max)
          return fk::Outcome::refuse(fk::Result::OUT_OF_RANGE, "out of range");
        p.value.staged = true;
        p.value.staged_value = v;
        return fk::Outcome::ok();
      }
    return fk::Outcome::refuse(fk::Result::OUT_OF_RANGE, "no such parameter");
  }
  fk::Outcome param_apply(bool save) override {
    for (auto &p : params)
      if (p.value.staged) {
        p.value.value = p.value.staged_value;
        p.value.staged = false;
      }
    emit_(save ? fk::Event::PARAMS_SAVED : fk::Event::PARAMS_APPLIED, fk::Result::OK, 0, "done");
    return fk::Outcome::ok();
  }
  fk::Outcome param_discard() override {
    for (auto &p : params)
      p.value.staged = false;
    return fk::Outcome::ok();
  }
};
