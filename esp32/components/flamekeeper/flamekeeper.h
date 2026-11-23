// The Flamekeeper hub: owns the Controller for the configured heater backend, ticks it (failsafe,
// room temperature), logs every request, and publishes the shared Home Assistant entities. Every
// entity here is the same on every board profile; none of them knows the backend.
#pragma once

#include <memory>
#include <vector>

#include "automation.h"
#include "board_services.h"
#include "controller.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/preferences.h"
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_CLIMATE
#include "esphome/components/climate/climate.h"
#endif
#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#endif
#ifdef USE_SELECT
#include "esphome/components/select/select.h"
#endif
#ifdef USE_NUMBER
#include "esphome/components/number/number.h"
#endif
#ifdef USE_BUTTON
#include "esphome/components/button/button.h"
#endif
#ifdef USE_TIME
#include "esphome/components/time/real_time_clock.h"
#endif

namespace esphome {
namespace flamekeeper {

class FlamekeeperHub;

// Something that shows part of the status: called from the hub's loop with the current status.
class StatusView {
 public:
  virtual ~StatusView() = default;
  virtual void show(const Status &s) = 0;
};

enum class SensorKey : uint8_t {
  ROOM_TEMPERATURE,
  HEAT_EXCHANGER_TEMPERATURE,
  SUPPLY_VOLTAGE,
  FAN_SPEED,
  PUMP_FREQUENCY,
  GLOW_PLUG_POWER,
  GLOW_PLUG_VOLTAGE,
  FAN_VOLTAGE,
  GLOW_PLUG_CURRENT,
  POWER_DEMAND,
  ALTITUDE,
  RUNNING_TIME,
  PRIMING_TIME_LEFT,
  INTAKE_TEMPERATURE,
  CO,
  SETTINGS_PENDING,
  FIRMWARE_UPDATE_PROGRESS,
  FAILSAFE_TIME_LEFT,
  FUEL_LEFT,
  FUEL_USED,      // lifetime, L
  BURNER_HOURS,   // lifetime, the board's own count
  GLOW_PLUG_HOURS,
  // The heater's fault log (the safety MCU's).
  HEATER_FAULT_LOG_ENTRIES,
};

enum class TextKey : uint8_t {
  RUN_STATE,
  CONTROLLER_STATUS,
  ERROR,
  HEATER_FIRMWARE,
  SETTINGS_STATUS,
  FIRMWARE_UPDATE_STATUS,
  RUN_STARTED_BY,
  ALERTS,
  RELAY,
  // The heater's fault log (the safety MCU's): its newest entry in words.
  LAST_HEATER_FAULT,
};

class FlamekeeperHub : public Component {
 public:
  void set_heater(Heater *heater) {
    this->controller_.reset(new Controller(heater));
    this->board_.reset(new BoardServices(this->controller_.get()));
    this->automation_.reset(new Automation(*this->controller_));
    this->controller_->set_bluetooth_room(this->bluetooth_room_);
  }
  // Bluetooth thermometers (flamekeeper_thermo): capability bit 7. May come before set_heater (the
  // order of code generation is not fixed), so the hub keeps it until the controller exists.
  void enable_bluetooth_room() {
    this->bluetooth_room_ = true;
    if (this->controller_)
      this->controller_->set_bluetooth_room(true);
  }
  // Frost protection, timers, fuel meters and tank, alerts, the clock, maintenance.
  Automation &automation() { return *this->automation_; }
#ifdef USE_TIME
  // Home Assistant's (or any) time: sets the board's clock while it has a valid time.
  void set_time(time::RealTimeClock *rtc) { this->rtc_ = rtc; }
#endif
  Controller &controller() { return *this->controller_; }
  // The board's own settings (PIN, Wi-Fi, Home Assistant, Wi-Fi keys) for both transports.
  BoardServices &board() { return *this->board_; }
  const Status &status() const { return this->controller_->status(); }

  void set_failsafe_timeout(uint32_t ms) { this->failsafe_ms_ = ms; }
#ifdef USE_SENSOR
  void set_room_probe(sensor::Sensor *s);
  void set_room_home_assistant(sensor::Sensor *s);
  void set_altitude_sensor(sensor::Sensor *s);
  void set_pressure_sensor(sensor::Sensor *s);
  void set_intake_sensor(sensor::Sensor *s);
  void add_sensor(SensorKey key, sensor::Sensor *s) { this->sensors_.push_back({key, s, NAN, 0}); }
#endif
#ifdef USE_TEXT_SENSOR
  void add_text_sensor(TextKey key, text_sensor::TextSensor *s) { this->texts_.push_back({key, s}); }
#endif
  void add_view(StatusView *view) { this->views_.push_back(view); }

  void setup() override;
  void loop() override;
  void dump_config() override;
  // After the backend (DATA), before the entities that restore their state through it.
  float get_setup_priority() const override { return setup_priority::DATA - 0.5f; }

 protected:
  bool ha_connected_() const;
  void publish_(uint32_t now, bool force);

