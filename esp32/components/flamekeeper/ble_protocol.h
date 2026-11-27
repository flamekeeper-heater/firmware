// The Flamekeeper protocol's records and commands (PROTOCOL.md), with no transport
// in sight: encoders for Info / State / Telemetry, the command handler that turns a Command record
// into a Controller request (or a board setting) and a Result record, and event records. Bluetooth
// and the Wi-Fi WebSocket carry the same bytes.
//
// Plain C++ (host-tested in tests/test_ble_protocol.cpp).
#pragma once

#include <cstddef>
#include <cstdint>

#include "board.h"
#include "controller.h"

namespace esphome {
namespace flamekeeper {
namespace ble {

static const uint8_t PROTOCOL_MAJOR = 1;

// 81ceXXXX-024c-414c-9ca7-e0972fed52ee
// 1000 the service; characteristics 1001 Info, 1002 State, 1003 Telemetry, 1004 Command,
// 1005 Result, 1006 board update data.
static const char *const SERVICE_UUID = "81ce1000-024c-414c-9ca7-e0972fed52ee";
// The same, as the 16 bytes Bluetooth sends (least significant byte first).
void uuid_bytes(uint16_t short_id, uint8_t out[16]);

enum Board : uint8_t { BOARD_FLAMEKEEPER_REV_A2 = 2 };  // other codes: other boards

static const size_t INFO_SIZE = 14;
static const size_t STATE_SIZE = 39;
static const size_t TELEMETRY_SIZE = 45;
// What a result or event record may grow to: one indication at the minimum ATT MTU of 135.
static const size_t RESULT_MAX = 132;
// The longest command with room to spare: WEB_PASSWORD_SET with two 64-byte passwords is request
// id, opcode, 1 + 64, 1 + 64 = 132 bytes (WIFI_SET is at most 100). handle_command checks every
// command's own length.
static const size_t COMMAND_MAX = 160;
static const size_t PARAM_NAME_MAX = 40;    // parameter names in PARAM_DESCRIBE
static const size_t PARAM_DESC_FIXED = 21;  // PARAM_DESCRIBE before the name
static const uint8_t PRIME_CONFIRM = 0xA5;

namespace op {
static const uint8_t SET_MODE = 0x01;
static const uint8_t SET_HEAT_MODE = 0x02;
static const uint8_t SET_POWER = 0x03;  // 0-100 %
static const uint8_t SET_TARGET = 0x04;
static const uint8_t SET_ROOM_SOURCE = 0x06;
static const uint8_t PRIME_START = 0x07;
static const uint8_t PRIME_STOP = 0x08;
static const uint8_t FAULT_RESET = 0x09;
static const uint8_t HEATER_LOG = 0x0A;        // the heater's own fault log, newest first
static const uint8_t HEATER_LOG_CLEAR = 0x0B;
static const uint8_t PARAM_DESCRIBE = 0x20;
static const uint8_t PARAM_GET = 0x21;
static const uint8_t PARAM_SET = 0x22;
static const uint8_t PARAM_APPLY = 0x23;
static const uint8_t PARAM_DISCARD = 0x24;
static const uint8_t PARAM_GROUP = 0x25;
static const uint8_t PARAM_HELP = 0x26;
static const uint8_t PARAM_DESCRIBE_ID = 0x27;
static const uint8_t FORGET_ME = 0x30;
static const uint8_t WIFI_KEY = 0x32;
static const uint8_t PIN_SET = 0x40;
static const uint8_t WEB_PASSWORD_SET = 0x41;
static const uint8_t WIFI_STATUS = 0x50;
static const uint8_t WIFI_SET = 0x51;
static const uint8_t WIFI_OFF = 0x52;
static const uint8_t HA_STATUS = 0x60;
static const uint8_t HA_SET = 0x61;
static const uint8_t HA_KEY = 0x62;
static const uint8_t HA_KEY_NEW = 0x63;
static const uint8_t CLOCK_SET = 0x70;
static const uint8_t CLOCK_GET = 0x71;
static const uint8_t METERS = 0x72;
static const uint8_t TANK_SET = 0x73;
static const uint8_t ALERTS_CLEAR = 0x74;
static const uint8_t AUTO_REARM = 0x75;
static const uint8_t TIMER_GET = 0x76;
static const uint8_t TIMER_SET = 0x77;
static const uint8_t TIMER_DELETE = 0x78;
static const uint8_t LOG_READ = 0x79;
static const uint8_t GLOW_HEALTH = 0x7A;
static const uint8_t GLOW_NEW_PLUG = 0x7B;
static const uint8_t BURNOFF_START = 0x7C;
static const uint8_t RUN_UNTIL = 0x7D;  // u32 UTC: the current run stops then (0 = no end)
static const uint8_t START_AT = 0x7E;   // u32 UTC start (0 = cancel), u16 run minutes
static const uint8_t SENSOR_SCAN = 0x80;
static const uint8_t SENSOR_FOUND = 0x81;
static const uint8_t SENSOR_USE = 0x82;
static const uint8_t SENSOR_STATUS = 0x83;
static const uint8_t UPDATE_BEGIN = 0x90;
static const uint8_t UPDATE_STATUS = 0x91;
static const uint8_t UPDATE_FINISH = 0x92;
static const uint8_t UPDATE_ABORT = 0x93;
static const uint8_t RELAY_STATUS = 0xA0;
static const uint8_t RELAY_SET = 0xA1;
static const uint8_t PUSH_PREFS = 0xA2;
static const uint8_t PUSH_PREFS_GET = 0xA3;
}  // namespace op

// Capability bit 6 (Info): Home Assistant can be switched from the app.
static const uint32_t CAP_HA_SWITCH = 1u << 6;
// Capability bit 7: Bluetooth thermometers.
static const uint32_t CAP_BLUETOOTH_ROOM = 1u << 7;
// Capability bit 8: board firmware updates from the app (UPDATE_*).
static const uint32_t CAP_BOARD_UPDATE = 1u << 8;
// Capability bit 9: the internet relay and push notifications (RELAY_*, PUSH_*).
static const uint32_t CAP_RELAY = 1u << 9;
static const uint32_t CAP_HA_ROOM = 1u << 10;  // a Home Assistant room temperature sensor is set up
static const uint32_t CAP_HEATER_LOG = 1u << 11;  // the heater keeps a fault log (HEATER_LOG, State byte 32)
static const size_t HEATER_LOG_ENTRY_SIZE = 14;
static const size_t HEATER_LOG_PAGE = 8;  // entries in one HEATER_LOG result
static const size_t PUSH_PREFS_SIZE = 4;  // u16 alerts, u8 events, u8 flags
// Bluetooth: an Update data write is the ATT MTU less the ATT header (3) and the offset (4).
static const size_t UPDATE_BLE_OVERHEAD = 7;
static const size_t TIMER_RECORD_SIZE = 15;
static const size_t SENSOR_NAME_MAX = 20;

// Event 05: a Wi-Fi change finished.
static const uint8_t EVENT_WIFI_CHANGED = 0x05;

size_t encode_info(uint8_t board, uint32_t heater_capabilities, const BoardInfo &info, uint8_t *out);
size_t encode_state(const Status &s, uint8_t *out);
size_t encode_telemetry(const Status &s, uint8_t *out);

// One Command record: runs it through the controller (source: PHONE over Bluetooth, WIFI_APP over
// Wi-Fi) or the board settings, and writes the Result record into out (out_cap >= RESULT_MAX).
// Returns its length; 0 when there is nothing to answer (an empty record, or request id 0, which is
// reserved for events).
size_t handle_command(const uint8_t *in, size_t len, Controller &controller, Link &link, BoardSettings &board,
                      uint8_t *out, size_t out_cap);

// An unsolicited event record (request id 0); 0 for an event that is not sent to apps.
size_t encode_event(Event event, Result result, uint16_t id, const char *text, uint8_t *out, size_t out_cap);
// Event 05: connected (address and SSID), or failed with the reason (the old settings are back).
size_t encode_wifi_event(const WifiStatus *connected, const char *failure, uint8_t *out, size_t out_cap);

}  // namespace ble
}  // namespace flamekeeper
}  // namespace esphome
