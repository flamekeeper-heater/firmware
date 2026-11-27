#include "flamekeeper_ble.h"

#ifdef USE_ESP32

#include <esp_heap_caps.h>
#include <esp_bt_defs.h>
#include <esp_gatt_common_api.h>
#include <esp_system.h>

#include <algorithm>
#include <cstring>

#include "esphome/components/esp32_ble/ble.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace esp32_ble {
// ESPHome's GATT server callback only queues the event (ble.cpp); declared here to forward to it.
template<typename... Args> void enqueue_ble_event(Args... args);
}  // namespace esp32_ble

namespace flamekeeper_ble {

static const char *const TAG = "flamekeeper_ble";
static const uint16_t APP_ID = 0x464B;  // "FK"; ESPHome's own BLE server is app 0

// ESPHome copies each GATT server event and hands it to the components later, from its main loop.
// That is too late or too unsure for the service's own start:
// - The attribute-table event only points at its handle list, and Bluedroid clears that list as
//   soon as the callback returns (btc_gatts.c, "reset the env after sent the data to app"): read
//   later, every handle is 0.
// - ESPHome's queue drops events when it is full ("Dropped N BLE events"), and the scanner for
//   Bluetooth thermometers keeps it busy.
// A service that never starts is invisible: a phone sees only Device Information, which Bluedroid
// then reports as ending at 0xFFFF (the last started primary service). So registration, the
// handle list and the start are taken here, in the Bluetooth task, as ESP-IDF's own examples do;
// the main loop only reads the result (poll_table_).
static volatile esp_gatt_if_t g_our_if = ESP_GATT_IF_NONE;
static uint16_t g_tab_handles[A_COUNT];
static volatile int g_tab_status = -1;           // the table event's status, -1 until it comes
static volatile uint16_t g_tab_service = 0;      // a failed table's service handle (deleted before a retry)
static volatile esp_err_t g_start_err = ESP_OK;  // esp_ble_gatts_start_service's own answer
static volatile int g_start_status = -1;         // the start event's status, -1 until it comes

static void gatts_callback(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param) {
  if (param != nullptr) {
    if (event == ESP_GATTS_REG_EVT && param->reg.app_id == APP_ID && param->reg.status == ESP_GATT_OK) {
      g_our_if = gatts_if;
    } else if (event == ESP_GATTS_CREAT_ATTR_TAB_EVT && gatts_if == g_our_if && g_tab_status < 0) {
      const auto &t = param->add_attr_tab;
      if (t.status == ESP_GATT_OK && t.handles != nullptr && t.num_handle == A_COUNT) {
        memcpy(g_tab_handles, t.handles, sizeof(g_tab_handles));
        g_start_err = esp_ble_gatts_start_service(g_tab_handles[A_SERVICE]);
        g_tab_status = ESP_GATT_OK;
      } else {
        g_tab_service = t.handles != nullptr && t.num_handle > 0 ? t.handles[0] : 0;
        g_tab_status = t.status != ESP_GATT_OK ? int(t.status) : int(ESP_GATT_ERROR);
      }
    } else if (event == ESP_GATTS_START_EVT && gatts_if == g_our_if && g_tab_status == ESP_GATT_OK &&
               param->start.service_handle == g_tab_handles[A_SERVICE]) {
      g_start_status = param->start.status;
    }
  }
  esp32_ble::enqueue_ble_event(event, gatts_if, param);  // what ESPHome's own callback does
  App.wake_loop_threadsafe();
}
static const uint32_t UNBONDED_DROP_MS = 30000;
static const uint32_t STATE_MIN_MS = 250;          // at most 4 State notifications a second
static const uint32_t TELEMETRY_RUN_MS = 1000;     // Telemetry while the heater is not off
static const uint32_t TELEMETRY_IDLE_MS = 10000;   // ... and while it is off

static const uint16_t UUID_PRIMARY = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t UUID_CHAR_DECL = ESP_GATT_UUID_CHAR_DECLARE;
static const uint16_t UUID_CCCD = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
static const uint8_t PROP_READ = ESP_GATT_CHAR_PROP_BIT_READ;
static const uint8_t PROP_READ_NOTIFY = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
static const uint8_t PROP_WRITE = ESP_GATT_CHAR_PROP_BIT_WRITE;
static const uint8_t PROP_INDICATE = ESP_GATT_CHAR_PROP_BIT_INDICATE;
static const uint8_t PROP_UPDATE = ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
static const uint16_t UPDATE_WRITE_MAX = 512;  // the longest attribute value ATT allows
static const uint16_t LOCAL_MTU = 247;         // the ATT MTU the board offers
static uint8_t cccd_init[2] = {0, 0};

// Encrypted and authenticated (passkey: MITM protection) on everything but Info.
static const esp_gatt_perm_t PERM_SECURE_READ = ESP_GATT_PERM_READ_ENC_MITM;
static const esp_gatt_perm_t PERM_SECURE_RW = ESP_GATT_PERM_READ_ENC_MITM | ESP_GATT_PERM_WRITE_ENC_MITM;

static esp_gatts_attr_db_t attr(uint8_t rsp, uint16_t uuid_len, const uint8_t *uuid, esp_gatt_perm_t perm,
                                uint16_t max_len, uint16_t len, uint8_t *value) {
  esp_gatts_attr_db_t a{};
  a.attr_control.auto_rsp = rsp;
  a.att_desc.uuid_length = uuid_len;
  a.att_desc.uuid_p = const_cast<uint8_t *>(uuid);
  a.att_desc.perm = perm;
  a.att_desc.max_length = max_len;
  a.att_desc.length = len;
  a.att_desc.value = value;
  return a;
}

void FlamekeeperBLE::build_table_() {
  using namespace fk::ble;
  uuid_bytes(0x1000, this->uuid_service_);
  uuid_bytes(0x1001, this->uuid_info_);
  uuid_bytes(0x1002, this->uuid_state_);
  uuid_bytes(0x1003, this->uuid_telemetry_);
  uuid_bytes(0x1004, this->uuid_command_);
  uuid_bytes(0x1005, this->uuid_result_);
  uuid_bytes(0x1006, this->uuid_update_);
  const uint8_t AUTO = ESP_GATT_AUTO_RSP;
  const uint8_t BY_APP = ESP_GATT_RSP_BY_APP;
  auto u16 = [](const uint16_t &u) { return reinterpret_cast<const uint8_t *>(&u); };
  auto prop = [](const uint8_t &p) { return const_cast<uint8_t *>(&p); };
  // A stack-answered value's maximum is its size, never a literal: Bluedroid refuses a characteristic
  // whose value is longer than its maximum (gatt_db.c, gatts_add_char_desc_value_check), and then the
  // whole table fails to build and no phone sees the service (a 32-byte maximum did that once State
  // and Telemetry outgrew it). What can still refuse a record that grows is checked here.
  static_assert(INFO_SIZE <= ESP_GATT_MAX_ATTR_LEN && STATE_SIZE <= ESP_GATT_MAX_ATTR_LEN &&
                    TELEMETRY_SIZE <= ESP_GATT_MAX_ATTR_LEN && RESULT_MAX <= ESP_GATT_MAX_ATTR_LEN &&
                    fk::ble::COMMAND_MAX <= ESP_GATT_MAX_ATTR_LEN && UPDATE_WRITE_MAX <= ESP_GATT_MAX_ATTR_LEN,
                "an attribute longer than the stack's maximum");
  // Notifications and indications are not split: each record fits one at the MTU the board offers.
  static_assert(STATE_SIZE <= LOCAL_MTU - 3 && TELEMETRY_SIZE <= LOCAL_MTU - 3 && RESULT_MAX <= LOCAL_MTU - 3,
                "a notified record longer than one notification");
  esp_gatts_attr_db_t *t = this->table_;
  t[A_SERVICE] = attr(AUTO, ESP_UUID_LEN_16, u16(UUID_PRIMARY), ESP_GATT_PERM_READ, 16, 16, this->uuid_service_);
  t[A_INFO_DECL] = attr(AUTO, ESP_UUID_LEN_16, u16(UUID_CHAR_DECL), ESP_GATT_PERM_READ, 1, 1, prop(PROP_READ));
  t[A_INFO] = attr(AUTO, ESP_UUID_LEN_128, this->uuid_info_, ESP_GATT_PERM_READ, INFO_SIZE, INFO_SIZE, this->info_);
  t[A_STATE_DECL] = attr(AUTO, ESP_UUID_LEN_16, u16(UUID_CHAR_DECL), ESP_GATT_PERM_READ, 1, 1, prop(PROP_READ_NOTIFY));
  t[A_STATE] = attr(AUTO, ESP_UUID_LEN_128, this->uuid_state_, PERM_SECURE_READ, STATE_SIZE, STATE_SIZE, this->state_);
  t[A_STATE_CCCD] = attr(AUTO, ESP_UUID_LEN_16, u16(UUID_CCCD), PERM_SECURE_RW, 2, 2, cccd_init);
  t[A_TELEMETRY_DECL] =
      attr(AUTO, ESP_UUID_LEN_16, u16(UUID_CHAR_DECL), ESP_GATT_PERM_READ, 1, 1, prop(PROP_READ_NOTIFY));
  t[A_TELEMETRY] = attr(AUTO, ESP_UUID_LEN_128, this->uuid_telemetry_, PERM_SECURE_READ, TELEMETRY_SIZE,
                        TELEMETRY_SIZE, this->telemetry_);
  t[A_TELEMETRY_CCCD] = attr(AUTO, ESP_UUID_LEN_16, u16(UUID_CCCD), PERM_SECURE_RW, 2, 2, cccd_init);
  t[A_COMMAND_DECL] = attr(AUTO, ESP_UUID_LEN_16, u16(UUID_CHAR_DECL), ESP_GATT_PERM_READ, 1, 1, prop(PROP_WRITE));
  // Answered by us: a write is refused (0xFD) while Result indications are off. The stack does not
  // enforce the length of a write answered by the app; the command handler checks it.
  t[A_COMMAND] = attr(BY_APP, ESP_UUID_LEN_128, this->uuid_command_, ESP_GATT_PERM_WRITE_ENC_MITM,
                      fk::ble::COMMAND_MAX, 0, nullptr);
  t[A_RESULT_DECL] = attr(AUTO, ESP_UUID_LEN_16, u16(UUID_CHAR_DECL), ESP_GATT_PERM_READ, 1, 1, prop(PROP_INDICATE));
  t[A_RESULT] = attr(AUTO, ESP_UUID_LEN_128, this->uuid_result_, PERM_SECURE_READ, RESULT_MAX, 0, nullptr);
  t[A_RESULT_CCCD] = attr(AUTO, ESP_UUID_LEN_16, u16(UUID_CCCD), PERM_SECURE_RW, 2, 2, cccd_init);
  // Board firmware update data, written without response (the app asks UPDATE_STATUS after a block).
  t[A_UPDATE_DECL] = attr(AUTO, ESP_UUID_LEN_16, u16(UUID_CHAR_DECL), ESP_GATT_PERM_READ, 1, 1, prop(PROP_UPDATE));
  t[A_UPDATE] = attr(BY_APP, ESP_UUID_LEN_128, this->uuid_update_, ESP_GATT_PERM_WRITE_ENC_MITM, UPDATE_WRITE_MAX, 0,
                     nullptr);
}

// ---------------------------------------------------------------- setup

// The connection a command came in on, as the command handler sees it.
class ConnLink : public fk::Link {
 public:
  ConnLink(FlamekeeperBLE *ble, uint16_t id) : ble_(ble), id_(id) {}
  fk::Transport transport() const override { return fk::Transport::BLUETOOTH; }
  int phone_slot() const override {
    FlamekeeperBLE::Conn *c = this->ble_->conn_(this->id_);
    return c == nullptr ? -1 : this->ble_->hub_->board().access().app_slot_of_owner(c->identity);
  }
  uint16_t update_chunk_max() const override {
    FlamekeeperBLE::Conn *c = this->ble_->conn_(this->id_);
    uint16_t mtu = c != nullptr ? c->mtu : 23;
    if (mtu > UPDATE_WRITE_MAX + 3)
      mtu = UPDATE_WRITE_MAX + 3;
    return uint16_t(mtu - fk::ble::UPDATE_BLE_OVERHEAD);
  }
  fk::Outcome forget_me() override {
    FlamekeeperBLE::Conn *c = this->ble_->conn_(this->id_);
    return c == nullptr ? fk::Outcome::refuse(fk::Result::REFUSED, "no connection") : this->ble_->forget_me_(*c);
  }
  fk::Outcome issue_wifi_key(uint8_t id[fk::KEY_ID_SIZE], uint8_t secret[fk::KEY_SECRET_SIZE]) override {
    FlamekeeperBLE::Conn *c = this->ble_->conn_(this->id_);
    return c == nullptr ? fk::Outcome::refuse(fk::Result::REFUSED, "no connection")
                        : this->ble_->issue_wifi_key_(*c, id, secret);
  }

