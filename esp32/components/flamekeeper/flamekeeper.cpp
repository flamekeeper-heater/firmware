#include "flamekeeper.h"
#include "heater_log.h"

#include <esp_random.h>
#include <esp_system.h>
#include <sys/time.h>
#include <ctime>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#ifdef USE_API
#include "esphome/components/api/api_server.h"
#endif

namespace esphome {
namespace flamekeeper {

static const char *const TAG = "flamekeeper";
static const uint32_t PUBLISH_MS = 500;
static const uint32_t REPUBLISH_MS = 60000;

// ---------------------------------------------------------------- hub

#ifdef USE_SENSOR
void FlamekeeperHub::set_room_probe(sensor::Sensor *s) {
  s->add_on_state_callback([this](float v) { this->controller_->room().probe(v, millis()); });
}
void FlamekeeperHub::set_room_home_assistant(sensor::Sensor *s) {
  this->controller_->set_ha_room(true);
  s->add_on_state_callback([this](float v) { this->controller_->room().home_assistant(v); });
}
void FlamekeeperHub::set_altitude_sensor(sensor::Sensor *s) {
  s->add_on_state_callback([this](float v) { this->controller_->set_altitude(v); });
}

void FlamekeeperHub::set_pressure_sensor(sensor::Sensor *s) {
  s->add_on_state_callback([this](float v) { this->controller_->set_pressure(v); });
}
void FlamekeeperHub::set_intake_sensor(sensor::Sensor *s) {
  s->add_on_state_callback([this](float v) { this->controller_->set_intake_temperature(v); });
}
#endif

void FlamekeeperHub::setup() {
  // First: it decides whether the Home Assistant API starts at all (it sets up later).
  this->board_->setup();
  Controller &c = *this->controller_;
  // The board's settings (tuning.h): one record of (id, value) pairs.
  this->tuning_pref_ = global_preferences->make_preference<Tuning::Store>(Tuning::STORE_KEY);
  {
    auto *stored = new Tuning::Store();  // 400 bytes: off the loop task's stack
    if (this->tuning_pref_.load(stored))
      c.load_tuning(*stored);
    delete stored;
  }
  c.on_tuning_changed([this](const Tuning &t) {
    auto *st = new Tuning::Store();
    t.save(*st);
    this->tuning_pref_.save(st);
    delete st;
  });

  // Automation: timers, the disarmed state, the chosen thermometer, the time zone; the fuel meters.
  Automation &a = *this->automation_;
  this->automation_pref_ = global_preferences->make_preference<Automation::Saved>(Automation::SAVED_CHECK);
  this->meter_pref_ = global_preferences->make_preference<FuelMeter::Saved>(FuelMeter::SAVED_CHECK);
  this->maintenance_pref_ =
      global_preferences->make_preference<Automation::Maintenance>(Automation::MAINTENANCE_CHECK);
  this->log_pref_ = global_preferences->make_preference<EventLog::Saved>(EventLog::SAVED_CHECK);
  this->start_at_pref_ = global_preferences->make_preference<Automation::StartAt>(Automation::START_AT_CHECK);
  {
    auto *saved = new Automation::Saved();  // too big for the loop task's stack with the meters
    if (this->automation_pref_.load(saved))
      a.load(*saved);
    delete saved;
    auto *meter = new FuelMeter::Saved();
    if (this->meter_pref_.load(meter))
      a.load_meter(*meter);
    delete meter;
    Automation::Maintenance maint;
    if (this->maintenance_pref_.load(&maint))
      a.load_maintenance(maint);
    auto *log = new EventLog::Saved();
    if (this->log_pref_.load(log))
      a.load_log(*log);
    delete log;
    Automation::StartAt start;
    if (this->start_at_pref_.load(&start))
      a.load_start_at(start);
  }
  a.on_maintenance_save([this](const Automation::Maintenance &m) { this->maintenance_pref_.save(&m); });
  a.on_log_save([this](const EventLog::Saved &l) { this->log_pref_.save(&l); });
  a.on_save([this](const Automation::Saved &s) { this->automation_pref_.save(&s); });
  a.on_meter_save([this](const FuelMeter::Saved &s) { this->meter_pref_.save(&s); });
  a.on_start_at_save([this](const Automation::StartAt &s) { this->start_at_pref_.save(&s); });
  a.on_clock_set([](uint32_t utc) {
    struct timeval tv {};
    tv.tv_sec = time_t(utc);
    settimeofday(&tv, nullptr);
  });
  // The system clock survives a restart (not a power cut): keep using it.
  const time_t sys = ::time(nullptr);
  if (sys > time_t(Automation::EARLIEST_UTC))
    a.restore_clock(uint32_t(sys), millis());
  a.log_board_started(uint16_t(esp_reset_reason()));
  c.set_failsafe_timeout(this->failsafe_ms_);
  c.set_boot_id(uint16_t(esp_random()));
  c.on_request([](Source source, const char *what, const Outcome &o) {
    if (o.result == Result::OK)
      ESP_LOGI(TAG, "%s: %s", source_name(source), what);
    else if (o.result == Result::STORED)
      ESP_LOGI(TAG, "%s: %s (stored: %s)", source_name(source), what, o.reason);
    else
      ESP_LOGW(TAG, "%s: %s refused: %s", source_name(source), what, o.reason);
  });
  c.on_event([](Event e, Result r, uint16_t id, const char *text) {
    if (e == Event::FAILSAFE_STOP)
      ESP_LOGW(TAG, "Failsafe stop: %s", text);
    else if (e == Event::ALERT)
      ESP_LOGW(TAG, "Alert: %s", text);
    else if (e != Event::PARAM_CHANGED)
      ESP_LOGI(TAG, "Parameters: %s (%s)", text, r == Result::OK ? "ok" : "failed");
    (void) id;
  });
  c.tick(millis(), this->ha_connected_());
}

void FlamekeeperHub::dump_config() {
  ESP_LOGCONFIG(TAG, "Flamekeeper hub");
  if (this->failsafe_ms_ == 0)
    ESP_LOGCONFIG(TAG, "  Home Assistant failsafe: off");
  else
    ESP_LOGCONFIG(TAG, "  Home Assistant failsafe: %u s, for runs started from Home Assistant",
                  (unsigned) (this->failsafe_ms_ / 1000));
  ESP_LOGCONFIG(TAG, "  Heater capabilities: 0x%02X", (unsigned) this->status().capabilities);
  this->board_->dump_config();
}

bool FlamekeeperHub::ha_connected_() const {
#ifdef USE_API
  return api::global_api_server != nullptr && api::global_api_server->is_connected();
#else
  return false;
#endif
}

void FlamekeeperHub::loop() {
  uint32_t now = millis();
  this->controller_->tick(now, this->ha_connected_());
  this->automation_->tick(now);
  this->board_->loop(now);
#ifdef USE_TIME
  // Home Assistant's time every 10 minutes, unless an app set the clock in the last hour.
  if (this->rtc_ != nullptr && (this->last_rtc_ms_ == 0 || now - this->last_rtc_ms_ >= 10 * 60 * 1000)) {
    ESPTime t = this->rtc_->now();
    if (t.is_valid()) {
      this->last_rtc_ms_ = now == 0 ? 1 : now;
      Automation &a = *this->automation_;
      bool app_recent = a.clock_source() == ClockSource::APP && a.utc(now) != 0 &&
                        uint32_t(t.timestamp) - a.utc(now) < 3600 && a.utc(now) - uint32_t(t.timestamp) < 3600;
      if (!app_recent || a.utc(now) == 0)
        a.set_clock(uint32_t(t.timestamp), "", ClockSource::HOME_ASSISTANT, now);
    }
  }
#endif
  if (now - this->last_publish_ms_ < PUBLISH_MS)
    return;
  this->last_publish_ms_ = now;
  bool force = now - this->last_force_ms_ >= REPUBLISH_MS;
  if (force)
    this->last_force_ms_ = now;
  this->publish_(now, force);
}

void FlamekeeperHub::publish_(uint32_t now, bool force) {
  const Status &s = this->status();
  for (StatusView *v : this->views_)
    v->show(s);
#ifdef USE_SENSOR
  for (SensorSlot &slot : this->sensors_) {
    float v = NAN;
    switch (slot.key) {
      case SensorKey::ROOM_TEMPERATURE: v = s.room_c; break;
      case SensorKey::HEAT_EXCHANGER_TEMPERATURE: v = s.heat_exchanger_c; break;
      case SensorKey::SUPPLY_VOLTAGE: v = s.supply_v; break;
      case SensorKey::FAN_SPEED: v = s.fan_rpm; break;
      case SensorKey::PUMP_FREQUENCY: v = s.pump_hz; break;
      case SensorKey::GLOW_PLUG_POWER: v = s.glow_w; break;
      case SensorKey::GLOW_PLUG_VOLTAGE: v = s.glow_v; break;
      case SensorKey::FAN_VOLTAGE: v = s.fan_v; break;
      case SensorKey::GLOW_PLUG_CURRENT: v = s.glow_a; break;
      case SensorKey::POWER_DEMAND: v = s.power_pct; break;
      case SensorKey::ALTITUDE: v = s.altitude_m; break;
      case SensorKey::RUNNING_TIME: v = std::isnan(s.run_time_s) ? NAN : s.run_time_s / 3600.0f; break;
      case SensorKey::PRIMING_TIME_LEFT: v = s.prime_left_s; break;
      case SensorKey::INTAKE_TEMPERATURE: v = s.intake_c; break;
      case SensorKey::CO: v = s.co_ppm; break;
      case SensorKey::SETTINGS_PENDING: v = s.params_pending; break;
      case SensorKey::FIRMWARE_UPDATE_PROGRESS: v = s.update_progress; break;
      case SensorKey::FAILSAFE_TIME_LEFT:
        v = s.failsafe_left_s == UINT32_MAX ? NAN : float(s.failsafe_left_s);
        break;
      case SensorKey::FUEL_LEFT: v = s.fuel_left == 0xFF ? NAN : float(s.fuel_left); break;
      case SensorKey::FUEL_USED: v = float(this->automation_->meter().saved().life_fuel_ml / 1000.0); break;
      case SensorKey::BURNER_HOURS: v = float(this->automation_->meter().saved().life_burn_s / 3600.0); break;
      case SensorKey::GLOW_PLUG_HOURS: v = float(this->automation_->meter().saved().life_glow_s / 3600.0); break;
      // The heater's fault log: entries as last read (kept while it does not answer).
      case SensorKey::HEATER_FAULT_LOG_ENTRIES: v = float(s.fault_log_count); break;
    }
    bool same = (std::isnan(v) && std::isnan(slot.last)) || v == slot.last;
    if (same && !force && slot.last_ms != 0)
      continue;
    if (same && std::isnan(v) && slot.last_ms != 0)
      continue;  // unknown stays unknown: nothing to repeat
    slot.last = v;
    slot.last_ms = now == 0 ? 1 : now;
    slot.sensor->publish_state(v);
  }
#endif
#ifdef USE_TEXT_SENSOR
  for (TextSlot &slot : this->texts_) {
    const char *t = "";
    Heater *h = this->controller_->heater();
    switch (slot.key) {
      case TextKey::RUN_STATE: t = heater_state_name(s.state); break;
      case TextKey::CONTROLLER_STATUS: t = control_status_name(s.control); break;
      case TextKey::ERROR: t = fault_name(s.fault); break;
      case TextKey::FAULT_REASON:
        fault_reason_text(s, this->reason_text_, sizeof(this->reason_text_));
        t = this->reason_text_;
        break;
      case TextKey::CO_SENSOR: t = co_state_name(s); break;
      case TextKey::HEATER_FIRMWARE: t = heater_firmware_text(*h); break;
      case TextKey::SETTINGS_STATUS:
        t = (s.capabilities & cap::PARAMETERS) ? h->param_status_text() : "This heater has no parameters";
        break;
      case TextKey::FIRMWARE_UPDATE_STATUS: t = firmware_update_status(s, *h); break;
      case TextKey::RUN_STARTED_BY: t = s.mode == Mode::OFF ? "Not running" : source_name(s.run_source); break;
      case TextKey::RELAY:
        t = this->board_->relay_client() != nullptr ? relay_state_name(this->board_->relay_client()->state()) : "Not available";
        break;
      case TextKey::ALERTS: {
        this->alerts_text_.clear();
        for (uint8_t i = 0; alert_name(i) != nullptr; i++) {
          if (s.alerts & (1u << i)) {
            if (!this->alerts_text_.empty())
              this->alerts_text_ += ", ";
            this->alerts_text_ += alert_name(i);
          }
        }
        t = this->alerts_text_.empty() ? "None" : this->alerts_text_.c_str();
        break;
      }
      // The heater's fault log: its newest entry, as last read.
      case TextKey::LAST_HEATER_FAULT: {
        HeaterLogEntry e;
        const bool any = this->controller_->fault_log(0, &e, 1) == 1;
        heater_log_text(any ? &e : nullptr, this->heater_log_text_, sizeof(this->heater_log_text_));
        t = this->heater_log_text_;
        break;
      }
    }
    if (!force && slot.sensor->has_state() && slot.sensor->state == t)
      continue;
    slot.sensor->publish_state(t);
  }
#endif
}

// ---------------------------------------------------------------- climate

#ifdef USE_CLIMATE
void FlamekeeperClimate::setup() {
  // Restore only the target temperature: the heater always comes up off after a restart.
  auto restore = this->restore_state_();
  if (restore.has_value() && !std::isnan(restore->target_temperature))
    this->hub_->controller().set_target(restore->target_temperature, Source::BOARD);
  this->hub_->add_view(this);
  this->mode = climate::CLIMATE_MODE_OFF;
  this->show(this->hub_->status());
}

void FlamekeeperClimate::dump_config() { LOG_CLIMATE("", "Flamekeeper heater", this); }

climate::ClimateTraits FlamekeeperClimate::traits() {
  climate::ClimateTraits traits;
  traits.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE | climate::CLIMATE_SUPPORTS_ACTION);
  traits.add_supported_mode(climate::CLIMATE_MODE_OFF);
  traits.add_supported_mode(climate::CLIMATE_MODE_HEAT);
  traits.add_supported_mode(climate::CLIMATE_MODE_FAN_ONLY);  // refused when the heater has none
  Heater *h = this->hub_->controller().heater();
  traits.set_visual_min_temperature(h->min_target());
  traits.set_visual_max_temperature(h->max_target());
  traits.set_visual_target_temperature_step(1);
  traits.set_visual_current_temperature_step(0.1f);
  return traits;
}

void FlamekeeperClimate::control(const climate::ClimateCall &call) {
  Controller &c = this->hub_->controller();
  if (call.get_target_temperature().has_value())
    c.set_target(*call.get_target_temperature(), Source::HOME_ASSISTANT);
  if (call.get_mode().has_value()) {
    climate::ClimateMode m = *call.get_mode();
    c.set_mode(m == climate::CLIMATE_MODE_HEAT       ? Mode::HEAT
               : m == climate::CLIMATE_MODE_FAN_ONLY ? Mode::FAN
                                                     : Mode::OFF,
               Source::HOME_ASSISTANT);
  }
  this->published_once_ = false;  // show what the heater took, even if it refused
  this->show(this->hub_->status());
}

void FlamekeeperClimate::show(const Status &s) {
  // Priming shows as Off: it is not heating, and Off ends it.
  climate::ClimateMode mode = s.mode == Mode::HEAT  ? climate::CLIMATE_MODE_HEAT
                              : s.mode == Mode::FAN ? climate::CLIMATE_MODE_FAN_ONLY
                                                    : climate::CLIMATE_MODE_OFF;
  climate::ClimateAction action;
  switch (s.state) {
    case HeaterState::STARTING:
    case HeaterState::HEATING:
    case HeaterState::PRIMING: action = climate::CLIMATE_ACTION_HEATING; break;
    case HeaterState::STOPPING:
    case HeaterState::COOLING:
    case HeaterState::FAN_ONLY: action = climate::CLIMATE_ACTION_FAN; break;
    default: action = mode == climate::CLIMATE_MODE_OFF ? climate::CLIMATE_ACTION_OFF : climate::CLIMATE_ACTION_IDLE;
  }
  float current = std::isnan(s.room_c) ? NAN : std::round(s.room_c * 10.0f) / 10.0f;
  bool same_current =
      (std::isnan(current) && std::isnan(this->current_temperature)) || current == this->current_temperature;
  if (this->published_once_ && mode == this->mode && action == this->action && same_current &&
      s.target_c == this->target_temperature)
    return;
  this->mode = mode;
  this->action = action;
  this->current_temperature = current;
  this->target_temperature = s.target_c;
  this->published_once_ = true;
  this->publish_state();
}
#endif

// ---------------------------------------------------------------- switch

#ifdef USE_SWITCH
void ParamSwitch::setup() {
  this->hub_->add_view(this);
  this->show(this->hub_->status());
}

void ParamSwitch::show(const Status &s) {
  (void) s;
  ParamValue v;
  if (!this->hub_->controller().param_get(this->id_, v) || !v.known)
    return;  // stays unknown until the heater has told us
  int8_t on = (v.staged ? v.staged_value : v.value) != 0 ? 1 : 0;
  if (on != this->shown_) {
    this->shown_ = on;
    this->publish_state(on != 0);
  }
}

void ParamSwitch::write_state(bool state) {
  this->hub_->controller().param_set(this->id_, state ? 1 : 0, Source::HOME_ASSISTANT);
  this->shown_ = -1;
  this->show(this->hub_->status());
}

#endif

// ---------------------------------------------------------------- selects

#ifdef USE_SELECT
void FlamekeeperSelect::setup() {
  this->hub_->add_view(this);
  this->pref_ = this->make_entity_preference<uint8_t>();
  uint8_t index;
  if (this->pref_.load(&index) && index < this->size())
    this->apply_(index, Source::BOARD);
  this->show(this->hub_->status());
}

bool FlamekeeperSelect::apply_(size_t index, Source source) {
  Controller &c = this->hub_->controller();
  Outcome o = this->kind_ == SelectKind::HEAT_MODE ? c.set_heat_mode(HeatMode(index), source)
                                                   : c.set_room_source(RoomSource(index), source);
  return o.accepted();
}

void FlamekeeperSelect::control(size_t index) {
  if (this->apply_(index, Source::HOME_ASSISTANT)) {
    uint8_t v = uint8_t(index);
    this->pref_.save(&v);
  }
  this->publish_state(this->kind_ == SelectKind::HEAT_MODE ? size_t(this->hub_->status().heat_mode)
                                                           : size_t(this->hub_->status().room_source));
}

void FlamekeeperSelect::show(const Status &s) {
  size_t index = this->kind_ == SelectKind::HEAT_MODE ? size_t(s.heat_mode) : size_t(s.room_source);
  if (!this->has_state() || this->active_index() != index) {
    this->publish_state(index);
    uint8_t v = uint8_t(index);
    this->pref_.save(&v);  // a change from the phone is kept over a restart too
  }
}
#endif

// ---------------------------------------------------------------- number

#ifdef USE_NUMBER
void PowerNumber::setup() {
  this->hub_->add_view(this);
  // Stored as a float (0-100).
  this->pref_ = this->make_entity_preference<float>();
  float power;
  if (this->pref_.load(&power) && power >= 0.0f && power <= 100.0f)
    this->hub_->controller().set_power(int(std::lround(power)), Source::BOARD);
  this->show(this->hub_->status());
}

void PowerNumber::control(float value) {
  this->hub_->controller().set_power(int(std::lround(value)), Source::HOME_ASSISTANT);
  this->publish_state(this->hub_->status().power);
}

void PowerNumber::show(const Status &s) {
  if (!this->has_state() || this->state != float(s.power)) {
    this->publish_state(s.power);
    float v = s.power;
    this->pref_.save(&v);
  }
}
#endif

#ifdef USE_NUMBER
void ParamNumber::setup() {
  this->hub_->add_view(this);
  this->show(this->hub_->status());
}

void ParamNumber::show(const Status &s) {
  (void) s;
  ParamValue v;
  float shown = NAN;
  if (this->hub_->controller().param_get(this->id_, v) && v.known)
    shown = float(v.staged ? v.staged_value : v.value) / this->multiply_;
  bool same = (std::isnan(shown) && std::isnan(this->state)) || shown == this->state;
  if (same && this->shown_once_)
    return;
  if (std::isnan(shown) && !this->shown_once_)
    return;  // stays unknown until the heater has told us
  this->shown_once_ = true;
  this->publish_state(shown);
}

void ParamNumber::control(float value) {
  this->hub_->controller().param_set(this->id_, int32_t(std::lround(value * this->multiply_)), Source::HOME_ASSISTANT);
  this->shown_once_ = false;  // show what is in force now: the new value, or the old one if refused
  this->show(this->hub_->status());
}
#endif

// ---------------------------------------------------------------- buttons

#ifdef USE_BUTTON
void FlamekeeperButton::press_action() {
  Controller &c = this->hub_->controller();
  const Source src = Source::HOME_ASSISTANT;
  switch (this->kind_) {
    case ButtonKind::PRIME: c.start_prime(src); break;
    case ButtonKind::STOP_PRIME:
      // On a heater without "stop priming", Off is how priming ends.
      if (c.status().capabilities & cap::PRIME_STOP)
        c.stop_prime(src);
      else if (c.status().mode == Mode::PRIME)
        c.set_mode(Mode::OFF, src);
      break;
    case ButtonKind::APPLY_SETTINGS: c.param_apply(false, src); break;
    case ButtonKind::SAVE_SETTINGS: c.param_apply(true, src); break;
    case ButtonKind::DISCARD_SETTINGS: c.param_discard(src); break;
    case ButtonKind::FIRMWARE_UPDATE: c.start_firmware_update(src); break;
    case ButtonKind::RESET_FAULT: c.reset_fault(src); break;
    case ButtonKind::TANK_FILLED: this->hub_->automation().tank_set(100, Source::HOME_ASSISTANT); break;
    case ButtonKind::REARM_AUTOMATIC: this->hub_->automation().rearm(Source::HOME_ASSISTANT); break;
    case ButtonKind::CLEAR_ALERTS: this->hub_->automation().clear_alerts(0xFFFF); break;
    case ButtonKind::CARBON_BURNOFF: {
      Automation &a = this->hub_->automation();
      a.burnoff_start(uint8_t(c.tuning().extra(M_LENGTH)), src);
      break;
    }
    case ButtonKind::GLOW_NEW_PLUG: this->hub_->automation().glow_new_plug(); break;
    case ButtonKind::CLEAR_HEATER_FAULT_LOG: c.clear_fault_log(src); break;
  }
}
#endif

}  // namespace flamekeeper
}  // namespace esphome
