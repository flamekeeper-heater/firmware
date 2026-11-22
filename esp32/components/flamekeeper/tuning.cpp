#include "tuning.h"

#include <cmath>
#include <string>

namespace esphome {
namespace flamekeeper {

namespace {

using pflag::BOARD;
using G = ParamGroup;
using L = ParamLevel;

struct Entry {
  ParamInfo info;
  const char *help;  // PARAM_HELP_MAX bytes at most (tests/test_controller.cpp checks)
};

// Raw value = the shown value x 10^decimals. Ranges are the ones the Home Assistant numbers had.
constexpr Entry TABLE[] = {
    {{T_FUEL_TRIM, BOARD, Unit::PERCENT, 0, -30, 30, 1, "Fuel trim", G::FUEL_PUMP, L::BASIC},
     "Adds or removes fuel at every power level, at once. Lower it if the exhaust smokes; raise it if the flame goes out."},
    {{T_FAN_TRIM, BOARD, Unit::RPM, 0, -1000, 1000, 50, "Fan trim", G::FAN, L::BASIC},
     "Adds or removes fan speed at every power level, at once. More air burns cleaner; less is quieter."},
    {{T_BAND, BOARD, Unit::CELSIUS_DIFFERENCE, 1, 5, 100, 5, "Thermostat band", G::THERMOSTAT, L::BASIC},
     "How far below the target the room must be for full power. Smaller reacts harder; larger is gentler."},
    {{T_INTEGRAL, BOARD, Unit::MINUTES, 0, 0, 120, 1, "Thermostat integral time", G::THERMOSTAT, L::EXPERT},
     "Slowly adds power while the room stays below the target. Shorter catches up sooner but may overshoot; 0 is off."},
    {{T_MIN_POWER, BOARD, Unit::PERCENT, 0, 0, 100, 1, "Thermostat min power", G::THERMOSTAT, L::BASIC},
     "The least power the thermostat asks for. Higher keeps the heater cleaner; lower holds a mild room more evenly."},
    {{T_MAX_POWER, BOARD, Unit::PERCENT, 0, 0, 100, 1, "Thermostat max power", G::THERMOSTAT, L::BASIC},
     "The most power the heater runs at in temperature mode. Lower is quieter and uses less fuel, but warms slower."},
    {{T_RAMP_UP, BOARD, Unit::PERCENT, 0, 0, 200, 1, "Thermostat ramp up per minute", G::THERMOSTAT, L::EXPERT},
     "How fast the thermostat may raise the power, per minute. Slower is gentler on the heater; 0 is no limit."},
    {{T_RAMP_DOWN, BOARD, Unit::PERCENT, 0, 0, 200, 1, "Thermostat ramp down per minute", G::THERMOSTAT, L::EXPERT},
     "How fast the thermostat may lower the power, per minute. Slower keeps it from hunting; 0 is no limit."},
    {{T_CYCLE, BOARD, Unit::ON_OFF, 0, 0, 1, 1, "Auto start/stop", G::THERMOSTAT, L::BASIC},
     "Lets the thermostat stop the heater when the room is warm enough, and start it again when the room cools."},
    {{T_STOP_ABOVE, BOARD, Unit::CELSIUS_DIFFERENCE, 1, 0, 50, 1, "Thermostat stop above target", G::THERMOSTAT,
      L::ADVANCED},
     "With auto start/stop: the heater stops once the room is this much above the target."},
    {{T_START_BELOW, BOARD, Unit::CELSIUS_DIFFERENCE, 1, 0, 50, 1, "Thermostat start below target", G::THERMOSTAT,
      L::ADVANCED},
     "With auto start/stop: the heater starts again once the room is this much below the target."},
    {{T_MIN_RUN, BOARD, Unit::MINUTES, 0, 0, 120, 1, "Thermostat min run time", G::THERMOSTAT, L::ADVANCED},
     "With auto start/stop: the heater runs at least this long before the thermostat may stop it, so it burns clean."},
    {{T_MIN_OFF, BOARD, Unit::MINUTES, 0, 0, 120, 1, "Thermostat min off time", G::THERMOSTAT, L::ADVANCED},
     "With auto start/stop: the heater stays off at least this long before the thermostat may start it again."},
    {{T_FALLBACK, BOARD, Unit::PERCENT, 0, 0, 100, 1, "Power without room temperature", G::THERMOSTAT,
      L::ADVANCED},
     "The power the heater runs at while no room temperature is known, for example when the sensor stops reporting."},
    {{A_ENABLED, BOARD, Unit::ON_OFF, 0, 0, 1, 1, "Altitude fuel trim", G::ALTITUDE, L::BASIC},
     "Takes fuel away as the altitude rises (from the board's pressure sensor), so the heater burns clean in thin air."},
    {{A_OFFSET, BOARD, Unit::METRES, 0, -2000, 2000, 10, "Altitude offset", G::ALTITUDE, L::BASIC},
     "Added to the altitude the board's pressure sensor reads, to make it the real one. The trim counts from sea level."},
    {{A_PER_1000, BOARD, Unit::PERCENT, 1, 0, 300, 5, "Altitude fuel reduction per 1000 m", G::ALTITUDE, L::ADVANCED},
     "Fuel taken away for every 1000 m above sea level."},
    {{A_MAX_REDUCTION, BOARD, Unit::PERCENT, 0, 0, 30, 1, "Altitude max fuel reduction", G::ALTITUDE, L::ADVANCED},
     "The most fuel the altitude trim may take away."},
    {{A_MAX_INCREASE, BOARD, Unit::PERCENT, 0, 0, 30, 1, "Altitude max fuel increase", G::ALTITUDE, L::ADVANCED},
     "The most fuel the altitude trim may add below sea level. 0 never adds fuel."},
    {{A_SMOOTHING, BOARD, Unit::SECONDS, 0, 0, 3600, 10, "Altitude smoothing time", G::ALTITUDE, L::EXPERT},
     "Averages the altitude over about this long, so weather and gusts do not move the fuel. 0 is no averaging."},
    {{A_DEADBAND, BOARD, Unit::PERCENT, 1, 5, 50, 5, "Altitude trim deadband", G::ALTITUDE, L::EXPERT},
     "The trim changes only once the ideal trim is this far from it, so the fuel does not step back and forth."},
    {{F_PUMP_SIZE, BOARD, Unit::MILLILITRES, 1, 100, 800, 1, "Fuel pump size", G::FUEL_USE,
      L::ADVANCED},
     "Fuel the pump moves in 1000 strokes, for the fuel meters. The common pump is 22 mL; a 28 mL pump is also sold."},
    {{F_TANK, BOARD, Unit::LITRES, 1, 0, 2000, 1, "Fuel tank size", G::FUEL_USE, L::BASIC},
     "How much the fuel tank holds, to estimate the fuel left. 0 turns the estimate off."},
    {{F_LOW_ALERT, BOARD, Unit::PERCENT, 0, 0, 50, 1, "Low fuel alert", G::FUEL_USE, L::BASIC},
     "Alerts when the estimated fuel left falls to this. 0 never alerts."},
    {{F_STOP_AT, BOARD, Unit::PERCENT, 0, 0, 50, 1, "Stop at fuel left", G::FUEL_USE, L::BASIC},
     "Stops the heater and refuses starts at this much fuel left, before the pump draws air. 0 never stops."},
    {{FR_ENABLED, BOARD, Unit::ON_OFF, 0, 0, 1, 1, "Frost protection", G::FROST, L::BASIC},
     "Starts the heater by itself when the room gets this cold, and stops it once the room is warmer again."},
    {{FR_TEMP, BOARD, Unit::CELSIUS, 1, 10, 150, 1, "Frost protection temperature", G::FROST, L::BASIC},
     "The room temperature at which frost protection starts the heater."},
    {{FR_STOP_ABOVE, BOARD, Unit::CELSIUS_DIFFERENCE, 1, 5, 100, 1, "Frost protection stop above", G::FROST,
      L::BASIC},
     "Frost protection stops the heater once the room is this much above its temperature."},
    {{O_PROBE, BOARD, Unit::CELSIUS_DIFFERENCE, 1, -50, 50, 1, "Board probe offset", G::ROOM, L::ADVANCED},
     "Added to the board's own probe, to match a thermometer you trust."},
    {{O_HOME_ASSISTANT, BOARD, Unit::CELSIUS_DIFFERENCE, 1, -50, 50, 1, "Home Assistant temperature offset", G::ROOM,
      L::ADVANCED},
     "Added to the room temperature from Home Assistant."},
    {{O_BLUETOOTH, BOARD, Unit::CELSIUS_DIFFERENCE, 1, -50, 50, 1, "Bluetooth thermometer offset", G::ROOM,
      L::ADVANCED},
     "Added to the Bluetooth thermometer's readings."},
    {{B_CUTOFF, BOARD, Unit::VOLTS, 1, 0, 280, 1, "Low battery cut-off", G::BATTERY, L::BASIC},
     "Below this the heater is stopped and will not start, to keep the battery able to restart it. 0 is off."},
    {{B_DELAY, BOARD, Unit::SECONDS, 0, 10, 300, 5, "Low battery delay", G::BATTERY, L::ADVANCED},
     "How long the battery must stay low before the heater is stopped, so a short dip does not stop it."},
    {{M_REMINDER, BOARD, Unit::HOURS, 0, 0, 500, 5, "Carbon burn-off reminder", G::MAINTENANCE, L::BASIC},
     "Reminds you to run a carbon burn-off after this many hours at low power, which soots the heater. 0 never."},
    {{M_LENGTH, BOARD, Unit::MINUTES, 0, 10, 30, 1, "Carbon burn-off length", G::MAINTENANCE, L::ADVANCED},
     "How long a carbon burn-off run lasts, at full power."},
    {{M_LOW_POWER, BOARD, Unit::PERCENT, 0, 10, 80, 5, "Low power for the reminder", G::MAINTENANCE, L::EXPERT},
     "Running at or below this power counts toward the carbon burn-off reminder."},
    {{M_GLOW_DRIFT, BOARD, Unit::PERCENT, 0, 0, 80, 5, "Glow plug drift alert", G::MAINTENANCE, L::ADVANCED},
     "Alerts when the glow plug's resistance moves this far from when it was new: it may be wearing out. 0 never."},
};
constexpr bool helps_fit() {
  for (const Entry &e : TABLE)
    if (std::char_traits<char>::length(e.help) > PARAM_HELP_MAX)
      return false;
  return true;
}
static_assert(helps_fit(), "a parameter description is longer than PARAM_HELP_MAX");

const size_t TABLE_SIZE = sizeof(TABLE) / sizeof(TABLE[0]);

// The settings in extra_, in its order.
const uint16_t EXTRA_IDS[Tuning::EXTRA] = {
    F_PUMP_SIZE, F_TANK,           F_LOW_ALERT, F_STOP_AT, FR_ENABLED, FR_TEMP,
    FR_STOP_ABOVE, O_PROBE,        O_HOME_ASSISTANT, O_BLUETOOTH,
    B_CUTOFF,    B_DELAY,          M_REMINDER,  M_LENGTH,  M_LOW_POWER, M_GLOW_DRIFT};
int extra_index(uint16_t id) {
  for (size_t i = 0; i < Tuning::EXTRA; i++)
    if (EXTRA_IDS[i] == id)
      return int(i);
  return -1;
}

const Entry *find_entry(uint16_t id) {
  for (const Entry &e : TABLE)
    if (e.info.id == id)
      return &e;
  return nullptr;
}
const ParamInfo *find(uint16_t id) {
  const Entry *e = find_entry(id);
  return e != nullptr ? &e->info : nullptr;
}

int32_t clamp32(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

size_t Tuning::count() { return TABLE_SIZE; }

bool Tuning::info(size_t index, ParamInfo &out) {
  if (index >= TABLE_SIZE)
    return false;
  out = TABLE[index].info;
  return true;
}

const char *Tuning::help(uint16_t id) {
  const Entry *e = find_entry(id);
  return e != nullptr ? e->help : "";
}

bool Tuning::has(uint16_t id) { return find(id) != nullptr; }

int32_t Tuning::value_(uint16_t id) const {
  const ThermostatSettings &t = this->thermostat_;
  const AltitudeSettings &a = this->altitude_;
  switch (id) {
    case T_FUEL_TRIM: return this->fuel_trim_;
    case T_FAN_TRIM: return this->fan_trim_;
    case T_BAND: return int32_t(std::lround(t.band * 10.0f));
    case T_INTEGRAL: return int32_t(std::lround(t.integral_min));
    case T_MIN_POWER: return int32_t(std::lround(t.min_power));
    case T_MAX_POWER: return int32_t(std::lround(t.max_power));
    case T_RAMP_UP: return int32_t(std::lround(t.ramp_up));
    case T_RAMP_DOWN: return int32_t(std::lround(t.ramp_down));
    case T_CYCLE: return t.cycle ? 1 : 0;
    case T_STOP_ABOVE: return int32_t(std::lround(t.stop_above * 10.0f));
    case T_START_BELOW: return int32_t(std::lround(t.start_below * 10.0f));
    case T_MIN_RUN: return int32_t(t.min_run_s / 60);
    case T_MIN_OFF: return int32_t(t.min_off_s / 60);
    case T_FALLBACK: return int32_t(std::lround(t.fallback_power));
    case A_ENABLED: return a.enabled ? 1 : 0;
    case A_OFFSET: return int32_t(std::lround(a.offset_m));
    case A_PER_1000: return int32_t(std::lround(a.pct_per_1000m * 10.0f));
    case A_MAX_REDUCTION: return int32_t(std::lround(a.max_reduction));
    case A_MAX_INCREASE: return int32_t(std::lround(a.max_increase));
    case A_SMOOTHING: return int32_t(std::lround(a.smoothing_s));
    case A_DEADBAND: return int32_t(std::lround(a.deadband_pct * 10.0f));
    default: {
      int i = extra_index(id);
      return i >= 0 ? this->extra_[i] : 0;
    }
  }
}

void Tuning::put_(uint16_t id, int32_t v) {
  ThermostatSettings &t = this->thermostat_;
  AltitudeSettings &a = this->altitude_;
  switch (id) {
    case T_FUEL_TRIM: this->fuel_trim_ = v; break;
    case T_FAN_TRIM: this->fan_trim_ = v; break;
    case T_BAND: t.band = v / 10.0f; break;
    case T_INTEGRAL: t.integral_min = float(v); break;
    case T_MIN_POWER: t.min_power = float(v); break;
    case T_MAX_POWER: t.max_power = float(v); break;
    case T_RAMP_UP: t.ramp_up = float(v); break;
    case T_RAMP_DOWN: t.ramp_down = float(v); break;
    case T_CYCLE: t.cycle = v != 0; break;
    case T_STOP_ABOVE: t.stop_above = v / 10.0f; break;
    case T_START_BELOW: t.start_below = v / 10.0f; break;
    case T_MIN_RUN: t.min_run_s = uint32_t(v) * 60; break;
    case T_MIN_OFF: t.min_off_s = uint32_t(v) * 60; break;
    case T_FALLBACK: t.fallback_power = float(v); break;
    case A_ENABLED: a.enabled = v != 0; break;
    case A_OFFSET: a.offset_m = float(v); break;
    case A_PER_1000: a.pct_per_1000m = v / 10.0f; break;
    case A_MAX_REDUCTION: a.max_reduction = float(v); break;
    case A_MAX_INCREASE: a.max_increase = float(v); break;
    case A_SMOOTHING: a.smoothing_s = float(v); break;
    case A_DEADBAND: a.deadband_pct = v / 10.0f; break;
    default: {
      int i = extra_index(id);
      if (i >= 0)
        this->extra_[i] = v;
      break;
    }
  }
}

bool Tuning::get(uint16_t id, ParamValue &out) const {
  if (find(id) == nullptr)
    return false;
  out = ParamValue();
  out.known = true;
  out.value = this->value_(id);
  return true;
}

Outcome Tuning::set(uint16_t id, int32_t value) {
  const ParamInfo *p = find(id);
  if (p == nullptr)
    return Outcome::refuse(Result::OUT_OF_RANGE, "no such parameter");
  if (value < p->min || value > p->max || (p->step > 1 && (value - p->min) % p->step != 0))
    return Outcome::refuse(Result::OUT_OF_RANGE, "value out of range for this parameter");
  this->put_(id, value);
  return Outcome::ok();
}

void Tuning::save(Store &out) const {
  out = Store{};
  out.check = STORE_CHECK;
  for (const Entry &e : TABLE) {
    if (e.info.id == T_FUEL_TRIM || e.info.id == T_FAN_TRIM)
      continue;  // live: back to 0 after a restart
    if (out.count < STORE_MAX)
      out.items[out.count++] = StoredValue{e.info.id, this->value_(e.info.id)};
  }
}

bool Tuning::load(const Store &in) {
  if (in.check != STORE_CHECK || in.count > STORE_MAX)
    return false;
  for (size_t i = 0; i < in.count; i++) {
    const ParamInfo *p = find(in.items[i].id);
    if (p != nullptr && in.items[i].id != T_FUEL_TRIM && in.items[i].id != T_FAN_TRIM)
      this->put_(in.items[i].id, clamp32(in.items[i].value, p->min, p->max));
  }
  return true;
}

}  // namespace flamekeeper
}  // namespace esphome

namespace esphome {
namespace flamekeeper {

int32_t Tuning::extra(uint16_t id) const {
  int i = extra_index(id);
  return i >= 0 ? this->extra_[i] : 0;
}

float Tuning::offset(RoomSource source) const {
  switch (source) {
    case RoomSource::PROBE: return this->extra(O_PROBE) / 10.0f;
    case RoomSource::HOME_ASSISTANT: return this->extra(O_HOME_ASSISTANT) / 10.0f;
    case RoomSource::BLUETOOTH: return this->extra(O_BLUETOOTH) / 10.0f;
    default: return 0.0f;
  }
}

}  // namespace flamekeeper
}  // namespace esphome