 protected:
  FlamekeeperBLE *ble_;
  uint16_t id_;
};

void FlamekeeperBLE::setup() {
  fk::BoardServices &board = this->hub_->board();
  board.on_pin_change([this](uint32_t pin) { this->set_passkey_(pin); });
  board.on_forget_bonds([this]() { this->forget_all_bonds(); });
  board.on_forget_bond([this](const uint8_t addr[6]) { this->remove_bond_(addr); });
  board.set_pairing_info([this](fk::BoardInfo &p) {
    p.window_open = this->window_.is_open(millis());
  });
  board.on_record([this](const uint8_t *data, size_t len) {
    for (Conn &c : this->conns_)
      if (c.result_sub)
        this->queue_result_(c, data, len);
  });
  this->failures_pref_ = global_preferences->make_preference<uint8_t>(fnv1_hash("flamekeeper_pair_fails"), true);
  uint8_t fails = 0;
  if (this->failures_pref_.load(&fails))
    this->window_.load_failures(fails);
  // Power-up (not an OTA or watchdog restart) opens the pairing window: someone switched the heater
  // on. Not after repeated failed pairings (PIN guessing): then only BOOT or Home Assistant open it.
  if (this->open_on_power_up_ && esp_reset_reason() == ESP_RST_POWERON) {
    if (this->window_.power_up_allowed()) {
      this->window_.open(millis(), this->window_ms_);
      ESP_LOGI(TAG, "Power-up: Bluetooth pairing open for %u s", (unsigned) (this->window_ms_ / 1000));
    } else {
      ESP_LOGW(TAG, "Power-up: pairing stays closed after %u failed pairings; use BOOT or Home Assistant",
               this->window_.failures_since_paired());
    }
  }
  this->hub_->controller().on_event(
      [this](fk::Event e, fk::Result r, uint16_t id, const char *text) { this->broadcast_event_(e, r, id, text); });
  this->build_table_();
}

void FlamekeeperBLE::dump_config() {
  ESP_LOGCONFIG(TAG, "Flamekeeper BLE service (protocol %u)", fk::ble::PROTOCOL_MAJOR);
  ESP_LOGCONFIG(TAG, "  Service: %s (handles %u-%u)",
                g_start_status == ESP_GATT_OK ? "running" : this->service_settled_ ? "NOT RUNNING" : "starting",
                this->handles_[A_SERVICE], this->handles_[A_COUNT - 1]);
  ESP_LOGCONFIG(TAG, "  Phones paired: %u of %u", this->bonds_, this->max_bonds_);
  if (this->stack_started_)
    this->log_bonds_();
  ESP_LOGCONFIG(TAG, "  Pairing window: %u s, opens on power-up: %s", (unsigned) (this->window_ms_ / 1000),
                YESNO(this->open_on_power_up_));
}

// Security for every pairing on this board: LE Secure Connections only, MITM (passkey), bonding.
void FlamekeeperBLE::start_stack_() {
  esp_ble_auth_req_t auth = ESP_LE_AUTH_REQ_SC_MITM_BOND;
  esp_ble_io_cap_t iocap = ESP_IO_CAP_OUT;  // "display only": the phone types the board's passkey
  uint8_t key_size = 16;
  uint8_t keys = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
  uint8_t only_specified = ESP_BLE_ONLY_ACCEPT_SPECIFIED_AUTH_ENABLE;
  esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth, sizeof(auth));
  esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(iocap));
  esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size));
  esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &keys, sizeof(keys));
  esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &keys, sizeof(keys));
  esp_ble_gap_set_security_param(ESP_BLE_SM_ONLY_ACCEPT_SPECIFIED_SEC_AUTH, &only_specified, sizeof(only_specified));
  this->set_passkey_(this->hub_->board().pin());
  esp_ble_gatt_set_local_mtu(LOCAL_MTU);
  // Our callback in front of ESPHome's (it forwards every event): see gatts_callback.
  esp_err_t err = esp_ble_gatts_register_callback(gatts_callback);
  if (err != ESP_OK)
    ESP_LOGE(TAG, "esp_ble_gatts_register_callback failed: %d", err);
  err = esp_ble_gatts_app_register(APP_ID);
  if (err != ESP_OK)
    ESP_LOGE(TAG, "esp_ble_gatts_app_register failed: %d", err);
