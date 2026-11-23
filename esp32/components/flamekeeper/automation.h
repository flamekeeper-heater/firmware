// What the board does by itself (PROTOCOL.md): frost
// protection and timers (automatic starts, through the controller like any other source), the fuel
// meters and tank, alerts, the clock, and the Bluetooth thermometers' readings into the room
// temperature.
//
// Frost protection and timers stay armed across restarts and start fresh when
// due; a refused or failed automatic start, or the heater stopping by itself in an automatic run,
// disarms both until AUTO_REARM. They never retry by themselves.
//
// Plain C++ (host-tested in tests/test_automation.cpp). Time: a wrapping uint32_t millisecond count
// as everywhere, plus UTC seconds from the clock (0 = not known) and the local time zone through
// the C library (TZ).
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "controller.h"
#include "event_log.h"
#include "fuel_meter.h"
#include "room_sensors.h"

namespace esphome {
namespace flamekeeper {

// One timer slot (PROTOCOL.md); all zero = empty.
struct TimerSpec {
  uint8_t flags{0};  // bit 0 enabled, bit 1 one-off
  uint8_t days{0};   // weekly: bit 0 Monday .. bit 6 Sunday
  uint16_t start_min{0};  // weekly: minutes after local midnight
  uint32_t once_utc{0};   // one-off: start
  uint16_t duration_min{0};
  uint8_t mode{0};       // 1 heat, 2 fan only
  uint8_t heat_mode{0};  // 0 power, 1 temperature
  uint8_t power{0};
  int16_t target_x10{0};

  static const uint8_t ENABLED = 0x01;
  static const uint8_t ONE_OFF = 0x02;
  bool enabled() const { return (this->flags & ENABLED) != 0; }
  bool one_off() const { return (this->flags & ONE_OFF) != 0; }
  bool empty() const { return this->flags == 0 && this->duration_min == 0 && this->mode == 0; }
};

namespace alert {
static const uint16_t FUEL_LOW = 1u << 0;
static const uint16_t FUEL_STOP = 1u << 1;
static const uint16_t BURN = 1u << 2;
static const uint16_t AUTO_OFF = 1u << 3;
static const uint16_t SENSOR_LOST = 1u << 4;
static const uint16_t NO_CLOCK = 1u << 5;
static const uint16_t FROST_NO_TEMP = 1u << 6;
static const uint16_t BATTERY_LOW = 1u << 7;
static const uint16_t GLOW_DRIFT = 1u << 8;
static const uint16_t BURNOFF_DUE = 1u << 9;
static const uint16_t CO_ALARM = 1u << 10;
static const uint16_t CO_SENSOR = 1u << 11;
static const uint16_t GLOW_PLUG_TYPE = 1u << 12;  // a run started with a plug-type warning
}  // namespace alert
// An alert bit's name, as Home Assistant's "Alerts" lists it; nullptr past the last bit.
const char *alert_name(uint8_t bit);

namespace automatic {
static const uint8_t FROST_ON = 1u << 0;
static const uint8_t DISARMED = 1u << 1;
static const uint8_t CLOCK_KNOWN = 1u << 2;
static const uint8_t TIMER_ENABLED = 1u << 3;
}  // namespace automatic

enum class ClockSource : uint8_t { NONE = 0, APP = 1, HOME_ASSISTANT = 2 };

class Automation {
 public:
  static const size_t TIMERS = 8;
  static const size_t TZ_MAX = 63;
  static const uint32_t SAVED_CHECK = 0x41544F31UL;  // "ATO1"
  static const uint32_t METER_SAVE_MS = 5 * 60 * 1000;
  static const uint32_t EARLIEST_UTC = 1735689600u;  // 2025-01-01

  // Kept over restarts (the board's preferences); the fuel meters are kept apart (FuelMeter::Saved).
  struct Saved {
    uint32_t check;
    TimerSpec timers[TIMERS];
    uint8_t disarmed;
    uint8_t sensor_address[6];
    uint8_t sensor_kind;
    uint8_t clock_source;
    char tz[TZ_MAX + 1];
  };

  // Glow plug readings and the carbon burn-off counter, kept with the meters.
  static const size_t GLOW_READINGS = 16;
  struct Maintenance {
    uint32_t check;
    uint16_t glow_baseline_mohm;  // 0xFFFF: none yet
    uint16_t glow_starts;         // readings since the new plug
    uint8_t glow_n;
    uint16_t glow_readings[GLOW_READINGS];  // oldest first
    uint32_t low_power_s;  // burner time at low power since the last burn-off
  };
  static const uint32_t MAINTENANCE_CHECK = 0x4D4E5431UL;  // "MNT1"

