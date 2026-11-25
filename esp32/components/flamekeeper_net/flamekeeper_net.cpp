#include "flamekeeper_net.h"

#ifdef USE_ESP32

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#endif
#ifdef USE_MDNS
#include <mdns.h>
#endif

#include "esphome/components/flamekeeper/web_files.h"
#include "flamekeeper_web_files.h"  // generated in src/ by __init__.py from the web_app option

namespace esphome {
namespace flamekeeper_net {

static const char *const TAG = "flamekeeper_net";
static const char *const SUBPROTOCOL = "flamekeeper.v1";
static const uint32_t STATE_MIN_MS = 250;
// At boot the relay waits this long at most for Bluetooth's service to start (relay_loop_).
static const uint32_t RELAY_BOOT_HOLD_MS = 20000;
static const uint32_t TELEMETRY_RUN_MS = 1000;
static const uint32_t TELEMETRY_IDLE_MS = 10000;
static const uint32_t PING_MS = 20000;
static const size_t MAX_CLIENTS = 4;  // local WebSockets; relay channels come on top (3)
// The server's connections: the WebSockets and the page's files (a browser loads up to 6 at once).
// HTTP_SOCKETS in __init__.py reserves them in lwIP.
static const size_t MAX_OPEN_SOCKETS = 8;
static_assert(MAX_OPEN_SOCKETS >= MAX_CLIENTS + 2, "room for the page beside the WebSockets");

static FlamekeeperNet *g_net = nullptr;  // the HTTP server's handlers reach the component through it

// Served when the firmware was built without the app (no web_app, or its folder missing).
static const char PAGE[] =
    "<!doctype html><html lang=en><head><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>Flamekeeper</title><style>body{font-family:system-ui,sans-serif;margin:2rem;max-width:36rem;"
    "line-height:1.5}code{background:#eee;padding:0 .2em}</style></head><body>"
    "<h1>Flamekeeper</h1><p>The web app is not built into this firmware yet. Use the phone app, or Home "
    "Assistant.</p><p>Apps talk to the board over the WebSocket at <code>/api/v1</code> "
    "(protocol <code>flamekeeper.v1</code>).</p></body></html>";

// ---------------------------------------------------------------- server task side

// A host as a Host header or an Origin gives it ("name.local", "10.0.0.5", with or without ":80").
bool FlamekeeperNet::host_match_(char *host) {
  char *colon = strrchr(host, ':');  // ":80"
  if (colon != nullptr && strchr(host, ']') == nullptr)
    *colon = 0;
  bool ok = false;
  if (xSemaphoreTake(g_net->lock_, portMAX_DELAY) == pdTRUE) {
    ok = (g_net->host_name_[0] != 0 && strcasecmp(host, g_net->host_name_) == 0) ||
         (g_net->host_ip_[0] != 0 && strcmp(host, g_net->host_ip_) == 0);
    xSemaphoreGive(g_net->lock_);
  }
  return ok;
}

bool FlamekeeperNet::host_ok_(httpd_req_t *req) {
  char host[96] = {};
  if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK)
    return false;
  return host_match_(host);
}

// WebSockets are not bound by the same-origin rule: a page from anywhere could open one to the
// board. Accepted: no Origin (a native client), the phone apps' own web views (Capacitor:
// "capacitor://localhost" on iOS, "https://localhost" / "http://localhost" on Android), and the
// board's own page. Anything else is a page from some other site.
bool FlamekeeperNet::origin_ok_(httpd_req_t *req) {
  size_t len = httpd_req_get_hdr_value_len(req, "Origin");
  if (len == 0)
    return true;
  char origin[104] = {};
  if (len >= sizeof(origin) || httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK)
    return false;
  static const char *const APPS[] = {"capacitor://localhost", "https://localhost", "http://localhost"};
  for (const char *app : APPS)
    if (strcasecmp(origin, app) == 0)
      return true;
  static const char HTTP[] = "http://";
  if (strncasecmp(origin, HTTP, sizeof(HTTP) - 1) != 0)
    return false;
  char *host = origin + sizeof(HTTP) - 1;
  if (strchr(host, '/') != nullptr)
    return false;
  return host_match_(host);
}

esp_err_t FlamekeeperNet::page_handler_(httpd_req_t *req) {
  char host[96] = {};
  if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
    httpd_resp_set_status(req, "421 Misdirected Request");
    return httpd_resp_send(req, nullptr, 0);
  }
  char checked[sizeof(host)];
  memcpy(checked, host, sizeof(host));
  if (!host_match_(checked)) {
    httpd_resp_set_status(req, "421 Misdirected Request");
    return httpd_resp_send(req, nullptr, 0);
  }
  // The WebSocket is named in full: older Safari does not count ws:// as 'self'. The Host is one
  // of the board's own names (checked above), so it is safe to repeat. Inline styles: Svelte's
  // style directives and transitions; inline scripts stay forbidden.
  char csp[224];
  snprintf(csp, sizeof(csp),
           "default-src 'self'; connect-src 'self' ws://%s; style-src 'self' 'unsafe-inline'; "
           "img-src 'self' data:; frame-ancestors 'none'",
           host);
  httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
  httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
  if (WEB_FILE_COUNT == 0) {
    if (strcmp(req->uri, "/") != 0 && strcmp(req->uri, "/index.html") != 0) {
      httpd_resp_send_404(req);
      return ESP_OK;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Security-Policy", csp);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, PAGE, sizeof(PAGE) - 1);
  }
  const fk::web::File *f = fk::web::find(WEB_FILES, WEB_FILE_COUNT, req->uri);
  if (f == nullptr) {
    httpd_resp_send_404(req);
    return ESP_OK;
  }
  httpd_resp_set_hdr(req, "Content-Security-Policy", csp);
  httpd_resp_set_hdr(req, "ETag", f->etag);
  httpd_resp_set_hdr(req, "Cache-Control", f->immutable ? "public, max-age=31536000, immutable" : "no-cache");
  char inm[160] = {};
  if (httpd_req_get_hdr_value_str(req, "If-None-Match", inm, sizeof(inm)) == ESP_OK &&
      fk::web::etag_matches(inm, f->etag)) {
    httpd_resp_set_status(req, "304 Not Modified");
    return httpd_resp_send(req, nullptr, 0);
  }
  // Always gzipped: every browser takes it, and the board keeps no other copy.
  httpd_resp_set_type(req, f->type);
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  return httpd_resp_send(req, reinterpret_cast<const char *>(f->gz), f->gz_size);
}

// ESP-IDF 5.x answers the handshake itself and no longer calls the handler for it (httpd_uri.c:
// "If the request is websocket handshake, then do not call the uri->handler"): the checks run
// before it is answered, and the connection is announced to the loop once it is.
esp_err_t FlamekeeperNet::ws_pre_handshake_(httpd_req_t *req) {
  if (!host_ok_(req)) {
    ESP_LOGW(TAG, "WebSocket refused: wrong Host");
    return ESP_FAIL;  // no 101: the socket closes
  }
  if (!origin_ok_(req)) {
    ESP_LOGW(TAG, "WebSocket refused: opened from another site's page");
    return ESP_FAIL;
  }
  return ESP_OK;
}

esp_err_t FlamekeeperNet::ws_post_handshake_(httpd_req_t *req) {
  g_net->push_({Inbox::OPEN, httpd_req_to_sockfd(req), {}});
  return ESP_OK;
}

esp_err_t FlamekeeperNet::ws_handler_(httpd_req_t *req) {
  int fd = httpd_req_to_sockfd(req);
  if (req->method == HTTP_GET)
    return ESP_OK;  // not called for the handshake on ESP-IDF 5 (see above); harmless if it is
  httpd_ws_frame_t pkt{};
  pkt.type = HTTPD_WS_TYPE_BINARY;
  if (httpd_ws_recv_frame(req, &pkt, 0) != ESP_OK)
    return ESP_FAIL;
  if (pkt.len > fk::wire::FRAME_MAX)
    return ESP_FAIL;
  std::vector<uint8_t> data(pkt.len);
  pkt.payload = data.data();
  if (pkt.len != 0 && httpd_ws_recv_frame(req, &pkt, pkt.len) != ESP_OK)
    return ESP_FAIL;
  if (pkt.type == HTTPD_WS_TYPE_BINARY)
    g_net->push_({Inbox::FRAME, fd, std::move(data)});
  else if (pkt.type == HTTPD_WS_TYPE_TEXT)
    g_net->push_({Inbox::FRAME, fd, {}});  // an empty frame: the session closes the connection
  return ESP_OK;
}

void FlamekeeperNet::close_fn_(httpd_handle_t hd, int fd) {
  (void) hd;
  if (g_net != nullptr)
    g_net->push_({Inbox::CLOSED, fd, {}});
  close(fd);
}

void FlamekeeperNet::push_(Inbox &&in) {
  if (xSemaphoreTake(this->lock_, portMAX_DELAY) != pdTRUE)
    return;
  if (this->inbox_.size() < 32 || in.kind == Inbox::CLOSED)
    this->inbox_.push_back(std::move(in));
  xSemaphoreGive(this->lock_);
}

namespace {
struct SendJob {
  httpd_handle_t server;
  int fd;
  httpd_ws_type_t type;
  std::vector<uint8_t> data;
};
void send_job(void *arg) {
  std::unique_ptr<SendJob> job(static_cast<SendJob *>(arg));
  httpd_ws_frame_t pkt{};
  pkt.type = job->type;
  pkt.final = true;
  pkt.payload = job->data.data();
  pkt.len = job->data.size();
  httpd_ws_send_frame_async(job->server, job->fd, &pkt);
}
}  // namespace

// ---------------------------------------------------------------- main loop side

void FlamekeeperNet::setup() {
  g_net = this;
  this->lock_ = xSemaphoreCreateMutex();
  fk::Controller &c = this->hub_->controller();
  c.on_event([this](fk::Event e, fk::Result r, uint16_t id, const char *text) {
    uint8_t out[fk::ble::RESULT_MAX];
    size_t n = fk::ble::encode_event(e, r, id, text, out, sizeof(out), this->hub_->controller().event_seq());
    this->broadcast_(out, n);
  });
  this->hub_->board().on_record([this](const uint8_t *data, size_t len) { this->broadcast_(data, len); });

  // The internet relay: its socket here, its logic in the hub's board services; each phone on a
  // relay channel is one more session, like a local WebSocket.
  fk::RelayClient *rc = this->hub_->board().relay_client();
  if (rc != nullptr) {
    rc->set_io({[this](const char *host, const char *path) {
                  this->relay_sock_.connect(host, path, fk::relay::SUBPROTOCOL);
                },
                [this]() { this->relay_sock_.disconnect(); },
                [this](const uint8_t *data, size_t len) { return this->relay_sock_.send(data, len); }});
    // A channel opens inside relay_loop_: stamp it with that pass's clock, which update_ then
    // compares against (a fresh millis() here was later than it, and the sign-in deadline check
    // underflowed and closed every relay phone at once).
    rc->set_channels({[this](uint8_t ch, int slot) { this->relay_open_(ch, uint8_t(slot), this->loop_ms_); },
                      [this](uint8_t ch, uint16_t) { this->drop_(-int(ch)); },
                      [this](uint8_t ch, const uint8_t *data, size_t len) {
                        this->relay_inbox_.push_back({Inbox::FRAME, -int(ch), std::vector<uint8_t>(data, data + len)});
                      }});
    c.on_refresh([this](fk::Status &s) {
      uint8_t n = 0;
      for (const Client &x : this->clients_)
        n += x.fd < 0 && !x.closing && x.session->signed_in() ? 1 : 0;
      s.relay_phones = n;
    });
  }
}

void FlamekeeperNet::relay_loop_(uint32_t now, bool online) {
  fk::RelayClient *rc = this->hub_->board().relay_client();
  if (rc == nullptr)
    return;
  // The relay's TLS handshake takes most of the free heap for a moment; Bluetooth building its
  // service at the same time could fail, and then no phone sees the board over Bluetooth. So at
  // boot the relay waits until that service has started (or will not), or RELAY_BOOT_HOLD_MS.
  if (!this->relay_released_) {
    const bool settled = this->hub_->board().bluetooth_settled();
    if (!settled && now < RELAY_BOOT_HOLD_MS)
      return;
    this->relay_released_ = true;
    ESP_LOGI(TAG, "Relay may connect (Bluetooth service %s)", settled ? "settled" : "not settled after 20 s");
  }
  std::deque<RelaySocket::Event> &events = this->relay_events_;
  this->relay_sock_.poll(events);
  for (RelaySocket::Event &e : events) {
    switch (e.kind) {
      case RelaySocket::Event::CONNECTED:
        ESP_LOGI(TAG, "Relay connected (lowest free heap so far %u bytes)", unsigned(this->relay_sock_.handshake_min_free()));
        rc->connected(now);
        break;
      case RelaySocket::Event::MESSAGE:
        rc->message(e.data.data(), e.data.size(), now);
        break;
      case RelaySocket::Event::CLOSED:
        ESP_LOGI(TAG, "Relay connection closed (code %u)", unsigned(e.code));
        rc->closed(e.code, now);
        break;
    }
  }
  events.clear();
  rc->loop(now, online);
}

void FlamekeeperNet::relay_open_(uint8_t channel, uint8_t slot, uint32_t now) {
  fk::RelayClient *rc = this->hub_->board().relay_client();
  this->drop_(-int(channel));
  const fk::WifiKey *key = this->hub_->board().access().app_key(slot);
  if (key == nullptr) {
    rc->close_channel(channel, fk::relay::CLOSE_KEY_REMOVED);
    return;
  }
  Client c{};
  c.fd = -int(channel);
  c.slot = int8_t(slot);
  c.since_ms = now;
  c.ping_ms = now;
  c.session.reset(new fk::WifiSession(this->hub_->board().access()));
  this->clients_.push_back(std::move(c));
  Client &added = this->clients_.back();
  int fd = added.fd;
  // Hello at once: no Info in the clear over the relay.
  added.session->start_relay(key->id, [this, fd](const uint8_t *p, size_t n) { this->send_(fd, p, n); });
  ESP_LOGI(TAG, "A phone (key slot %u) on relay channel %u", unsigned(slot), unsigned(channel));
}

void FlamekeeperNet::dump_config() {
  ESP_LOGCONFIG(TAG, "Flamekeeper Wi-Fi transport: port %u, WebSocket /api/v1 (%s)", this->port_, SUBPROTOCOL);
  size_t gz = 0;
  for (size_t i = 0; i < WEB_FILE_COUNT; i++)
    gz += WEB_FILES[i].gz_size;
  if (WEB_FILE_COUNT == 0)
    ESP_LOGCONFIG(TAG, "  Web page: placeholder (no web app built in)");
  else
    ESP_LOGCONFIG(TAG, "  Web page: the app, %u files, %u bytes gzipped", unsigned(WEB_FILE_COUNT), unsigned(gz));
}

bool FlamekeeperNet::start_server_() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = this->port_;
  cfg.ctrl_port = 32770;
  cfg.max_open_sockets = MAX_OPEN_SOCKETS;
  cfg.lru_purge_enable = true;
  cfg.close_fn = close_fn_;
  cfg.uri_match_fn = httpd_uri_match_wildcard;
  cfg.stack_size = 6144;
  if (httpd_start(&this->server_, &cfg) != ESP_OK) {
    ESP_LOGE(TAG, "HTTP server did not start");
    this->server_ = nullptr;
    return false;
  }
  httpd_uri_t ws{};
  ws.uri = "/api/v1";
  ws.method = HTTP_GET;
  ws.handler = ws_handler_;
  ws.is_websocket = true;
  ws.supported_subprotocol = SUBPROTOCOL;
  ws.ws_pre_handshake_cb = ws_pre_handshake_;
  ws.ws_post_handshake_cb = ws_post_handshake_;
  httpd_register_uri_handler(this->server_, &ws);
  httpd_uri_t page{};
  page.uri = "/*";
  page.method = HTTP_GET;
  page.handler = page_handler_;
  httpd_register_uri_handler(this->server_, &page);
  ESP_LOGI(TAG, "Listening on port %u", this->port_);
  return true;
}

