#include "flamekeeper_thermo.h"

#include <cstring>

#include "esphome/components/flamekeeper/ble_thermo.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace flamekeeper_thermo {

static const char *const TAG = "flamekeeper_thermo";
namespace fk = flamekeeper;

void ThermoListener::dump_config() {
  ESP_LOGCONFIG(TAG, "Bluetooth thermometers: BTHome v2, Xiaomi (custom and unencrypted stock firmware), Govee, "
                "SwitchBot, RuuviTag, Qingping");
}

// While an app searches (SENSOR_SCAN, 5-60 s), listen about a third of the time instead of a tenth,
// so a thermometer that advertises every few seconds is heard within the search; then back to the
// light duty that leaves Wi-Fi and the phones' Bluetooth their airtime. The tracker applies a new
// window when its scan restarts, so the scan is stopped and its own loop starts it again.
void ThermoListener::loop() {
  if (this->search_window_ == 0)
    return;
  const bool searching = this->hub_->automation().sensors().scanning(millis());
  if (searching == this->searching_)
    return;
  this->searching_ = searching;
  this->tracker_->set_scan_window(searching ? this->search_window_ : this->normal_window_);
  this->tracker_->stop_scan();                 // also clears "continuous"...
  this->tracker_->set_scan_continuous(true);   // ...so the tracker starts it again once stopped
  ESP_LOGD(TAG, "Thermometer search %s: scan window %.0f ms", searching ? "on" : "off",
           (searching ? this->search_window_ : this->normal_window_) * 0.625f);
}

// The raw adverts, not the tracker's parsed devices: those are built for every advert of every
// device in range, with a few heap blocks each, which churned the heap all day.
void ThermoListener::setup() { this->tracker_->set_raw_advertisement_callback({this, &ThermoListener::on_advert_}); }

void ThermoListener::on_advert_(void *self, const ble_device_base::RawAdvertisement &adv) {
  static_cast<ThermoListener *>(self)->advert_(adv);
}

void ThermoListener::advert_(const ble_device_base::RawAdvertisement &adv) {
  const uint32_t now = millis();
  fk::RoomSensors &rs = this->hub_->automation().sensors();
  if (!rs.listening(now))
    return;
  fk::ThermoReading reading;
  char name[21];
  if (!fk::parse_thermo_advert(adv.data, adv.data_len, reading, name, sizeof(name)))
    return;
  uint8_t address[6];
  ble_device_base::uint64_to_mac_msb_first(adv.address, address);
  if (rs.is_chosen(address)) {
    ESP_LOGV(TAG, "Room thermometer: %.2f C", reading.temperature_c);
  }
  rs.advert(address, adv.rssi, name, reading, now);
}

}  // namespace flamekeeper_thermo
}  // namespace esphome
