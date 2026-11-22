#include "board_services.h"

#ifdef USE_ESP32

#include <esp_random.h>
#include <soc/soc_caps.h>

#include <cstdio>
#include <cstring>

#include "ble_protocol.h"
#include "crypto.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "pairing.h"

#if SOC_HMAC_SUPPORTED
#include <esp_efuse.h>
#include <esp_hmac.h>
#endif
#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#endif
#ifdef USE_API
#include "esphome/components/api/api_server.h"
#endif
#ifdef USE_MDNS
#include <mdns.h>
#endif
#ifdef USE_OTA_SIGNED_VERIFICATION
#include <esp_ota_ops.h>
#endif

// Home Assistant keys set from the app: only when the key is not fixed in the YAML.
#if defined(USE_API) && defined(USE_API_NOISE) && !defined(USE_API_NOISE_PSK_FROM_YAML)
#define FK_HA_KEYS
#endif

namespace esphome {
namespace flamekeeper {

static const char *const TAG = "flamekeeper.board";
static const uint32_t WIFI_CHANGE_MS = 60000;  // a new network has this long to connect
static_assert(WIFI_CHANGE_MS == 60000, "the fallback text says 60 s");
static const uint32_t RESET_DELAY_MS = 3000;   // the stop reaches the heater before the restart
static const uint32_t RESET_WAIT_MS = 8000;    // ... and the heater's answer to its parameter reset
static const uint32_t PARAMS_RETRY_MS = 10000; // after the restart: ask again this often
static const uint32_t PARAMS_RETRY_MAGIC = 0x50445231;  // "PDR1": the heater's reset is still owed

namespace {
struct WebPref {
  uint8_t len;
  uint8_t data[WEB_PASSWORD_MAX];
};
struct PinPref {
  uint32_t magic;  // PIN_MAGIC: a PIN the user set
  uint32_t pin;
};
static const uint32_t PIN_MAGIC = 0x50494E31;  // "PIN1"
// The network in use before an app's Wi-Fi change, kept until the change is confirmed or undone, so
// a restart in the middle of the trial goes back to it.
struct WifiFallbackPref {
  char ssid[33];
  char password[65];
  uint8_t mode;
  uint8_t enabled;
  uint32_t check;
};
static const uint32_t WIFI_FALLBACK_CHECK = 0x57464231;  // "WFB1"

struct KeysPref {
  uint8_t data[Access::SERIALIZED_MAX];
};
// The internet relay: on, the host ("" the default) and the board's key pair. A factory reset clears
// every preference, this one with it.
struct RelayPref {
  uint32_t magic;
  uint8_t on;
  uint8_t has_key;
  char host[relay::HOST_MAX + 1];
  uint8_t key[relay::PRIVATE_KEY_SIZE];
};
static const uint32_t RELAY_MAGIC = 0x524C5931;  // "RLY1"

uint32_t restart_at_ms = 0;  // 0: none pending
const char *restart_why = "";
bool reset_pending = false;
uint32_t reset_at_ms = 0;
bool mdns_api_hidden = false;
const char *const WIFI_PLACEHOLDER = "flamekeeper-not-set-up";  // flamekeeper-rev-a2.yaml: never joined

#ifdef FK_HA_KEYS
// ESPHome keeps the API key under this preference (api_server.cpp, NOISE_PSK_PREF_HASH, 2026.10).
const uint32_t API_KEY_PREF = 88491486UL;
bool load_api_key(noise::psk_t &out) {
  api::SavedNoisePsk saved{};
  if (!global_preferences->make_preference<api::SavedNoisePsk>(API_KEY_PREF, true).load(&saved))
    return false;
  bool any = false;
  for (uint8_t b : saved.psk)
    any |= b != 0;  // all zeros = no key
  out = saved.psk;
  crypto::wipe(saved.psk.data(), saved.psk.size());
  return any;
}
#endif
}  // namespace

BoardServices::BoardServices(Controller *controller)
    : controller_(controller), access_([](uint8_t *out, size_t len) { esp_fill_random(out, len); }) {}

void BoardServices::setup() {
  this->keys_pref_ = global_preferences->make_preference<KeysPref>(fnv1_hash("flamekeeper_wifi_keys"), true);
  this->web_pref_ = global_preferences->make_preference<WebPref>(fnv1_hash("flamekeeper_web_password"), true);
  this->pin_pref_ = global_preferences->make_preference<PinPref>(fnv1_hash("flamekeeper_user_pin"), true);
  this->random_pin_pref_ = global_preferences->make_preference<uint32_t>(fnv1_hash("flamekeeper_ble_passkey"), true);
  this->wifi_pref_ = global_preferences->make_preference<uint8_t>(fnv1_hash("flamekeeper_wifi_mode"), true);
  this->ha_pref_ = global_preferences->make_preference<uint8_t>(fnv1_hash("flamekeeper_ha_on"), true);
  this->wifi_fallback_pref_ =
      global_preferences->make_preference<WifiFallbackPref>(fnv1_hash("flamekeeper_wifi_fallback"), true);
  this->params_retry_pref_ = global_preferences->make_preference<uint32_t>(fnv1_hash("flamekeeper_params_reset"), true);
  uint32_t owed = 0;
  this->params_retry_ = this->params_retry_pref_.load(&owed) && owed == PARAMS_RETRY_MAGIC;
  // The internet relay (off until an app switches it on).
  this->relay_pref_ = global_preferences->make_preference<RelayPref>(fnv1_hash("flamekeeper_relay"), true);
  this->relay_ = std::make_unique<RelayClient>(this->access_, [](uint8_t *out, size_t n) { esp_fill_random(out, n); });
#ifdef ESPHOME_PROJECT_VERSION
  this->relay_->set_version(ESPHOME_PROJECT_VERSION);
#else
  this->relay_->set_version("dev");
#endif
  {
    RelayPref rp{};
    if (this->relay_pref_.load(&rp) && rp.magic == RELAY_MAGIC) {
      rp.host[relay::HOST_MAX] = '\0';
      this->relay_->configure(rp.on != 0, rp.host, rp.has_key ? rp.key : nullptr);
    }
    crypto::wipe(&rp, sizeof(rp));
  }
  this->relay_->on_settings([this]() {
    RelayPref rp{};
    rp.magic = RELAY_MAGIC;
    rp.on = this->relay_->on() ? 1 : 0;
    std::strncpy(rp.host, this->relay_->custom_host(), relay::HOST_MAX);
    rp.has_key = this->relay_->identity().valid() ? 1 : 0;
    if (rp.has_key)
      std::memcpy(rp.key, this->relay_->identity().private_key(), relay::PRIVATE_KEY_SIZE);
    this->relay_pref_.save(&rp);
    global_preferences->sync();
    crypto::wipe(&rp, sizeof(rp));
  });
  // Push notifications, through the relay. They start reading the event log here, before the hub
  // logs this start, so "the board restarted" can be pushed.
  if (this->controller_->automation() != nullptr) {
    Automation &a = *this->controller_->automation();
    this->push_ = std::make_unique<PushEngine>(*this->controller_, a, this->access_, *this->relay_,
                                               [](uint8_t *out, size_t n) { esp_fill_random(out, n); });
    this->push_pref_ = global_preferences->make_preference<PushEngine::Saved>(fnv1_hash("flamekeeper_push"), true);
    PushEngine::Saved ps{};
    if (this->push_pref_.load(&ps))
      this->push_->load(ps);
    this->push_->on_save([this](const PushEngine::Saved &s) {
      PushEngine::Saved copy = s;
      this->push_pref_.save(&copy);
      global_preferences->sync();
    });
    this->push_->begin(a.log().newest());
  }
#ifdef USE_OTA_SIGNED_VERIFICATION
  this->update_pref_ =
      global_preferences->make_preference<BoardUpdate::Saved>(fnv1_hash("flamekeeper_board_update"), true);
  this->update_ = std::make_unique<BoardUpdate>(*this->controller_, this->update_flash_);
  BoardUpdate::Saved saved{};
  const bool have = this->update_pref_.load(&saved);
  this->update_->load(have ? &saved : nullptr, uint32_t(App.get_build_time()));
  this->update_->on_save([this](const BoardUpdate::Saved &s) {
    BoardUpdate::Saved copy = s;
    this->update_pref_.save(&copy);
    global_preferences->sync();
  });
#endif
  // The heater's answer to "parameters back to defaults" (factory reset, or the retry after it).
  this->controller_->on_event([this](Event e, Result r, uint16_t, const char *) {
    if (e != Event::PARAMS_DEFAULTS)
      return;
    this->params_answered_ = true;
    // Not supported: this heater has no parameters to reset over the link; nothing owed.
    this->params_reset_ok_ = r == Result::OK || r == Result::NOT_SUPPORTED;
  });

  KeysPref keys{};
  if (this->keys_pref_.load(&keys))
    this->access_.deserialize(keys.data, sizeof(keys.data));
  WebPref web{};
  if (this->web_pref_.load(&web) && web.len <= WEB_PASSWORD_MAX)
    this->access_.load_web_password(web.data, web.len);
  memset(&web, 0, sizeof(web));
  this->load_pin_();

  // Wi-Fi: as configured (a network in the YAML), the app's network, or off.
  uint8_t mode = 0;
  if (this->wifi_pref_.load(&mode) && mode <= 2)
    this->wifi_mode_ = mode;
#ifdef USE_WIFI
  if (wifi::global_wifi_component != nullptr) {
    if (this->wifi_mode_ == 1)
      wifi::global_wifi_component->enable();
    else if (this->wifi_mode_ == 2)
      wifi::global_wifi_component->disable();
  }
#endif

  // Home Assistant: off until the app switches it on (Flamekeeper board), or always on.
  if (this->ha_mode_ == HaMode::HA_FROM_APP) {
    uint8_t on = 0;
    this->ha_on_ = this->ha_pref_.load(&on) && on == 1;
#ifdef USE_API
    if (!this->ha_on_ && api::global_api_server != nullptr) {
      // Before the API's own setup (the hub sets up earlier): it never listens.
      api::global_api_server->mark_failed();
      ESP_LOGI(TAG, "Home Assistant is off (switch it on from the app)");
    }
#endif
  }
}

void BoardServices::load_pin_() {
  switch (this->pin_source_) {
    case PinSource::PIN_FIXED:
      this->label_pin_ = this->configured_pin_;
      break;
    case PinSource::PIN_EFUSE: {
#if SOC_HMAC_SUPPORTED
      esp_efuse_block_t block = esp_efuse_block_t(EFUSE_BLK_KEY0 + this->efuse_key_);
      if (esp_efuse_get_key_purpose(block) == ESP_EFUSE_KEY_PURPOSE_HMAC_UP) {
        hmac_key_id_t key = hmac_key_id_t(HMAC_KEY0 + this->efuse_key_);
        uint32_t pin = 0;
        if (label_pin([key](const uint8_t *msg, size_t len, uint8_t out[32]) {
              return esp_hmac_calculate(key, msg, len, out) == ESP_OK;
            }, &pin)) {
          this->label_pin_ = pin;
          break;
        }
        ESP_LOGE(TAG, "The HMAC peripheral failed: falling back to a random PIN");
      } else {
        ESP_LOGW(TAG, "eFuse key %u holds no board key (not provisioned): using a random PIN", this->efuse_key_);
      }
#else
      ESP_LOGE(TAG, "This chip has no HMAC peripheral: using a random PIN");
#endif
    }
      // an unprovisioned board still needs a PIN
      [[fallthrough]];
    case PinSource::PIN_RANDOM: {
      uint32_t key;
      if (this->random_pin_pref_.load(&key) && passkey_valid(key)) {
        this->label_pin_ = key;
      } else {
        this->label_pin_ = passkey_from_random([] { return random_uint32(); });
        this->random_pin_pref_.save(&this->label_pin_);
        global_preferences->sync();
      }
      if (this->pin_source_ == PinSource::PIN_EFUSE) {
        char s[7];
        format_passkey(this->label_pin_, s);
        ESP_LOGW(TAG, "Development PIN: %s", s);  // only on boards without a board key
      }
      break;
    }
  }
  PinPref p{};
  this->user_pin_valid_ = this->pin_pref_.load(&p) && p.magic == PIN_MAGIC && passkey_valid(p.pin);
  this->user_pin_ = this->user_pin_valid_ ? p.pin : 0;
}

void BoardServices::dump_config() {
  ESP_LOGCONFIG(TAG, "Board services:");
  ESP_LOGCONFIG(TAG, "  PIN: %s%s",
                this->pin_source_ == PinSource::PIN_EFUSE      ? "from the eFuse board key"
                : this->pin_source_ == PinSource::PIN_FIXED ? "from the configuration"
                                                             : "random, kept on the board",
                this->user_pin_valid_ ? " (changed by the user)" : "");
  ESP_LOGCONFIG(TAG, "  Wi-Fi keys: %u apps, %u browsers; web password %s", this->access_.count(KeyKind::APP),
                this->access_.count(KeyKind::BROWSER), this->access_.web_password_set() ? "set" : "not set");
  if (this->relay_) {
    char id[relay::ID_TEXT_SIZE + 1] = "(none yet)";
    if (this->relay_->identity().valid())
      relay::id_text(this->relay_->identity().id(), id);
    ESP_LOGCONFIG(TAG, "  Internet relay: %s, host %s, board id %s", this->relay_->on() ? "on" : "off",
                  this->relay_->host(), id);
  }
#ifdef USE_OTA_SIGNED_VERIFICATION
  UpdateStatus us;
  this->update_->update_status(us);
  static const char *const LAST[] = {"none yet", "installed", "rolled back", "dropped"};
  ESP_LOGCONFIG(TAG, "  Firmware updates from the app: signed images only, slot %u bytes; last update: %s",
                unsigned(this->update_flash_.slot_size()), LAST[uint8_t(us.last) & 3]);
#else
  ESP_LOGCONFIG(TAG, "  Firmware updates from the app: off (no signed_ota_verification in this build)");
#endif
  ESP_LOGCONFIG(TAG, "  Home Assistant: %s",
                this->ha_mode_ == HaMode::HA_ALWAYS_ON ? "always on" : (this->ha_on_ ? "on (from the app)" : "off"));
}

void BoardServices::loop(uint32_t now) {
  if (this->push_)
    this->push_->loop(now);
#ifdef USE_OTA_SIGNED_VERIFICATION
  this->update_->loop(now);
#endif
  if (this->access_.dirty())
    this->save_access_();
  if (this->access_.web_password_dirty()) {
    WebPref web{};
    web.len = uint8_t(this->access_.web_password_len());
    memcpy(web.data, this->access_.web_password(), web.len);
    this->web_pref_.save(&web);
    memset(&web, 0, sizeof(web));
    global_preferences->sync();
    this->access_.web_password_clean();
  }

#if defined(USE_MDNS) && defined(USE_API)
  // Home Assistant off: take the API's service out of mDNS once ESPHome has announced it.
  if (!this->ha_on_ && !mdns_api_hidden && millis() > 5000) {
    mdns_api_hidden = true;
    mdns_service_remove("_esphomelib", "_tcp");
  }
#endif

#ifdef USE_WIFI
  if (!this->wifi_fallback_checked_ && wifi::global_wifi_component != nullptr) {
    // Once Wi-Fi is set up: a change that a restart interrupted goes back to the old network.
    this->wifi_fallback_checked_ = true;
    WifiFallbackPref fb{};
    if (this->wifi_fallback_pref_.load(&fb) && fb.check == WIFI_FALLBACK_CHECK) {
      fb.ssid[sizeof(fb.ssid) - 1] = 0;
      fb.password[sizeof(fb.password) - 1] = 0;
      this->old_ssid_ = fb.ssid;
      this->old_password_ = fb.password;
      this->old_mode_ = fb.mode;
      this->old_enabled_ = fb.enabled != 0;
      memset(&fb, 0, sizeof(fb));
      ESP_LOGW(TAG, "A Wi-Fi change was cut short by a restart: going back to the old settings");
      this->wifi_restore_old_();
    }
  }
  if (this->wifi_reconnect_pending_ && int32_t(now - this->wifi_reconnect_at_ms_) >= 0) {
    this->wifi_reconnect_pending_ = false;
    wifi::global_wifi_component->disable();
    wifi::global_wifi_component->enable();
  }
  if (this->wifi_changing_ && !this->wifi_reconnect_pending_ && wifi::global_wifi_component != nullptr) {
    char ssid[wifi::SSID_BUFFER_SIZE] = {};
    bool connected = wifi::global_wifi_component->is_connected();
    if (!connected)
      this->wifi_saw_down_ = true;  // the old connection is gone: a connection now is a new one
    if (connected)
      wifi::global_wifi_component->wifi_ssid_to(ssid);
    if (connected && this->wifi_saw_down_ && this->new_ssid_ == ssid) {
      this->wifi_change_done_(true, "");
    } else if (now - this->wifi_change_ms_ >= WIFI_CHANGE_MS) {
      this->wifi_change_done_(false, "the new network did not connect within 60 s; the old settings are back");
    }
  }
#endif

  if (reset_pending && int32_t(now - reset_at_ms) >= 0 &&
      (!this->params_asked_ || this->params_answered_ || int32_t(now - this->params_deadline_ms_) >= 0)) {
    reset_pending = false;
    ESP_LOGW(TAG, "Factory reset: every setting and every paired app is forgotten");
    bool owed = !(this->params_answered_ && this->params_reset_ok_);
    if (owed)
      ESP_LOGW(TAG, "The heater did not confirm its parameter reset: asking again after the restart");
    if (this->forget_bonds_)
      this->forget_bonds_();
    this->access_.clear();
    // Every ESPHome preference: Wi-Fi, the API key, entity settings, ours.
    global_preferences->reset();
    if (owed) {
      uint32_t magic = PARAMS_RETRY_MAGIC;
      this->params_retry_pref_.save(&magic);  // the one thing kept: the heater's reset is still owed
      global_preferences->sync();
    }
    App.safe_reboot();
  }
  // After a factory reset the heater had not confirmed: ask it again until it answers.
  if (this->params_retry_ && !reset_pending) {
    if (this->params_asked_ && this->params_answered_) {
      this->params_asked_ = false;
      if (this->params_reset_ok_) {
        this->params_retry_ = false;
        uint32_t none = 0;
        this->params_retry_pref_.save(&none);
        global_preferences->sync();
        ESP_LOGI(TAG, "The heater's parameters are back to their defaults (owed since the factory reset)");
      }
    }
    if (!this->params_asked_ || int32_t(now - this->params_deadline_ms_) >= 0) {
      if (this->params_next_ms_ == 0 || int32_t(now - this->params_next_ms_) >= 0) {
        this->params_next_ms_ = now + PARAMS_RETRY_MS;
        this->params_answered_ = false;
        this->params_asked_ = this->controller_->heater()->reset_parameters().result == Result::OK;
        this->params_deadline_ms_ = now + RESET_WAIT_MS;
      }
    }
  }
  if (restart_at_ms != 0 && int32_t(now - restart_at_ms) >= 0) {
    ESP_LOGI(TAG, "Restarting: %s", restart_why);
    restart_at_ms = 0;
    App.safe_reboot();
  }
}

void BoardServices::save_access_() {
  KeysPref keys{};
  this->access_.serialize(keys.data, sizeof(keys.data));
  this->keys_pref_.save(&keys);
  memset(&keys, 0, sizeof(keys));
  global_preferences->sync();
  this->access_.clean();
}

void BoardServices::info(BoardInfo &out) const {
  if (this->pairing_info_)
    this->pairing_info_(out);
  out.app_keys = this->access_.count(KeyKind::APP);
  out.max_app_keys = Access::APP_KEYS;
  out.browser_keys = this->access_.count(KeyKind::BROWSER);
  out.max_browser_keys = Access::BROWSER_KEYS;
  WifiStatus w;
  this->wifi_status(w);
  out.wifi = w.state;
  out.pin_changed = this->user_pin_valid_;
  out.web_password_set = this->access_.web_password_set();
  out.ha = this->ha_state();
  out.ha_switchable = this->ha_switchable();
#ifdef USE_OTA_SIGNED_VERIFICATION
  out.can_update = true;
#endif
#ifdef ESPHOME_PROJECT_VERSION
  // "major.minor.patch" (anything after a missing part stays 0)
  const char *v = ESPHOME_PROJECT_VERSION;
  for (int i = 0; i < 3 && *v != '\0'; i++) {
    unsigned n = 0;
    while (*v >= '0' && *v <= '9')
      n = n * 10 + unsigned(*v++ - '0');
    out.version[i] = uint8_t(n > 255 ? 255 : n);
    if (*v != '.')
      break;
    v++;
  }
#endif
  out.build_utc = uint32_t(App.get_build_time());
  out.has_relay = this->relay_ != nullptr;
}

#ifdef USE_OTA_SIGNED_VERIFICATION
uint32_t EspUpdateFlash::slot_size() const {
  const esp_partition_t *p = esp_ota_get_next_update_partition(nullptr);
  return p != nullptr ? p->size : 0;
}

bool EspUpdateFlash::begin(uint32_t size) {
  this->backend_ = ota::make_ota_backend();
  if (this->backend_->begin(size) == ota::OTA_RESPONSE_OK)
    return true;
  ESP_LOGW(TAG, "Firmware update: the update slot could not be prepared");
  this->backend_->abort();
  this->backend_.reset();
  return false;
}

bool EspUpdateFlash::write(const uint8_t *data, size_t len) {
  if (!this->backend_)
    return false;
  // The backend does not change the bytes (ota_backend.h).
  return this->backend_->write(const_cast<uint8_t *>(data), len) == ota::OTA_RESPONSE_OK;
}

UpdateFlash::End EspUpdateFlash::end() {
  if (!this->backend_)
    return End::FAILED;
  const ota::OTAResponseTypes r = this->backend_->end();
  this->backend_->abort();  // safe after end(); frees what a failed end() left
  this->backend_.reset();
  if (r == ota::OTA_RESPONSE_OK) {
    ESP_LOGI(TAG, "Firmware update: installed, restarting into it");
    return End::OK;
  }
  ESP_LOGW(TAG, "Firmware update: refused by the OTA check (0x%02X)", unsigned(r));
  switch (r) {
    case ota::OTA_RESPONSE_ERROR_SIGNATURE_INVALID:
    case ota::OTA_RESPONSE_ERROR_UPDATE_END:
    case ota::OTA_RESPONSE_ERROR_MAGIC:
    case ota::OTA_RESPONSE_ERROR_VERSION_DOWNGRADE:
      return End::BAD_IMAGE;
    default:
      return End::FAILED;
  }
}

void EspUpdateFlash::abort() {
  if (this->backend_)
    this->backend_->abort();
  this->backend_.reset();
}

void EspUpdateFlash::restart() { App.safe_reboot(); }
#endif

void BoardServices::broadcast_(const uint8_t *data, size_t len) {
  for (auto &cb : this->record_cbs_)
    cb(data, len);
}

void BoardServices::restart_soon_(const char *why) {
  restart_why = why;
  restart_at_ms = millis() + 1500;  // the result goes out first
  if (restart_at_ms == 0)
    restart_at_ms = 1;
}

void BoardServices::factory_reset() {
  if (reset_pending)
    return;
  ESP_LOGW(TAG, "Factory reset requested: stopping the heater first");
  this->controller_->set_mode(Mode::OFF, Source::BOARD);
  this->params_answered_ = false;
  this->params_reset_ok_ = false;
  Outcome o = this->controller_->heater()->reset_parameters();
  this->params_asked_ = o.result == Result::OK;
  if (!this->params_asked_)
    ESP_LOGW(TAG, "The heater's parameters could not be reset now: %s", o.reason);
  reset_pending = true;
  reset_at_ms = millis() + RESET_DELAY_MS;
  this->params_deadline_ms_ = millis() + RESET_WAIT_MS;
}

// ---------------------------------------------------------------- BoardSettings

Outcome BoardServices::set_pin(uint32_t pin) {
  PinPref p{PIN_MAGIC, pin};
  if (!this->pin_pref_.save(&p) || !global_preferences->sync())
    return Outcome::refuse(Result::REFUSED, "the PIN could not be saved");
  this->user_pin_ = pin;
  this->user_pin_valid_ = true;
  for (auto &cb : this->pin_cbs_)
    cb(pin);
  ESP_LOGI(TAG, "The PIN was changed from an app");
  return Outcome::ok();
}

Outcome BoardServices::set_web_password(const uint8_t *current, size_t current_len, const uint8_t *password,
                                        size_t len) {
  Outcome o = this->access_.change_web_password(current, current_len, password, len);
  if (o.accepted())
    ESP_LOGI(TAG, "Web password %s; every browser is signed out", len == 0 ? "removed" : "changed");
  return o;
}

void BoardServices::wifi_status(WifiStatus &out) const {
  out = WifiStatus{};
#ifdef USE_WIFI
  wifi::WiFiComponent *w = wifi::global_wifi_component;
  if (w == nullptr || w->is_disabled())
    return;
  if (!w->is_connected()) {
    out.state = WifiState::CONNECTING;
    return;
  }
  out.state = WifiState::CONNECTED;
  out.rssi = w->wifi_rssi();
  char ssid[wifi::SSID_BUFFER_SIZE] = {};
  w->wifi_ssid_to(ssid);
  strncpy(out.ssid, ssid, SSID_MAX);
  for (auto &ip : w->wifi_sta_ip_addresses()) {
    if (!ip.is_ip4())
      continue;
    char buf[network::IP_ADDRESS_BUFFER_SIZE];
    unsigned a, b, c, d;
    if (sscanf(ip.str_to(buf), "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
      out.ip[0] = uint8_t(a), out.ip[1] = uint8_t(b), out.ip[2] = uint8_t(c), out.ip[3] = uint8_t(d);
      break;
    }
  }
#endif
}

Outcome BoardServices::wifi_set(const uint8_t *ssid, size_t ssid_len, const uint8_t *password, size_t len) {
#ifdef USE_WIFI
  wifi::WiFiComponent *w = wifi::global_wifi_component;
  if (w == nullptr)
    return Outcome::refuse(Result::NOT_SUPPORTED, "this board has no Wi-Fi");
  if (this->wifi_changing_)
    return Outcome::refuse(Result::BUSY, "a Wi-Fi change is still being tried");
  // Keep the old settings until the new network connects, in RAM and in flash (a restart during the
  // trial goes back to them).
  wifi::WiFiAP old = w->get_sta();
  this->old_ssid_ = std::string(old.get_ssid().c_str());
  this->old_password_ = std::string(old.get_password().c_str());
  this->old_mode_ = this->wifi_mode_;
  this->old_enabled_ = !w->is_disabled();
  WifiFallbackPref fb{};
  strncpy(fb.ssid, this->old_ssid_.c_str(), sizeof(fb.ssid) - 1);
  strncpy(fb.password, this->old_password_.c_str(), sizeof(fb.password) - 1);
  fb.mode = this->old_mode_;
  fb.enabled = this->old_enabled_ ? 1 : 0;
  fb.check = WIFI_FALLBACK_CHECK;
  this->wifi_fallback_pref_.save(&fb);
  memset(&fb, 0, sizeof(fb));
  this->new_ssid_.assign(reinterpret_cast<const char *>(ssid), ssid_len);
  std::string pw(reinterpret_cast<const char *>(password), len);
  w->save_wifi_sta(this->new_ssid_, pw);  // syncs the preferences (the fallback too)
  pw.assign(pw.size(), '\0');
  // A connected board only takes new settings on its next connection: drop the current one, half a
  // second from now so this command's result still reaches an app on Wi-Fi. Success then needs the
  // link to go down and come back up on the new network.
  this->wifi_reconnect_at_ms_ = millis() + 500;
  this->wifi_reconnect_pending_ = true;
  this->wifi_saw_down_ = false;
  this->wifi_changing_ = true;
  this->wifi_change_ms_ = millis();
  ESP_LOGI(TAG, "Trying the Wi-Fi network '%s' from an app", this->new_ssid_.c_str());
  return Outcome::ok();
#else
  return Outcome::refuse(Result::NOT_SUPPORTED, "this board has no Wi-Fi");
#endif
}

void BoardServices::wifi_change_done_(bool ok, const char *why) {
  this->wifi_changing_ = false;
  uint8_t out[ble::RESULT_MAX];
  size_t n;
#ifdef USE_WIFI
  wifi::WiFiComponent *w = wifi::global_wifi_component;
  WifiFallbackPref none{};
  this->wifi_fallback_pref_.save(&none);  // confirmed or undone: nothing to go back to after a restart
  if (ok) {
    this->wifi_mode_ = 1;
    this->wifi_pref_.save(&this->wifi_mode_);
    global_preferences->sync();
    this->old_password_.assign(this->old_password_.size(), '\0');
    this->old_password_.clear();
    WifiStatus st;
    this->wifi_status(st);
    n = ble::encode_wifi_event(&st, nullptr, out, sizeof(out));
    ESP_LOGI(TAG, "Connected to the new Wi-Fi network");
  } else {
    this->wifi_restore_old_();
    n = ble::encode_wifi_event(nullptr, why, out, sizeof(out));
    ESP_LOGW(TAG, "Wi-Fi change failed: %s", why);
  }
#else
  (void) ok;
  n = ble::encode_wifi_event(nullptr, why, out, sizeof(out));
#endif
  this->broadcast_(out, n);
}

// Back to how it was before an app's Wi-Fi change: the old network (or none), on or off as before.
void BoardServices::wifi_restore_old_() {
#ifdef USE_WIFI
  wifi::WiFiComponent *w = wifi::global_wifi_component;
  bool had_network = !this->old_ssid_.empty() && this->old_ssid_ != WIFI_PLACEHOLDER;
  w->save_wifi_sta(had_network ? this->old_ssid_ : std::string(), had_network ? this->old_password_ : std::string());
  w->disable();
  if (this->old_enabled_)
    w->enable();  // reconnects with the old settings
  this->wifi_mode_ = this->old_mode_;
  this->wifi_pref_.save(&this->wifi_mode_);
  WifiFallbackPref none{};
  this->wifi_fallback_pref_.save(&none);
  global_preferences->sync();
  this->old_password_.assign(this->old_password_.size(), '\0');
  this->old_password_.clear();
#endif
}

Outcome BoardServices::wifi_off() {
#ifdef USE_WIFI
  wifi::WiFiComponent *w = wifi::global_wifi_component;
  if (w == nullptr)
    return Outcome::refuse(Result::NOT_SUPPORTED, "this board has no Wi-Fi");
  this->wifi_changing_ = false;
  this->wifi_reconnect_pending_ = false;
  WifiFallbackPref none{};
  this->wifi_fallback_pref_.save(&none);
  w->save_wifi_sta("", "");
  w->disable();
  this->wifi_mode_ = 2;
  this->wifi_pref_.save(&this->wifi_mode_);
  global_preferences->sync();
  ESP_LOGW(TAG, "Wi-Fi switched off from an app");
  return Outcome::ok();
#else
  return Outcome::refuse(Result::NOT_SUPPORTED, "this board has no Wi-Fi");
#endif
}

HaState BoardServices::ha_state() const {
  if (!this->ha_on_)
    return HaState::OFF;
#ifdef USE_API
  if (api::global_api_server != nullptr && api::global_api_server->is_connected())
    return HaState::CONNECTED;
#endif
  return HaState::WAITING;
}

bool BoardServices::ha_key_set() const {
#ifdef FK_HA_KEYS
  noise::psk_t psk;
  bool set = load_api_key(psk);
  crypto::wipe(psk.data(), psk.size());
  return set;
#else
  return false;
#endif
}

Outcome BoardServices::ha_set(bool on) {
  if (on == this->ha_on_)
    return Outcome::ok();
#ifdef FK_HA_KEYS
  if (on && !this->ha_key_set()) {
    uint8_t key[HA_KEY_SIZE];
    Outcome o = this->ha_key_new(key);
    crypto::wipe(key, sizeof(key));
    if (o.result != Result::OK)
      return o;
  }
#endif
  uint8_t v = on ? 1 : 0;
  if (!this->ha_pref_.save(&v) || !global_preferences->sync())
    return Outcome::refuse(Result::REFUSED, "the setting could not be saved");
  this->restart_soon_(on ? "Home Assistant switched on" : "Home Assistant switched off");
  return Outcome::ok();
}

Outcome BoardServices::ha_key(uint8_t out[HA_KEY_SIZE]) {
#ifdef FK_HA_KEYS
  noise::psk_t psk;
  if (!load_api_key(psk))
    return this->ha_key_new(out);
  memcpy(out, psk.data(), HA_KEY_SIZE);
  crypto::wipe(psk.data(), psk.size());
  return Outcome::ok();
#else
  (void) out;
  return Outcome::refuse(Result::NOT_SUPPORTED, "this firmware has no Home Assistant encryption");
#endif
}

Outcome BoardServices::ha_key_new(uint8_t out[HA_KEY_SIZE]) {
#ifdef FK_HA_KEYS
  if (api::global_api_server == nullptr)
    return Outcome::refuse(Result::NOT_SUPPORTED, "this firmware has no Home Assistant API");
  noise::psk_t psk;
  esp_fill_random(psk.data(), psk.size());
  if (!api::global_api_server->save_noise_psk(psk, true)) {
    crypto::wipe(psk.data(), psk.size());
    return Outcome::refuse(Result::REFUSED, "the key could not be saved");
  }
  memcpy(out, psk.data(), HA_KEY_SIZE);
  crypto::wipe(psk.data(), psk.size());
  ESP_LOGI(TAG, "New Home Assistant key from an app");
  return Outcome::ok();
#else
  (void) out;
  return Outcome::refuse(Result::NOT_SUPPORTED, "this firmware has no Home Assistant encryption");
#endif
}

}  // namespace flamekeeper
}  // namespace esphome

#endif  // USE_ESP32