void FlamekeeperNet::loop() {
  uint32_t now = millis();
  this->loop_ms_ = now;
#ifdef USE_WIFI
  bool online = wifi::global_wifi_component != nullptr && wifi::global_wifi_component->is_connected();
#else
  bool online = false;
#endif
  if (online) {
    // Keep the accepted Host values current (the address can change).
    char ip[16] = {};
#ifdef USE_WIFI
    for (auto &a : wifi::global_wifi_component->wifi_sta_ip_addresses()) {
      if (a.is_ip4()) {
        char buf[network::IP_ADDRESS_BUFFER_SIZE];
        strncpy(ip, a.str_to(buf), sizeof(ip) - 1);
        break;
      }
    }
#endif
    if (xSemaphoreTake(this->lock_, 0) == pdTRUE) {
      snprintf(this->host_name_, sizeof(this->host_name_), "%s.local", App.get_name().c_str());
      memcpy(this->host_ip_, ip, sizeof(this->host_ip_));
      xSemaphoreGive(this->lock_);
    }
    if (this->server_ == nullptr && !this->start_server_())
      return;
#ifdef USE_MDNS
    if (!this->mdns_added_) {
      // "major.minor", as Info bytes 0-1.
      static const char PROTOCOL[] = {char('0' + fk::ble::PROTOCOL_MAJOR), '.', char('0' + fk::ble::PROTOCOL_MINOR), 0};
      static_assert(fk::ble::PROTOCOL_MAJOR < 10 && fk::ble::PROTOCOL_MINOR < 10, "one digit each");
      static char board[4];  // Info byte 2, in decimal
      snprintf(board, sizeof(board), "%u", unsigned(this->hub_->board().board()));
      mdns_txt_item_t txt[2] = {{"protocol", PROTOCOL}, {"board", board}};
      if (mdns_service_add(nullptr, "_flamekeeper", "_tcp", this->port_, txt, 2) == ESP_OK)
        this->mdns_added_ = true;
    }
#endif
  }
  this->relay_loop_(now, online);
  std::deque<Inbox> &work = this->work_;  // empty here; kept, so no pass allocates its own
  work.swap(this->deferred_);
  for (Inbox &in : this->relay_inbox_)
    work.push_back(std::move(in));
  this->relay_inbox_.clear();
  if (this->server_ == nullptr && work.empty()) {
    work.clear();
    for (Client &c : this->clients_)
      this->update_(c, now);
    this->sweep_relay_();
    return;
  }
  if (xSemaphoreTake(this->lock_, 0) == pdTRUE) {
    for (Inbox &in : this->inbox_)
      work.push_back(std::move(in));
    this->inbox_.clear();
    xSemaphoreGive(this->lock_);
  }
  // Sign-in steps run elliptic-curve maths in this loop: at most one per pass, so a crowd of
  // connections cannot stall the loop (and the heater link) for long. The rest wait, in order.
  bool sign_in_step = false;
  for (Inbox &in : work) {
    bool waits = std::any_of(this->deferred_.begin(), this->deferred_.end(), [&](const Inbox &d) { return d.fd == in.fd; });
    if (!waits && in.kind == Inbox::FRAME) {
      Client *c = this->client_(in.fd);
      if (c != nullptr && !c->closing && !c->session->signed_in()) {
        waits = sign_in_step;
        sign_in_step = true;
      }
    }
    if (waits)
      this->deferred_.push_back(std::move(in));
    else
      this->handle_(in, now);
  }
  work.clear();
  for (Client &c : this->clients_)
    this->update_(c, now);
  this->sweep_relay_();
}

