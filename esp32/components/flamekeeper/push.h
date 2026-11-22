// Push notifications, board side (PROTOCOL.md): what to notify (alert rises
// and the events in the board's log), per phone preferences, coalescing, hold-off, CO alarm repeats,
// pushes held while the relay is down, and the per-phone encrypted blobs sent to the relay as PUSH.
//
// Plain C++ on the controller, the automation (its event log and alerts), the key table and the relay
// client (host-tested in tests/test_push.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "access.h"
#include "automation.h"
#include "board.h"
#include "controller.h"
#include "relay.h"

namespace esphome {
namespace flamekeeper {

class RelayClient;

namespace push {
static const uint8_t CO_BIT = 10;

// Event mask bits.
static const uint8_t EV_FROST_STARTED = 0;
static const uint8_t EV_FROST_ENDED = 1;
static const uint8_t EV_TIMER_STARTED = 2;
static const uint8_t EV_TIMER_ENDED = 3;
static const uint8_t EV_STARTED_BY_OTHER = 4;
static const uint8_t EV_STOPPED_BY_OTHER = 5;
static const uint8_t EV_BOARD_RESTARTED = 6;

static const size_t PAYLOAD_FIXED = 36;  // up to and including the text length byte
static const size_t TEXT_MAX = 48;
static const size_t NONCE_SIZE = 12;
static const size_t TAG_SIZE = 16;

// The urgency class of an alert bit (3 alarm, 2 urgent, 1 notice, 0 quiet). Events are 0.
uint8_t alert_class(uint8_t bit);
}  // namespace push

class PushEngine : public PushControl {
 public:
  static const uint32_t COALESCE_MS = 3000;
  static const uint32_t HOLD_MS = 60u * 60u * 1000u;
  static const uint32_t HOLD_BIT4_MS = 6u * 60u * 60u * 1000u;
  static const uint32_t CO_REPEAT_MS = 5u * 60u * 1000u;
  static const uint8_t CO_REPEATS = 2;  // after the first: 3 in all
  static const uint32_t WANT_WAIT_MS = 5000;  // after sign-in, wait this long for WANT before sending held pushes

  // Kept over restarts: the preferences, bound to the key they were set for.
  struct Saved {
    uint32_t check;
    struct Slot {
      uint8_t key_id[KEY_ID_SIZE];
      uint16_t alerts;
      uint16_t events;
      uint8_t flags;
      uint8_t used;
      uint8_t reserved[2];
    } slots[relay::SLOTS];
  };
  static const uint32_t SAVED_CHECK = 0x50534831UL;  // "PSH1"

  PushEngine(Controller &c, Automation &a, Access &access, RelayClient &relay, relay::Random random);

  void load(const Saved &in);
  void on_save(std::function<void(const Saved &)> &&cb) { this->save_cb_ = std::move(cb); }
  // Start reading the event log after this sequence number (the hub passes the record before its own
  // "board started", so a restart can be notified).
  void begin(uint32_t after_seq) {
    this->last_seq_ = after_seq;
    this->begun_ = true;
  }
  void loop(uint32_t now);
  // An app signed in (any transport): someone has seen the board, CO repeats stop.
  void app_seen() { this->app_seen_ = true; }

  // PushControl
  bool prefs_get(int slot, PushPrefs &out) const override;
  Outcome prefs_set(int slot, const PushPrefs &p) override;
  Outcome local_alerts(int slot, bool on) override;
  // A Bluetooth phone's link dropped: it no longer shows alerts itself.
  void bluetooth_gone(int slot);

  // How many PUSH messages went out (tests).
  uint32_t sent() const { return this->sent_; }

  // One payload (PROTOCOL.md); exposed for the golden vectors.
  struct Item {
    uint8_t what;  // 0 alert, 1 event
    uint8_t code;  // alert bit, or event bit
    uint8_t source;
    uint32_t seq;
    uint32_t utc;
  };
  static size_t encode_payload(const Item &lead, uint16_t other_alerts, uint16_t other_events, uint8_t flags,
                               const Status &s, const char *text, uint8_t *out, size_t cap);
  // nonce || AES-128-GCM(P, nonce, payload, AD = board id || class) || tag.
  static size_t seal_blob(const uint8_t key[relay::PUSH_KEY_SIZE], const uint8_t nonce[push::NONCE_SIZE],
                          const uint8_t board_id[relay::ID_SIZE], uint8_t cls, const uint8_t *payload, size_t len,
                          uint8_t *out, size_t cap);

 protected:
  struct Pending {
    Item item;
    uint8_t cls;
  };
  void read_log_(uint32_t now);
  void add_(const Item &it, uint32_t now);
  bool send_(const std::vector<Pending> &items, uint8_t flags, uint32_t now);
  bool wants_(int slot, const std::vector<Pending> &items) const;
  PushPrefs prefs_(int slot) const;
  void save_();

  Controller &c_;
  Automation &a_;
  Access &access_;
  RelayClient &relay_;
  relay::Random random_;
  std::function<void(const Saved &)> save_cb_;
  Saved saved_{};
  bool local_[relay::SLOTS]{};
  bool begun_{false};
  uint32_t last_seq_{0};
  std::vector<Pending> pending_;
  uint32_t window_start_ms_{0};
  std::vector<Pending> held_;         // alerts that rose while the relay was down (latest per bit)
  uint32_t last_push_ms_[16]{};
  bool pushed_once_[16]{};
  char texts_[16][push::TEXT_MAX + 1]{};  // the latest event 08 text per alert bit
  bool co_active_{false};
  uint32_t co_first_ms_{0};
  uint8_t co_repeats_{0};
  Item co_item_{};
  bool app_seen_{false};
  bool relay_was_up_{false};
  uint32_t relay_up_ms_{0};
  uint32_t sent_{0};
};

}  // namespace flamekeeper
}  // namespace esphome