#ifdef USE_ESP32_BLE_ADVERTISING
  esp32_ble::global_ble->advertising_add_service_uuid(esp32_ble::ESPBTUUID::from_raw(fk::ble::SERVICE_UUID));
  esp32_ble::global_ble->advertising_start();
#endif
  this->refresh_bonds_();
  this->stack_started_ = true;
  this->stack_ms_ = millis();
}

void FlamekeeperBLE::create_table_(esp_gatt_if_t gatts_if) {
  if (this->table_asked_)
    return;
  this->table_asked_ = true;
  this->gatts_if_ = gatts_if;
  esp_err_t err = esp_ble_gatts_create_attr_tab(this->table_, gatts_if, A_COUNT, 0);
  if (err != ESP_OK)
    ESP_LOGE(TAG, "esp_ble_gatts_create_attr_tab failed: %d", err);
}

// What the Bluetooth task did with the service (gatts_callback): a table that failed to build is
// deleted and built again, a start that failed is tried again, each a few times, 2 s apart (a
// failure is most likely a moment short of heap). Settled, started or not, it lets the relay connect.
void FlamekeeperBLE::poll_table_(uint32_t now) {
  if (this->service_settled_)
    return;
  if (this->gatts_if_ == ESP_GATT_IF_NONE && g_our_if != ESP_GATT_IF_NONE) {
    // Registered, but the event has not reached the main loop: lost, after a grace period (made
    // as soon as the event comes, the table keeps its place after Device Information's).
    if (this->reg_seen_ms_ == 0) {
      this->reg_seen_ms_ = now == 0 ? 1 : now;
    } else if (now - this->reg_seen_ms_ > 2000) {
      ESP_LOGW(TAG, "The registration event was lost; creating the attribute table now");
      this->create_table_(g_our_if);
    }
  }
  const int tab = g_tab_status;
  if (tab > 0) {
    if (this->builds_ >= RETRIES) {
      ESP_LOGE(TAG, "Service not started: the attribute table failed to build (status %d); phones cannot see it", tab);
      this->settle_();
    } else if (this->retry_ms_ == 0) {
      ESP_LOGW(TAG, "Attribute table failed to build (status %d); trying again in 2 s", tab);
      if (g_tab_service != 0)
        esp_ble_gatts_delete_service(g_tab_service);
      this->retry_ms_ = now == 0 ? 1 : now;
    } else if (now - this->retry_ms_ >= 2000) {
      this->retry_ms_ = 0;
      this->builds_++;
      g_tab_service = 0;
      g_tab_status = -1;
      this->table_asked_ = false;
      this->create_table_(this->gatts_if_);
    }
    return;
  }
  if (tab == ESP_GATT_OK) {
    if (!this->table_ready_) {
      memcpy(this->handles_, g_tab_handles, sizeof(this->handles_));
      this->table_ready_ = true;
    }
    const int st = g_start_status;
    if (st == ESP_GATT_OK) {
      ESP_LOGI(TAG, "Service started (handles %u-%u)", this->handles_[A_SERVICE], this->handles_[A_COUNT - 1]);
      this->settle_();
      return;
    }
    if (g_start_err != ESP_OK || st > 0) {
      if (this->starts_ >= RETRIES) {
        ESP_LOGE(TAG, "Service not started (start %d, status %d); phones cannot see it", g_start_err, st);
        this->settle_();
      } else if (this->retry_ms_ == 0) {
        ESP_LOGW(TAG, "Service did not start (start %d, status %d); trying again in 2 s", g_start_err, st);
        this->retry_ms_ = now == 0 ? 1 : now;
      } else if (now - this->retry_ms_ >= 2000) {
        this->retry_ms_ = 0;
        this->starts_++;
        g_start_status = -1;
        g_start_err = esp_ble_gatts_start_service(this->handles_[A_SERVICE]);
      }
      return;
    }
  }
  if (now - this->stack_ms_ > 30000) {
    ESP_LOGE(TAG, "Service not started 30 s after the stack (registered %s, table %s); phones cannot see it",
             YESNO(g_our_if != ESP_GATT_IF_NONE), YESNO(tab == ESP_GATT_OK));
    this->settle_();
  }
}