void FlamekeeperNet::sweep_relay_() {
  std::vector<int> gone;
  for (const Client &c : this->clients_)
    if (c.fd < 0 && c.closing)
      gone.push_back(c.fd);
  for (int fd : gone)
    this->drop_(fd);
}

FlamekeeperNet::Client *FlamekeeperNet::client_(int fd) {
  for (Client &c : this->clients_)
    if (c.fd == fd)
      return &c;
  return nullptr;
}

void FlamekeeperNet::send_(int fd, const uint8_t *data, size_t len, httpd_ws_type_t type) {
  if (fd < 0) {  // a relay channel: the relay's own socket pings
    fk::RelayClient *rc = this->hub_->board().relay_client();
    if (rc != nullptr && type == HTTPD_WS_TYPE_BINARY)
      rc->send_frame(uint8_t(-fd), data, len);
    return;
  }
  if (this->server_ == nullptr)
    return;
  auto *job = new SendJob{this->server_, fd, type, std::vector<uint8_t>(data, data + len)};
  if (httpd_queue_work(this->server_, send_job, job) != ESP_OK)
    delete job;
}

void FlamekeeperNet::send_sealed_(Client &c, uint8_t type, const uint8_t *record, size_t len) {
  uint8_t out[fk::ble::RESULT_MAX + fk::wire::SEAL_OVERHEAD];
  size_t n = c.session->seal(type, record, len, out, sizeof(out));
  if (n != 0)
    this->send_(c.fd, out, n);
}

