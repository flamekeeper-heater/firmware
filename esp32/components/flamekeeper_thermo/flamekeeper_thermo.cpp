#include "flamekeeper_thermo.h"

#include <cstring>

#include "esphome/components/flamekeeper/ble_thermo.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace flamekeeper_thermo {

static const char *const TAG = "flamekeeper_thermo";
namespace fk = flamekeeper;
using ble_device_base::ESPBTUUID;

void ThermoListener::dump_config() {
  ESP_LOGCONFIG(TAG, "Bluetooth thermometers: BTHome v2, Xiaomi (custom and unencrypted stock firmware), Govee, "
                "SwitchBot, RuuviTag, Qingping");
}

// While an app searches (SENSOR_SCAN, 5-60 s), listen about a third of the time instead of a tenth,
// so a thermometer that advertises every few seconds is heard within the search; then back to the
// light duty that leaves Wi-Fi and the phones' Bluetooth their airtime. The tracker applies a new
// window when its scan restarts, so the scan is stopped and its own loop starts it again.
void ThermoListener::loop() {
  if (this->parent_ == nullptr || this->search_window_ == 0)
    return;
  const bool searching = this->hub_->automation().sensors().scanning(millis());
  if (searching == this->searching_)
    return;
  this->searching_ = searching;
  this->parent_->set_scan_window(searching ? this->search_window_ : this->normal_window_);
  this->parent_->stop_scan();                 // also clears "continuous"...
  this->parent_->set_scan_continuous(true);   // ...so the tracker starts it again once stopped
  ESP_LOGD(TAG, "Thermometer search %s: scan window %.0f ms", searching ? "on" : "off",
           (searching ? this->search_window_ : this->normal_window_) * 0.625f);
}

bool ThermoListener::parse_device(const ble_device_base::ESPBTDevice &device) {
  const uint32_t now = millis();
  fk::RoomSensors &rs = this->hub_->automation().sensors();
  if (!rs.listening(now))
    return false;

  fk::ThermoReading reading;
  bool ok = false;
  // SwitchBot: the service data names the model, the manufacturer data may carry the reading.
  const uint8_t *switchbot = nullptr;
  size_t switchbot_len = 0;
  for (const auto &sd : device.get_service_datas()) {
    if (sd.uuid.type() != ESPBTUUID::Type::UUID16)
      continue;
    const uint16_t uuid = sd.uuid.uuid16();
    if (uuid == 0xFD3D || uuid == 0x0D00) {
      switchbot = sd.data.data();
      switchbot_len = sd.data.size();
    }
    if (!ok)
      ok = fk::parse_thermo_service_data(uuid, sd.data.data(), sd.data.size(), reading);
  }
  uint8_t buf[64];
  for (const auto &md : device.get_manufacturer_datas()) {
    if (md.uuid.type() != ESPBTUUID::Type::UUID16 || md.data.size() + 2 > sizeof(buf))
      continue;
    const uint16_t company = md.uuid.uuid16();
    buf[0] = uint8_t(company);  // as on air: the company id first
    buf[1] = uint8_t(company >> 8);
    std::memcpy(buf + 2, md.data.data(), md.data.size());
    const size_t len = md.data.size() + 2;
    if (company == 0x0969 && switchbot != nullptr) {
      fk::ThermoReading s;
      if (fk::parse_switchbot_meter(switchbot, switchbot_len, buf, len, s)) {
        reading = s;
        ok = true;
      }
    } else if (!ok) {
      ok = fk::parse_thermo_manufacturer_data(buf, len, reading);
    }
  }
  if (!ok)
    return false;
  char name[21];
  const auto n = device.get_name();
  const size_t name_len = n.size() < sizeof(name) - 1 ? n.size() : sizeof(name) - 1;
  std::memcpy(name, n.c_str(), name_len);
  name[name_len] = '\0';
  if (rs.is_chosen(device.address())) {
    ESP_LOGV(TAG, "Room thermometer: %.2f C", reading.temperature_c);
  }
  rs.advert(device.address(), int8_t(device.get_rssi()), name, reading, now);
  return true;
}

}  // namespace flamekeeper_thermo
}  // namespace esphome
