// The ESP32's own settings, shown to the apps and Home Assistant next to the heater's parameters
// (DESIGN.md): the live fuel and fan trims, the thermostat tuning
// and the altitude fuel trim. Ids from 0x100, the same on every board; the heater's own parameters
// come from the heater (over the link).
//
// Every setting but the live trims is stored, as (id, value) pairs, so a setting added later starts
// at its default and the others keep their values. Plain C++ (host-tested in tests/test_controller.cpp).
#pragma once

#include <cstddef>
#include <cstdint>

#include "altitude_trim.h"
#include "heater.h"
#include "thermostat.h"

namespace esphome {
namespace flamekeeper {

enum TuningId : uint16_t {
  T_FUEL_TRIM = 0x100,  // live: back to 0 after a restart
  T_FAN_TRIM = 0x101,
  T_BAND = 0x110,
  T_INTEGRAL,
  T_MIN_POWER,
  T_MAX_POWER,
  T_RAMP_UP,
  T_RAMP_DOWN,
  T_CYCLE,
  T_STOP_ABOVE,
  T_START_BELOW,
  T_MIN_RUN,
  T_MIN_OFF,
  T_FALLBACK,
  A_ENABLED = 0x120,
  A_OFFSET,
  A_PER_1000,
  A_MAX_REDUCTION,
  A_MAX_INCREASE,
  A_SMOOTHING,
  A_DEADBAND,
  F_PUMP_SIZE = 0x130,  // mL per 1000 strokes, x10
  F_TANK,               // L, x10; 0 = no tank tracking
  F_LOW_ALERT,          // %
  F_STOP_AT,            // %
  FR_ENABLED = 0x140,
  FR_TEMP,              // C x10
  FR_STOP_ABOVE,        // C x10
  O_PROBE = 0x150,      // C x10
  O_HOME_ASSISTANT,
  O_BLUETOOTH,
  B_CUTOFF = 0x160,  // V x10, 0 = off
  B_DELAY,           // s
  M_REMINDER = 0x170,  // h, 0 = never
  M_LENGTH,            // min
  M_LOW_POWER,         // %
  M_GLOW_DRIFT,        // %, 0 = never
};

class Tuning {
 public:
  // The stored record: room for every setting there may be (unknown ids are skipped on load).
  static const size_t STORE_MAX = 64;
  static const uint32_t STORE_KEY = 0x464B5354UL;  // "FKST"
  static const uint32_t STORE_CHECK = 0x464B5301;
  struct StoredValue {
    uint16_t id;
    int32_t value;
  };
  struct Store {
    uint32_t check;
    uint16_t count;
    StoredValue items[STORE_MAX];
  };
  // The settings kept in extra_ (all but the thermostat, altitude and trims).
  static const size_t EXTRA = 16;

  static size_t count();
  static bool info(size_t index, ParamInfo &out);
  static bool has(uint16_t id);
  static const char *help(uint16_t id);  // "" for an id not in the table

  bool get(uint16_t id, ParamValue &out) const;
  // Range-checked; applies at once.
  Outcome set(uint16_t id, int32_t value);

  const ThermostatSettings &thermostat() const { return this->thermostat_; }
  const AltitudeSettings &altitude() const { return this->altitude_; }
  int fuel_trim() const { return this->fuel_trim_; }
  int fan_trim() const { return this->fan_trim_; }

  // The other settings, raw (the table's scale).
  int32_t extra(uint16_t id) const;
  float pump_ml_per_stroke() const { return this->extra(F_PUMP_SIZE) / 10000.0f; }
  float tank_ml() const { return this->extra(F_TANK) * 100.0f; }
  float offset(RoomSource source) const;

  void save(Store &out) const;
  // False (defaults kept) when the record is missing or damaged; values are held to their ranges.
  bool load(const Store &in);

 protected:
  int32_t value_(uint16_t id) const;
  void put_(uint16_t id, int32_t v);

  ThermostatSettings thermostat_;
  AltitudeSettings altitude_;
  int fuel_trim_{0};
  int fan_trim_{0};
  int32_t extra_[EXTRA] = {220, 0, 20, 10, 0, 50, 30, 0, 0, 0,  // in EXTRA_IDS order
                           115, 30, 50, 20, 40, 30};
};

}  // namespace flamekeeper
}  // namespace esphome