void FlamekeeperNet::close_(Client &c, const char *why, uint16_t relay_code) {
  if (c.closing)
    return;
  c.closing = true;
  if (c.fd < 0) {
    ESP_LOGI(TAG, "Closing relay channel %d: %s", -c.fd, why);
    fk::RelayClient *rc = this->hub_->board().relay_client();
    if (rc != nullptr)
      rc->close_channel(uint8_t(-c.fd), relay_code);
    return;  // dropped at the end of this loop pass (the caller may be walking clients_)
  }
  ESP_LOGI(TAG, "Closing a Wi-Fi connection: %s", why);
  httpd_sess_trigger_close(this->server_, c.fd);
}

void FlamekeeperNet::drop_(int fd) {
  Client *c = this->client_(fd);
  if (c == nullptr)
    return;
  // A browser key meant for this tab only goes with the connection: the one it was given, and the
  // one it signed in with.
  if (c->fd < 0 && c->session->signed_in())
    this->hub_->automation().log_event(fk::LogKind::RELAY_SESSION, fk::Source::RELAY_APP, uint16_t(c->slot), 1);
  fk::Access &access = this->hub_->board().access();
  if (c->session->tab_key_id() != nullptr)
    access.revoke(c->session->tab_key_id());
  if (c->session->signed_in() && c->session->key_kind() == fk::KeyKind::BROWSER && !c->session->key_remembered())
    access.revoke(c->session->key_id());
  this->clients_.erase(std::remove_if(this->clients_.begin(), this->clients_.end(),
                                      [fd](const Client &x) { return x.fd == fd; }),
                       this->clients_.end());
}

