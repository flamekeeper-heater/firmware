// The board's own settings behind the protocol (PROTOCOL.md), on ESPHome: the PIN, the Wi-Fi
// keys and the web password (flamekeeper/access.*), Wi-Fi set up from the app, Home Assistant on
// demand, and the factory reset. Both transports (Bluetooth, the Wi-Fi WebSocket) use the one
// instance the hub owns.
#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include <functional>
#include <string>
#include <vector>

#include "access.h"
#include "board.h"
#include "board_update.h"
#include "push.h"
#include "relay_client.h"
#include "controller.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"

#include <memory>
#ifdef USE_OTA_SIGNED_VERIFICATION

#include "esphome/components/ota/ota_backend_factory.h"
#endif

namespace esphome {
namespace flamekeeper {

enum class PinSource : uint8_t { PIN_RANDOM, PIN_FIXED, PIN_EFUSE };  // prefixed: CONFIGURED is a build macro
enum class HaMode : uint8_t { HA_ALWAYS_ON, HA_FROM_APP };

#ifdef USE_OTA_SIGNED_VERIFICATION
// The other app slot, through ESPHome's OTA backend: it erases as it writes, checks the signature
// and the image at the end (esp32: signed_ota_verification) and selects the new build.
class EspUpdateFlash : public UpdateFlash {
 public:
  uint32_t slot_size() const override;
  bool begin(uint32_t size) override;
  bool write(const uint8_t *data, size_t len) override;
  End end() override;
  void abort() override;
  void restart() override;

 protected:
  ota::OTABackendPtr backend_;
};
#endif

class BoardServices : public BoardSettings {
 public:
  explicit BoardServices(Controller *controller);

  // --- configuration (from the hub's YAML) ---
  void set_board(uint8_t board) { this->board_ = board; }
  uint8_t board() const { return this->board_; }
  void set_fixed_pin(uint32_t pin) {
    this->configured_pin_ = pin;
    this->pin_source_ = PinSource::PIN_FIXED;
  }
  void set_pin_efuse_key(uint8_t key) {
    this->efuse_key_ = key;
    this->pin_source_ = PinSource::PIN_EFUSE;
  }
  void set_ha_mode(HaMode mode) { this->ha_mode_ = mode; }

  // Called from the hub's setup (before the API's) and loop.
  void setup();
  void loop(uint32_t now);
  void dump_config();

  // --- for the transports ---
  Access &access() { return this->access_; }
  uint32_t pin() const { return this->user_pin_valid_ ? this->user_pin_ : this->label_pin_; }
  void on_pin_change(std::function<void(uint32_t)> &&cb) { this->pin_cbs_.push_back(std::move(cb)); }
  // Bluetooth fills in the pairing fields of Info.
  void set_pairing_info(std::function<void(BoardInfo &)> &&fill) { this->pairing_info_ = std::move(fill); }
  // Bluetooth's own service has started, or will not: at boot the relay's first connection waits
  // for it (its TLS handshake takes most of the free heap for a moment).
  void set_bluetooth_settled() { this->bluetooth_settled_ = true; }
  bool bluetooth_settled() const { return this->bluetooth_settled_; }
  void info(BoardInfo &out) const;
  // Records for every connected app (event 05); each transport registers one.
  void on_record(std::function<void(const uint8_t *, size_t)> &&cb) { this->record_cbs_.push_back(std::move(cb)); }
  // Bluetooth registers how to forget every bond (factory reset).
  void on_forget_bonds(std::function<void()> &&cb) { this->forget_bonds_ = std::move(cb); }
  // ... and how to forget one phone's bond (FORGET_ME over Wi-Fi).
  void on_forget_bond(std::function<void(const uint8_t addr[6])> &&cb) { this->forget_bond_ = std::move(cb); }
  void forget_bond(const uint8_t addr[6]) {
    if (this->forget_bond_)
      this->forget_bond_(addr);
  }
  // Something changed that the transports' Info should show soon.

  // BOOT held 10 s: settings and access back to as shipped, then a restart.
  void factory_reset();

