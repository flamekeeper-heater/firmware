// The one place every request to the heater goes through, whatever its source (Home Assistant
// entity, an app, the board's own buttons). It applies the checks common to all sources, hands the
// request to the backend, remembers which source started the current run, runs the Home Assistant
// failsafe, and owns what the ESP32 owns on every board (DESIGN.md): the
// room temperature, the heat mode and the thermostat (with cycling), the altitude fuel trim, the
// live trims, and those settings as parameters next to the heater's.
//
// Plain C++ (host-tested in tests/test_controller.cpp). Time is a wrapping uint32_t millisecond
// count; elapsed time is always uint32_t(now - then).
#pragma once

#include <functional>
#include <vector>

#include "heater.h"
#include "thermostat.h"
#include "tuning.h"

namespace esphome {
namespace flamekeeper {

// Room temperature from the board's probe, a Home Assistant sensor and a Bluetooth thermometer
// the chosen source, else the first of probe, Home Assistant, Bluetooth that has a value.
// Each source's offset (a board setting) is added to its readings.
class RoomTemperature {
 public:
  static const uint32_t PROBE_STALE_MS = 5 * 60 * 1000;
  static const uint32_t BLUETOOTH_STALE_MS = 10 * 60 * 1000;

  void probe(float celsius, uint32_t now) {
    this->probe_c_ = celsius;
    this->probe_ms_ = now == 0 ? 1 : now;
  }
  // A Home Assistant sensor only updates on change, so it is valid while it has a number (HA sends
  // "unavailable" as NaN, and Controller::tick drops the value while the API is disconnected).
  void home_assistant(float celsius) { this->ha_c_ = celsius; }
  // The chosen Bluetooth thermometer's reading (as it sends it); NAN forgets it.
  void bluetooth(float celsius, uint32_t now) {
    this->bt_c_ = celsius;
    this->bt_ms_ = now == 0 ? 1 : now;
  }
  void set_source(RoomSource source) { this->source_ = source; }
  RoomSource source() const { return this->source_; }
  void set_offset(RoomSource source, float celsius) {
    if (uint8_t(source) < 3)
      this->offset_[uint8_t(source)] = celsius;
  }

  // One source's reading now, offset added (NAN if it has none).
  float reading(RoomSource source, uint32_t now) const;
  // The temperature to use now (NAN if no source has one); *fallback = it came from another source.
  float value(uint32_t now, bool *fallback) const;

 protected:
  RoomSource source_{RoomSource::PROBE};  // the board's probe by default
  float probe_c_{NAN};
  uint32_t probe_ms_{0};
  float ha_c_{NAN};
  float bt_c_{NAN};
  uint32_t bt_ms_{0};
  float offset_[3] = {0.0f, 0.0f, 0.0f};
};

// The heater's status plus what the controller adds.
struct Status : HeaterStatus {
  uint32_t capabilities{0};
  float room_c{NAN};
  RoomSource room_source{RoomSource::PROBE};
  float room_probe_c{NAN};  // each source's reading, offset added (Telemetry)
  float room_ha_c{NAN};
  float room_bt_c{NAN};
  bool room_fallback{false};
  float intake_c{NAN};
  Source run_source{Source::NONE};  // who started the current run (NONE while off)
  uint32_t run_id{0};               // this run: boot id << 16 | run number this boot (0 while off)
  uint32_t run_s{0};                // seconds this run has run, up to its stop (0 while off; Telemetry)
  Source ending_source{Source::NONE};  // who started the run now stopping or cooling down (State byte 12)
  bool failsafe_stopped{false};     // the last stop came from the failsafe
  bool thermo_paused{false};        // a heat run the thermostat holds off: warm enough (State flag bit 6)
  uint32_t failsafe_left_s{UINT32_MAX};  // UINT32_MAX = not counting
  // Filled by the board's automation (automation.h)
  uint16_t alerts{0};
  uint8_t automatic{0};      // State byte 20
  uint8_t fuel_left{0xFF};   // %, 0xFF = no tank size
  uint8_t burnoff_left_min{0};  // carbon burn-off run, minutes left
  uint8_t timer_slot{0xFF};      // the timer whose run this is (0xFF none)
  uint32_t timer_end_utc{0};     // ...and when it ends
  bool board_updating{false};    // a board firmware update runs: every start is refused
  uint8_t relay_phones{0};       // phones signed in through the internet relay (State flag bit 5)
};

class Automation;

class Controller : public HeaterListener {
 public:
  static const uint32_t DEFAULT_FAILSAFE_MS = 15 * 60 * 1000;