void FlamekeeperBLE::settle_() {
  this->service_settled_ = true;
  this->hub_->board().set_bluetooth_settled();
}

// ---------------------------------------------------------------- loop

void FlamekeeperBLE::loop() {
  if (this->window_.failures_dirty()) {
    // Counted in the Bluetooth task; stored from here.
    uint8_t fails = this->window_.failures_since_paired();
    this->failures_pref_.save(&fails);
    this->window_.failures_saved();
  }
  uint32_t now = millis();
  if (!this->stack_started_) {
    if (esp32_ble::global_ble != nullptr && esp32_ble::global_ble->is_active())
      this->start_stack_();
    return;
  }
  this->poll_table_(now);
  if (now - this->last_bonds_ms_ > 5000)
    this->refresh_bonds_();
  for (Conn &c : this->conns_)
    if (c.forgetting && now - c.forget_ms > 2000)
      this->finish_forget_(c);  // the result was never confirmed
  // Strangers may not hold the connection slots: drop links that never authenticated (unless they
  // are pairing inside the window).
  if (!this->window_.is_open(now)) {
    for (Conn &c : this->conns_)
      if (!c.authenticated && now - c.since_ms > UNBONDED_DROP_MS) {
        ESP_LOGI(TAG, "Dropping a connection that did not authenticate");
        c.since_ms = now;  // once
        esp_ble_gatts_close(this->gatts_if_, c.id);
      }
  }
  if (this->table_ready_)
    this->update_values_(now);
  this->publish_entities_(now);
}

