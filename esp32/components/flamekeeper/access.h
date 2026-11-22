// Who may sign in over Wi-Fi (PROTOCOL.md): the
// two key tables (phone apps, browsers), the web password and its lockout.
//
// Plain C++, no ESPHome or ESP-IDF headers (host-tested in tests/test_access.cpp). The board glue
// persists what serialize() returns whenever dirty() is set.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "board.h"

namespace esphome {
namespace flamekeeper {

enum class KeyKind : uint8_t { FREE = 0, APP = 1, BROWSER = 2 };

struct WifiKey {
  KeyKind kind{KeyKind::FREE};
  bool remember{true};          // browser keys only: false = deleted when the connection closes
  uint8_t id[KEY_ID_SIZE]{};
  uint8_t secret[KEY_SECRET_SIZE]{};
  uint8_t owner[6]{};           // app keys: the Bluetooth bond's address
  uint32_t last_used{0};        // a sequence number, for dropping the least recently used browser
};

// Constant-time comparison (false when the lengths differ).
bool equal_ct(const uint8_t *a, size_t a_len, const uint8_t *b, size_t b_len);

// Wrong web passwords: free tries, then a wait that doubles with every further wrong one.
class PasswordLockout {
 public:
  static const uint8_t FREE_TRIES = 5;
  static const uint32_t FIRST_WAIT_S = 60;
  static const uint32_t MAX_WAIT_S = 3600;

  uint32_t seconds_left(uint32_t now) const;
  void failed(uint32_t now);
  void succeeded() {
    this->fails_ = 0;
    this->locked_ = false;
  }
  uint8_t fails() const { return this->fails_; }

  // Password sign-ins are also paced, right or wrong: the J-PAKE maths runs in the main loop and
  // costs nothing to ask for. One start per PACE_MS, board-wide.
  static const uint32_t PACE_MS = 5000;
  uint32_t pace_left_s(uint32_t now) const {
    if (!this->started_)
      return 0;
    uint32_t elapsed = now - this->last_start_ms_;
    return elapsed >= PACE_MS ? 0 : (PACE_MS - elapsed + 999) / 1000;
  }
  void started(uint32_t now) {
    this->started_ = true;
    this->last_start_ms_ = now;
  }

 protected:
  bool started_{false};
  uint32_t last_start_ms_{0};
  uint8_t fails_{0};
  bool locked_{false};
  uint32_t since_ms_{0};
  uint32_t wait_ms_{0};
};

class Access {
 public:
  static const size_t APP_KEYS = 8;
  static const size_t BROWSER_KEYS = 8;
  static const size_t SLOTS = APP_KEYS + BROWSER_KEYS;
  static const size_t ENTRY_SIZE = 1 + KEY_ID_SIZE + KEY_SECRET_SIZE + 6 + 4;
  static const size_t SERIALIZED_MAX = 2 + SLOTS * ENTRY_SIZE;

  using Random = std::function<void(uint8_t *, size_t)>;
  explicit Access(Random random) : random_(std::move(random)) {}
  void random(uint8_t *out, size_t len) { this->random_(out, len); }

  // --- keys ---
  // A new key for a bonded phone; replaces the phone's previous one. nullptr when the table is full.
  const WifiKey *issue_app_key(const uint8_t owner[6]);
  // A new key for a browser that gave the web password; drops the least recently used browser key
  // when the table is full.
  const WifiKey *issue_browser_key(bool remember);
  // The key with this id, or nullptr. Looking a key up changes nothing (a failed sign-in naming a
  // real id must not write to flash: ids travel in the clear).
  const WifiKey *find_key(const uint8_t id[KEY_ID_SIZE]) const;
  bool has_key(const uint8_t id[KEY_ID_SIZE]) const { return this->find_key(id) != nullptr; }
  // A sign-in with this key succeeded: it becomes the most recently used.
  void mark_used(const uint8_t id[KEY_ID_SIZE]);
  bool revoke(const uint8_t id[KEY_ID_SIZE]);
  size_t revoke_owner(const uint8_t owner[6]);
  size_t revoke_browsers();
  void clear();
  uint8_t count(KeyKind kind) const;
  // Phone app keys by slot (0..APP_KEYS-1: their place in the table, the relay's roster slots).
  const WifiKey *app_key(size_t slot) const {
    return slot < APP_KEYS && this->keys_[slot].kind == KeyKind::APP ? &this->keys_[slot] : nullptr;
  }
  // The slot of the app key with this id, or of the key issued to this bonded phone; -1 if none.
  int app_slot(const uint8_t id[KEY_ID_SIZE]) const;
  int app_slot_of_owner(const uint8_t owner[6]) const;

  // --- web password ---
  bool web_password_set() const { return this->web_password_len_ != 0; }
  const uint8_t *web_password() const { return this->web_password_; }
  size_t web_password_len() const { return this->web_password_len_; }
  // From the app: needs the current one (anything when none is set). Empty turns web sign-in off.
  // A new password signs every browser out.
  Outcome change_web_password(const uint8_t *current, size_t current_len, const uint8_t *password, size_t len);
  // Restoring from storage (no checks, no sign-out).
  void load_web_password(const uint8_t *password, size_t len);
  PasswordLockout &lockout() { return this->lockout_; }

  // --- persistence: remembered keys only ---
  size_t serialize(uint8_t *out, size_t cap) const;
  bool deserialize(const uint8_t *in, size_t len);
  bool dirty() const { return this->dirty_; }
  void clean() { this->dirty_ = false; }
  bool web_password_dirty() const { return this->web_password_dirty_; }
  void web_password_clean() { this->web_password_dirty_ = false; }

 protected:
  WifiKey *fresh_(WifiKey *slot, KeyKind kind);
  Random random_;
  WifiKey keys_[SLOTS];
  uint32_t sequence_{0};
  uint8_t web_password_[WEB_PASSWORD_MAX]{};
  size_t web_password_len_{0};
  PasswordLockout lockout_;
  bool dirty_{false};
  bool web_password_dirty_{false};
};

}  // namespace flamekeeper
}  // namespace esphome