  explicit Controller(Heater *heater);

  Heater *heater() const { return this->heater_; }
  void set_failsafe_timeout(uint32_t ms) { this->failsafe_ms_ = ms; }
  // A random number per boot: the high half of State's run id, so runs from different boots differ.
  void set_boot_id(uint16_t id) { this->boot_id_ = id; }

  // --- requests (every source) ---
  Outcome set_mode(Mode mode, Source source);
  Outcome set_heat_mode(HeatMode mode, Source source);
  Outcome set_power(int percent, Source source);  // 0..100 %
  Outcome set_target(float celsius, Source source);
  Outcome set_room_source(RoomSource room, Source source);
  Outcome start_prime(Source source) { return this->set_mode(Mode::PRIME, source); }
  Outcome stop_prime(Source source);
  Outcome start_firmware_update(Source source);
  Outcome param_set(uint16_t id, int32_t value, Source source);
  Outcome param_apply(bool save, Source source);
  Outcome param_discard(Source source);
  Outcome reset_fault(Source source);

  // --- the heater's own fault log (cap::FAULT_LOG): read, and cleared as a request ---
  size_t fault_log(size_t from, HeaterLogEntry *out, size_t max) const;
  Outcome clear_fault_log(Source source);

  // --- the parameter table: the heater's, then the board's own (tuning.h) ---
  size_t param_count() const;
  bool param_info(size_t index, ParamInfo &out) const;
  bool param_get(uint16_t id, ParamValue &out) const;
  // What the parameter does; "" when not known (yet), nullptr when there is no such parameter.
  const char *param_help(uint16_t id) const;
  const Tuning &tuning() const { return this->tuning_; }
  // Stored tuning, from the board's preferences at start-up.
  void load_tuning(const Tuning::Store &store);
  // A stored tuning value changed: save it.
  void on_tuning_changed(std::function<void(const Tuning &)> &&cb) { this->tuning_cbs_.push_back(std::move(cb)); }

  // --- inputs from the board ---
  RoomTemperature &room() { return this->room_; }
  void set_altitude(float measured);  // the board's own reading; the altitude offset is added
  void set_pressure(float hpa) { this->pressure_hpa_ = hpa; }
  void set_intake_temperature(float celsius) { this->intake_c_ = celsius; }

  // The board can use Bluetooth thermometers (capability bit 7, room source 2).
  void set_bluetooth_room(bool on) { this->bluetooth_room_ = on; }
  void set_ha_room(bool on) { this->ha_room_ = on; }  // the build has a Home Assistant room sensor
  bool bluetooth_room() const { return this->bluetooth_room_; }
  // A board firmware update runs (board_update.h): every start is refused, whatever the source.
  void set_board_updating(bool on) {
    this->board_updating_ = on;
    this->refresh_();
  }
  // Asked before every start (off -> heat, fan or priming), whatever the source: a refusal stops it.
  void set_start_check(std::function<Outcome(Source)> &&check) { this->start_check_ = std::move(check); }
  // Fills the status fields the controller does not own, at every refresh.
  void on_refresh(std::function<void(Status &)> &&cb) { this->refresh_cbs_.push_back(std::move(cb)); }
  // The board's automation (frost protection, timers, fuel, alerts), for the protocol; may be null.
  void set_automation(Automation *a) { this->automation_ = a; }
  Automation *automation() const { return this->automation_; }
  // An event to every listener (apps, the log), as the heater's own. seq: the event log record
  // behind it (alerts, event 08), readable by the listeners as event_seq().
  void emit_event(Event event, Result result, uint16_t id, const char *text, uint32_t seq = 0) {
    this->event_seq_ = seq;
    for (auto &cb : this->event_cbs_)
      cb(event, result, id, text);
    this->event_seq_ = 0;
  }
  uint32_t event_seq() const { return this->event_seq_; }