void FlamekeeperBLE::update_values_(uint32_t now) {
  const fk::Status &s = this->hub_->status();
  fk::BoardInfo p;
  this->hub_->board().info(p);
  fk::ble::encode_info(this->hub_->board().board(), s.capabilities, p, this->info_);
  if (memcmp(this->info_, this->last_info_, sizeof(this->info_)) != 0 && now - this->last_info_ms_ >= 1000) {
    memcpy(this->last_info_, this->info_, sizeof(this->info_));
    this->last_info_ms_ = now;
    esp_ble_gatts_set_attr_value(this->handles_[A_INFO], sizeof(this->info_), this->info_);
  }

  fk::ble::encode_state(s, this->state_);
  if (memcmp(this->state_, this->last_state_, sizeof(this->state_)) != 0 && now - this->last_state_ms_ >= STATE_MIN_MS) {
    memcpy(this->last_state_, this->state_, sizeof(this->state_));
    this->last_state_ms_ = now;
    esp_ble_gatts_set_attr_value(this->handles_[A_STATE], sizeof(this->state_), this->state_);
    this->notify_(A_STATE, this->state_, sizeof(this->state_), &Conn::state_sub);
  }

  fk::ble::encode_telemetry(s, this->telemetry_);
  uint32_t every = s.state == fk::HeaterState::OFF ? TELEMETRY_IDLE_MS : TELEMETRY_RUN_MS;
  if (memcmp(this->telemetry_, this->last_telemetry_, sizeof(this->telemetry_)) != 0 &&
      now - this->last_telemetry_ms_ >= every) {
    memcpy(this->last_telemetry_, this->telemetry_, sizeof(this->telemetry_));
    this->last_telemetry_ms_ = now;
    esp_ble_gatts_set_attr_value(this->handles_[A_TELEMETRY], sizeof(this->telemetry_), this->telemetry_);
    this->notify_(A_TELEMETRY, this->telemetry_, sizeof(this->telemetry_), &Conn::telemetry_sub);
  }
}

void FlamekeeperBLE::notify_(uint8_t a, const uint8_t *data, size_t len, bool Conn::*sub) {
  for (Conn &c : this->conns_)
    if (c.*sub)
      esp_ble_gatts_send_indicate(this->gatts_if_, c.id, this->handles_[a], len, const_cast<uint8_t *>(data), false);
}

void FlamekeeperBLE::publish_entities_(uint32_t now) {
  if (now - this->last_entities_ms_ < 1000)
    return;
  this->last_entities_ms_ = now;
#ifdef USE_SENSOR
  if (this->bonds_sensor_ != nullptr && (!this->bonds_sensor_->has_state() || this->bonds_sensor_->state != this->bonds_))
    this->bonds_sensor_->publish_state(this->bonds_);
  float left = this->window_.seconds_left(now);
  if (this->window_sensor_ != nullptr && (!this->window_sensor_->has_state() || this->window_sensor_->state != left))
    this->window_sensor_->publish_state(left);
#endif
#ifdef USE_BINARY_SENSOR
  if (this->pairing_sensor_ != nullptr)
    this->pairing_sensor_->publish_state(this->window_.is_open(now));
#endif
}

// ---------------------------------------------------------------- pairing and bonds