// The connection a command came in on, as the command handler sees it.
class WsLink : public fk::Link {
 public:
  WsLink(fk::BoardServices &board, const uint8_t *key_id, bool *forget, bool relay = false)
      : board_(board), key_id_(key_id), forget_(forget), relay_(relay) {}
  fk::Transport transport() const override { return this->relay_ ? fk::Transport::RELAY : fk::Transport::WIFI; }
  int phone_slot() const override { return this->board_.access().app_slot(this->key_id_); }
  bool browser() const override {
    const fk::WifiKey *key = this->board_.access().find_key(this->key_id_);
    return key != nullptr && key->kind == fk::KeyKind::BROWSER;
  }
  uint16_t update_chunk_max() const override { return fk::UPDATE_CHUNK_WIFI; }
  fk::Outcome forget_me() override {
    // A phone app's key belongs to its Bluetooth bond: forgetting the board from anywhere frees both.
    const fk::WifiKey *key = this->board_.access().find_key(this->key_id_);
    uint8_t owner[6] = {};
    bool app = key != nullptr && key->kind == fk::KeyKind::APP;
    if (app)
      memcpy(owner, key->owner, sizeof(owner));
    this->board_.access().revoke(this->key_id_);
    if (app)
      this->board_.forget_bond(owner);
    *this->forget_ = true;  // closed after the result goes out
    return fk::Outcome::ok();
  }

