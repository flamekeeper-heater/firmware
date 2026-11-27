// One WebSocket connection, board side (PROTOCOL.md): the sign-in (with a Wi-Fi
// key, or with the web password over EC J-PAKE), then AES-128-GCM on every frame in both directions.
// The transport (the HTTP server) only moves frames; the records inside are the Bluetooth ones.
//
// Plain C++ on mbedTLS (host-tested in tests/test_wifi_session.cpp against an app-side client).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include "access.h"

namespace esphome {
namespace flamekeeper {

namespace wire {
static const uint8_t INFO = 0x01;
static const uint8_t STATE = 0x02;
static const uint8_t TELEMETRY = 0x03;
static const uint8_t COMMAND = 0x04;
static const uint8_t RESULT = 0x05;
// 0x06 (board firmware data) is no longer taken: firmware updates go over Bluetooth only.
static const uint8_t HELLO = 0x10;
static const uint8_t SIGN_IN = 0x11;
static const uint8_t SIGNED_IN = 0x12;
static const uint8_t JPAKE_BOARD = 0x13;
static const uint8_t JPAKE_APP = 0x14;
static const uint8_t CONFIRM = 0x15;

static const size_t NONCE_SIZE = 16;
static const size_t FRAME_MAX = 1024;    // the longest frame either side may send
static const size_t SEAL_OVERHEAD = 17;  // type byte + GCM tag
static const uint32_t SIGN_IN_TIMEOUT_MS = 30000;

// A connection opened at `opened_ms` has had its time to sign in. Signed difference: a stamp a few
// ms after `now` (an open stamped with a fresh clock during the pass that took `now`) is just
// opened, not 49 days old.
inline bool sign_in_expired(uint32_t now, uint32_t opened_ms) {
  return int32_t(now - opened_ms) > int32_t(SIGN_IN_TIMEOUT_MS);
}
}  // namespace wire

class WifiSession {
 public:
  using Send = std::function<void(const uint8_t *, size_t)>;
  enum class Step : uint8_t { CONTINUE, COMMAND, CLOSE };

  explicit WifiSession(Access &access);
  ~WifiSession();
  WifiSession(const WifiSession &) = delete;
  WifiSession &operator=(const WifiSession &) = delete;

  // Sends Hello (the board's nonce). Call once, after Info.
  void start(const Send &send);
  // A relay channel (PROTOCOL.md): Hello without Info first, sign-in with a key
  // only, and only with the phone key the relay matched (its id here).
  void start_relay(const uint8_t key_id[KEY_ID_SIZE], const Send &send);
  bool relay() const { return this->relay_; }
  // One binary frame from the app. COMMAND: the decrypted Command record is in command
  // (command_cap >= the frame's length). CLOSE: close the connection (close_reason() says why).
  Step receive(const uint8_t *frame, size_t len, uint32_t now, const Send &send, uint8_t *command,
               size_t *command_len, size_t command_cap);

  bool signed_in() const { return this->state_ == State::SIGNED_IN; }
  // The key this connection signed in with (valid once signed in).
  const uint8_t *key_id() const { return this->key_id_; }
  KeyKind key_kind() const { return this->key_kind_; }
  bool key_remembered() const { return this->key_remember_; }
  // The "this tab only" key this connection was given (to revoke when it closes), or nullptr.
  const uint8_t *tab_key_id() const { return this->tab_key_ ? this->tab_key_id_ : nullptr; }
  // The name a browser gave with the web password ("" otherwise).
  const char *close_reason() const { return this->close_reason_; }

  // An encrypted frame for the app (STATE, TELEMETRY, RESULT); out_cap >= len + SEAL_OVERHEAD.
  // Returns its length, 0 if not signed in or out of room.
  size_t seal(uint8_t type, const uint8_t *record, size_t len, uint8_t *out, size_t out_cap);

 protected:
  enum class State : uint8_t { NEW, WAIT_SIGN_IN, WAIT_ROUND_TWO, WAIT_CONFIRM, SIGNED_IN, CLOSED };
  struct Jpake;

  Step close_(const char *why);
  Step sign_in_key_(const uint8_t *p, size_t n, const Send &send);
  Step sign_in_password_(const uint8_t *p, size_t n, uint32_t now, const Send &send);
  Step round_two_(const uint8_t *frame, size_t len);
  Step confirm_(const uint8_t *frame, size_t len, uint32_t now, const Send &send);
  void transcript_(const uint8_t *frame, size_t len);
  static void nonce_(const uint8_t salt[4], uint64_t counter, uint8_t out[12]);

  Access &access_;
  State state_{State::NEW};
  uint8_t board_nonce_[wire::NONCE_SIZE]{};
  bool password_done_{false};  // a J-PAKE sign-in happened: only a key sign-in may follow
  bool relay_{false};
  uint8_t relay_key_id_[KEY_ID_SIZE]{};
  std::unique_ptr<Jpake> jpake_;

  uint8_t key_id_[KEY_ID_SIZE]{};
  uint8_t tab_key_id_[KEY_ID_SIZE]{};
  bool tab_key_{false};
  KeyKind key_kind_{KeyKind::FREE};
  bool key_remember_{true};

  uint8_t key_in_[16]{};   // app -> board
  uint8_t key_out_[16]{};  // board -> app
  uint8_t salt_in_[4]{};
  uint8_t salt_out_[4]{};
  uint64_t count_in_{0};
  uint64_t count_out_{0};
  const char *close_reason_{""};
};

}  // namespace flamekeeper
}  // namespace esphome
