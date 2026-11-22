#include "pairing.h"

#include <cstdio>

namespace esphome {
namespace flamekeeper {

void PairingWindow::open(uint32_t now, uint32_t duration_ms) {
  this->open_ = duration_ms != 0;
  this->failures_ = 0;
  this->opened_ms_ = now;
  this->duration_ms_ = duration_ms;
}

bool PairingWindow::is_open(uint32_t now) const {
  return this->open_ && uint32_t(now - this->opened_ms_) < this->duration_ms_;
}

uint16_t PairingWindow::seconds_left(uint32_t now) const {
  if (!this->is_open(now))
    return 0;
  uint32_t left = this->duration_ms_ - uint32_t(now - this->opened_ms_);
  uint32_t s = (left + 999) / 1000;
  return s > 0xFFFF ? 0xFFFF : uint16_t(s);
}

bool PairingWindow::accept(uint32_t now, uint8_t bonds, uint8_t max_bonds) const {
  return this->is_open(now) && bonds < max_bonds;
}

uint32_t passkey_from_random(const std::function<uint32_t()> &rng) {
  // Reject the top of the range so every passkey is equally likely.
  const uint32_t limit = UINT32_MAX - (UINT32_MAX % PASSKEY_LIMIT);
  for (;;) {
    uint32_t r = rng();
    if (r < limit)
      return r % PASSKEY_LIMIT;
  }
}

void format_passkey(uint32_t key, char *out) { snprintf(out, 7, "%06u", unsigned(key % PASSKEY_LIMIT)); }

bool label_pin(const PinHmac &hmac, uint32_t *pin) {
  const uint32_t limit = UINT32_MAX - (UINT32_MAX % PASSKEY_LIMIT);
  uint8_t msg[8] = {'f', 'k', '1', ' ', 'p', 'i', 'n', 0};
  for (int counter = 0; counter < 256; counter++) {
    msg[7] = uint8_t(counter);
    uint8_t mac[32];
    if (!hmac(msg, sizeof(msg), mac))
      return false;
    uint32_t r = (uint32_t(mac[0]) << 24) | (uint32_t(mac[1]) << 16) | (uint32_t(mac[2]) << 8) | mac[3];
    if (r < limit) {
      *pin = r % PASSKEY_LIMIT;
      return true;
    }
  }
  return false;  // 256 rejections in a row: practically impossible
}

}  // namespace flamekeeper
}  // namespace esphome