void FlamekeeperBLE::open_pairing_window() {
  this->refresh_bonds_();
  this->window_.open(millis(), this->window_ms_);
  ESP_LOGI(TAG, "Bluetooth pairing open for %u s", (unsigned) (this->window_ms_ / 1000));
}

void FlamekeeperBLE::refresh_bonds_() {
  this->last_bonds_ms_ = millis();
  if (!this->stack_started_ && !(esp32_ble::global_ble != nullptr && esp32_ble::global_ble->is_active()))
    return;
  int n = esp_ble_get_bond_device_num();
  const uint8_t bonds = n < 0 ? 0 : uint8_t(n > 255 ? 255 : n);
  const bool changed = bonds != this->bonds_ || !this->bonds_logged_;
  this->bonds_ = bonds;
  if (changed)
    this->log_bonds_();
}

// The stored bonds, as the stack keeps them: a phone that reconnects from a private address is
// recognised only through its identity key (IRK, "PID"), so the log shows whether each bond has one.
void FlamekeeperBLE::log_bonds_() {
  this->bonds_logged_ = true;
  int n = esp_ble_get_bond_device_num();
  ESP_LOGI(TAG, "Bonds stored: %d", n);
  if (n <= 0)
    return;
  std::vector<esp_ble_bond_dev_t> list(n);
  if (esp_ble_get_bond_device_list(&n, list.data()) != ESP_OK)
    return;
  for (int i = 0; i < n; i++) {
    const esp_ble_bond_dev_t &d = list[i];
    const esp_ble_bond_key_info_t &k = d.bond_key;
    bool irk = false;
    for (uint8_t b : k.pid_key.irk)
      irk = irk || b != 0;
    const uint8_t *a = d.bd_addr, *s = k.pid_key.static_addr;
    ESP_LOGI(TAG,
             "  Bond %d: %02X:%02X:%02X:%02X:%02X:%02X type %u, keys 0x%02X (LTK %s, ID %s, IRK %s), identity "
             "%02X:%02X:%02X:%02X:%02X:%02X type %u",
             i, a[0], a[1], a[2], a[3], a[4], a[5], unsigned(d.bd_addr_type), unsigned(k.key_mask),
             YESNO(k.key_mask & ESP_LE_KEY_PENC), YESNO(k.key_mask & ESP_LE_KEY_PID), irk ? "set" : "zero", s[0], s[1],
             s[2], s[3], s[4], s[5], unsigned(k.pid_key.addr_type));
  }
}

void FlamekeeperBLE::forget_all_bonds() {
  int n = esp_ble_get_bond_device_num();
  if (n > 0) {
    std::vector<esp_ble_bond_dev_t> list(n);
    esp_ble_get_bond_device_list(&n, list.data());
    for (int i = 0; i < n; i++)
      esp_ble_remove_bond_device(list[i].bd_addr);
  }
  for (Conn &c : this->conns_)
    esp_ble_gatts_close(this->gatts_if_, c.id);
  ESP_LOGW(TAG, "Forgot every paired phone (%d)", n < 0 ? 0 : n);
  this->refresh_bonds_();
}

void FlamekeeperBLE::set_passkey_(uint32_t pin) {
  esp_ble_gap_set_security_param(ESP_BLE_SM_SET_STATIC_PASSKEY, &pin, sizeof(pin));
}

fk::Outcome FlamekeeperBLE::forget_me_(Conn &c) {
  this->hub_->board().access().revoke_owner(c.identity);
  // The bond goes (and the link closes) once the result has been confirmed, so the phone sees it;
  // loop() finishes it after 2 s if no confirmation comes.
  c.forgetting = true;
  c.forget_ms = millis();
  ESP_LOGI(TAG, "A phone removed its pairing and its Wi-Fi key");
  return fk::Outcome::ok();
}

void FlamekeeperBLE::finish_forget_(Conn &c) {
  c.forgetting = false;
  esp_ble_remove_bond_device(c.identity);
  if (memcmp(c.identity, c.bda, sizeof(esp_bd_addr_t)) != 0)
    esp_ble_remove_bond_device(c.bda);
  esp_ble_gatts_close(this->gatts_if_, c.id);
  this->refresh_bonds_();
}

// FORGET_ME from the phone over Wi-Fi: its bond goes too.
void FlamekeeperBLE::remove_bond_(const uint8_t addr[6]) {
  esp_bd_addr_t bda;
  memcpy(bda, addr, sizeof(bda));
  for (Conn &c : this->conns_)
    if (memcmp(c.identity, bda, sizeof(bda)) == 0)
      esp_ble_gatts_close(this->gatts_if_, c.id);
  esp_ble_remove_bond_device(bda);
  ESP_LOGI(TAG, "A phone forgot this board over Wi-Fi: its Bluetooth pairing is removed");
  this->refresh_bonds_();
}

fk::Outcome FlamekeeperBLE::issue_wifi_key_(Conn &c, uint8_t id[fk::KEY_ID_SIZE],
                                            uint8_t secret[fk::KEY_SECRET_SIZE]) {
  const fk::WifiKey *k = this->hub_->board().access().issue_app_key(c.identity);
  if (k == nullptr)
    return fk::Outcome::refuse(fk::Result::TABLE_FULL, "no room for another app's Wi-Fi key");
  memcpy(id, k->id, fk::KEY_ID_SIZE);
  memcpy(secret, k->secret, fk::KEY_SECRET_SIZE);
  ESP_LOGI(TAG, "Wi-Fi key issued to a paired phone");
  return fk::Outcome::ok();
}

