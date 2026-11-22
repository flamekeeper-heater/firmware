// BLE pairing rules (PROTOCOL.md): a new phone may pair only while a
// pairing window is open (power-up, BOOT, the Home Assistant button), the window closes after one
// pairing or three failed ones, and the bond table has a fixed size. The passkey is the board's
// PIN: the label's (from its eFuse key, "The PIN"), or the one the user set.
//
// Plain C++ (host-tested in tests/test_pairing.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace esphome {
namespace flamekeeper {

class PairingWindow {
 public:
  static const uint32_t DEFAULT_MS = 2 * 60 * 1000;

  void open(uint32_t now, uint32_t duration_ms = DEFAULT_MS);
  void close() { this->open_ = false; }
  bool is_open(uint32_t now) const;
  uint16_t seconds_left(uint32_t now) const;

  // A pairing request from a new phone: accepted while the window is open and the table has room.
  bool accept(uint32_t now, uint8_t bonds, uint8_t max_bonds) const;
  // A phone paired: one phone per window. Clears the failures kept across windows.
  void paired() {
    this->open_ = false;
    if (this->stray_ != 0)
      this->stray_dirty_ = true;
    this->stray_ = 0;
  }
  // A pairing failed (a wrong PIN): the third one in a window closes it.
  void failed() {
    if (++this->failures_ >= MAX_FAILURES)
      this->open_ = false;
    if (this->stray_ < 255)
      this->stray_++;
    this->stray_dirty_ = true;
  }

  // Failed pairings since the last good one, kept over restarts (the board stores them). A phone
  // that fails on purpose learns the PIN a bit at a time, so after POWER_UP_LIMIT of them a power-up
  // no longer opens a window: only a deliberate one (BOOT, Home Assistant) does.
  void load_failures(uint8_t n) { this->stray_ = n; }
  uint8_t failures_since_paired() const { return this->stray_; }
  bool power_up_allowed() const { return this->stray_ < POWER_UP_LIMIT; }
  bool failures_dirty() const { return this->stray_dirty_; }
  void failures_saved() { this->stray_dirty_ = false; }

  static const uint8_t MAX_FAILURES = 3;
  static const uint8_t POWER_UP_LIMIT = 5;

 protected:
  bool open_{false};
  uint8_t failures_{0};
  uint8_t stray_{0};
  bool stray_dirty_{false};
  uint32_t opened_ms_{0};
  uint32_t duration_ms_{0};
};

static const uint32_t PASSKEY_LIMIT = 1000000;  // six digits: 000000..999999

// A uniform six-digit passkey from a 32-bit random source (hardware RNG on the board).
uint32_t passkey_from_random(const std::function<uint32_t()> &rng);
inline bool passkey_valid(uint32_t key) { return key < PASSKEY_LIMIT; }
// "012345": always six digits; out holds 7 bytes.
void format_passkey(uint32_t key, char *out);

// The label PIN: HMAC(board key, "fk1 pin" || counter), first four bytes big-endian, counters 0, 1,
// ... until one falls below the unbiased limit. hmac computes HMAC-SHA256 under the board's key
// (the C6's HMAC peripheral; production uses the master key and the serial). Returns false if the
// HMAC fails.
using PinHmac = std::function<bool(const uint8_t *message, size_t len, uint8_t out[32])>;
bool label_pin(const PinHmac &hmac, uint32_t *pin);

}  // namespace flamekeeper
}  // namespace esphome
