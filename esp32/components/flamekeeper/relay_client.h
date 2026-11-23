// The board's side of the internet relay connection (PROTOCOL.md), without the
// socket: when to connect and back off, the sign-in (CHALLENGE -> AUTH), the roster, and the app
// channels (OPEN / CLOSE / frames), handed to whoever runs the sessions. The ESPHome glue owns the
// TLS WebSocket and calls connected() / message() / closed(); this class asks it to connect,
// disconnect and send.
//
// Plain C++ (host-tested in tests/test_relay.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>

#include "access.h"
#include "relay.h"

namespace esphome {
namespace flamekeeper {

const char *relay_state_name(RelayState s);

class RelayClient : public RelayControl {
 public:
  static const uint32_t AUTH_TIMEOUT_MS = 10000;   // the relay's CHALLENGE, then our AUTH
  static const uint32_t BACKOFF_FIRST_MS = 2000;
  static const uint32_t BACKOFF_MAX_MS = 300000;
  static const uint32_t BACKOFF_RESET_MS = 600000;  // connected this long: start again at 2 s
  static const uint32_t REPLACED_WAIT_MS = 60000;   // close 4001: another board took this id

  // What the socket side does for us.
  struct Io {
    std::function<void(const char *host, const char *path)> connect;  // wss://host + path, subprotocol
    std::function<void()> disconnect;
    std::function<bool(const uint8_t *, size_t)> send;  // one binary message; false if not sent
  };
  // What the session side does for us.
  struct Channels {
    std::function<void(uint8_t channel, int slot)> open;  // a phone on a channel, its key slot
    std::function<void(uint8_t channel, uint16_t code)> close;
    std::function<void(uint8_t channel, const uint8_t *frame, size_t len)> frame;
  };

  RelayClient(Access &access, relay::Random random) : access_(access), random_(std::move(random)) {}
  void set_io(Io io) { this->io_ = std::move(io); }
  void set_channels(Channels ch) { this->ch_ = std::move(ch); }
  void set_version(const char *v) { std::strncpy(this->version_, v, relay::VERSION_MAX); }

  // Settings (the glue keeps them in flash). The host "" is the default.
  void configure(bool on, const char *host, const uint8_t *private_key);
  bool on() const { return this->on_; }
  const char *host() const { return this->host_[0] != 0 ? this->host_ : relay::DEFAULT_HOST; }
  const char *custom_host() const { return this->host_; }
  const relay::Identity &identity() const { return this->id_; }
  // RELAY_SET: makes the key pair at the first "on". False: no key pair could be made.
  bool set(bool on, const uint8_t *host, size_t host_len);
  // Settings changed (on, host, a new key pair): the glue saves them.
  void on_settings(std::function<void()> &&cb) { this->settings_cb_ = std::move(cb); }
  // Factory reset: off, default host, no key pair.

  // From the glue.
  void loop(uint32_t now, bool wifi_up);
  void connected(uint32_t now);
  void message(const uint8_t *data, size_t len, uint32_t now);
  void closed(uint16_t code, uint32_t now);  // 0: the connection failed or dropped without a code
  // A session on a channel ends from our side.
  void close_channel(uint8_t channel, uint16_t code);
  // A frame for a phone on a channel.
  bool send_frame(uint8_t channel, const uint8_t *frame, size_t len);
  // The push to the relay (built by the push engine), channel 0.
  bool send_control(const uint8_t *msg, size_t len);

  // RelayControl (RELAY_STATUS, RELAY_SET).
  void relay_status(RelayStatus &out, uint32_t now) const override;
  Outcome relay_set(bool on, const uint8_t *host, size_t n) override;

  // RELAY_STATUS and the Home Assistant sensor.
  RelayState state() const { return this->state_; }
  bool authed() const { return this->authed_; }
  uint32_t seconds_since_connected(uint32_t now) const;
  uint8_t open_channels() const;
  uint8_t push_mask() const { return this->push_mask_; }
  uint8_t live_mask() const { return this->live_mask_; }
  uint8_t startable_mask() const { return this->startable_mask_; }

 protected:
  void fail_(uint32_t now, uint32_t wait_ms);
  void send_roster_();
  bool roster_changed_() const;
  void close_all_channels_(uint16_t code);

  Access &access_;
  relay::Random random_;
  Io io_;
  Channels ch_;
  std::function<void()> settings_cb_;
  relay::Identity id_;
  bool on_{false};
  char host_[relay::HOST_MAX + 1]{};
  char version_[relay::VERSION_MAX + 1]{};

  RelayState state_{RelayState::OFF};
  bool linked_{false};     // the socket is up (between connected() and closed())
  bool authed_{false};     // AUTH sent and the relay did not close
  bool trying_{false};     // connect() asked, waiting for connected() or closed()
  bool no_answer_{false};  // the last try got no connection at all (state 4 while retrying)
  uint32_t linked_ms_{0};
  bool ever_connected_{false};
  uint32_t last_connected_ms_{0};
  uint32_t next_try_ms_{0};
  uint32_t backoff_ms_{BACKOFF_FIRST_MS};
  uint8_t sent_roster_[relay::SLOTS][KEY_ID_SIZE]{};  // the key ids the last ROSTER described
  bool roster_sent_{false};
  int8_t channel_slot_[relay::CHANNELS + 1]{-1, -1, -1, -1};
  uint8_t push_mask_{0};
  uint8_t live_mask_{0};
  uint8_t startable_mask_{0};
};

}  // namespace flamekeeper
}  // namespace esphome
