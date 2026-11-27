// The heater as the rest of the firmware sees it: what can be asked of it, what it reports, and its
// parameters. Every backend (the Flamekeeper safety MCU, or another heater's) implements Heater;
// nothing above this file may know which one is in use.
//
// Plain C++ with no ESPHome or ESP-IDF headers, so the controller and the BLE codec built on it run
// as host tests (tests/).
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace esphome {
namespace flamekeeper {

// What is asked of the heater.
enum class Mode : uint8_t { OFF = 0, HEAT = 1, FAN = 2, PRIME = 3 };
enum class HeatMode : uint8_t { POWER = 0, TEMPERATURE = 1 };  // a fixed power, or the thermostat
enum class RoomSource : uint8_t { PROBE = 0, HOME_ASSISTANT = 1, BLUETOOTH = 2 };

// Who asked. A run's source decides whether the Home Assistant failsafe covers it.
// PHONE: an app over Bluetooth; WIFI_APP: a phone app signed in over Wi-Fi; BROWSER: the web page (PROTOCOL.md).
// FROST and TIMER: the board's own automatic starts; never a person.
enum class Source : uint8_t {
  NONE = 0,
  HOME_ASSISTANT = 1,
  PHONE = 2,
  BOARD = 3,
  WIFI_APP = 4,
  FROST = 5,
  TIMER = 6,
  RELAY_APP = 7,  // a phone through the internet relay
  BROWSER = 8,    // the board's web page, signed in with the web password
};
inline bool is_automatic(Source s) { return s == Source::FROST || s == Source::TIMER; }

// The heater's own state, mapped by the backend (PROTOCOL.md).
enum class HeaterState : uint8_t {
  UNKNOWN = 0,
  OFF = 1,
  STARTING = 2,
  HEATING = 3,
  FAN_ONLY = 4,
  STOPPING = 5,
  COOLING = 6,
  PRIMING = 7,
  UPDATING = 8,
};

// What the controller is doing (PROTOCOL.md).
enum class ControlStatus : uint8_t {
  IDLE = 0,
  STARTING = 1,
  RUNNING = 2,
  PRIMING = 3,
  STOPPING = 4,
  NO_REPLY = 5,
};

// Heater faults, mapped by the backend from the heater's own code (PROTOCOL.md).
enum class Fault : uint8_t {
  NONE = 0,
  VOLTAGE = 1,
  VOLTAGE_LOW = 2,
  VOLTAGE_HIGH = 3,
  GLOW_PLUG = 4,
  PUMP = 5,
  OVERHEAT = 6,
  FAN = 7,
  FLAME_OUT = 8,
  TEMP_SENSOR = 9,
  START_FAILURE = 10,
  CO = 11,
  INTAKE_SENSOR = 12,
  COMMS = 13,
  CO_SENSOR = 14,  // the CO sensor's reading is not plausible (open, unheated)
  CONTROLLER = 15, // the safety MCU's own supply or clock failed (PWR-5, GLW-1)
  OTHER = 255,
};

// Why the heater stopped or refused a start, beyond its fault code (HeaterStatus::panel_fault,
// Telemetry byte 19): the Flamekeeper safety MCU's codes (stm32/core/burn.h, BURN_WHY_*). Their text
// and the alert each raises are in one table (controller.cpp, REASONS).
enum class Reason : uint8_t {
  NONE = 0,
  ASKED = 1,            // a normal stop
  DEADMAN = 2,          // the board went silent during a burn
  NO_FLAME = 3,         // every ignition attempt failed
  FLAME_LOST = 4,
  OVERHEAT_LIMIT = 5,   // the firmware's limit
  OVERHEAT_LATCH = 6,   // the hardware latch tripped
  FAN_STALL = 7,        // asked to turn, and it did not: retried (see fault_retrying)
  FAN_SPIN_UP = 8,      // too slow to reach the glow's minimum speed in time: retried
  SENSOR_OPEN = 9,      // the heat-exchanger sensor reads open
  NO_TEMP_RISE = 10,    // fuel and glow, but the heat exchanger never warmed
  SUPPLY = 11,          // the supply voltage out of range
  COOLDOWN_LIMIT = 12,  // the heat exchanger never cooled within the cooldown limit
  CO = 13,              // the CO alarm
  GLOW_STAGE = 14,      // the glow plug's switching stopped it (stuck low side, repeated trips)
  FAN_STAGE = 15,       // the fan's switch stuck on, or the fan turning against its command
  SELF_TEST = 16,       // the pre-start self-test of the interlocks failed
  PUMP_STAGE = 17,      // the pump's circuit (overcurrent, open coil, high side)
  BOARD_HOT = 18,       // the board stayed too hot even at the lowest power
  PLUG_TYPE = 19,       // the glow plug is not the type set, or its check is missing
  CO_SENSOR = 20,       // the CO sensor reads open, or failed its heater check
  VDDA_HIGH = 21,       // the safety MCU's 3.3 V supply above 3.6 V
  VDDA_LOW = 22,        // the safety MCU's 3.3 V supply below 3.0 V, or never up after a reset
  TICK = 23,            // the safety loop's clock and the second clock disagree
};

// The alert a fault raises (automation.h maps it to the alert bit, push.h to the urgency).
enum class FaultAlert : uint8_t {
  NONE = 0,       // shown, never alerted
  BURN = 1,       // "burn problem" (alert bit 2)
  CO_ALARM = 2,   // the CO alarm (alert bit 10)
  CO_SENSOR = 3,  // the CO sensor fault (alert bit 11)
};

// The outcome of a request. Codes are the BLE result codes (PROTOCOL.md).
enum class Result : uint8_t {
  OK = 0x00,
  STORED = 0x01,  // accepted, takes effect later
  UNKNOWN_OPCODE = 0x10,
  BAD_LENGTH = 0x11,
  OUT_OF_RANGE = 0x12,
  NOT_SUPPORTED = 0x13,
  NOT_OFF = 0x20,
  NO_REPLY = 0x21,
  BUSY = 0x22,
  NOT_CONFIRMED = 0x23,
  KEY_UNKNOWN = 0x24,      // Wi-Fi sign-in with a key the board does not have
  WRONG_PASSWORD = 0x25,   // the web password (sign-in, or the current one when changing it)
  LOCKED_OUT = 0x26,       // too many wrong web passwords; try again later
  TABLE_FULL = 0x27,       // no room for another key
  BLUETOOTH_ONLY = 0x28,   // a command only a bonded phone may send
  NO_WEB_PASSWORD = 0x29,  // web sign-in with no web password set
  TOO_BIG = 0x2A,          // a board firmware image larger than the update slot
  HASH_MISMATCH = 0x2B,    // a board firmware image not as announced (SHA-256), or incomplete
  BAD_IMAGE = 0x2C,        // a board firmware image not signed by a trusted key, or not valid
  REFUSED = 0x2F,
};

struct Outcome {
  Result result{Result::OK};
  const char *reason{""};  // English, as shown in the log; static storage

  static Outcome ok() { return {Result::OK, ""}; }
  static Outcome stored(const char *why) { return {Result::STORED, why}; }
  static Outcome refuse(Result r, const char *why) { return {r, why}; }
  bool accepted() const { return result == Result::OK || result == Result::STORED; }
};

// Capability bits (PROTOCOL.md).
namespace cap {
static const uint32_t FAN_ONLY = 1u << 0;
static const uint32_t TEMPERATURE = 1u << 1;
static const uint32_t PRIME = 1u << 2;
static const uint32_t PRIME_STOP = 1u << 3;
static const uint32_t PARAMETERS = 1u << 4;
static const uint32_t ROOM_SOURCE = 1u << 5;
static const uint32_t FIRMWARE_UPDATE = 1u << 6;  // not in the BLE capability word (no BLE command)
static const uint32_t BLUETOOTH_ROOM = 1u << 7;   // Bluetooth thermometers as a room source
static const uint32_t HA_ROOM = 1u << 8;          // a Home Assistant room sensor is set up in this build
static const uint32_t FAULT_LOG = 1u << 9;        // the heater keeps a fault log (fault_log, clear_fault_log)
}  // namespace cap

// Everything the heater reports. NAN / 0xFF... = not reported by this heater.
struct HeaterStatus {
  // what is asked of it (held by the backend, which acts on it)
  Mode mode{Mode::OFF};
  HeatMode heat_mode{HeatMode::POWER};
  uint8_t power{0};  // %, the fixed power demand (and the fan speed in fan-only mode)
  float target_c{20.0f};

  // what it and its controller are doing
  bool answering{false};
  bool update_running{false};
  HeaterState state{HeaterState::UNKNOWN};
  ControlStatus control{ControlStatus::NO_REPLY};
  Fault fault{Fault::NONE};
  uint8_t panel_fault{0};  // why, beyond the fault: the safety MCU's reason code (Reason; Telemetry byte 19)
  uint16_t prime_left_s{0};
  uint8_t params_pending{0};
  bool plug_warning{false};  // the run started although the glow plug may not be the type set
  float update_progress{NAN};  // %, heater firmware update

  // measurements
  float heat_exchanger_c{NAN};
  float supply_v{NAN};
  float fan_rpm{NAN};
  float pump_hz{NAN};
  float glow_w{NAN};
  float glow_v{NAN};
  float fan_v{NAN};  // the fan drive voltage
  float glow_a{NAN};
  float power_pct{NAN};
  float co_ppm{NAN};
  float altitude_m{NAN};
  float pressure_hpa{NAN};  // barometric pressure (a board sensor)
  uint8_t co_state{0};      // CoState: the CO sensor as the safety MCU judges it
  float run_time_s{NAN};  // lifetime running time
  // The heater's own fault log (cap::FAULT_LOG), as last read: its newest entry's number (1-255,
  // 0 when empty) and its entries. Kept while the heater does not answer.
  uint8_t fault_log_seq{0};
  uint8_t fault_log_count{0};
};

// One entry of the heater's own fault log (PROTOCOL.md, HEATER_LOG): the Flamekeeper safety MCU's,
// field for field (DESIGN.md).
struct HeaterLogEntry {
  uint8_t kind{0};   // HEATER_LOG_FAULT, HEATER_LOG_RESET
  uint8_t code{0};   // a fault: the Fault (0: a stop for a reason, the why); a reset: its reason
  uint8_t why{0};    // a fault: why the run or start ended (the heater's own code, as panel_fault)
  uint8_t step{0};   // the heater's own step it happened in (a reset: the step after it)
  uint8_t seq{0};    // 1-255, one more for each entry, wrapping past 0
  int8_t board_c{-128};       // the board's temperature then, C (-128 unknown)
  int16_t hx_c10{INT16_MIN};  // the heat exchanger then, 0.1 C
  uint16_t supply_cv{0xFFFF}; // the supply then, 0.01 V
  uint32_t uptime_s{0};       // seconds since the heater's controller last reset
};
static const uint8_t HEATER_LOG_FAULT = 1;
static const uint8_t HEATER_LOG_RESET = 2;
static const size_t HEATER_LOG_MAX = 32;  // entries a heater's log holds at most

// One parameter in the heater's table (PROTOCOL.md).
namespace pflag {
static const uint8_t NEEDS_OFF = 1u << 0;
static const uint8_t BOARD = 1u << 1;  // a board setting: applied at once, no Apply needed
static const uint8_t READ_ONLY = 1u << 2;
}  // namespace pflag

// The settings menu a parameter belongs in (PROTOCOL.md). Codes are stable; the order
// here is the menu order, with OTHER last.
enum class ParamGroup : uint8_t {
  OTHER = 0,
  THERMOSTAT = 1,
  FUEL_PUMP = 2,
  FAN = 3,
  START_UP = 4,
  GLOW_PLUG = 5,
  SHUT_DOWN = 6,
  ALTITUDE = 7,
  FUEL_USE = 8,
  FROST = 9,
  ROOM = 10,
  BATTERY = 11,
  MAINTENANCE = 12,
};
static const ParamGroup PARAM_GROUP_ORDER[] = {
    ParamGroup::THERMOSTAT, ParamGroup::FROST,     ParamGroup::ROOM,     ParamGroup::FUEL_USE,
    ParamGroup::BATTERY,    ParamGroup::FUEL_PUMP, ParamGroup::FAN,      ParamGroup::START_UP,
    ParamGroup::GLOW_PLUG,  ParamGroup::SHUT_DOWN, ParamGroup::ALTITUDE, ParamGroup::MAINTENANCE,
    ParamGroup::OTHER};

// The CO sensor as the safety MCU judges it (PROTOCOL.md).
enum class CoState : uint8_t {
  NOT_FITTED = 0,
  WARMING_UP = 1,
  LEARNING = 2,
  OK = 3,
  ALARM = 4,
  SENSOR_FAULT = 5,
  TOO_COLD = 6,
};
// Who should see it: everyone, those who open Advanced, or only with Expert mode on.
enum class ParamLevel : uint8_t { BASIC = 0, ADVANCED = 1, EXPERT = 2 };
// What a parameter does, as the app shows it: one or two sentences, at most this many bytes (a
// PARAM_HELP result must fit one Bluetooth indication at the minimum MTU).
static const size_t PARAM_HELP_MAX = 120;
enum class Unit : uint8_t {
  UNITLESS = 0,
  SECONDS = 1,
  PERCENT = 2,
  HERTZ = 3,
  RPM = 4,
  CELSIUS = 5,
  WATTS = 6,
  METRES = 7,
  MINUTES = 8,
  ON_OFF = 9,
  CELSIUS_DIFFERENCE = 10,  // a temperature difference (a band, an offset): x1.8 in F, no +32
  LITRES = 11,              // a volume (the app may show US gallons)
  MILLILITRES = 12,
  VOLTS = 13,
  HOURS = 14,
};  // long names: HZ and friends are macros on some toolchains

struct ParamInfo {
  uint16_t id{0};
  uint8_t flags{0};
  Unit unit{Unit::UNITLESS};
  uint8_t decimals{0};  // shown value = raw / 10^decimals
  int32_t min{0};
  int32_t max{0};
  int32_t step{1};
  const char *name{""};
  ParamGroup group{ParamGroup::OTHER};
  ParamLevel level{ParamLevel::BASIC};
};

struct ParamValue {
  bool known{false};
  bool staged{false};
  int32_t value{0};
  int32_t staged_value{0};
};

// Things a backend reports as they happen.
enum class Event : uint8_t {
  PARAMS_APPLIED = 0x01,  // applied and saved in the heater, or refused
  // 0x02 (saved) is no longer sent: every Apply saves.
  PARAM_CHANGED = 0x03,
  FAILSAFE_STOP = 0x04,
  // 0x05 is the app protocol's Wi-Fi event (ble_protocol.h).
  FAULT_RESET = 0x06,      // the heater's answer to "Reset heater fault"
  PARAMS_DEFAULTS = 0x07,  // the heater's answer to "parameters back to defaults" (factory reset)
  ALERT = 0x08,            // an alert came on (id = the alert bit)
};

class HeaterListener {
 public:
  virtual ~HeaterListener() = default;
  virtual void on_heater_event(Event event, Result result, uint16_t id, const char *text) = 0;
};

// The interface every backend implements. Calls come only from the Controller, which applies the
// checks common to every source; the backend applies its own (wire, state) and says why it refuses.
class Heater {
 public:
  virtual ~Heater() = default;

  virtual uint32_t capabilities() const = 0;
  virtual void read_status(HeaterStatus &out) const = 0;
  // A short description of the heater's firmware ("Extension 0.1 (protocol 1)", ...).
  virtual const char *firmware_text() const { return ""; }
  virtual float min_target() const { return 5.0f; }
  virtual float max_target() const { return 35.0f; }

  virtual Outcome set_mode(Mode mode) = 0;  // PRIME starts priming
  virtual Outcome set_power_demand(uint8_t percent) = 0;  // 0..100
  virtual Outcome set_target(float celsius) = 0;
  virtual Outcome stop_prime() { return Outcome::refuse(Result::NOT_SUPPORTED, "stop priming is not supported"); }
  virtual Outcome start_firmware_update() {
    return Outcome::refuse(Result::NOT_SUPPORTED, "this heater has no firmware update");
  }
  virtual const char *firmware_update_text() const { return ""; }
  // Factory reset: the heater's parameters back to their defaults where the board stores them (the
  // Flamekeeper safety MCU). Heaters that keep their own do nothing.
  virtual Outcome reset_parameters() { return Outcome::ok(); }

  // Inputs from the board: NAN when unknown.
  virtual void set_room_temperature(float celsius) = 0;
  virtual void set_altitude(float metres) { (void) metres; }
  // The fuel trim (%, the altitude trim included) and fan trim (rpm) the controller wants applied.
  virtual void set_trims(int fuel_pct, int fan_rpm) {
    (void) fuel_pct;
    (void) fan_rpm;
  }
  // A new glow plug is fitted: a heater that checks the plug's type forgets the old one.
  virtual Outcome glow_plug_fitted() { return Outcome::ok(); }
  // Clears a latched fault that needs a user reset, if its cause is gone.
  virtual Outcome reset_fault() { return Outcome::refuse(Result::NOT_SUPPORTED, "this heater has no fault reset"); }
  // The heater's fault log (cap::FAULT_LOG), newest first: up to max entries from the from-th newest
  // (0: the newest). Returns how many.
  virtual size_t fault_log(size_t from, HeaterLogEntry *out, size_t max) const {
    (void) from;
    (void) out;
    (void) max;
    return 0;
  }
  // Empties it, in the heater too.
  virtual Outcome clear_fault_log() { return Outcome::refuse(Result::NOT_SUPPORTED, "this heater has no fault log"); }

  // Parameters: an empty table by default.
  virtual size_t param_count() const { return 0; }
  virtual bool param_info(size_t index, ParamInfo &out) const {
    (void) index;
    (void) out;
    return false;
  }
  virtual bool param_get(uint16_t id, ParamValue &out) const {
    (void) id;
    (void) out;
    return false;
  }
  virtual Outcome param_set(uint16_t id, int32_t value) {
    (void) id;
    (void) value;
    return Outcome::refuse(Result::NOT_SUPPORTED, "this heater has no parameters");
  }
  // The staged changes applied and saved in the heater.
  virtual Outcome param_apply() { return Outcome::refuse(Result::NOT_SUPPORTED, "this heater has no parameters"); }
  virtual Outcome param_discard() { return Outcome::refuse(Result::NOT_SUPPORTED, "this heater has no parameters"); }
  // What the parameter does (PARAM_HELP_MAX bytes at most), "" if not known (yet).
  virtual const char *param_help(uint16_t id) const {
    (void) id;
    return "";
  }
  // A text for the parameter state ("Saved in the heater", "2 change(s) not sent yet", ...).
  virtual const char *param_status_text() const { return ""; }

  void set_listener(HeaterListener *listener) { this->listener_ = listener; }

 protected:
  void emit_(Event event, Result result, uint16_t id, const char *text) {
    if (this->listener_ != nullptr)
      this->listener_->on_heater_event(event, result, id, text);
  }
  HeaterListener *listener_{nullptr};
};

const char *heater_state_name(HeaterState s);
const char *control_status_name(ControlStatus s);
const char *fault_name(Fault f);

// The fault and reason table (controller.cpp): every fault and reason code with its text and alert.
// The reason's text, lower case ("the fan stalled"), "" for none; nullptr for a code not in the table.
const char *reason_text(uint8_t reason);
// Whether the heater retries a fault with this reason (while it starts or runs fan only).
bool reason_retried(uint8_t reason);
// The alert a fault raises: by its reason when the table knows it, else by its fault code.
FaultAlert fault_alert(Fault fault, uint8_t reason);
// A fault the heater is still retrying (a fan stall while it starts or runs fan only, FAN-5): the
// fault clears if the retry works, and the run ends with it (alerted then) if not.
bool fault_retrying(const HeaterStatus &s);
// Home Assistant's "Fault reason": the reason in a sentence, "None", or "Reason <n>" for a code not
// in the table. While retrying: "...: trying again".
void fault_reason_text(const HeaterStatus &s, char *out, size_t size);
// The CO sensor's state as Home Assistant shows it ("Unknown" while the heater does not answer).
const char *co_state_name(const HeaterStatus &s);
const char *param_group_name(ParamGroup g);

}  // namespace flamekeeper
}  // namespace esphome
