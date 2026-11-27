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

static const size_t PAYLOAD_FIXED = 27;  // up to and including the text length byte
static const size_t TEXT_MAX = 48;
static const size_t NONCE_SIZE = 12;
static const size_t TAG_SIZE = 16;

// The live card (LIVE, PROTOCOL.md).
static const size_t LIVE_RECORD_SIZE = 28;
static const uint8_t LIVE_START = 0, LIVE_UPDATE = 1, LIVE_END = 2;
static const uint8_t LIVE_IMPORTANT = 0x01, LIVE_FAULT_END = 0x02;  // LIVE flags
static const uint16_t LIVE_FAULT_ALERTS = (1u << 1) | (1u << 2) | (1u << 7) | (1u << 10);

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
  static const uint32_t WANT_WAIT_MS = 5000;
  static const uint32_t LIVE_GAP_MS = 20000;        // at most one LIVE per 20 s (an end never waits)...
  static const uint32_t LIVE_PROMPT_GAP_MS = 5000;  // ...or 5 s for a start, an important change, or a
                                                    // target, power, kind or run end change
  static const uint32_t LIVE_HEARTBEAT_MS = 120000;  // an update at least this often while a card runs
  static const int LIVE_ROOM_C10 = 5;                // a room change this big is a change  // after sign-in, wait this long for WANT before sending held pushes

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
  // Changed with the record's layout, so a record saved in another is never read as this one.
  static const uint32_t SAVED_CHECK = 0x50534833UL;  // "PSH3"

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

  // How many PUSH messages went out (tests).
  uint32_t sent() const { return this->sent_; }
  // How many LIVE messages went out (tests).
  uint32_t live_sent() const { return this->live_sent_; }

  // The live card's content (PROTOCOL.md "Live card").
  struct LiveContent {
    uint8_t state{0}, kind{0}, source{0}, fault{0};
    uint16_t alerts{0};
    int16_t room_c10{INT16_MIN}, target_c10{INT16_MIN};
    uint8_t power{0xFF};
    uint32_t run_end_utc{0}, burnoff_end_utc{0}, started_utc{0};
    uint8_t fuel_left{0xFF};
  };
  static size_t encode_live_record(uint32_t utc, const LiveContent &c, uint8_t out[push::LIVE_RECORD_SIZE]);
  // 00 07 || event || flags || run id (u32, never 0) || slots (a bit per key slot: the phones it is
  // for); the plain record follows.
  static const size_t LIVE_HEADER_SIZE = 9;
  static size_t encode_live_header(uint8_t event, uint8_t flags, uint32_t run_id, uint8_t slots,
                                   uint8_t out[LIVE_HEADER_SIZE]);

  // One payload (PROTOCOL.md); exposed for the golden vectors.
  struct Item {
    uint8_t what;  // 0 alert, 1 event
    uint8_t code;  // alert bit, or event bit
    uint8_t source;
    uint32_t seq;
    uint32_t utc;
  };
  static size_t encode_payload(const Item &lead, uint8_t flags, const Status &s, const char *text, uint8_t *out,
                               size_t cap);
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
  // The live card: what it shows now, whether to send, and the sending.
  void live_loop_(uint32_t now, bool ready);
  LiveContent live_content_(uint32_t now, const Status &s);
  bool live_wants_(int slot, uint8_t source) const;
  bool live_send_(uint8_t event, uint8_t flags, uint32_t run_id, const LiveContent &c, uint32_t now);

  Controller &c_;
  Automation &a_;
  Access &access_;
  RelayClient &relay_;
  relay::Random random_;
  std::function<void(const Saved &)> save_cb_;
  Saved saved_{};
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

  // live card
  uint32_t live_run_{0};         // the run the card follows (State's run id), 0 none
  uint8_t live_starter_{0xFF};
  bool live_started_{false};     // its start went out
  bool live_end_owed_{false};    // it ended while the relay was down
  LiveContent live_last_{};      // what was last sent
  LiveContent live_end_{};       // the final content, while the end is owed
  uint8_t live_end_flags_{0};
  uint32_t live_end_run_{0};  // the owed end's run
  uint32_t live_sent_ms_{0};
  bool live_dirty_{false};       // changed since the last send
  bool live_important_{false};   // ...and the change is worth priority
  bool live_prompt_{false};      // ...or worth sending soon (LIVE_PROMPT_GAP_MS)
  bool live_was_ready_{false};
  uint32_t live_sent_{0};
  uint32_t live_burnoff_end_{0};
};

}  // namespace flamekeeper
}  // namespace esphome