void FlamekeeperBLE::gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  if (param == nullptr)
    return;
  uint32_t now = millis();
  switch (event) {
    case ESP_GAP_BLE_SEC_REQ_EVT: {
      // A phone asks to pair. Only inside the window and with room in the bond table.
      this->refresh_bonds_();
      bool accept = this->window_.accept(now, this->bonds_, this->max_bonds_);
      this->bonds_before_pairing_ = this->bonds_;
      esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, accept);
      if (accept)
        ESP_LOGI(TAG, "Pairing request accepted (window open)");
      else
        ESP_LOGW(TAG, "Pairing request refused: %s",
                 this->window_.is_open(now) ? "bond table full" : "pairing window closed");
      break;
    }
    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
      ESP_LOGD(TAG, "Passkey shown to the phone");
      break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT: {
      const esp_ble_auth_cmpl_t &a = param->ble_security.auth_cmpl;
      ESP_LOGI(TAG, "Authentication with %02X:%02X:%02X:%02X:%02X:%02X type %u: %s (reason 0x%02X, mode 0x%02X)",
               a.bd_addr[0], a.bd_addr[1], a.bd_addr[2], a.bd_addr[3], a.bd_addr[4], a.bd_addr[5],
               unsigned(a.addr_type), a.success ? "ok" : "failed", unsigned(a.fail_reason), unsigned(a.auth_mode));
      if (!a.success) {
        ESP_LOGW(TAG, "Bluetooth authentication failed (reason 0x%02X)", a.fail_reason);
        if (this->window_.is_open(now)) {
          this->window_.failed();  // a wrong PIN; the third closes the window
          if (!this->window_.is_open(now))
            ESP_LOGW(TAG, "Three failed pairings: pairing window closed");
        }
        break;
      }
      // Mark the link: by address, or else the one link still waiting (a resolved private address
      // can differ from the one the connection reported).
      Conn *match = nullptr;
      for (Conn &c : this->conns_)
        if (memcmp(c.bda, a.bd_addr, sizeof(esp_bd_addr_t)) == 0)
          match = &c;
      if (match == nullptr)
        for (Conn &c : this->conns_)
          if (!c.authenticated)
            match = &c;
      if (match != nullptr) {
        match->authenticated = true;
        memcpy(match->identity, a.bd_addr, sizeof(esp_bd_addr_t));
      }
      this->refresh_bonds_();
      if (this->bonds_ > this->bonds_before_pairing_ && this->window_.is_open(now)) {
        this->window_.paired();  // one new phone per window
        ESP_LOGI(TAG, "New phone paired (%u of %u); pairing window closed", this->bonds_, this->max_bonds_);
      }
      break;
    }
    default:
      break;
  }
}

// ---------------------------------------------------------------- GATT

FlamekeeperBLE::Conn *FlamekeeperBLE::conn_(uint16_t id) {
  for (Conn &c : this->conns_)
    if (c.id == id)
      return &c;
  return nullptr;
}

