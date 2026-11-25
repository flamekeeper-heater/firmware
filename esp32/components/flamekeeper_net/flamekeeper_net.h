// The apps over Wi-Fi (PROTOCOL.md): the board's web page and one
// WebSocket on ESP-IDF's HTTP server. It only moves frames. The sign-in and the encryption are in
// flamekeeper/wifi_session.*, the records and commands in flamekeeper/ble_protocol.*, the board
// settings in the hub's BoardServices: the same code the Bluetooth service uses.
//
// The HTTP server runs its own task; it only queues what arrives. Everything else (the sessions,
// the controller, the answers) runs in ESPHome's main loop, like every entity change.
#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <deque>
#include <memory>
#include <vector>

#include "esphome/components/flamekeeper/ble_protocol.h"
#include "esphome/components/flamekeeper/flamekeeper.h"
#include "esphome/components/flamekeeper/wifi_session.h"
#include "esphome/core/component.h"

#include "relay_socket.h"

namespace esphome {
namespace flamekeeper_net {

namespace fk = ::esphome::flamekeeper;

class FlamekeeperNet : public Component {
 public:
  void set_hub(fk::FlamekeeperHub *hub) { this->hub_ = hub; }
  void set_port(uint16_t port) { this->port_ = port; }
  void set_limits(uint8_t max_sessions, uint8_t http_sockets) {
    this->max_sessions_ = max_sessions;
    this->http_sockets_ = http_sockets;
  }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

 protected:
  // What the server task hands the main loop.
  struct Inbox {
    enum Kind : uint8_t { OPEN, FRAME, CLOSED } kind;
    int fd;
    std::vector<uint8_t> data;
  };
  // A local WebSocket (fd >= 0) or a phone on a relay channel (fd = -channel).
  struct Client {
    int fd;
    int8_t slot{-1};  // relay: the phone key slot the relay matched
    uint32_t since_ms;
    std::unique_ptr<fk::WifiSession> session;
    uint8_t last_state[fk::ble::STATE_SIZE]{};
    uint8_t last_telemetry[fk::ble::TELEMETRY_SIZE]{};
    uint8_t last_info[fk::ble::INFO_SIZE]{};
    uint32_t state_ms{0}, telemetry_ms{0}, info_ms{0}, ping_ms{0};
    bool closing{false};
  };

  bool start_server_();
  void handle_(Inbox &in, uint32_t now);
  void update_(Client &c, uint32_t now);
  Client *client_(int fd);
  void send_(int fd, const uint8_t *data, size_t len, httpd_ws_type_t type = HTTPD_WS_TYPE_BINARY);
  void send_sealed_(Client &c, uint8_t type, const uint8_t *record, size_t len);
  void close_(Client &c, const char *why, uint16_t relay_code = fk::relay::CLOSE_PROTOCOL);
  void relay_loop_(uint32_t now, bool online);
  void sweep_relay_();
  void relay_open_(uint8_t channel, uint8_t slot, uint32_t now);
  void send_info_(Client &c, const uint8_t *rec);
  void drop_(int fd);
  void broadcast_(const uint8_t *record, size_t len);
  void push_(Inbox &&in);

  static esp_err_t page_handler_(httpd_req_t *req);
  static esp_err_t ws_handler_(httpd_req_t *req);
  static esp_err_t ws_pre_handshake_(httpd_req_t *req);
  static esp_err_t ws_post_handshake_(httpd_req_t *req);
  static void close_fn_(httpd_handle_t hd, int fd);
  static bool host_match_(char *host);
  static bool host_ok_(httpd_req_t *req);
  static bool origin_ok_(httpd_req_t *req);

  fk::FlamekeeperHub *hub_{nullptr};
  uint16_t port_{80};
  uint8_t max_sessions_{4};  // local WebSockets at once
  uint8_t http_sockets_{8};  // the HTTP server's connections: the WebSockets and the page's files
  httpd_handle_t server_{nullptr};
  bool mdns_added_{false};
  bool relay_released_{false};  // the boot hold is over (relay_loop_)
  SemaphoreHandle_t lock_{nullptr};
  // The Host header values the board answers (its mDNS name, its address), kept by the main loop.
  char host_name_[72]{};
  char host_ip_[16]{};
  std::deque<Inbox> inbox_;
  std::deque<Inbox> deferred_;  // loop side only: sign-in steps held for the next pass
  std::vector<Client> clients_;
  RelaySocket relay_sock_;
  std::deque<Inbox> relay_inbox_;  // frames from relay channels, handled like the server's
  // The loop's working queues, kept between passes: a std::deque allocates a block when made.
  std::deque<Inbox> work_;
  std::deque<RelaySocket::Event> relay_events_;
  uint32_t loop_ms_{0};            // the clock of the loop pass running now
};

}  // namespace flamekeeper_net
}  // namespace esphome

#endif  // USE_ESP32