  // Call often (every loop is fine). ha_connected: the Home Assistant API has a client.
  void tick(uint32_t now, bool ha_connected);

  // Refreshed by tick() and after every request.
  const Status &status() const { return this->status_; }
  uint32_t now() const { return this->now_; }  // the last tick's time
  void refresh() { this->refresh_(); }

  // Every request and its outcome, for the log: (source, what, outcome). "what" is a log text only:
  // code that needs to know what was asked uses a typed callback (on_stop_asked).
  void on_request(std::function<void(Source, const char *, const Outcome &)> &&cb) {
    this->request_cbs_.push_back(std::move(cb));
  }
  // An accepted request to turn the heater off (set_mode(OFF)), after it is logged.
  void on_stop_asked(std::function<void(Source)> &&cb) { this->stop_cbs_.push_back(std::move(cb)); }
  // Events from the backend and the failsafe.
  void on_event(std::function<void(Event, Result, uint16_t, const char *)> &&cb) {
    this->event_cbs_.push_back(std::move(cb));
  }

  void on_heater_event(Event event, Result result, uint16_t id, const char *text) override;

 protected:
  Outcome finish_(const char *what, Source source, const Outcome &outcome);
  void refresh_();
  void apply_tuning_();
  void thermostat_step_(uint32_t now);
  void send_trims_();
  Mode user_mode_(const HeaterStatus &hs) const { return this->thermo_stopped_ ? Mode::HEAT : hs.mode; }
  Outcome set_mode_(Mode mode, Source source);
  void note_(const char *what, const Outcome &o);

  Heater *heater_;
  Status status_;
  RoomTemperature room_;
  float altitude_m_{NAN};
  float pressure_hpa_{NAN};
  float intake_c_{NAN};
  uint32_t now_{0};
  uint32_t failsafe_ms_{DEFAULT_FAILSAFE_MS};
  uint16_t boot_id_{0};
  uint16_t run_number_{0};  // runs started since boot
  uint32_t run_start_ms_{0};
  // State's run (id, seconds, source) lasts until the heater is off: through stopping and cooling
  // down, after run_source_ has gone back to NONE.
  bool run_shown_{false};
  Source run_shown_source_{Source::NONE};
  uint32_t run_stop_ms_{0};  // when the stop was asked (0: still running); the run time stops there
  Source run_source_{Source::NONE};
  bool failsafe_stopped_{false};
  uint32_t ha_lost_ms_{0};  // 0 = not counting

  HeatMode heat_mode_{HeatMode::POWER};
  float target_c_{20.0f};
  uint8_t power_{0};          // the user's power setting
  Thermostat thermostat_;
  bool thermo_stopped_{false};  // the thermostat has the heater off between cycles: still a heat run
  uint32_t last_thermo_ms_{0};
  int demand_sent_{-1};       // the last power demand handed to the heater in temperature mode
  Tuning tuning_;
  AltitudeTrim altitude_;
  int altitude_trim_{0};
  int trims_sent_fuel_{0x7FFF}, trims_sent_fan_{0x7FFF};
  bool bluetooth_room_{false};
  bool ha_room_{false};
  bool board_updating_{false};
  uint32_t event_seq_{0};
  std::function<Outcome(Source)> start_check_;
  std::vector<std::function<void(Status &)>> refresh_cbs_;
  Automation *automation_{nullptr};
  std::vector<std::function<void(const Tuning &)>> tuning_cbs_;
  std::vector<std::function<void(Source, const char *, const Outcome &)>> request_cbs_;
  std::vector<std::function<void(Source)>> stop_cbs_;
  std::vector<std::function<void(Event, Result, uint16_t, const char *)>> event_cbs_;
};

const char *source_name(Source s);
// Home Assistant's "Heater firmware" (the safety MCU's name and version, "Unknown" before it
// answered) and "Heater firmware update status" ("Not available" without an image to install).
const char *heater_firmware_text(const Heater &h);
const char *firmware_update_status(const Status &s, const Heater &h);

}  // namespace flamekeeper
}  // namespace esphome