  explicit Automation(Controller &controller);

  // Call often (every loop is fine); the work happens about once a second.
  void tick(uint32_t now);

  // --- clock ---
  // From an app or Home Assistant. tz: a POSIX TZ string ("" keeps the one the board has).
  Outcome set_clock(uint32_t utc, const char *tz, ClockSource source, uint32_t now);
  // The system clock after a restart (kept over it, not over a power cut): no log, no save.
  void restore_clock(uint32_t utc, uint32_t now);
  // A clock set is logged when the clock was unknown, off by more than this, or the zone changed.
  static const uint32_t CLOCK_LOG_S = 60;
  uint32_t utc(uint32_t now) const;  // 0 = not known
  ClockSource clock_source() const { return this->clock_source_; }
  const std::string &time_zone() const { return this->tz_; }
  // The system clock was set (the board keeps it over a restart): the ESPHome side sets it there.
  void on_clock_set(std::function<void(uint32_t utc)> &&cb) { this->clock_cb_ = std::move(cb); }

  // --- fuel ---
  FuelMeter::Totals meters(uint32_t now) const;
  Outcome tank_set(uint8_t percent, Source source);
  const FuelMeter &meter() const { return this->meter_; }

  // --- alerts and automatic starts ---
  uint16_t alerts() const { return this->alerts_; }
  Outcome clear_alerts(uint16_t mask);
  Outcome rearm(Source source);
  bool disarmed() const { return this->disarmed_; }

  // --- timers ---
  bool timer_get(size_t slot, TimerSpec &out) const;
  Outcome timer_set(size_t slot, const TimerSpec &t);
  Outcome timer_delete(size_t slot);

  // --- maintenance, battery, CO ---
  Outcome burnoff_start(uint8_t minutes, Source source);
  // The current run, whoever started it, stops at end_utc (0: no end). Only while it runs, with the
  // clock known, at most 24 hours ahead; the run's end clears it, and so does a board restart (which
  // stops the heater).
  Outcome run_until(uint32_t end_utc, Source source);
  uint8_t burnoff_minutes_left(uint32_t now) const;
  const Maintenance &maintenance() const { return this->maint_; }
  Outcome glow_new_plug();
  // The board's restart, for the event log (ESP-IDF's reset reason).
  void log_board_started(uint16_t reason) { this->log_(LogKind::BOARD_STARTED, Source::NONE, reason, 0); }
  void log_event(LogKind kind, Source source, uint16_t a, uint32_t b) { this->log_(kind, source, a, b); }
  EventLog &log() { return this->log_book_; }

  // --- Bluetooth thermometers ---
  RoomSensors &sensors() { return this->sensors_; }
  void sensor_use(const uint8_t address[6], ThermoKind kind);

  // --- persistence ---
  void load(const Saved &in);
  void load_meter(const FuelMeter::Saved &in) { this->meter_.load(in); }
  void on_save(std::function<void(const Saved &)> &&cb) { this->save_cb_ = std::move(cb); }
  void on_meter_save(std::function<void(const FuelMeter::Saved &)> &&cb) { this->meter_cb_ = std::move(cb); }
  void load_maintenance(const Maintenance &in) {
    if (in.check == MAINTENANCE_CHECK && in.glow_n <= GLOW_READINGS)
      this->maint_ = in;
  }
  void on_maintenance_save(std::function<void(const Maintenance &)> &&cb) { this->maint_cb_ = std::move(cb); }
  void load_log(const EventLog::Saved &in) { this->log_book_.load(in); }
  void on_log_save(std::function<void(const EventLog::Saved &)> &&cb) { this->log_cb_ = std::move(cb); }

  // For tests and the log: whether the current run is one this started.

 protected:
  struct Restore {
    bool set{false};
    HeatMode heat_mode{HeatMode::POWER};
    float target_c{20.0f};
    uint8_t power{0};
  };

