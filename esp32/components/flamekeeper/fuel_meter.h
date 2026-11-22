// Fuel and hour meters (PROTOCOL.md): fuel from the pump's strokes, burner
// and glow plug time, for this run, the last 24 hours, 7 and 30 days, the heater's lifetime, and
// the fuel used since the tank was last set. Plain C++ (host-tested in tests/test_automation.cpp).
//
// The 24 h / 7 / 30 day values need the time: hour buckets by UTC hour, day buckets by local date
// (a day number the caller works out). Without the time, fuel still goes to the run, the lifetime
// and the tank.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace esphome {
namespace flamekeeper {

class FuelMeter {
 public:
  static const uint32_t UNKNOWN = 0xFFFFFFFFu;
  static const uint32_t SAVED_CHECK = 0x464D5431UL;  // "FMT1"

  struct Hour {
    uint32_t index;  // UTC hours since 1970 (0 = empty)
    float fuel_ml;
    uint32_t burn_s;
  };
  struct Day {
    int32_t index;  // local days since 1970 (0 = empty)
    float fuel_ml;
    uint32_t burn_s;
  };
  // What is kept over restarts (in the board's preferences).
  struct Saved {
    uint32_t check;
    double life_fuel_ml;
    double life_burn_s;
    double life_glow_s;
    double tank_used_ml;
    float run_fuel_ml;
    float run_burn_s;
    float run_glow_s;
    Hour hours[24];
    Day days[31];
  };

  // What METERS reports, in mL and seconds.
  struct Totals {
    uint32_t run_fuel, run_burn, run_glow;
    uint32_t day_fuel, day_burn;      // last 24 hours
    uint32_t week_fuel, week_burn;    // last 7 days
    uint32_t month_fuel, month_burn;  // last 30 days
    uint32_t life_fuel, life_burn, life_glow;
    uint32_t tank_used;
  };

  FuelMeter() { this->clear(); }

  void clear() {
    this->s_ = Saved{};
    this->s_.check = SAVED_CHECK;
  }
  bool load(const Saved &in) {
    if (in.check != SAVED_CHECK || !std::isfinite(in.life_fuel_ml) || !std::isfinite(in.tank_used_ml))
      return false;
    this->s_ = in;
    return true;
  }
  const Saved &saved() const { return this->s_; }

  // A run starts: the run's meters start again.
  void new_run() {
    this->s_.run_fuel_ml = 0.0f;
    this->s_.run_burn_s = 0.0f;
    this->s_.run_glow_s = 0.0f;
  }

  // dt_s seconds at pump_hz (strokes per second, 0 when not pumping), ml_per_stroke each.
  // utc: 0 when the time is not known; local_day: the local date as days since 1970.
  void add(float dt_s, float pump_hz, float ml_per_stroke, bool burning, bool glowing, uint32_t utc,
           int32_t local_day) {
    if (!(dt_s > 0.0f) || dt_s > 60.0f)
      return;  // a stall or a clock jump: not fuel
    float fuel = (std::isfinite(pump_hz) && pump_hz > 0.0f && pump_hz < 20.0f) ? pump_hz * dt_s * ml_per_stroke : 0.0f;
    float burn = burning ? dt_s : 0.0f;
    float glow = glowing ? dt_s : 0.0f;
    if (fuel == 0.0f && burn == 0.0f && glow == 0.0f)
      return;
    this->s_.run_fuel_ml += fuel;
    this->s_.run_burn_s += burn;
    this->s_.run_glow_s += glow;
    this->s_.life_fuel_ml += fuel;
    this->s_.life_burn_s += burn;
    this->s_.life_glow_s += glow;
    this->s_.tank_used_ml += fuel;
    if (utc == 0)
      return;
    uint32_t hour = utc / 3600;
    Hour &h = this->s_.hours[hour % 24];
    if (h.index != hour)
      h = Hour{hour, 0.0f, 0};
    h.fuel_ml += fuel;
    h.burn_s += uint32_t(std::lround(burn));
    Day &d = this->s_.days[uint32_t(local_day) % 31];
    if (d.index != local_day)
      d = Day{local_day, 0.0f, 0};
    d.fuel_ml += fuel;
    d.burn_s += uint32_t(std::lround(burn));
  }

  // The fuel used since the tank held this much (TANK_SET).
  void set_tank(float used_ml) { this->s_.tank_used_ml = used_ml < 0.0f ? 0.0f : used_ml; }
  float tank_used_ml() const { return float(this->s_.tank_used_ml); }

  Totals totals(uint32_t utc, int32_t local_day) const {
    Totals t{};
    t.run_fuel = whole(this->s_.run_fuel_ml);
    t.run_burn = whole(this->s_.run_burn_s);
    t.run_glow = whole(this->s_.run_glow_s);
    t.life_fuel = whole(this->s_.life_fuel_ml);
    t.life_burn = whole(this->s_.life_burn_s);
    t.life_glow = whole(this->s_.life_glow_s);
    t.tank_used = whole(this->s_.tank_used_ml);
    if (utc == 0) {
      t.day_fuel = t.day_burn = t.week_fuel = t.week_burn = t.month_fuel = t.month_burn = UNKNOWN;
      return t;
    }
    const uint32_t hour = utc / 3600;
    double f = 0, b = 0;
    for (const Hour &h : this->s_.hours) {
      if (h.index != 0 && h.index <= hour && hour - h.index < 24) {
        f += h.fuel_ml;
        b += h.burn_s;
      }
    }
    t.day_fuel = whole(f);
    t.day_burn = whole(b);
    double wf = 0, wb = 0, mf = 0, mb = 0;
    for (const Day &d : this->s_.days) {
      if (d.index == 0 || d.index > local_day)
        continue;
      int32_t age = local_day - d.index;
      if (age < 7) {
        wf += d.fuel_ml;
        wb += d.burn_s;
      }
      if (age < 30) {
        mf += d.fuel_ml;
        mb += d.burn_s;
      }
    }
    t.week_fuel = whole(wf);
    t.week_burn = whole(wb);
    t.month_fuel = whole(mf);
    t.month_burn = whole(mb);
    return t;
  }

 protected:
  static uint32_t whole(double v) {
    if (!(v > 0.0))
      return 0;
    return v >= 4294967294.0 ? 0xFFFFFFFEu : uint32_t(std::lround(v));
  }
  Saved s_;
};

}  // namespace flamekeeper
}  // namespace esphome