 protected:
  fk::BoardServices &board_;
  const uint8_t *key_id_;
  bool *forget_;
  bool relay_;
};

void FlamekeeperNet::handle_(Inbox &in, uint32_t now) {
  switch (in.kind) {
    case Inbox::OPEN: {
      if (this->client_(in.fd) != nullptr)
        return;
      if (std::count_if(this->clients_.begin(), this->clients_.end(), [](const Client &x) { return x.fd >= 0; }) >=
          long(MAX_CLIENTS)) {
        httpd_sess_trigger_close(this->server_, in.fd);
        return;
      }
      Client c{};
      c.fd = in.fd;
      c.since_ms = now;
      c.ping_ms = now;
      c.session.reset(new fk::WifiSession(this->hub_->board().access()));
      this->clients_.push_back(std::move(c));
      Client &added = this->clients_.back();
      // Info in the clear, then Hello.
      fk::BoardInfo info;
      this->hub_->board().info(info);
      uint8_t frame[1 + fk::ble::INFO_SIZE];
      frame[0] = fk::wire::INFO;
      fk::ble::encode_info(this->hub_->board().board(), this->hub_->status().capabilities, info, frame + 1);
      memcpy(added.last_info, frame + 1, fk::ble::INFO_SIZE);
      added.info_ms = now;
      this->send_(added.fd, frame, sizeof(frame));
      int fd = added.fd;
      added.session->start([this, fd](const uint8_t *p, size_t n) { this->send_(fd, p, n); });
      ESP_LOGI(TAG, "Wi-Fi connection opened (%u)", (unsigned) this->clients_.size());
      return;
    }
    case Inbox::CLOSED:
      this->drop_(in.fd);
      return;
    case Inbox::FRAME: {
      Client *c = this->client_(in.fd);
      if (c == nullptr || c->closing)
        return;
      int fd = c->fd;
      bool was_signed_in = c->session->signed_in();
      uint8_t cmd[fk::wire::FRAME_MAX];
      size_t cmd_len = 0;
      auto step = c->session->receive(in.data.data(), in.data.size(), now,
                                      [this, fd](const uint8_t *p, size_t n) { this->send_(fd, p, n); }, cmd,
                                      &cmd_len, sizeof(cmd));
      if (step == fk::WifiSession::Step::CLOSE) {
        this->close_(*c, c->session->close_reason());
        return;
      }
      if (!was_signed_in && c->session->signed_in()) {
        if (this->hub_->board().push_engine() != nullptr)
          this->hub_->board().push_engine()->app_seen();
        if (c->fd < 0)
          ESP_LOGI(TAG, "Signed in over the internet relay (phone app, channel %d)", -c->fd);
        else
          ESP_LOGI(TAG, "Signed in over Wi-Fi (%s)", c->session->key_kind() == fk::KeyKind::APP ? "phone app" : "web browser");
        if (c->fd < 0) {
          // Over the relay Info comes now, sealed (PROTOCOL.md).
          this->hub_->automation().log_event(fk::LogKind::RELAY_SESSION, fk::Source::RELAY_APP, uint16_t(c->slot), 0);
          memset(c->last_info, 0, sizeof(c->last_info));
          c->info_ms = 0;
        }
        memset(c->last_state, 0, sizeof(c->last_state));  // send the first State and Telemetry now
        c->state_ms = c->telemetry_ms = 0;
        c->last_telemetry[0] ^= 0xFF;
      }
      if ((step == fk::WifiSession::Step::COMMAND || step == fk::WifiSession::Step::UPDATE_DATA) &&
          !this->hub_->board().access().has_key(c->session->key_id())) {
        this->close_(*c, "its Wi-Fi key was removed", fk::relay::CLOSE_KEY_REMOVED);
        return;
      }
      if (step == fk::WifiSession::Step::UPDATE_DATA) {
        // Board firmware data: taken in order, progress back to this app.
        fk::BoardUpdate *u = this->hub_->board().board_update();
        uint8_t report[fk::UPDATE_REPORT_SIZE];
        if (u != nullptr && u->data(cmd, cmd_len, report))
          this->send_sealed_(*c, fk::wire::UPDATE, report, sizeof(report));
        return;
      }
      if (step == fk::WifiSession::Step::COMMAND) {
        bool forget = false;
        WsLink link(this->hub_->board(), c->session->key_id(), &forget, c->fd < 0);
        uint8_t out[fk::ble::RESULT_MAX];
        size_t n = fk::ble::handle_command(cmd, cmd_len, this->hub_->controller(), link, this->hub_->board(), out,
                                           sizeof(out));
        c = this->client_(fd);
        if (c == nullptr)
          return;
        if (n != 0)
          this->send_sealed_(*c, fk::wire::RESULT, out, n);
        memset(cmd, 0, sizeof(cmd));
        if (forget)
          this->close_(*c, "the app removed its key", fk::relay::CLOSE_KEY_REMOVED);
      }
      return;
    }
  }
}

