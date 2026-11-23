#include "controller.h"

#include <cctype>
#include <cstdio>

namespace esphome {
namespace flamekeeper {

float RoomTemperature::reading(RoomSource source, uint32_t now) const {
  float v = NAN;
  switch (source) {
    case RoomSource::PROBE:
      if (this->probe_ms_ != 0 && uint32_t(now - this->probe_ms_) < PROBE_STALE_MS)
        v = this->probe_c_;
      break;
    case RoomSource::HOME_ASSISTANT: v = this->ha_c_; break;
    case RoomSource::BLUETOOTH:
      if (this->bt_ms_ != 0 && uint32_t(now - this->bt_ms_) < BLUETOOTH_STALE_MS)
        v = this->bt_c_;
      break;
    default: return NAN;
  }
  return std::isnan(v) ? NAN : v + this->offset_[uint8_t(source)];
}

float RoomTemperature::value(uint32_t now, bool *fallback) const {
  float chosen = this->reading(this->source_, now);
  *fallback = false;
  if (!std::isnan(chosen))
    return chosen;
  for (RoomSource s : {RoomSource::PROBE, RoomSource::HOME_ASSISTANT, RoomSource::BLUETOOTH}) {
    if (s == this->source_)
      continue;
    float v = this->reading(s, now);
    if (!std::isnan(v)) {
      *fallback = true;
      return v;
    }
  }
  return NAN;
}

Controller::Controller(Heater *heater) : heater_(heater) {
  heater->set_listener(this);
  this->apply_tuning_();
  this->refresh_();
}

void Controller::note_(const char *what, const Outcome &o) {
  for (auto &cb : this->request_cbs_)
    cb(Source::BOARD, what, o);
}

Outcome Controller::finish_(const char *what, Source source, const Outcome &outcome) {
  this->refresh_();
  for (auto &cb : this->request_cbs_)
    cb(source, what, outcome);
  return outcome;
}

// What the heater would refuse a start for, from its status (for a heat run the thermostat holds off
// from the start: it must be one the heater could start now).
static Outcome start_refusal(const HeaterStatus &hs) {
  if (hs.update_running)
    return Outcome::refuse(Result::BUSY, "the heater firmware is being updated");
  if (!hs.answering)
    return Outcome::refuse(Result::NO_REPLY, "the heater does not answer");
  if (hs.state != HeaterState::OFF)
    return Outcome::refuse(Result::NOT_OFF, "the heater is not off yet (wait for the cooldown)");
  return Outcome::ok();
}

Outcome Controller::set_mode(Mode mode, Source source) {
  const Outcome o = this->set_mode_(mode, source);
  if (mode == Mode::OFF && o.accepted())
    for (auto &cb : this->stop_cbs_)
      cb(source);
  return o;
}

Outcome Controller::set_mode_(Mode mode, Source source) {
  static const char *const WHAT[] = {"off", "heat", "fan only", "priming"};
  uint8_t m = uint8_t(mode);
  if (m > uint8_t(Mode::PRIME))
    return this->finish_("mode", source, Outcome::refuse(Result::OUT_OF_RANGE, "unknown mode"));
  uint32_t caps = this->heater_->capabilities();
  if (mode == Mode::FAN && !(caps & cap::FAN_ONLY))
    return this->finish_(WHAT[m], source, Outcome::refuse(Result::NOT_SUPPORTED, "this heater has no fan-only mode"));
  if (mode == Mode::PRIME && !(caps & cap::PRIME))
    return this->finish_(WHAT[m], source, Outcome::refuse(Result::NOT_SUPPORTED, "this heater cannot prime"));

  HeaterStatus before;
  this->heater_->read_status(before);
  const Mode was = this->user_mode_(before);
  if (was == Mode::OFF && mode != Mode::OFF && this->board_updating_)
    return this->finish_(WHAT[m], source,
                         Outcome::refuse(Result::BUSY, "the board is installing a firmware update"));
  if (was == Mode::OFF && mode != Mode::OFF && this->start_check_) {
    Outcome refused = this->start_check_(source);
    if (!refused.accepted())
      return this->finish_(WHAT[m], source, refused);
  }
  Outcome outcome = Outcome::ok();
  if (mode == Mode::OFF) {
    this->thermo_stopped_ = false;
    this->thermostat_.disable();
    outcome = this->heater_->set_mode(Mode::OFF);
  } else if (this->thermo_stopped_ && mode == Mode::HEAT) {
    // Already a heat run, the thermostat holding the heater off.
  } else if (mode == Mode::HEAT && this->heat_mode_ == HeatMode::TEMPERATURE && before.mode == Mode::OFF) {
    bool fallback = false;
    float room = this->room_.value(this->now_, &fallback);
    this->thermostat_.enable(this->now_, room, this->target_c_, float(this->power_));
    if (!this->thermostat_.want_heat()) {
      // Warm enough already: the run is on, the heater starts when the room cools down; only if the
      // heater could start now.
      outcome = start_refusal(before);
      if (outcome.accepted())
        this->thermo_stopped_ = true;
      else
        this->thermostat_.disable();
    } else {
      this->demand_sent_ = int(std::lround(this->thermostat_.demand()));
      this->heater_->set_power_demand(uint8_t(this->demand_sent_));
      outcome = this->heater_->set_mode(Mode::HEAT);
      if (!outcome.accepted())
        this->thermostat_.disable();
    }
  } else {
    this->thermo_stopped_ = false;
    if (mode != Mode::HEAT || this->heat_mode_ == HeatMode::POWER)
      this->heater_->set_power_demand(this->power_);  // fixed power, or the fan speed
    outcome = this->heater_->set_mode(mode);
  }
  HeaterStatus after;
  this->heater_->read_status(after);
  // A run starts when the heater goes from off to anything else; its source is kept until it is off
  // again, whatever changes on the way (power, target, heat <-> fan, thermostat cycles).
  if (outcome.accepted() && was == Mode::OFF && this->user_mode_(after) != Mode::OFF) {
    this->run_source_ = source;
    if (++this->run_number_ == 0)
      this->run_number_ = 1;  // never 0: State's run id is 0 only while off
    this->run_start_ms_ = this->now_;
    this->run_shown_ = true;
    this->run_shown_source_ = source;
    this->run_stop_ms_ = 0;
    this->failsafe_stopped_ = false;
    this->ha_lost_ms_ = 0;
  }
  return this->finish_(WHAT[m], source, outcome);
}

Outcome Controller::set_heat_mode(HeatMode mode, Source source) {
  if (uint8_t(mode) > uint8_t(HeatMode::TEMPERATURE))
    return this->finish_("heat mode", source, Outcome::refuse(Result::OUT_OF_RANGE, "unknown heat mode"));
  // The controller runs the thermostat; the heater always gets a power demand.
  this->heat_mode_ = mode;
  this->demand_sent_ = -1;
  if (mode == HeatMode::POWER) {
    this->thermostat_.disable();
    this->heater_->set_power_demand(this->power_);
  }
  return this->finish_("heat mode", source, Outcome::ok());
}

Outcome Controller::set_power(int percent, Source source) {
  if (percent < 0 || percent > 100)
    return this->finish_("power", source, Outcome::refuse(Result::OUT_OF_RANGE, "the power is 0 to 100 %"));
  this->power_ = uint8_t(percent);
  HeaterStatus hs;
  this->heater_->read_status(hs);
  // In temperature mode the thermostat sets the demand of a heat run; the setting is kept for later.
  if (this->heat_mode_ == HeatMode::TEMPERATURE && this->user_mode_(hs) == Mode::HEAT)
    return this->finish_("power", source, Outcome::ok());
  return this->finish_("power", source, this->heater_->set_power_demand(uint8_t(percent)));
}

Outcome Controller::set_target(float celsius, Source source) {
  if (std::isnan(celsius) || celsius < this->heater_->min_target() - 0.01f ||
      celsius > this->heater_->max_target() + 0.01f)
    return this->finish_("target temperature", source,
                         Outcome::refuse(Result::OUT_OF_RANGE, "target temperature out of range"));
  this->target_c_ = celsius;
  return this->finish_("target temperature", source, this->heater_->set_target(celsius));
}

Outcome Controller::set_room_source(RoomSource room, Source source) {
  if (uint8_t(room) > uint8_t(RoomSource::BLUETOOTH))
    return this->finish_("room temperature source", source, Outcome::refuse(Result::OUT_OF_RANGE, "unknown source"));
  if (room == RoomSource::BLUETOOTH && !this->bluetooth_room_)
    return this->finish_("room temperature source", source,
                         Outcome::refuse(Result::NOT_SUPPORTED, "this board has no Bluetooth thermometers"));
  this->room_.set_source(room);
  bool fallback = false;
  this->heater_->set_room_temperature(this->room_.value(this->now_, &fallback));  // at once, not next tick
  return this->finish_("room temperature source", source, Outcome::ok());
}

Outcome Controller::stop_prime(Source source) {
  if (!(this->heater_->capabilities() & cap::PRIME_STOP))
    return this->finish_("stop priming", source,
                         Outcome::refuse(Result::NOT_SUPPORTED, "set the heater to off to stop priming"));
  Outcome o = this->heater_->stop_prime();
  if (o.accepted())
    for (auto &cb : this->stop_cbs_)  // a stop someone asked for, like off
      cb(source);
  return this->finish_("stop priming", source, o);
}

Outcome Controller::start_firmware_update(Source source) {
  if (this->board_updating_)
    return this->finish_("heater firmware update", source,
                         Outcome::refuse(Result::BUSY, "the board is installing a firmware update"));
  return this->finish_("heater firmware update", source, this->heater_->start_firmware_update());
}

Outcome Controller::param_set(uint16_t id, int32_t value, Source source) {
  if (Tuning::has(id)) {
    Outcome o = this->tuning_.set(id, value);
    if (o.result == Result::OK) {
      this->apply_tuning_();
      if (id != T_FUEL_TRIM && id != T_FAN_TRIM)
        for (auto &cb : this->tuning_cbs_)
          cb(this->tuning_);
      for (auto &cb : this->event_cbs_)
        cb(Event::PARAM_CHANGED, Result::OK, id, "");
    }
    return this->finish_("parameter", source, o);
  }
  if (!(this->heater_->capabilities() & cap::PARAMETERS))
    return this->finish_("parameter", source, Outcome::refuse(Result::NOT_SUPPORTED, "this heater has no parameters"));
  return this->finish_("parameter", source, this->heater_->param_set(id, value));
}

Outcome Controller::param_apply(bool save, Source source) {
  if (!(this->heater_->capabilities() & cap::PARAMETERS))
    return this->finish_(save ? "save parameters" : "apply parameters", source,
                         Outcome::refuse(Result::NOT_SUPPORTED, "this heater has no parameters"));
  return this->finish_(save ? "save parameters" : "apply parameters", source, this->heater_->param_apply(save));
}

Outcome Controller::param_discard(Source source) {
  if (!(this->heater_->capabilities() & cap::PARAMETERS))
    return this->finish_("discard parameter changes", source,
                         Outcome::refuse(Result::NOT_SUPPORTED, "this heater has no parameters"));
  return this->finish_("discard parameter changes", source, this->heater_->param_discard());
}

Outcome Controller::reset_fault(Source source) {
  return this->finish_("reset heater fault", source, this->heater_->reset_fault());
}

size_t Controller::param_count() const {
  size_t n = (this->heater_->capabilities() & cap::PARAMETERS) ? this->heater_->param_count() : 0;
  return n + Tuning::count();
}

bool Controller::param_info(size_t index, ParamInfo &out) const {
  size_t n = (this->heater_->capabilities() & cap::PARAMETERS) ? this->heater_->param_count() : 0;
  if (index < n)
    return this->heater_->param_info(index, out);
  return Tuning::info(index - n, out);
}

bool Controller::param_get(uint16_t id, ParamValue &out) const {
  if (Tuning::has(id))
    return this->tuning_.get(id, out);
  return (this->heater_->capabilities() & cap::PARAMETERS) && this->heater_->param_get(id, out);
}

const char *Controller::param_help(uint16_t id) const {
  if (Tuning::has(id))
    return Tuning::help(id);
  ParamValue v;
  if (!(this->heater_->capabilities() & cap::PARAMETERS) || !this->heater_->param_get(id, v))
    return nullptr;
  const char *h = this->heater_->param_help(id);
  return h != nullptr ? h : "";
}

void Controller::load_tuning(const Tuning::Store &store) {
  if (this->tuning_.load(store))
    this->apply_tuning_();
}

void Controller::apply_tuning_() {
  this->thermostat_.set(this->tuning_.thermostat());
  for (RoomSource r : {RoomSource::PROBE, RoomSource::HOME_ASSISTANT, RoomSource::BLUETOOTH})
    this->room_.set_offset(r, this->tuning_.offset(r));
  this->altitude_.set(this->tuning_.altitude());
  this->send_trims_();
}

void Controller::send_trims_() {
  int fuel = this->tuning_.fuel_trim() + this->altitude_trim_;
  fuel = fuel < -30 ? -30 : (fuel > 30 ? 30 : fuel);
  int fan = this->tuning_.fan_trim();
  if (fuel == this->trims_sent_fuel_ && fan == this->trims_sent_fan_)
    return;
  this->trims_sent_fuel_ = fuel;
  this->trims_sent_fan_ = fan;
  this->heater_->set_trims(fuel, fan);
}

void Controller::set_altitude(float measured) {
  // The board's pressure sensor reads weather as altitude: the offset setting corrects it.
  const float metres = measured + this->tuning_.altitude().offset_m;
  this->altitude_m_ = metres;
  if (!std::isnan(metres)) {
    this->heater_->set_altitude(metres);
    this->altitude_.sample(this->now_, metres);
  }
}

// Once a second: in temperature mode the thermostat sets the demand of a heat run and, with cycling
// allowed, stops and restarts the heater (each restart a new start).
void Controller::thermostat_step_(uint32_t now) {
  HeaterStatus hs;
  this->heater_->read_status(hs);
  const bool heat_run = this->user_mode_(hs) == Mode::HEAT;
  if (!heat_run || this->heat_mode_ != HeatMode::TEMPERATURE) {
    if (this->thermostat_.enabled())
      this->thermostat_.disable();
    if (heat_run && this->thermo_stopped_ && hs.state == HeaterState::OFF) {
      // Switched to the power setting while the thermostat had the heater off: heat means heat.
      this->heater_->set_power_demand(this->power_);
      Outcome o = this->heater_->set_mode(Mode::HEAT);
      this->note_("restart at the power setting", o);
      if (o.accepted())
        this->thermo_stopped_ = false;
    }
    return;
  }
  bool fallback = false;
  float room = this->room_.value(now, &fallback);
  if (!this->thermostat_.enabled())
    this->thermostat_.enable(now, room, this->target_c_, hs.power_pct >= 0.0f ? hs.power_pct : float(this->power_));
  ThermostatOutput out = this->thermostat_.update(now, room, this->target_c_, hs.state == HeaterState::HEATING);
  int demand = int(std::lround(out.demand));
  if (demand != this->demand_sent_) {
    this->demand_sent_ = demand;
    this->heater_->set_power_demand(uint8_t(demand));
  }
  if (!out.want_heat && !this->thermo_stopped_ && hs.mode == Mode::HEAT &&
      (hs.state == HeaterState::STARTING || hs.state == HeaterState::HEATING)) {
    this->thermo_stopped_ = true;
    this->note_("thermostat: room above the stop point, heater off", this->heater_->set_mode(Mode::OFF));
  } else if (out.want_heat && this->thermo_stopped_ && hs.state == HeaterState::OFF && hs.fault != Fault::NONE) {
    // A fault during the off period (an overheat in the cooldown, say): the run ends; never restart.
    this->thermo_stopped_ = false;
    this->thermostat_.disable();
    this->note_("thermostat: the heater reports a fault, run ended", Outcome::refuse(Result::REFUSED, "fault"));
  } else if (out.want_heat && this->thermo_stopped_ && hs.state == HeaterState::OFF) {
    Outcome o = this->heater_->set_mode(Mode::HEAT);
    this->note_("thermostat: room below the start point, heater on", o);
    if (o.accepted())
      this->thermo_stopped_ = false;
  }
}

void Controller::tick(uint32_t now, bool ha_connected) {
  this->now_ = now;
  // A Home Assistant value is only current while the API is connected (Home Assistant sends the
  // state again when it reconnects); a stale one would keep the thermostat on an old reading.
  if (!ha_connected)
    this->room_.home_assistant(NAN);
  bool fallback = false;
  this->heater_->set_room_temperature(this->room_.value(now, &fallback));

  if (now - this->last_thermo_ms_ >= 1000) {
    this->last_thermo_ms_ = now;
    this->altitude_trim_ = this->altitude_.trim();
    this->thermostat_step_(now);
  }
  this->send_trims_();

  HeaterStatus hs;
  this->heater_->read_status(hs);
  const bool was_running = this->run_source_ != Source::NONE;
  if (this->user_mode_(hs) == Mode::OFF) {
    this->run_source_ = Source::NONE;
    this->ha_lost_ms_ = 0;
  } else if (this->run_source_ == Source::HOME_ASSISTANT && this->failsafe_ms_ != 0) {
    // Only a run started from Home Assistant stops when Home Assistant goes away.
    if (ha_connected) {
      this->ha_lost_ms_ = 0;
    } else if (this->ha_lost_ms_ == 0) {
      this->ha_lost_ms_ = now == 0 ? 1 : now;
    } else if (uint32_t(now - this->ha_lost_ms_) >= this->failsafe_ms_) {
      static const char *const WHY = "Home Assistant unreachable during a run it started";
      this->thermo_stopped_ = false;
      this->thermostat_.disable();
      this->heater_->set_mode(Mode::OFF);
      this->run_source_ = Source::NONE;
      this->ha_lost_ms_ = 0;
      this->failsafe_stopped_ = true;
      for (auto &cb : this->event_cbs_)
        cb(Event::FAILSAFE_STOP, Result::OK, 0, WHY);
    }
  } else {
    this->ha_lost_ms_ = 0;
  }
  // The run stops counting when it is asked to stop, and is shown until the heater is really off.
  if (was_running && this->run_source_ == Source::NONE)
    this->run_stop_ms_ = now == 0 ? 1 : now;
  if (this->run_shown_ && this->run_source_ == Source::NONE &&
      (!hs.answering || hs.state == HeaterState::OFF || hs.state == HeaterState::UNKNOWN))
    this->run_shown_ = false;
  this->refresh_();
}

void Controller::refresh_() {
  HeaterStatus &hs = this->status_;
  this->heater_->read_status(hs);
  Status &s = this->status_;
  // What the controller holds rather than the heater.
  s.mode = this->user_mode_(hs);
  s.heat_mode = this->heat_mode_;
  s.power = this->power_;
  s.target_c = this->target_c_;
  s.capabilities = this->heater_->capabilities() | cap::ROOM_SOURCE | cap::TEMPERATURE |
                   (this->bluetooth_room_ ? cap::BLUETOOTH_ROOM : 0) | (this->ha_room_ ? cap::HA_ROOM : 0);
  s.room_source = this->room_.source();
  s.room_probe_c = this->room_.reading(RoomSource::PROBE, this->now_);
  s.room_ha_c = this->room_.reading(RoomSource::HOME_ASSISTANT, this->now_);
  s.room_bt_c = this->room_.reading(RoomSource::BLUETOOTH, this->now_);
  bool fallback = false;
  s.room_c = this->room_.value(this->now_, &fallback);
  s.room_fallback = fallback;
  s.intake_c = this->intake_c_;
  if (std::isnan(s.altitude_m))
    s.altitude_m = this->altitude_m_;
  if (std::isnan(s.pressure_hpa))
    s.pressure_hpa = this->pressure_hpa_;
  s.run_source = this->run_source_;
  const bool shown = this->run_shown_;
  s.run_id = shown ? (uint32_t(this->boot_id_) << 16) | this->run_number_ : 0;
  const uint32_t until = this->run_stop_ms_ != 0 ? this->run_stop_ms_ : this->now_;
  s.run_s = shown ? uint32_t(until - this->run_start_ms_) / 1000 : 0;
  s.ending_source = shown && this->run_source_ == Source::NONE ? this->run_shown_source_ : Source::NONE;
  s.board_updating = this->board_updating_;
  s.failsafe_stopped = this->failsafe_stopped_;
  s.thermo_paused = this->thermo_stopped_ && this->run_source_ != Source::NONE;
  if (this->ha_lost_ms_ != 0) {
    uint32_t gone = uint32_t(this->now_ - this->ha_lost_ms_);
    s.failsafe_left_s = gone >= this->failsafe_ms_ ? 0 : (this->failsafe_ms_ - gone + 999) / 1000;
  } else {
    s.failsafe_left_s = UINT32_MAX;
  }
  for (auto &cb : this->refresh_cbs_)
    cb(s);
}

void Controller::on_heater_event(Event event, Result result, uint16_t id, const char *text) {
  for (auto &cb : this->event_cbs_)
    cb(event, result, id, text);
}

const char *source_name(Source s) {
  switch (s) {
    case Source::HOME_ASSISTANT: return "Home Assistant";
    case Source::PHONE: return "Phone";
    case Source::BOARD: return "Board";
    case Source::WIFI_APP: return "App over Wi-Fi";
    case Source::RELAY_APP: return "App over the internet";
    case Source::BROWSER: return "Web browser";
    case Source::FROST: return "Frost protection";
    case Source::TIMER: return "Timer";
    default: return "None";
  }
}

const char *heater_state_name(HeaterState s) {
  switch (s) {
    case HeaterState::OFF: return "Off";
    case HeaterState::STARTING: return "Starting";
    case HeaterState::HEATING: return "Heating";
    case HeaterState::FAN_ONLY: return "Fan only";
    case HeaterState::STOPPING: return "Stopping";
    case HeaterState::COOLING: return "Cooling down";
    case HeaterState::PRIMING: return "Priming";
    case HeaterState::UPDATING: return "Updating firmware";
    default: return "Unknown";
  }
}

const char *control_status_name(ControlStatus s) {
  switch (s) {
    case ControlStatus::IDLE: return "Idle";
    case ControlStatus::STARTING: return "Starting";
    case ControlStatus::RUNNING: return "Running";
    case ControlStatus::PRIMING: return "Priming";
    case ControlStatus::STOPPING: return "Stopping";
    case ControlStatus::NO_REPLY: return "No reply from the heater";
    case ControlStatus::UPDATING: return "Updating heater firmware";
    default: return "Unknown";
  }
}

const char *param_group_name(ParamGroup g) {
  switch (g) {
    case ParamGroup::THERMOSTAT: return "Thermostat";
    case ParamGroup::FUEL_PUMP: return "Fuel pump";
    case ParamGroup::FAN: return "Fan";
    case ParamGroup::START_UP: return "Start-up";
    case ParamGroup::GLOW_PLUG: return "Glow plug";
    case ParamGroup::SHUT_DOWN: return "Shut-down";
    case ParamGroup::ALTITUDE: return "Altitude";
    case ParamGroup::FUEL_USE: return "Fuel use";
    case ParamGroup::FROST: return "Frost protection";
    case ParamGroup::ROOM: return "Room temperature";
    case ParamGroup::BATTERY: return "Battery";
    case ParamGroup::MAINTENANCE: return "Maintenance";
    default: return "Other";
  }
}

const char *heater_firmware_text(const Heater &h) {
  const char *t = h.firmware_text();
  return t[0] != '\0' ? t : "Unknown";
}

const char *firmware_update_status(const Status &s, const Heater &h) {
  return (s.capabilities & cap::FIRMWARE_UPDATE) ? h.firmware_update_text() : "Not available";
}

// ---------------------------------------------------------------- faults and reasons
//
// THE table for fault and reason codes: a new code gets its row here, with its text and the alert
// it raises. Reasons decide the alert when they are known; a fault with no reason (or one not in
// the table) alerts by its fault code. Texts reach Home Assistant ("Error", "Fault reason") and the
// alert texts; apps word the codes themselves (PROTOCOL.md).

namespace {

struct FaultRow {
  Fault fault;
  const char *name;
  FaultAlert alert;
};
const FaultRow FAULTS[] = {
    {Fault::NONE, "None", FaultAlert::NONE},
    {Fault::VOLTAGE, "Supply voltage", FaultAlert::BURN},
    {Fault::VOLTAGE_LOW, "Supply voltage low", FaultAlert::BURN},
    {Fault::VOLTAGE_HIGH, "Supply voltage high", FaultAlert::BURN},
    {Fault::GLOW_PLUG, "Glow plug", FaultAlert::BURN},
    {Fault::PUMP, "Fuel pump", FaultAlert::BURN},
    {Fault::OVERHEAT, "Overheat", FaultAlert::BURN},
    {Fault::FAN, "Fan", FaultAlert::BURN},
    {Fault::FLAME_OUT, "Flame-out or fuel shortage", FaultAlert::BURN},
    {Fault::TEMP_SENSOR, "Temperature sensor", FaultAlert::BURN},
    {Fault::START_FAILURE, "Start failure", FaultAlert::BURN},
    {Fault::CO, "CO above limit", FaultAlert::CO_ALARM},
    {Fault::INTAKE_SENSOR, "Intake air sensor", FaultAlert::BURN},
    {Fault::COMMS, "Heater communication", FaultAlert::BURN},
    {Fault::CO_SENSOR, "CO sensor", FaultAlert::CO_SENSOR},
    {Fault::CONTROLLER, "Heater controller", FaultAlert::BURN},
};

struct ReasonRow {
  Reason reason;
  const char *text;  // lower case: it follows "heater fault: Fan (" and starts "Fault reason"
  FaultAlert alert;
  bool retried;  // the safety MCU tries again while the heater starts or runs fan only
};
const ReasonRow REASONS[] = {
    {Reason::NONE, "", FaultAlert::NONE, false},
    {Reason::ASKED, "stopped as asked", FaultAlert::NONE, false},
    {Reason::DEADMAN, "the link from the board went silent during a burn", FaultAlert::BURN, false},
    {Reason::NO_FLAME, "no flame after every ignition attempt", FaultAlert::BURN, false},
    {Reason::FLAME_LOST, "the flame went out", FaultAlert::BURN, false},
    {Reason::OVERHEAT_LIMIT, "the heat exchanger passed its temperature limit", FaultAlert::BURN, false},
    {Reason::OVERHEAT_LATCH, "the overheat cut-out tripped", FaultAlert::BURN, false},
    {Reason::FAN_STALL, "the fan stalled", FaultAlert::BURN, true},
    {Reason::FAN_SPIN_UP, "the fan did not reach its speed", FaultAlert::BURN, true},
    {Reason::SENSOR_OPEN, "the heat exchanger sensor reads open", FaultAlert::BURN, false},
    {Reason::NO_TEMP_RISE, "the heat exchanger did not warm up", FaultAlert::BURN, false},
    {Reason::SUPPLY, "the supply voltage is out of range", FaultAlert::BURN, false},
    {Reason::COOLDOWN_LIMIT, "the heater did not cool down in time", FaultAlert::BURN, false},
    {Reason::CO, "carbon monoxide alarm", FaultAlert::CO_ALARM, false},
    {Reason::GLOW_STAGE, "the glow plug circuit failed", FaultAlert::BURN, false},
    {Reason::FAN_STAGE, "the fan's drive failed: stuck on, or the fan turned by itself", FaultAlert::BURN, false},
    {Reason::SELF_TEST, "the start-up self-test failed: the board needs checking", FaultAlert::BURN, false},
    {Reason::PUMP_STAGE, "the fuel pump circuit failed", FaultAlert::BURN, false},
    {Reason::BOARD_HOT, "the board stayed too hot, even at the lowest power", FaultAlert::BURN, false},
    {Reason::PLUG_TYPE, "the glow plug may not be the type set", FaultAlert::BURN, false},
    {Reason::CO_SENSOR, "the CO sensor reads open or failed its check", FaultAlert::CO_SENSOR, false},
    {Reason::VDDA_HIGH, "the controller's 3.3 V supply is too high", FaultAlert::BURN, false},
    {Reason::VDDA_LOW, "the controller's 3.3 V supply is too low", FaultAlert::BURN, false},
    {Reason::TICK, "the controller's clock check failed", FaultAlert::BURN, false},
    // A new code from the safety MCU (stm32/core/burn.h) gets a Reason value (heater.h) and a row
    // here (a new fault code: a Fault value and a FAULTS row), and test_controller's loop its new
    // last code. The input budget (PWR-1, PWR-2) only caps the glow and the fan: no code.
};

const FaultRow *fault_row(Fault f) {
  for (const FaultRow &r : FAULTS)
    if (r.fault == f)
      return &r;
  return nullptr;
}

const ReasonRow *reason_row(uint8_t reason) {
  for (const ReasonRow &r : REASONS)
    if (uint8_t(r.reason) == reason)
      return &r;
  return nullptr;
}

}  // namespace

const char *fault_name(Fault f) {
  const FaultRow *r = fault_row(f);
  return r != nullptr ? r->name : "Other";
}

const char *reason_text(uint8_t reason) {
  const ReasonRow *r = reason_row(reason);
  return r != nullptr ? r->text : nullptr;
}

bool reason_retried(uint8_t reason) {
  const ReasonRow *r = reason_row(reason);
  return r != nullptr && r->retried;
}

FaultAlert fault_alert(Fault fault, uint8_t reason) {
  if (fault == Fault::NONE)
    return FaultAlert::NONE;
  const ReasonRow *r = reason_row(reason);
  if (r != nullptr && r->reason != Reason::NONE)
    return r->alert;
  const FaultRow *f = fault_row(fault);
  return f != nullptr ? f->alert : FaultAlert::BURN;
}

bool fault_retrying(const HeaterStatus &s) {
  return s.answering && s.fault != Fault::NONE && reason_retried(s.panel_fault) &&
         (s.state == HeaterState::STARTING || s.state == HeaterState::FAN_ONLY);
}

void fault_reason_text(const HeaterStatus &s, char *out, size_t size) {
  if (size == 0)
    return;
  const char *t = s.answering ? reason_text(s.panel_fault) : "";
  if (t == nullptr)
    std::snprintf(out, size, "Reason %u", unsigned(s.panel_fault));
  else if (t[0] == '\0')
    std::snprintf(out, size, "None");
  else
    std::snprintf(out, size, "%c%s%s", std::toupper(static_cast<unsigned char>(t[0])), t + 1,
                  fault_retrying(s) ? ": trying again" : "");
}

const char *co_state_name(const HeaterStatus &s) {
  if (!s.answering)
    return "Unknown";
  switch (CoState(s.co_state)) {
    case CoState::NOT_FITTED: return "Not fitted";
    case CoState::WARMING_UP: return "Warming up";
    case CoState::LEARNING: return "Learning clean air";
    case CoState::OK: return "OK";
    case CoState::ALARM: return "Alarm";
    case CoState::SENSOR_FAULT: return "Sensor fault";
    case CoState::TOO_COLD: return "Too cold";
    default: return "Unknown";
  }
}

}  // namespace flamekeeper
}  // namespace esphome
