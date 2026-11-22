// Room thermostat (any heater; the controller runs it): turns room temperature and target into a
// power demand (0..100 %) and, optionally, into stop/restart decisions. Plain C++ with no ESPHome dependencies so
// it builds and is unit-tested on the host (tests/).
//
// Time is the uint32_t millis() value and wraps: elapsed time is always uint32_t(now - then).
#pragma once

#include <cstdint>

namespace esphome {
namespace flamekeeper {

struct ThermostatSettings {
  float band{2.0f};           // degrees C of error that swings the proportional term from 0 to 100 %
  float integral_min{0.0f};   // integral (reset) time in minutes; 0 = proportional only
  float min_power{0.0f};      // demand limits, percent
  float max_power{100.0f};
  float ramp_up{20.0f};       // slew limits, percent per minute (0 = unlimited)
  float ramp_down{20.0f};
  bool cycle{false};          // may the thermostat stop and restart the heater?
  float stop_above{1.5f};     // cycle: stop when room >= target + stop_above
  float start_below{1.0f};    // cycle: start again when room <= target - start_below
  uint32_t min_run_s{600};    // cycle: never stop sooner than this after asking for heat
  uint32_t min_off_s{300};    // cycle: never restart sooner than this after asking for stop
  float fallback_power{0.0f}; // demand used while the room temperature is unknown (NaN)
};

struct ThermostatOutput {
  bool want_heat;
  float demand;  // percent, min_power..max_power
};

class Thermostat {
 public:
  // Sanitises: band >= 0.1, powers 0..100 with max >= min, negative numbers -> 0.
  void set(const ThermostatSettings &s);
  const ThermostatSettings &settings() const { return s_; }

  // The user switched heat on in thermostat mode.
  void enable(uint32_t now_ms, float room, float target, float initial_demand);
  void disable();

  // Call about once a second (any interval works). burning = the heater is in its normal running
  // state (not starting, not stopping); the heater ignores demand otherwise, so it is held then.
  ThermostatOutput update(uint32_t now_ms, float room, float target, bool burning);

  bool enabled() const { return enabled_; }
  bool want_heat() const { return want_heat_; }
  float demand() const { return demand_; }

 protected:
  float clamp_power(float p) const;

  ThermostatSettings s_;
  bool enabled_{false};
  bool want_heat_{false};
  bool off_expired_{false};  // min_off_s already satisfied (enable() above the stop threshold)
  float integral_{0.0f};     // 0..1
  float demand_{0.0f};
  uint32_t last_ms_{0};
  uint32_t heat_since_ms_{0};  // want_heat last became true
  uint32_t off_since_ms_{0};   // want_heat last became false
};

}  // namespace flamekeeper
}  // namespace esphome