void FlamekeeperBLE::gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                                         esp_ble_gatts_cb_param_t *param) {
  if (event == ESP_GATTS_REG_EVT) {
    if (param->reg.app_id != APP_ID)
      return;
    if (param->reg.status != ESP_GATT_OK) {
      ESP_LOGE(TAG, "GATT application registration failed: %d", param->reg.status);
      return;
    }
    this->create_table_(gatts_if);
    return;
  }
  if (gatts_if != this->gatts_if_ || this->gatts_if_ == ESP_GATT_IF_NONE)
    return;

  switch (event) {
    case ESP_GATTS_CREAT_ATTR_TAB_EVT:
      // Taken and started in the Bluetooth task (gatts_callback); poll_table_ reports it.
      if (param->add_attr_tab.status != ESP_GATT_OK || param->add_attr_tab.num_handle != A_COUNT)
        ESP_LOGE(TAG, "Attribute table not created: status %d", param->add_attr_tab.status);
      break;
    case ESP_GATTS_CONNECT_EVT: {
      Conn c{};
      c.id = param->connect.conn_id;
      memcpy(c.bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
      memcpy(c.identity, param->connect.remote_bda, sizeof(esp_bd_addr_t));
      c.since_ms = millis();
      this->conns_.push_back(c);
      const uint8_t *a = param->connect.remote_bda;
      // With the heap as it stands now: what each Bluetooth link costs (2026-11-24, heap budget).
      ESP_LOGI(TAG, "Phone connected from %02X:%02X:%02X:%02X:%02X:%02X (%u connected; heap %u free, %u block)", a[0],
               a[1], a[2], a[3], a[4], a[5], (unsigned) this->conns_.size(),
               (unsigned) heap_caps_get_free_size(MALLOC_CAP_8BIT),
               (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
      break;
    }
    case ESP_GATTS_DISCONNECT_EVT: {
      uint16_t id = param->disconnect.conn_id;
      this->conns_.erase(std::remove_if(this->conns_.begin(), this->conns_.end(),
                                        [id](const Conn &c) { return c.id == id; }),
                         this->conns_.end());
      ESP_LOGI(TAG, "Phone disconnected (heap %u free, %u block)", (unsigned) heap_caps_get_free_size(MALLOC_CAP_8BIT),
               (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
      break;
    }
    case ESP_GATTS_WRITE_EVT:
      this->on_write_(param);
      break;
    case ESP_GATTS_MTU_EVT: {
      Conn *c = this->conn_(param->mtu.conn_id);
      if (c != nullptr)
        c->mtu = param->mtu.mtu;
      break;
    }
    case ESP_GATTS_CONF_EVT: {
      Conn *c = this->conn_(param->conf.conn_id);
      if (c != nullptr && param->conf.handle == this->handles_[A_RESULT]) {
        c->indicating = false;
        if (c->forgetting && c->pending.empty())
          this->finish_forget_(*c);  // FORGET_ME's result (queued last) has been confirmed
        else
          this->pump_indications_(*c);
      }
      break;
    }
    default:
      break;
  }
}

void FlamekeeperBLE::on_write_(esp_ble_gatts_cb_param_t *param) {
  auto &w = param->write;
  Conn *c = this->conn_(w.conn_id);
  if (c == nullptr)
    return;
  // Any write here passed the stack's encrypted + authenticated permission check.
  if (!c->authenticated && this->hub_->board().push_engine() != nullptr)
    this->hub_->board().push_engine()->app_seen();
  c->authenticated = true;
  uint16_t h = w.handle;
  if (h == this->handles_[A_STATE_CCCD] || h == this->handles_[A_TELEMETRY_CCCD] || h == this->handles_[A_RESULT_CCCD]) {
    uint16_t v = w.len >= 2 ? uint16_t(w.value[0] | (w.value[1] << 8)) : 0;
    if (h == this->handles_[A_STATE_CCCD])
      c->state_sub = v & 0x0001;
    else if (h == this->handles_[A_TELEMETRY_CCCD])
      c->telemetry_sub = v & 0x0001;
    else
      c->result_sub = v & 0x0002;
    return;
  }
  if (h == this->handles_[A_UPDATE]) {
    // Board firmware data: taken in order or ignored, never answered.
    if (w.is_prep) {
      if (w.need_rsp)
        esp_ble_gatts_send_response(this->gatts_if_, w.conn_id, w.trans_id, ESP_GATT_REQ_NOT_SUPPORTED, nullptr);
      return;
    }
    if (w.need_rsp)
      esp_ble_gatts_send_response(this->gatts_if_, w.conn_id, w.trans_id, ESP_GATT_OK, nullptr);
    fk::BoardUpdate *u = this->hub_->board().board_update();
    if (u != nullptr)
      u->data(w.value, w.len);
    return;
  }
  if (h != this->handles_[A_COMMAND])
    return;
  if (w.is_prep) {
    if (w.need_rsp)
      esp_ble_gatts_send_response(this->gatts_if_, w.conn_id, w.trans_id, ESP_GATT_REQ_NOT_SUPPORTED, nullptr);
    return;
  }
  if (!c->result_sub) {
    if (w.need_rsp)
      esp_ble_gatts_send_response(this->gatts_if_, w.conn_id, w.trans_id, ESP_GATT_CCC_CFG_ERR, nullptr);
    return;
  }
  if (w.need_rsp)
    esp_ble_gatts_send_response(this->gatts_if_, w.conn_id, w.trans_id, ESP_GATT_OK, nullptr);
  uint8_t out[fk::ble::RESULT_MAX];
  uint16_t conn_id = c->id;
  ConnLink link(this, conn_id);
  size_t n = fk::ble::handle_command(w.value, w.len, this->hub_->controller(), link, this->hub_->board(), out,
                                     sizeof(out));
  c = this->conn_(conn_id);  // FORGET_ME may have closed it (the disconnect comes later, as an event)
  if (n != 0 && c != nullptr)
    this->queue_result_(*c, out, n);
}

void FlamekeeperBLE::queue_result_(Conn &c, const uint8_t *data, size_t len) {
  if (c.pending.size() >= 8)
    c.pending.erase(c.pending.begin());  // a phone that never confirms: keep the newest
  c.pending.emplace_back(data, data + len);
  this->pump_indications_(c);
}

// One indication in flight per connection; the next goes out on its confirmation.
void FlamekeeperBLE::pump_indications_(Conn &c) {
  if (c.indicating || c.pending.empty() || !c.result_sub)
    return;
  std::vector<uint8_t> &r = c.pending.front();
  esp_err_t err = esp_ble_gatts_send_indicate(this->gatts_if_, c.id, this->handles_[A_RESULT], r.size(), r.data(), true);
  c.pending.erase(c.pending.begin());
  c.indicating = err == ESP_OK;
}

void FlamekeeperBLE::broadcast_event_(fk::Event e, fk::Result r, uint16_t id, const char *text) {
  uint8_t out[fk::ble::RESULT_MAX];
  size_t n = fk::ble::encode_event(e, r, id, text, out, sizeof(out));
  if (n == 0)
    return;
  for (Conn &c : this->conns_)
    if (c.result_sub)
      this->queue_result_(c, out, n);
}

}  // namespace flamekeeper_ble
}  // namespace esphome

#endif  // USE_ESP32
