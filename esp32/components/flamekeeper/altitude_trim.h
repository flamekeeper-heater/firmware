// Altitude fuel trim: turns a noisy altitude reading into a whole-percent fuel trim (thinner air
// -> less fuel). Plain C++ with no ESPHome dependencies so it is unit-tested on the host (tests/).
//
// Time is the uint32_t millis() value and wraps: elapsed time is always uint32_t(now - then).
#pragma once

#include <cmath>
#include <cstdint>

namespace esphome {
namespace flamekeeper {

struct AltitudeSettings {
  bool enabled{false};
  float offset_m{0.0f};          // added to the board's measured altitude (the controller applies it)
  float pct_per_1000m{10.0f};    // fuel reduction per 1000 m above sea level (0..30)
  float max_reduction{30.0f};    // most fuel removed, percent (0..30)
  float max_increase{0.0f};      // most fuel added below sea level, percent (0..30)
  float smoothing_s{120.0f};     // time constant of the first-order filter; 0 = none
  float deadband_pct{1.0f};      // the output only changes when the ideal trim is this far from it (0.5..5)
};

class AltitudeTrim {
 public:
  void set(const AltitudeSettings &in) {
    s_ = in;
    s_.pct_per_1000m = hold(s_.pct_per_1000m, 0.0f, 30.0f);
    s_.max_reduction = hold(s_.max_reduction, 0.0f, 30.0f);
    s_.max_increase = hold(s_.max_increase, 0.0f, 30.0f);
    s_.smoothing_s = hold(s_.smoothing_s, 0.0f, 3600.0f);
    s_.deadband_pct = hold(s_.deadband_pct, 0.5f, 5.0f);
    if (!std::isfinite(s_.offset_m))
      s_.offset_m = 0.0f;
  }
  const AltitudeSettings &settings() const { return s_; }

  // Ignores NaN and implausible values. The first valid sample initialises the filter.
  void sample(uint32_t now_ms, float altitude_m) {
    if (std::isnan(altitude_m) || altitude_m < -500.0f || altitude_m > 9000.0f)
      return;
    if (!valid_) {
      smoothed_ = altitude_m;
      valid_ = true;
    } else {
      float tau = s_.smoothing_s;
      float dt = float(uint32_t(now_ms - last_ms_)) / 1000.0f;
      if (dt > 10.0f * tau)
        dt = 10.0f * tau;
      float alpha = tau > 0.0f ? 1.0f - std::exp(-dt / tau) : 1.0f;
      smoothed_ += alpha * (altitude_m - smoothed_);
    }
    last_ms_ = now_ms;
  }

  bool valid() const { return valid_; }
  bool active() const { return s_.enabled && valid_; }  // trimming: on, with an altitude
  float smoothed_m() const { return valid_ ? smoothed_ : NAN; }

  // Unquantised trim in percent (negative = less fuel).
  float ideal_trim() const {
    if (!s_.enabled || !valid_)
      return 0.0f;
    float t = -smoothed_ / 1000.0f * s_.pct_per_1000m;  // from sea level
    return hold(t, -s_.max_reduction, s_.max_increase);
  }

  // Whole percent with hysteresis, so the value sent to the heater does not chatter.
  int trim() {
    if (!s_.enabled || !valid_) {
      held_ = 0;
      return 0;
    }
    float ideal = ideal_trim();
    if (std::fabs(ideal - float(held_)) >= s_.deadband_pct)
      held_ = int(std::lround(ideal));
    return held_;
  }

 protected:
  static float hold(float v, float lo, float hi) { return !(v >= lo) ? lo : (v > hi ? hi : v); }

  AltitudeSettings s_;
  bool valid_{false};
  float smoothed_{0.0f};
  uint32_t last_ms_{0};
  int held_{0};
};

}  // namespace flamekeeper
}  // namespace esphome
