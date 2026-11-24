// The board's outbound WebSocket to the internet relay (PROTOCOL.md), over TLS
// (esp-tls on mbedTLS, the server checked against the roots in relay_roots.pem). It runs in its own
// task, made at the first connect() with a static stack and kept, asleep while no connection is
// wanted: a stack made and freed at each reconnect would land wherever there was room, and split the
// heap's large blocks over the hours. The TLS buffers still go back to the heap between connections.
// The main loop asks it to connect, disconnect and send, and collects what happened with poll(); the
// relay logic (flamekeeper/relay_client.*) never touches the socket.
#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <deque>
#include <string>
#include <vector>

namespace esphome {
namespace flamekeeper_net {

class RelaySocket {
 public:
  static const size_t MESSAGE_MAX = 2048;
  // The keepalive: a text message "ping" this often, which the relay answers "pong" (it cannot see
  // WebSocket pings, and closes a board silent for 120 s). Two unanswered: the connection is dead.
  static const uint32_t PING_MS = 45000;
  // The handshake used 3.96 KB of 7 KB on the classic ESP32 (measured): 1.6 KB to spare.
  static const uint32_t STACK_SIZE = 5632;
  // A TLS handshake takes about 25 KB of heap on the classic ESP32 (measured), in pieces: with
  // dynamic buffers each received record gets a buffer of its own size, and the largest is the
  // server's certificates (2.5 KB for relay.flamekeeper.app; a long RSA chain is about 6 KB). It is
  // started only with this much free, and this big a block, so it never leaves the board short; a
  // handshake that still finds no room fails like any other and is tried again later.
  static const uint32_t MIN_FREE_TO_CONNECT = 32768;
  static const uint32_t MIN_BLOCK_TO_CONNECT = 8192;

  struct Event {
    enum Kind : uint8_t { CONNECTED, MESSAGE, CLOSED } kind;
    uint16_t code;  // CLOSED: the relay's close code, 0 when the connection failed or dropped
    std::vector<uint8_t> data;
  };

  RelaySocket();
  // wss://host + path with the given subprotocol. A connection already up or under way is dropped.
  void connect(const char *host, const char *path, const char *subprotocol);
  void disconnect();
  // One binary message; false if there is no connection or the queue is full.
  bool send(const uint8_t *data, size_t len);
  // What happened since the last call, in order, added to out.
  void poll(std::deque<Event> &out);
  // The lowest free heap seen during the TLS handshake (for the measurements), 0 before the first.
  uint32_t handshake_min_free() const { return this->handshake_min_free_; }

 protected:
  static void task_main_(void *arg);
  void run_();
  bool handshake_(void *tls, const std::string &host, const std::string &path, const std::string &proto);
  bool write_frame_(void *tls, uint8_t opcode, const uint8_t *data, size_t len);
  bool next_out_(std::vector<uint8_t> &out);  // the oldest message waiting to go, if any
  void push_(Event &&e);

  SemaphoreHandle_t lock_;
  TaskHandle_t task_{nullptr};
  // Asked by the main loop, read by the task (under lock_).
  std::string host_, path_, subprotocol_;
  uint32_t generation_{0};  // bumped by connect() and disconnect(): a task with an older one stops
  std::deque<std::vector<uint8_t>> outbox_;
  std::deque<Event> events_;
  volatile uint32_t handshake_min_free_{0};
  // The task's (one per connection, reserved when it comes up; the task alone uses them).
  std::vector<uint8_t> frame_;    // an outbound frame: header and masked payload, one TLS write
  std::vector<uint8_t> rx_;       // bytes read, not yet a whole frame
  std::vector<uint8_t> message_;  // a fragmented message being put together
};

}  // namespace flamekeeper_net
}  // namespace esphome

#endif  // USE_ESP32
