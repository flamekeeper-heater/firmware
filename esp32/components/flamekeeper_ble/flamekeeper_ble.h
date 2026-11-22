// The Flamekeeper BLE service (PROTOCOL.md) on ESP-IDF's Bluedroid, through
// ESPHome's esp32_ble event queue: every GAP and GATT event is handled in the main loop, like any
// other entity change. The records and commands themselves are in flamekeeper/ble_protocol.*, the
// pairing rules in flamekeeper/pairing.*.
//
// Its own GATT application and attribute table, so the stack itself enforces the permissions:
// everything but Info needs an encrypted link authenticated with the board's PIN (LE Secure
// Connections, MITM), and a new phone can only pair while the pairing window is open. The PIN, the
// Wi-Fi keys and the board settings come from the hub's BoardServices, shared with the Wi-Fi
// transport (flamekeeper_net).
#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include <esp_gap_ble_api.h>
#include <esp_gatts_api.h>

#include <vector>

#include "esphome/components/flamekeeper/ble_protocol.h"
#include "esphome/components/flamekeeper/flamekeeper.h"
#include "esphome/components/flamekeeper/pairing.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_BUTTON
#include "esphome/components/button/button.h"
#endif

namespace esphome {
namespace flamekeeper_ble {

namespace fk = ::esphome::flamekeeper;

// Attribute table indexes.
enum Attr : uint8_t {
  A_SERVICE,
  A_INFO_DECL,
  A_INFO,
  A_STATE_DECL,
  A_STATE,
  A_STATE_CCCD,
  A_TELEMETRY_DECL,
  A_TELEMETRY,
  A_TELEMETRY_CCCD,
  A_COMMAND_DECL,
  A_COMMAND,
  A_RESULT_DECL,
  A_RESULT,
  A_RESULT_CCCD,
  A_UPDATE_DECL,
  A_UPDATE,
  A_UPDATE_CCCD,
  A_COUNT,
};

class FlamekeeperBLE : public Component {
 public:
  void set_hub(fk::FlamekeeperHub *hub) { this->hub_ = hub; }
  void set_max_bonds(uint8_t n) { this->max_bonds_ = n; }
  void set_window_ms(uint32_t ms) { this->window_ms_ = ms; }
  void set_open_on_power_up(bool open) { this->open_on_power_up_ = open; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }

  void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
  void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);

  // Someone at the heater or in Home Assistant lets a new phone pair for the next two minutes.
  void open_pairing_window();
  // Every phone has to pair again (factory reset).
  void forget_all_bonds();

#ifdef USE_SENSOR
  void set_bonds_sensor(sensor::Sensor *s) { this->bonds_sensor_ = s; }
  void set_window_sensor(sensor::Sensor *s) { this->window_sensor_ = s; }
#endif
#ifdef USE_BINARY_SENSOR
  void set_pairing_binary_sensor(binary_sensor::BinarySensor *s) { this->pairing_sensor_ = s; }
#endif

 protected:
  struct Conn {
    uint16_t id;
    esp_bd_addr_t bda;
    esp_bd_addr_t identity;  // the bonded phone's address from pairing (owner of its Wi-Fi key)
    uint32_t since_ms;
    bool authenticated{false};
    bool state_sub{false};
    bool telemetry_sub{false};
    bool result_sub{false};
    bool update_sub{false};  // board firmware update progress (1006)
    uint16_t mtu{23};
    bool indicating{false};
    bool forgetting{false};  // FORGET_ME: bond removed and link closed once its result has gone out
    uint32_t forget_ms{0};
    std::vector<std::vector<uint8_t>> pending;  // results / events waiting for the indication slot
  };

  void start_stack_();
  void build_table_();
  Conn *conn_(uint16_t id);
  void on_write_(esp_ble_gatts_cb_param_t *param);
  void queue_result_(Conn &c, const uint8_t *data, size_t len);
  void pump_indications_(Conn &c);
  void broadcast_event_(fk::Event e, fk::Result r, uint16_t id, const char *text);
  void update_values_(uint32_t now);
  void notify_(uint8_t attr, const uint8_t *data, size_t len, bool Conn::*sub);
  void refresh_bonds_();
  void publish_entities_(uint32_t now);
  void set_passkey_(uint32_t pin);
  fk::Outcome forget_me_(Conn &c);
  void finish_forget_(Conn &c);
  void remove_bond_(const uint8_t addr[6]);
  fk::Outcome issue_wifi_key_(Conn &c, uint8_t id[fk::KEY_ID_SIZE], uint8_t secret[fk::KEY_SECRET_SIZE]);
  friend class ConnLink;

  fk::FlamekeeperHub *hub_{nullptr};
  uint8_t max_bonds_{8};
  uint32_t window_ms_{fk::PairingWindow::DEFAULT_MS};
  bool open_on_power_up_{true};

  fk::PairingWindow window_;
  ESPPreferenceObject failures_pref_;
  uint8_t bonds_{0};
  uint8_t bonds_before_pairing_{0};

  bool stack_started_{false};
  esp_gatt_if_t gatts_if_{ESP_GATT_IF_NONE};
  uint16_t handles_[A_COUNT]{};
  bool table_ready_{false};
  std::vector<Conn> conns_;

  // attribute table storage (the stack keeps pointers to some of it)
  esp_gatts_attr_db_t table_[A_COUNT]{};
  uint8_t uuid_service_[16]{}, uuid_info_[16]{}, uuid_state_[16]{}, uuid_telemetry_[16]{}, uuid_command_[16]{},
      uuid_result_[16]{}, uuid_update_[16]{};
  uint8_t info_[fk::ble::INFO_SIZE]{};
  uint8_t state_[fk::ble::STATE_SIZE]{};
  uint8_t telemetry_[fk::ble::TELEMETRY_SIZE]{};
  uint8_t last_state_[fk::ble::STATE_SIZE]{};
  uint8_t last_telemetry_[fk::ble::TELEMETRY_SIZE]{};
  uint8_t last_info_[fk::ble::INFO_SIZE]{};
  uint32_t last_state_ms_{0};
  uint32_t last_telemetry_ms_{0};
  uint32_t last_info_ms_{0};
  uint32_t last_bonds_ms_{0};
  uint32_t last_entities_ms_{0};

#ifdef USE_SENSOR
  sensor::Sensor *bonds_sensor_{nullptr};
  sensor::Sensor *window_sensor_{nullptr};
#endif
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *pairing_sensor_{nullptr};
#endif
};

#ifdef USE_BUTTON
// "Allow Bluetooth pairing". Forgetting every phone is the board's factory reset.
class FlamekeeperBLEButton : public button::Button, public Component {
 public:
  void set_parent(FlamekeeperBLE *parent) { this->parent_ = parent; }

 protected:
  void press_action() override { this->parent_->open_pairing_window(); }
  FlamekeeperBLE *parent_{nullptr};
};
#endif

}  // namespace flamekeeper_ble
}  // namespace esphome

#endif  // USE_ESP32