  // --- BoardSettings ---
  Outcome set_pin(uint32_t pin) override;
  Outcome set_web_password(const uint8_t *current, size_t current_len, const uint8_t *password,
                           size_t len) override;
  void wifi_status(WifiStatus &out) const override;
  Outcome wifi_set(const uint8_t *ssid, size_t ssid_len, const uint8_t *password, size_t len) override;
  Outcome wifi_off() override;
  bool ha_switchable() const override { return this->ha_mode_ == HaMode::HA_FROM_APP; }
  HaState ha_state() const override;
  bool ha_key_set() const override;
  Outcome ha_set(bool on) override;
  Outcome ha_key(uint8_t out[HA_KEY_SIZE]) override;
  Outcome ha_key_new(uint8_t out[HA_KEY_SIZE]) override;
  BoardUpdater *updater() override { return this->board_update(); }
  RelayControl *relay() override { return this->relay_.get(); }
  PushControl *push() override { return this->push_.get(); }
  // The push engine (an app signed in, a Bluetooth phone left), or nullptr.
  PushEngine *push_engine() { return this->push_.get(); }
  // The internet relay's connection logic, for the Wi-Fi transport that carries it.
  RelayClient *relay_client() { return this->relay_.get(); }
  // For the transports' data records; nullptr when this build takes no updates from the app.
  BoardUpdate *board_update() {
#ifdef USE_OTA_SIGNED_VERIFICATION
    return this->update_.get();
#else
    return nullptr;
#endif
  }

 protected:
  void load_pin_();
  void save_access_();
  void broadcast_(const uint8_t *data, size_t len);
  void wifi_change_done_(bool ok, const char *why);
  void wifi_restore_old_();
  void restart_soon_(const char *why);

  Controller *controller_;
  uint8_t board_{2};
  Access access_;

  PinSource pin_source_{PinSource::PIN_RANDOM};
  uint32_t configured_pin_{0};
  uint8_t efuse_key_{0};
  uint32_t label_pin_{0};
  uint32_t user_pin_{0};
  bool user_pin_valid_{false};

  HaMode ha_mode_{HaMode::HA_ALWAYS_ON};
  bool ha_on_{true};

  // Wi-Fi: 0 as configured (YAML), 1 on with the app's network, 2 switched off from the app.
  uint8_t wifi_mode_{0};
  bool wifi_changing_{false};
  bool wifi_saw_down_{false};       // the old connection went down during a change
  bool wifi_reconnect_pending_{false};
  uint32_t wifi_reconnect_at_ms_{0};
  bool wifi_fallback_checked_{false};
  uint32_t wifi_change_ms_{0};
  std::string old_ssid_, old_password_;
  uint8_t old_mode_{0};
  bool old_enabled_{false};
  std::string new_ssid_;

  ESPPreferenceObject keys_pref_, web_pref_, pin_pref_, random_pin_pref_, wifi_pref_, ha_pref_, wifi_fallback_pref_,
      params_retry_pref_;
  // The heater's parameter reset (factory reset): asked, answered, and still owed after a restart.
  bool params_asked_{false};
  bool params_answered_{false};
  bool params_reset_ok_{false};
  bool params_retry_{false};
  uint32_t params_deadline_ms_{0};
  uint32_t params_next_ms_{0};
  std::vector<std::function<void(uint32_t)>> pin_cbs_;
  std::vector<std::function<void(const uint8_t *, size_t)>> record_cbs_;
  std::function<void(BoardInfo &)> pairing_info_;
  std::function<void()> forget_bonds_;
  std::function<void(const uint8_t addr[6])> forget_bond_;
  bool resetting_{false};
  bool bluetooth_settled_{false};
  std::unique_ptr<RelayClient> relay_;
  ESPPreferenceObject relay_pref_;
  std::unique_ptr<PushEngine> push_;
  ESPPreferenceObject push_pref_;
#ifdef USE_OTA_SIGNED_VERIFICATION
  EspUpdateFlash update_flash_;
  std::unique_ptr<BoardUpdate> update_;
  ESPPreferenceObject update_pref_;
#endif
};

}  // namespace flamekeeper
}  // namespace esphome

#endif  // USE_ESP32
