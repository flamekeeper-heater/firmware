// Bluetooth thermometers into the hub (see __init__.py): reads each advert the BLE tracker hands
// over, raw and in place, and passes the readings to the automation's thermometer list.
#pragma once

#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/components/flamekeeper/flamekeeper.h"
#include "esphome/core/component.h"

namespace esphome {
namespace flamekeeper_thermo {

class ThermoListener : public Component {
 public:
  void set_hub(flamekeeper::FlamekeeperHub *hub) { this->hub_ = hub; }
  void set_tracker(esp32_ble_tracker::ESP32BLETracker *tracker) { this->tracker_ = tracker; }
  void setup() override;
  void dump_config() override;
  void loop() override;
  // Scan windows in 0.625 ms units: the usual one, and the wider one while an app searches.
  void set_windows(uint32_t normal, uint32_t search) {
    this->normal_window_ = normal;
    this->search_window_ = search;
  }

 protected:
  static void on_advert_(void *self, const ble_device_base::RawAdvertisement &adv);
  void advert_(const ble_device_base::RawAdvertisement &adv);

  flamekeeper::FlamekeeperHub *hub_{nullptr};
  esp32_ble_tracker::ESP32BLETracker *tracker_{nullptr};
  uint32_t normal_window_{0};
  uint32_t search_window_{0};
  bool searching_{false};
};

}  // namespace flamekeeper_thermo
}  // namespace esphome