void FlamekeeperNet::update_(Client &c, uint32_t now) {
  if (c.closing)
    return;
  if (!c.session->signed_in()) {
    static_assert(fk::wire::SIGN_IN_TIMEOUT_MS == 30000, "the text says 30 s");
    if (fk::wire::sign_in_expired(now, c.since_ms))
      this->close_(c, "no sign-in within 30 s");
    return;
  }
  // A key removed while signed in (web password changed, the phone forgotten, the oldest browser
  // dropped, factory reset) signs that connection out at once.
  if (!this->hub_->board().access().has_key(c.session->key_id())) {
    this->close_(c, "its Wi-Fi key was removed", fk::relay::CLOSE_KEY_REMOVED);
    return;
  }
  if (c.fd >= 0 && now - c.ping_ms >= PING_MS) {
    c.ping_ms = now;
    this->send_(c.fd, nullptr, 0, HTTPD_WS_TYPE_PING);
  }
  fk::BoardInfo info;
  this->hub_->board().info(info);
  uint8_t rec[fk::ble::INFO_SIZE];
  fk::ble::encode_info(this->hub_->board().board(), this->hub_->status().capabilities, info, rec);
  if (memcmp(rec, c.last_info, sizeof(rec)) != 0 && now - c.info_ms >= 1000) {
    memcpy(c.last_info, rec, sizeof(rec));
    c.info_ms = now;
    this->send_info_(c, rec);
  }
  const fk::Status &s = this->hub_->status();
  uint8_t state[fk::ble::STATE_SIZE];
  fk::ble::encode_state(s, state);
  if (memcmp(state, c.last_state, sizeof(state)) != 0 && now - c.state_ms >= STATE_MIN_MS) {
    memcpy(c.last_state, state, sizeof(state));
    c.state_ms = now;
    this->send_sealed_(c, fk::wire::STATE, state, sizeof(state));
  }
  uint8_t tel[fk::ble::TELEMETRY_SIZE];
  fk::ble::encode_telemetry(s, tel);
  uint32_t every = s.state == fk::HeaterState::OFF ? TELEMETRY_IDLE_MS : TELEMETRY_RUN_MS;
  if (memcmp(tel, c.last_telemetry, sizeof(tel)) != 0 && (c.telemetry_ms == 0 || now - c.telemetry_ms >= every)) {
    memcpy(c.last_telemetry, tel, sizeof(tel));
    c.telemetry_ms = now;
    this->send_sealed_(c, fk::wire::TELEMETRY, tel, sizeof(tel));
  }
}

void FlamekeeperNet::send_info_(Client &c, const uint8_t *rec) {
  if (c.fd < 0) {  // over the relay: sealed, and only after the sign-in
    this->send_sealed_(c, fk::wire::INFO, rec, fk::ble::INFO_SIZE);
    return;
  }
  uint8_t frame[1 + fk::ble::INFO_SIZE];
  frame[0] = fk::wire::INFO;
  memcpy(frame + 1, rec, fk::ble::INFO_SIZE);
  this->send_(c.fd, frame, sizeof(frame));  // Info stays in the clear, as on Bluetooth
}

void FlamekeeperNet::broadcast_(const uint8_t *record, size_t len) {
  for (Client &c : this->clients_)
    if (!c.closing && c.session->signed_in())
      this->send_sealed_(c, fk::wire::RESULT, record, len);
}

}  // namespace flamekeeper_net
}  // namespace esphome

#endif  // USE_ESP32
