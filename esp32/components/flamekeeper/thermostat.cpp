#include "thermostat.h"

#include <cmath>

namespace esphome {
namespace flamekeeper {

static float hold(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Negative and NaN both become 0.
static float non_negative(float v) { return v >= 0.0f ? v : 0.0f; }

// A gap longer than this (stalled loop, first call) must not make the integral or slew jump.
static const float MAX_DT_S = 10.0f;

void Thermostat::set(const ThermostatSettings &in) {
  ThermostatSettings s = in;
  s.band = s.band >= 0.1f ? s.band : 0.1f;
  s.integral_min = non_negative(s.integral_min);
  s.min_power = hold(non_negative(s.min_power), 0.0f, 100.0f);
  s.max_power = hold(non_negative(s.max_power), 0.0f, 100.0f);
  if (s.max_power < s.min_power)
    s.max_power = s.min_power;
  s.ramp_up = non_negative(s.ramp_up);
  s.ramp_down = non_negative(s.ramp_down);
  s.stop_above = non_negative(s.stop_above);
  s.start_below = non_negative(s.start_below);
  s.fallback_power = hold(non_negative(s.fallback_power), 0.0f, 100.0f);
  s_ = s;
  demand_ = clamp_power(demand_);
}

float Thermostat::clamp_power(float p) const { return hold(p, s_.min_power, s_.max_power); }

void Thermostat::enable(uint32_t now_ms, float room, float target, float initial_demand) {
  enabled_ = true;
  integral_ = 0.0f;
  demand_ = clamp_power(std::isnan(initial_demand) ? s_.min_power : initial_demand);
  last_ms_ = now_ms;
  want_heat_ = true;
  heat_since_ms_ = now_ms;
  off_expired_ = false;
  if (s_.cycle && !std::isnan(room) && room >= target + s_.stop_above) {
    // Already too warm: wait for the room to cool, with no minimum off time on top of that.
    want_heat_ = false;
    off_since_ms_ = now_ms;
    off_expired_ = true;
  }
}

void Thermostat::disable() {
  enabled_ = false;
  want_heat_ = false;
}

ThermostatOutput Thermostat::update(uint32_t now_ms, float room, float target, bool burning) {
  if (!enabled_)
    return {false, s_.min_power};

  float dt = float(uint32_t(now_ms - last_ms_)) / 1000.0f;
  if (dt > MAX_DT_S)
    dt = MAX_DT_S;
  last_ms_ = now_ms;

  bool known = !std::isnan(room);

  if (s_.cycle && known) {
    if (want_heat_) {
      uint64_t run_ms = uint32_t(now_ms - heat_since_ms_);
      if (room >= target + s_.stop_above && run_ms >= uint64_t(s_.min_run_s) * 1000u) {
        want_heat_ = false;
        off_since_ms_ = now_ms;
        off_expired_ = false;
      }
    } else if (room <= target - s_.start_below) {
      uint64_t off_ms = uint32_t(now_ms - off_since_ms_);
      if (off_expired_ || off_ms >= uint64_t(s_.min_off_s) * 1000u) {
        want_heat_ = true;
        heat_since_ms_ = now_ms;
        integral_ = 0.0f;
        demand_ = s_.min_power;
      }
    }
  }

  if (!want_heat_)
    return {false, demand_};

  float wanted;
  if (known) {
    float error = target - room;
    float p = error / s_.band;
    // Anti-windup: do not integrate further into a saturated output.
    if (s_.integral_min > 0.0f && burning) {
      float unslewed = p + integral_;
      bool pushing_up = error > 0.0f && unslewed >= 1.0f;
      bool pushing_down = error < 0.0f && unslewed <= 0.0f;
      if (!pushing_up && !pushing_down)
        integral_ = hold(integral_ + p * dt / (s_.integral_min * 60.0f), 0.0f, 1.0f);
    }
    float raw = hold(p + integral_, 0.0f, 1.0f);
    wanted = s_.min_power + raw * (s_.max_power - s_.min_power);
  } else {
    wanted = clamp_power(s_.fallback_power);
  }

  if (burning) {
    float ramp = wanted > demand_ ? s_.ramp_up : s_.ramp_down;
    if (ramp <= 0.0f) {
      demand_ = wanted;
    } else {
      float step = ramp * dt / 60.0f;
      if (wanted > demand_)
        demand_ = wanted - demand_ <= step ? wanted : demand_ + step;
      else
        demand_ = demand_ - wanted <= step ? wanted : demand_ - step;
    }
  }
  demand_ = clamp_power(demand_);
  return {true, demand_};
}

}  // namespace flamekeeper
}  // namespace esphome