  void step_(uint32_t now);
  void step_fuel_(uint32_t now, const Status &s, uint32_t utc);
  void step_runs_(uint32_t now, const Status &s);
  void step_burn_(uint32_t now, const Status &s);
  void step_frost_(uint32_t now, const Status &s);
  void step_timers_(uint32_t now, const Status &s, uint32_t utc);
  void step_battery_(uint32_t now, const Status &s);
  void step_glow_(uint32_t now, const Status &s);
  void step_maintenance_(uint32_t now, const Status &s, float dt);
  void step_co_(const Status &s);
  void step_fault_(const Status &s);
  void glow_reading_(uint16_t mohm);
  void save_maintenance_now_();
  void log_(LogKind kind, Source source, uint16_t a, uint32_t b);
  // Why a start is refused now (low fuel, low battery, CO), or ok.
  Outcome start_check_() const;
  void set_alert_(uint16_t bit, bool on, const char *text);
  void raise_(uint16_t bit, const char *text);
  // Starts a run as asked; false when it was refused (then disarmed, unless it only waits for the
  // cooldown: *wait).
  bool start_(Source source, Mode mode, HeatMode heat_mode, uint8_t power, float target_c, bool *wait);
  void stop_(Source source, const char *why);
  void restore_();
  void disarm_(const char *why);
  void save_();
  void save_meter_();
  int32_t local_day_(uint32_t utc) const;
  // The local start (UTC) of a weekly timer on the local date `days_ago` days before utc; 0 if the
  // timer does not run that day.
  uint32_t weekly_start_(const TimerSpec &t, uint32_t utc, int days_ago) const;
  void apply_tz_() const;
  uint8_t fuel_left_() const;

  Controller &c_;
  RoomSensors sensors_;
  FuelMeter meter_;
  TimerSpec timers_[TIMERS];
  uint32_t fired_[TIMERS] = {};  // the occurrence (its UTC start) each timer last acted on

  // clock
  uint32_t utc_base_{0};
  uint32_t utc_base_ms_{0};
  uint32_t boot_utc_{0};  // the time the board started, once known: occurrences before it are not resumed
  ClockSource clock_source_{ClockSource::NONE};
  std::string tz_;

  // runs
  uint32_t last_ms_{0};
  bool started_{false};
  Source prev_run_{Source::NONE};
  Source auto_run_{Source::NONE};
  int timer_slot_{-1};
  uint32_t run_end_utc_{0};  // the current run stops then: its timer's end, or RUN_UNTIL (0 none)
  bool stop_asked_{false};    // someone (or this) asked for off during the current run
  bool person_off_{false};    // a person did
  bool own_stop_{false};      // this is stopping it (low fuel), not a person
  uint32_t sensor_chosen_ms_{0};
  Restore saved_settings_;
  bool disarmed_{false};
  uint16_t alerts_{0};
  Fault prev_fault_{Fault::NONE};
  uint8_t prev_reason_{0};
  bool fault_alerted_{false};  // the current fault has raised its alert (or step_co_ raises it)
  bool fault_retried_{false};  // ...it is a retry the heater has not given up yet

  // burn check: the heat exchanger every 10 s over 5 minutes, and the fuel and air it burns on
  bool priming_run_{false};  // the current run is a fuel prime
  float hx_window_[30];
  size_t hx_count_{0};
  uint32_t hx_last_ms_{0};
  uint32_t heating_since_ms_{0};
  bool burn_warned_{false};    // the fast fall or the fading flame has been alerted this run
  float fuel_ref_hz_{NAN};     // the pump rate since the last fuel cut (it follows rises)
  float air_ref_rpm_{NAN};     // the fan speed since the last air rise (it follows falls)
  uint32_t steady_since_ms_{0};  // fuel and air steady since (the last cut, or heating began)
  float fading_from_c_{NAN};   // the level the fading flame was warned from; NAN = not fading

  // battery
  uint32_t battery_low_since_ms_{0};
  // glow plug
  uint32_t glow_on_since_ms_{0};
  uint16_t glow_window_[10];
  size_t glow_window_n_{0};
  bool glow_read_this_run_{false};
  // maintenance
  Maintenance maint_{MAINTENANCE_CHECK, 0xFFFF, 0, 0, {}, 0};
  float high_power_s_{0.0f};
  bool burnoff_active_{false};
  bool burnoff_done_{false};
  uint8_t burnoff_minutes_{0};
  uint32_t burnoff_end_ms_{0};
  // log
  EventLog log_book_;
  uint32_t log_saved_ms_{0};

  uint32_t meter_saved_ms_{0};
  std::function<void(const Maintenance &)> maint_cb_;
  std::function<void(const EventLog::Saved &)> log_cb_;
  std::function<void(uint32_t)> clock_cb_;
  std::function<void(const Saved &)> save_cb_;
  std::function<void(const FuelMeter::Saved &)> meter_cb_;
};

}  // namespace flamekeeper
}  // namespace esphome