  std::unique_ptr<Controller> controller_;
  std::unique_ptr<BoardServices> board_;
  std::unique_ptr<Automation> automation_;
  bool bluetooth_room_{false};
  ESPPreferenceObject tuning_pref_;
  ESPPreferenceObject maintenance_pref_;
  ESPPreferenceObject log_pref_;
  ESPPreferenceObject automation_pref_;
  ESPPreferenceObject meter_pref_;
  uint32_t last_rtc_ms_{0};
  uint32_t app_clock_ms_{0};
  std::string alerts_text_;
  char heater_log_text_[200]{};  // TextKey::LAST_HEATER_FAULT
#ifdef USE_TIME
  time::RealTimeClock *rtc_{nullptr};
#endif
  uint32_t failsafe_ms_{Controller::DEFAULT_FAILSAFE_MS};
  uint32_t last_publish_ms_{0};
  uint32_t last_force_ms_{0};
  std::vector<StatusView *> views_;
#ifdef USE_SENSOR
  struct SensorSlot {
    SensorKey key;
    sensor::Sensor *sensor;
    float last;
    uint32_t last_ms;
  };
  std::vector<SensorSlot> sensors_;
#endif
#ifdef USE_TEXT_SENSOR
  struct TextSlot {
    TextKey key;
    text_sensor::TextSensor *sensor;
  };
  std::vector<TextSlot> texts_;
#endif
};

// ---------------------------------------------------------------- entities
// Every change made through an entity is a Home Assistant request; a value restored at boot is the
// board's own.

#ifdef USE_CLIMATE
class FlamekeeperClimate : public climate::Climate, public Component, public StatusView {
 public:
  void set_hub(FlamekeeperHub *hub) { this->hub_ = hub; }
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA - 1.0f; }
  void show(const Status &s) override;

 protected:
  climate::ClimateTraits traits() override;
  void control(const climate::ClimateCall &call) override;
  FlamekeeperHub *hub_{nullptr};
  bool published_once_{false};
};
#endif

#ifdef USE_SWITCH
// A parameter as a switch (on/off parameters: 1 / 0), the heater's or the board's own. Not restored:
// the value is the heater's (or the stored tuning), shown as it is.
class ParamSwitch : public switch_::Switch, public Component, public StatusView {
 public:
  void set_hub(FlamekeeperHub *hub) { this->hub_ = hub; }
  void set_param(uint16_t id) { this->id_ = id; }
  void setup() override;
  float get_setup_priority() const override { return setup_priority::DATA - 1.0f; }
  void show(const Status &s) override;

 protected:
  void write_state(bool state) override;
  FlamekeeperHub *hub_{nullptr};
  uint16_t id_{0};
  int8_t shown_{-1};
};
#endif

#ifdef USE_SELECT
enum class SelectKind : uint8_t { HEAT_MODE, ROOM_SOURCE };
class FlamekeeperSelect : public select::Select, public Component, public StatusView {
 public:
  void set_hub(FlamekeeperHub *hub) { this->hub_ = hub; }
  void set_kind(SelectKind kind) { this->kind_ = kind; }
  void setup() override;
  float get_setup_priority() const override { return setup_priority::DATA - 1.0f; }
  void show(const Status &s) override;

 protected:
  void control(size_t index) override;
  bool apply_(size_t index, Source source);
  FlamekeeperHub *hub_{nullptr};
  SelectKind kind_{SelectKind::HEAT_MODE};
  ESPPreferenceObject pref_;
};
#endif

#ifdef USE_NUMBER
// "Power" 0-100 %: the fixed power demand, and the fan speed in fan-only mode.
class PowerNumber : public number::Number, public Component, public StatusView {
 public:
  void set_hub(FlamekeeperHub *hub) { this->hub_ = hub; }
  void setup() override;
  float get_setup_priority() const override { return setup_priority::DATA - 1.0f; }
  void show(const Status &s) override;

 protected:
  void control(float value) override;
  FlamekeeperHub *hub_{nullptr};
  ESPPreferenceObject pref_;
};
#endif

#ifdef USE_NUMBER
// A parameter as a number, the heater's (through the link) or the board's own: shown = raw /
// multiply. Heater parameters are staged until Apply / Save; board settings apply at once.
class ParamNumber : public number::Number, public Component, public StatusView {
 public:
  void set_hub(FlamekeeperHub *hub) { this->hub_ = hub; }
  void set_param(uint16_t id, float multiply) {
    this->id_ = id;
    this->multiply_ = multiply;
  }
  void setup() override;
  float get_setup_priority() const override { return setup_priority::DATA - 1.0f; }
  void show(const Status &s) override;

 protected:
  void control(float value) override;
  FlamekeeperHub *hub_{nullptr};
  uint16_t id_{0};
  float multiply_{1.0f};
  bool shown_once_{false};
};
#endif

#ifdef USE_BUTTON
enum class ButtonKind : uint8_t {
  PRIME,
  STOP_PRIME,
  APPLY_SETTINGS,
  SAVE_SETTINGS,
  DISCARD_SETTINGS,
  FIRMWARE_UPDATE,
  RESET_FAULT,
  TANK_FILLED,
  REARM_AUTOMATIC,
  CLEAR_ALERTS,
  CARBON_BURNOFF,
  GLOW_NEW_PLUG,
  // The heater's fault log (the safety MCU's): empties it.
  CLEAR_HEATER_FAULT_LOG,
};
class FlamekeeperButton : public button::Button, public Component {
 public:
  void set_hub(FlamekeeperHub *hub) { this->hub_ = hub; }
  void set_kind(ButtonKind kind) { this->kind_ = kind; }

 protected:
  void press_action() override;
  FlamekeeperHub *hub_{nullptr};
  ButtonKind kind_{ButtonKind::PRIME};
};
#endif

}  // namespace flamekeeper
}  // namespace esphome
