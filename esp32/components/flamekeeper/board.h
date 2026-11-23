// What the apps can ask of the board itself, beside the heater (PROTOCOL.md): the PIN, the web
// password, Wi-Fi, Home Assistant, and the connection the command came in on. The command handler
// (ble_protocol.h) checks lengths and ranges and calls these; the ESPHome components implement them.
//
// Plain C++, no ESPHome or ESP-IDF headers (host-tested through tests/test_ble_protocol.cpp).
#pragma once

#include <cstddef>
#include <cstdint>

#include "bytes.h"
#include "heater.h"

namespace esphome {
namespace flamekeeper {

enum class Transport : uint8_t { BLUETOOTH = 0, WIFI = 1, RELAY = 2 };
enum class WifiState : uint8_t { OFF = 0, CONNECTING = 1, CONNECTED = 2, FAILED = 3 };
enum class HaState : uint8_t { OFF = 0, WAITING = 1, CONNECTED = 2 };

static const size_t SSID_MAX = 32;
static const size_t WIFI_PASSWORD_MIN = 8;  // 0 = an open network
static const size_t WIFI_PASSWORD_MAX = 63;
static const size_t WEB_PASSWORD_MIN = 8;
static const size_t WEB_PASSWORD_MAX = 64;
static const size_t HA_KEY_SIZE = 32;
static const size_t KEY_ID_SIZE = 16;
static const size_t KEY_SECRET_SIZE = 32;

struct WifiStatus {
  WifiState state{WifiState::OFF};
  int8_t rssi{0};      // dBm, 0 when not connected
  uint8_t ip[4]{};     // IPv4, zero when not connected
  char ssid[SSID_MAX + 1]{};
};

// Everything Info (1001) reports beyond the heater's capabilities.
struct BoardInfo {
  bool window_open{false};
  bool bonds_full{false};
  uint16_t window_left_s{0};
  uint8_t bonds{0};
  uint8_t max_bonds{0};
  uint8_t app_keys{0};
  uint8_t max_app_keys{0};
  uint8_t browser_keys{0};
  uint8_t max_browser_keys{0};
  WifiState wifi{WifiState::OFF};
  bool pin_changed{false};
  bool web_password_set{false};
  HaState ha{HaState::OFF};
  bool ha_switchable{false};  // capability bit 6: Home Assistant can be switched from the app
  bool can_update{false};     // capability bit 8: board firmware updates from the app
  bool has_relay{false};      // capability bit 9: the internet relay
  uint8_t version[3]{};       // this build: major, minor, patch
  uint32_t build_utc{0};      // ...and when it was built
};

// A board firmware update from the app (PROTOCOL.md).
enum class UpdateState : uint8_t { NONE = 0, RECEIVING = 1, RECEIVED = 2, INSTALLING = 3 };
enum class UpdateLast : uint8_t { NOTHING = 0, INSTALLED = 1, ROLLED_BACK = 2, DROPPED = 3 };
static const size_t UPDATE_HASH_SIZE = 32;
static const size_t UPDATE_HEADER_SIZE = 4;   // a data chunk: u32 offset, then the bytes
static const size_t UPDATE_REPORT_SIZE = 6;   // progress: u32 next offset, u8 state, u8 reason
// Wi-Fi: a WebSocket frame is at most 1024 bytes (wifi_session.h), less the type, tag and offset.
static const uint16_t UPDATE_CHUNK_WIFI = 1000;

// Why a progress record went out.
enum class UpdateReport : uint8_t {
  PROGRESS = 0,  // bytes taken (every 4096, and the last byte)
  REPEAT = 1,    // a chunk below the expected offset: already taken, nothing lost
  GAP = 2,       // a chunk beyond the expected offset: something was lost, send from `next`
  REFUSED = 3,   // not taken for another reason: no update receiving (see the state), malformed, too long
};

// The progress record (board -> app): u32 the offset expected next, u8 the state, u8 the reason.
inline void encode_update_report(uint32_t next, UpdateState state, UpdateReport why, uint8_t out[UPDATE_REPORT_SIZE]) {
  put_u32(out, next);
  out[4] = uint8_t(state);
  out[5] = uint8_t(why);
}

struct UpdateStatus {
  UpdateState state{UpdateState::NONE};
  uint32_t size{0};
  uint32_t received{0};
  uint8_t sha256[UPDATE_HASH_SIZE]{};
  UpdateLast last{UpdateLast::NOTHING};
};

class BoardUpdater {
 public:
  virtual ~BoardUpdater() = default;
  // offset: where to send from (0, or a resumed update's progress).
  virtual Outcome update_begin(uint32_t size, const uint8_t sha256[UPDATE_HASH_SIZE], uint32_t *offset) = 0;
  virtual void update_status(UpdateStatus &out) const = 0;
  virtual Outcome update_finish() = 0;
  virtual void update_abort() = 0;
};

// The connection a command came in on: a bonded phone, or an app signed in over Wi-Fi.
class Link {
 public:
  virtual ~Link() = default;
  virtual Transport transport() const = 0;
  // Deletes this app's bond (Bluetooth) or its key (Wi-Fi), then disconnects.
  virtual Outcome forget_me() = 0;
  // Bluetooth only: a new Wi-Fi key for this bonded phone (id, then secret).
  virtual Outcome issue_wifi_key(uint8_t id[KEY_ID_SIZE], uint8_t secret[KEY_SECRET_SIZE]) {
    (void) id;
    (void) secret;
    return Outcome::refuse(Result::BLUETOOTH_ONLY, "only a phone paired over Bluetooth can get a Wi-Fi key");
  }
  // The largest board firmware chunk this connection takes (bytes after the offset).
  virtual uint16_t update_chunk_max() const { return UPDATE_CHUNK_WIFI; }
  // The phone key slot of this app (the key it signed in with, or the one issued to this bonded
  // phone), -1 if it has none.
  virtual int phone_slot() const { return -1; }
  // Wi-Fi only: signed in with a browser key (the board's web page), not a phone app's.
  virtual bool browser() const { return false; }
};

// The internet relay as the commands see it (PROTOCOL.md).
enum class RelayState : uint8_t { OFF = 0, CONNECTING = 1, CONNECTED = 2, REFUSED = 3, NO_INTERNET = 4 };
static const size_t RELAY_HOST_MAX = 63;
struct RelayStatus {
  bool on{false};
  RelayState state{RelayState::OFF};
  bool has_id{false};
  uint8_t id[16]{};
  uint32_t since_s{0xFFFFFFFFu};  // since the last connection; 0 while connected
  char host[RELAY_HOST_MAX + 1]{};  // "" = the default
};
class RelayControl {
 public:
  virtual ~RelayControl() = default;
  virtual void relay_status(RelayStatus &out, uint32_t now) const = 0;
  // host: n bytes, already checked (relay_host_ok); n = 0 the default.
  virtual Outcome relay_set(bool on, const uint8_t *host, size_t n) = 0;
};
// A relay host: a name or address with an optional :port; letters, digits, '.', '-', ':'.
inline bool relay_host_ok(const uint8_t *host, size_t len) {
  if (len == 0 || len > RELAY_HOST_MAX || host[0] == '.' || host[0] == '-' || host[0] == ':')
    return false;
  for (size_t i = 0; i < len; i++) {
    const uint8_t c = host[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == ':'))
      return false;
  }
  return true;
}

// Push notification preferences, per phone key (PROTOCOL.md).
namespace push {
static const uint16_t DEFAULT_ALERTS = 0x1FEF;  // every alert but bit 4 (thermometer not heard)
static const uint16_t DEFAULT_EVENTS = 0;
static const uint8_t FLAG_ON = 0x01;           // notifications on (off: nothing but the CO alarm)
static const uint8_t FLAG_LIVE = 0x02;         // the live card on
static const uint8_t FLAG_LIVE_BOARD = 0x04;   // ...for the board's own runs (timer, frost, burn-off)
static const uint8_t FLAG_LIVE_OTHERS = 0x08;  // ...for others' runs (other phones, Home Assistant, the panel)
static const uint8_t FLAGS_KNOWN = 0x0F;
static const uint8_t DEFAULT_FLAGS = FLAG_ON | FLAG_LIVE | FLAG_LIVE_BOARD;
}  // namespace push
struct PushPrefs {
  uint16_t alerts{push::DEFAULT_ALERTS};
  uint16_t events{push::DEFAULT_EVENTS};
  uint8_t flags{push::DEFAULT_FLAGS};
};

// The commands' side (PUSH_PREFS, PUSH_PREFS_GET, LOCAL_ALERTS), on a phone key slot.
class PushControl {
 public:
  virtual ~PushControl() = default;
  virtual bool prefs_get(int slot, PushPrefs &out) const = 0;
  virtual Outcome prefs_set(int slot, const PushPrefs &p) = 0;
  // bit 0: this phone shows alerts itself; bit 1: it watches the live card itself (no LIVE for it).
  virtual Outcome local_alerts(int slot, uint8_t bits) = 0;
};


// Board-wide settings. Arguments arrive checked for length and range.
class BoardSettings {
 public:
  virtual ~BoardSettings() = default;
  virtual Outcome set_pin(uint32_t pin) = 0;
  virtual Outcome set_web_password(const uint8_t *current, size_t current_len, const uint8_t *password,
                                   size_t len) = 0;
  virtual void wifi_status(WifiStatus &out) const = 0;
  virtual Outcome wifi_set(const uint8_t *ssid, size_t ssid_len, const uint8_t *password, size_t len) = 0;
  virtual Outcome wifi_off() = 0;
  virtual bool ha_switchable() const = 0;
  virtual HaState ha_state() const = 0;
  virtual bool ha_key_set() const = 0;
  virtual Outcome ha_set(bool on) = 0;  // restarts the board when it changes
  virtual Outcome ha_key(uint8_t out[HA_KEY_SIZE]) = 0;
  virtual Outcome ha_key_new(uint8_t out[HA_KEY_SIZE]) = 0;
  // Board firmware updates; nullptr when this build takes none.
  virtual BoardUpdater *updater() { return nullptr; }
  // The internet relay and push notifications; nullptr when this build has none.
  virtual RelayControl *relay() { return nullptr; }
  virtual PushControl *push() { return nullptr; }
};

}  // namespace flamekeeper
}  // namespace esphome
