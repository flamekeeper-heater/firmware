#include "relay_client.h"

#include <cstdio>

#include "crypto.h"

namespace esphome {
namespace flamekeeper {

const char *relay_state_name(RelayState s) {
  switch (s) {
    case RelayState::OFF: return "Off";
    case RelayState::CONNECTING: return "Connecting";
    case RelayState::CONNECTED: return "Connected";
    case RelayState::REFUSED: return "Refused by the relay";
    case RelayState::NO_INTERNET: return "No internet";
  }
  return "Off";
}

// As the relay sees the URL the board dialled: lower case, no default port.
static void normalise_host(char *h) {
  for (char *p = h; *p; p++)
    if (*p >= 'A' && *p <= 'Z')
      *p = char(*p - 'A' + 'a');
  const size_t n = std::strlen(h);
  if (n > 4 && std::strcmp(h + n - 4, ":443") == 0)
    h[n - 4] = '\0';
}

void RelayClient::configure(bool on, const char *host, const uint8_t *private_key) {
  this->id_.clear();
  if (private_key != nullptr)
    this->id_.load(private_key, this->random_);
  this->on_ = on && this->id_.valid();
  std::memset(this->host_, 0, sizeof(this->host_));
  if (host != nullptr)
    std::strncpy(this->host_, host, relay::HOST_MAX);
  normalise_host(this->host_);
  this->state_ = this->on_ ? RelayState::CONNECTING : RelayState::OFF;
}

bool RelayClient::set(bool on, const uint8_t *host, size_t host_len) {
  char h[relay::HOST_MAX + 1] = {};
  if (host != nullptr && host_len != 0)
    std::memcpy(h, host, host_len > relay::HOST_MAX ? relay::HOST_MAX : host_len);
  normalise_host(h);
  if (std::strcmp(h, relay::DEFAULT_HOST) == 0)
    h[0] = '\0';  // the default, stored as such
  const bool host_changed = std::strcmp(h, this->host_) != 0;
  if (on && !this->id_.valid() && !this->id_.generate(this->random_))
    return false;
  std::memcpy(this->host_, h, sizeof(h));
  const bool was = this->on_;
  this->on_ = on;
  if (this->linked_ || this->trying_) {
    if (!on || host_changed) {
      this->close_all_channels_(relay::CLOSE_NORMAL);
      if (this->io_.disconnect)
        this->io_.disconnect();
      this->linked_ = this->authed_ = this->trying_ = false;
    }
  }
  if (on && (!was || host_changed)) {
    this->next_try_ms_ = 0;  // at once
    this->backoff_ms_ = BACKOFF_FIRST_MS;
  }
  this->state_ = on ? (this->authed_ ? RelayState::CONNECTED : RelayState::CONNECTING) : RelayState::OFF;
  if (this->settings_cb_)
    this->settings_cb_();
  return true;
}

void RelayClient::loop(uint32_t now, bool wifi_up) {
  if (!this->on_) {
    this->state_ = RelayState::OFF;
    return;
  }
  if (!wifi_up) {
    if (this->linked_ || this->trying_) {
      this->close_all_channels_(relay::CLOSE_NORMAL);
      if (this->io_.disconnect)
        this->io_.disconnect();
      this->linked_ = this->authed_ = this->trying_ = false;
    }
    this->state_ = RelayState::NO_INTERNET;
    return;
  }
  if (this->linked_) {
    if (!this->authed_ && uint32_t(now - this->linked_ms_) >= AUTH_TIMEOUT_MS) {
      if (this->io_.disconnect)
        this->io_.disconnect();
      this->linked_ = this->trying_ = false;
      this->fail_(now, this->backoff_ms_);
      return;
    }
    if (this->authed_) {
      if (!this->roster_sent_ || this->roster_changed_())
        this->send_roster_();
      if (uint32_t(now - this->linked_ms_) >= BACKOFF_RESET_MS)
        this->backoff_ms_ = BACKOFF_FIRST_MS;
    }
    return;
  }
  if (this->trying_)
    return;
  if (this->next_try_ms_ != 0 && int32_t(now - this->next_try_ms_) < 0)
    return;
  if (!this->io_.connect)
    return;
  char path[64];
  char id[relay::ID_TEXT_SIZE + 1];
  relay::id_text(this->id_.id(), id);
  std::snprintf(path, sizeof(path), "/v1/board/%s", id);
  this->trying_ = true;
  if (this->state_ != RelayState::REFUSED)
    this->state_ = this->no_answer_ ? RelayState::NO_INTERNET : RelayState::CONNECTING;
  this->io_.connect(this->host(), path);
}

void RelayClient::connected(uint32_t now) {
  this->trying_ = false;
  this->no_answer_ = false;
  this->linked_ = true;
  this->authed_ = false;
  this->linked_ms_ = now;
  this->roster_sent_ = false;
}

void RelayClient::fail_(uint32_t now, uint32_t wait_ms) {
  // ±20 % at random, so boards that lost the relay together do not come back together.
  uint8_t r[2];
  this->random_(r, 2);
  const uint32_t spread = wait_ms / 5;
  const uint32_t jitter = spread == 0 ? 0 : uint32_t((uint32_t(r[0]) << 8 | r[1]) % (2 * spread + 1));
  this->next_try_ms_ = now + wait_ms - spread + jitter;
  if (this->next_try_ms_ == 0)
    this->next_try_ms_ = 1;
  this->backoff_ms_ = this->backoff_ms_ >= BACKOFF_MAX_MS / 2 ? BACKOFF_MAX_MS : this->backoff_ms_ * 2;
}

void RelayClient::closed(uint16_t code, uint32_t now) {
  const bool was_linked = this->linked_;
  const bool was_authed = this->authed_;
  this->close_all_channels_(relay::CLOSE_NORMAL);
  this->linked_ = this->authed_ = this->trying_ = false;
  if (!this->on_) {
    this->state_ = RelayState::OFF;
    return;
  }
  if (was_authed) {
    this->ever_connected_ = true;
    this->last_connected_ms_ = now;
  }
  if (code == relay::CLOSE_REPLACED) {
    this->state_ = RelayState::CONNECTING;
    this->fail_(now, REPLACED_WAIT_MS);
    return;
  }
  this->no_answer_ = !was_linked;
  if (code == relay::CLOSE_AUTH_FAILED) {
    this->state_ = RelayState::REFUSED;
  } else {
    this->state_ = was_linked ? RelayState::CONNECTING : RelayState::NO_INTERNET;
  }
  // A connection that was up for a while starts the back-off again from the bottom.
  if (was_authed && uint32_t(now - this->linked_ms_) >= BACKOFF_RESET_MS)
    this->backoff_ms_ = BACKOFF_FIRST_MS;
  this->fail_(now, this->backoff_ms_);
}

void RelayClient::message(const uint8_t *data, size_t len, uint32_t now) {
  (void) now;
  if (!this->linked_ || len == 0)
    return;
  const uint8_t ch = data[0];
  if (ch != 0) {
    if (ch <= relay::CHANNELS && this->channel_slot_[ch] >= 0 && this->ch_.frame)
      this->ch_.frame(ch, data + 1, len - 1);
    return;
  }
  relay::Control c;
  if (!relay::parse_control(data, len, c))
    return;  // unknown or malformed control: ignored
  switch (c.op) {
    case relay::op::CHALLENGE: {
      uint8_t m[2 + relay::PUBLIC_KEY_SIZE + relay::SIGNATURE_SIZE];
      size_t n = relay::encode_auth(this->id_, c.challenge, this->host(), this->random_, m, sizeof(m));
      if (n != 0 && this->io_.send && this->io_.send(m, n)) {
        this->authed_ = true;
        this->authed_ms_ = now;
        this->state_ = RelayState::CONNECTED;
        this->send_roster_();
      }
      return;
    }
    case relay::op::OPEN: {
      if (!this->authed_)
        return;
      if (this->channel_slot_[c.channel] >= 0 && this->ch_.close)
        this->ch_.close(c.channel, relay::CLOSE_NORMAL);  // a new phone session replaces the old
      this->channel_slot_[c.channel] = int8_t(c.slot);
      if (this->ch_.open)
        this->ch_.open(c.channel, c.slot);
      return;
    }
    case relay::op::CLOSE:
      if (this->channel_slot_[c.channel] >= 0) {
        this->channel_slot_[c.channel] = -1;
        if (this->ch_.close)
          this->ch_.close(c.channel, c.code);
      }
      return;
    default:
      return;
  }
}

void RelayClient::close_channel(uint8_t channel, uint16_t code) {
  if (channel < 1 || channel > relay::CHANNELS || this->channel_slot_[channel] < 0)
    return;
  this->channel_slot_[channel] = -1;
  if (this->authed_ && this->io_.send) {
    uint8_t m[5];
    relay::encode_close(channel, code, m);
    this->io_.send(m, sizeof(m));
  }
}

void RelayClient::close_all_channels_(uint16_t code) {
  for (uint8_t ch = 1; ch <= relay::CHANNELS; ch++) {
    if (this->channel_slot_[ch] < 0)
      continue;
    this->channel_slot_[ch] = -1;
    if (this->ch_.close)
      this->ch_.close(ch, code);
  }
}

bool RelayClient::send_frame(uint8_t channel, const uint8_t *frame, size_t len) {
  if (!this->authed_ || channel < 1 || channel > relay::CHANNELS || this->channel_slot_[channel] < 0 ||
      len + 1 > relay::MESSAGE_MAX || !this->io_.send)
    return false;
  uint8_t m[relay::MESSAGE_MAX];
  m[0] = channel;
  std::memcpy(m + 1, frame, len);
  return this->io_.send(m, len + 1);
}

bool RelayClient::send_control(const uint8_t *msg, size_t len) {
  return this->authed_ && this->io_.send && this->io_.send(msg, len);
}

bool RelayClient::roster_changed_() const {
  for (size_t s = 0; s < relay::SLOTS; s++) {
    const WifiKey *k = this->access_.app_key(s);
    static const uint8_t NONE[KEY_ID_SIZE] = {};
    if (std::memcmp(this->sent_roster_[s], k != nullptr ? k->id : NONE, KEY_ID_SIZE) != 0)
      return true;
  }
  return false;
}

void RelayClient::send_roster_() {
  relay::RosterEntry e[relay::SLOTS];
  size_t n = 0;
  for (size_t s = 0; s < relay::SLOTS; s++) {
    const WifiKey *k = this->access_.app_key(s);
    if (k == nullptr) {
      std::memset(this->sent_roster_[s], 0, KEY_ID_SIZE);
      continue;
    }
    std::memcpy(this->sent_roster_[s], k->id, KEY_ID_SIZE);
    uint8_t t[relay::TOKEN_SIZE];
    e[n].slot = uint8_t(s);
    if (relay::relay_token(k->secret, KEY_SECRET_SIZE, t) && crypto::sha256({{t, relay::TOKEN_SIZE}}, e[n].hash))
      n++;
  }
  uint8_t m[2 + relay::SLOTS * 33];
  size_t len = relay::encode_roster(e, n, m, sizeof(m));
  if (len != 0 && this->io_.send && this->io_.send(m, len))
    this->roster_sent_ = true;
  // A phone whose key left the roster: its session goes too (the relay closes its socket).
  for (uint8_t ch = 1; ch <= relay::CHANNELS; ch++) {
    const int8_t slot = this->channel_slot_[ch];
    if (slot >= 0 && this->access_.app_key(size_t(slot)) == nullptr) {
      this->channel_slot_[ch] = -1;
      if (this->ch_.close)
        this->ch_.close(ch, relay::CLOSE_KEY_REMOVED);
    }
  }
}

uint32_t RelayClient::seconds_since_connected(uint32_t now) const {
  if (this->authed_)
    return 0;
  if (!this->ever_connected_)
    return 0xFFFFFFFFu;
  return uint32_t(now - this->last_connected_ms_) / 1000;
}

void RelayClient::relay_status(RelayStatus &out, uint32_t now) const {
  out.on = this->on_;
  out.state = this->state_;
  out.has_id = this->id_.valid();
  std::memcpy(out.id, this->id_.id(), relay::ID_SIZE);
  out.since_s = this->seconds_since_connected(now);
  std::memcpy(out.host, this->host_, sizeof(out.host));
}

Outcome RelayClient::relay_set(bool on, const uint8_t *host, size_t n) {
  if (!this->set(on, host, n))
    return Outcome::refuse(Result::REFUSED, "the board could not make its relay key");
  return Outcome::ok();
}

uint8_t RelayClient::open_channels() const {
  uint8_t n = 0;
  for (uint8_t ch = 1; ch <= relay::CHANNELS; ch++)
    n += this->channel_slot_[ch] >= 0 ? 1 : 0;
  return n;
}

}  // namespace flamekeeper
}  // namespace esphome
