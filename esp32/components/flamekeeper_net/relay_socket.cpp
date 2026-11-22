#include "relay_socket.h"

#ifdef USE_ESP32

#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_tls.h>
#include <lwip/sockets.h>
#include <mbedtls/base64.h>

#include <cstring>
#include <strings.h>

#include "esphome/core/log.h"

namespace esphome {
namespace flamekeeper_net {

static const char *const TAG = "flamekeeper.relay";

namespace {

uint32_t ms() { return uint32_t(esp_timer_get_time() / 1000); }

struct Conn {
  esp_tls_t *tls{nullptr};
  std::vector<uint8_t> rx;        // bytes read, not yet a whole frame
  std::vector<uint8_t> message;   // a fragmented message being put together
  bool in_message{false};
};

bool write_all(esp_tls_t *tls, const uint8_t *p, size_t n) {
  while (n > 0) {
    ssize_t w = esp_tls_conn_write(tls, p, n);
    if (w > 0) {
      p += w;
      n -= size_t(w);
    } else if (w != ESP_TLS_ERR_SSL_WANT_READ && w != ESP_TLS_ERR_SSL_WANT_WRITE) {
      return false;
    }
  }
  return true;
}

}  // namespace

RelaySocket::RelaySocket() { this->lock_ = xSemaphoreCreateMutex(); }

void RelaySocket::connect(const char *host, const char *path, const char *subprotocol) {
  xSemaphoreTake(this->lock_, portMAX_DELAY);
  this->host_ = host;
  this->path_ = path;
  this->subprotocol_ = subprotocol;
  this->generation_++;
  this->outbox_.clear();
  bool start = this->task_ == nullptr;
  xSemaphoreGive(this->lock_);
  // Below the main loop's priority (ESPHome's loop runs at 1), and on the other core where there is
  // one: the handshake's maths takes about a second and must not stall the heater link or Bluetooth.
#if portNUM_PROCESSORS > 1
  const BaseType_t core = 0;
#else
  const BaseType_t core = tskNO_AFFINITY;
#endif
  if (start && xTaskCreatePinnedToCore(task_main_, "fk_relay", STACK_SIZE, this, tskIDLE_PRIORITY + 1, &this->task_,
                                       core) != pdPASS) {
    this->task_ = nullptr;
    ESP_LOGW(TAG, "No memory for the relay task");
    this->push_({Event::CLOSED, 0, {}});
  }
}

void RelaySocket::disconnect() {
  xSemaphoreTake(this->lock_, portMAX_DELAY);
  this->host_.clear();
  this->generation_++;
  this->outbox_.clear();
  xSemaphoreGive(this->lock_);
}

bool RelaySocket::send(const uint8_t *data, size_t len) {
  if (len > MESSAGE_MAX)
    return false;
  xSemaphoreTake(this->lock_, portMAX_DELAY);
  bool ok = this->task_ != nullptr && !this->host_.empty() && this->outbox_.size() < 48;
  if (ok)
    this->outbox_.emplace_back(data, data + len);
  xSemaphoreGive(this->lock_);
  return ok;
}

void RelaySocket::poll(std::deque<Event> &out) {
  xSemaphoreTake(this->lock_, portMAX_DELAY);
  for (Event &e : this->events_)
    out.push_back(std::move(e));
  this->events_.clear();
  xSemaphoreGive(this->lock_);
}

void RelaySocket::push_(Event &&e) {
  xSemaphoreTake(this->lock_, portMAX_DELAY);
  this->events_.push_back(std::move(e));
  xSemaphoreGive(this->lock_);
}

void RelaySocket::task_main_(void *arg) {
  RelaySocket *self = static_cast<RelaySocket *>(arg);
  for (;;) {
    self->run_();
    // A connect() that came in as the last attempt ended finds this task still here: serve it.
    xSemaphoreTake(self->lock_, portMAX_DELAY);
    if (self->host_.empty()) {
      self->task_ = nullptr;
      xSemaphoreGive(self->lock_);
      vTaskDelete(nullptr);
      return;
    }
    xSemaphoreGive(self->lock_);
  }
}

bool RelaySocket::write_frame_(void *tlsp, uint8_t opcode, const uint8_t *data, size_t len) {
  // A client's frames are always masked (RFC 6455, 5.3).
  uint8_t h[14];
  size_t k = 0;
  h[k++] = uint8_t(0x80 | opcode);
  if (len < 126) {
    h[k++] = uint8_t(0x80 | len);
  } else {
    h[k++] = 0x80 | 126;
    h[k++] = uint8_t(len >> 8);
    h[k++] = uint8_t(len);
  }
  uint8_t mask[4];
  esp_fill_random(mask, 4);
  std::memcpy(h + k, mask, 4);
  k += 4;
  std::vector<uint8_t> f(h, h + k);
  f.resize(k + len);
  for (size_t i = 0; i < len; i++)
    f[k + i] = data[i] ^ mask[i & 3];
  return write_all(static_cast<esp_tls_t *>(tlsp), f.data(), f.size());
}

bool RelaySocket::handshake_(void *tlsp, const std::string &host, const std::string &path, const std::string &proto) {
  esp_tls_t *tls = static_cast<esp_tls_t *>(tlsp);
  uint8_t key[16];
  esp_fill_random(key, sizeof(key));
  unsigned char key64[32];
  size_t key64_len = 0;
  mbedtls_base64_encode(key64, sizeof(key64), &key64_len, key, sizeof(key));
  std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host +
                    "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
                    "Sec-WebSocket-Key: " +
                    std::string(reinterpret_cast<char *>(key64), key64_len) +
                    "\r\nSec-WebSocket-Protocol: " + proto +
                    "\r\nUser-Agent: Flamekeeper\r\n\r\n";
  if (!write_all(tls, reinterpret_cast<const uint8_t *>(req.data()), req.size()))
    return false;
  // The answer's headers, up to the blank line (and no further: frames may follow at once).
  std::string head;
  const uint32_t deadline = ms() + 10000;
  while (head.find("\r\n\r\n") == std::string::npos) {
    if (int32_t(ms() - deadline) > 0 || head.size() > 2048)
      return false;
    char c;
    ssize_t n = esp_tls_conn_read(tls, &c, 1);
    if (n == 1)
      head.push_back(c);
    else if (n != ESP_TLS_ERR_SSL_WANT_READ && n != ESP_TLS_ERR_SSL_WANT_WRITE)
      return false;
  }
  if (head.compare(0, 12, "HTTP/1.1 101") != 0) {
    ESP_LOGW(TAG, "The relay did not accept the WebSocket: %.*s", int(head.find("\r\n")), head.c_str());
    return false;
  }
  // The subprotocol must be ours.
  std::string want = "sec-websocket-protocol: " + proto;
  std::string lower = head;
  for (char &c : lower)
    c = char(tolower(uint8_t(c)));
  std::string want_lower = want;
  for (char &c : want_lower)
    c = char(tolower(uint8_t(c)));
  if (lower.find(want_lower) == std::string::npos) {
    ESP_LOGW(TAG, "The relay answered without the %s subprotocol", proto.c_str());
    return false;
  }
  return true;
}

void RelaySocket::run_() {
  for (;;) {
    xSemaphoreTake(this->lock_, portMAX_DELAY);
    const uint32_t gen = this->generation_;
    std::string host = this->host_, path = this->path_, proto = this->subprotocol_;
    xSemaphoreGive(this->lock_);
    if (host.empty())
      return;
    std::string name = host;
    int port = 443;
    size_t colon = host.rfind(':');
    if (colon != std::string::npos) {
      port = atoi(host.c_str() + colon + 1);
      name = host.substr(0, colon);
    }
    auto current = [&]() {
      xSemaphoreTake(this->lock_, portMAX_DELAY);
      bool same = this->generation_ == gen;
      xSemaphoreGive(this->lock_);
      return same;
    };

    // Not with the heap short: a failed try backs off and comes again later.
    const uint32_t free8 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const uint32_t block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (free8 < MIN_FREE_TO_CONNECT || block < MIN_BLOCK_TO_CONNECT) {
      ESP_LOGW(TAG, "Not connecting to the relay: %u bytes free (largest block %u)", unsigned(free8), unsigned(block));
      if (current())
        this->push_({Event::CLOSED, 0, {}});
      xSemaphoreTake(this->lock_, portMAX_DELAY);
      if (this->generation_ == gen)
        this->host_.clear();
      const bool again = this->generation_ != gen && !this->host_.empty();
      xSemaphoreGive(this->lock_);
      if (again)
        continue;
      return;
    }
    esp_tls_cfg_t cfg{};
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 10000;
    cfg.common_name = name.c_str();
    esp_tls_t *tls = esp_tls_init();
    bool up = tls != nullptr && esp_tls_conn_new_sync(name.c_str(), int(name.size()), port, &cfg, tls) == 1;
    const uint32_t low = esp_get_minimum_free_heap_size();
    this->handshake_min_free_ = low;
    if (up)
      ESP_LOGI(TAG, "TLS to %s up; lowest free heap so far %u bytes; stack unused %u bytes", name.c_str(),
               unsigned(low), unsigned(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
    up = up && current() && this->handshake_(tls, host, path, proto);
    uint16_t close_code = 0;
    bool report = true;
    if (up) {
      int fd = -1;
      esp_tls_get_conn_sockfd(tls, &fd);
      struct timeval tv = {0, 50 * 1000};
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      this->push_({Event::CONNECTED, 0, {}});
      Conn c;
      c.tls = tls;
      uint32_t last_ping = ms();
      int unanswered = 0;
      uint8_t buf[512];
      for (;;) {
        if (!current()) {
          const uint8_t bye[2] = {0x03, 0xE8};  // 1000
          this->write_frame_(tls, 0x8, bye, 2);
          report = false;
          break;
        }
        // Out.
        std::deque<std::vector<uint8_t>> out;
        xSemaphoreTake(this->lock_, portMAX_DELAY);
        out.swap(this->outbox_);
        xSemaphoreGive(this->lock_);
        bool ok = true;
        for (auto &m : out)
          ok = ok && this->write_frame_(tls, 0x2, m.data(), m.size());
        if (!ok)
          break;
        if (ms() - last_ping >= PING_MS) {
          if (unanswered >= 2) {
            ESP_LOGW(TAG, "The relay stopped answering pings");
            break;
          }
          this->write_frame_(tls, 0x9, nullptr, 0);
          unanswered++;
          last_ping = ms();
        }
        // In.
        ssize_t n = esp_tls_conn_read(tls, buf, sizeof(buf));
        if (n == 0)
          break;  // closed under us
        if (n < 0) {
          if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE)
            continue;
          break;
        }
        c.rx.insert(c.rx.end(), buf, buf + n);
        // Whole frames.
        bool stop = false;
        while (!stop && c.rx.size() >= 2) {
          const uint8_t b0 = c.rx[0], b1 = c.rx[1];
          if (b1 & 0x80) {  // a server never masks
            stop = true;
            break;
          }
          size_t hl = 2, len = b1 & 0x7F;
          if (len == 126) {
            if (c.rx.size() < 4)
              break;
            len = size_t(c.rx[2]) << 8 | c.rx[3];
            hl = 4;
          } else if (len == 127) {
            stop = true;  // never this big
            break;
          }
          if (len > MESSAGE_MAX) {
            stop = true;
            break;
          }
          if (c.rx.size() < hl + len)
            break;
          const uint8_t opcode = b0 & 0x0F;
          const bool fin = b0 & 0x80;
          std::vector<uint8_t> payload(c.rx.begin() + hl, c.rx.begin() + hl + len);
          c.rx.erase(c.rx.begin(), c.rx.begin() + hl + len);
          switch (opcode) {
            case 0x9:  // ping
              this->write_frame_(tls, 0xA, payload.data(), payload.size());
              break;
            case 0xA:  // pong
              unanswered = 0;
              break;
            case 0x8:  // close
              close_code = payload.size() >= 2 ? uint16_t(payload[0] << 8 | payload[1]) : 1005;
              this->write_frame_(tls, 0x8, payload.data(), payload.size() >= 2 ? 2 : 0);
              stop = true;
              break;
            case 0x1:  // text: never part of this protocol
              close_code = 0;
              stop = true;
              break;
            case 0x2:
            case 0x0:
              if (opcode == 0x2) {
                c.message.clear();
                c.in_message = true;
              }
              if (!c.in_message || c.message.size() + payload.size() > MESSAGE_MAX) {
                stop = true;
                break;
              }
              c.message.insert(c.message.end(), payload.begin(), payload.end());
              if (fin) {
                c.in_message = false;
                this->push_({Event::MESSAGE, 0, std::move(c.message)});
                c.message.clear();
              }
              break;
            default:
              stop = true;
          }
        }
        if (stop)
          break;
      }
    }
    if (tls != nullptr)
      esp_tls_conn_destroy(tls);
    if (report && current())
      this->push_({Event::CLOSED, close_code, {}});
    // A connect() or disconnect() while this one ran: go round for the new one, or stop.
    xSemaphoreTake(this->lock_, portMAX_DELAY);
    const bool again = this->generation_ != gen && !this->host_.empty();
    const bool stop = this->generation_ == gen || this->host_.empty();
    if (this->generation_ == gen)
      this->host_.clear();  // this attempt is over; the loop asks again
    xSemaphoreGive(this->lock_);
    if (stop && !again)
      return;
  }
}

}  // namespace flamekeeper_net
}  // namespace esphome

#endif  // USE_ESP32
