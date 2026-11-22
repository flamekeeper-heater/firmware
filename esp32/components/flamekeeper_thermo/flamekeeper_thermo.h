// Bluetooth thermometers into the hub (see __init__.py): decodes each advert the BLE tracker hands
// over and passes the readings to the automation's thermometer list.
#pragma once

#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/components/flamekeeper/flamekeeper.h"
#include "esphome/core/component.h"

namespace esphome {
namespace flamekeeper_thermo {

class ThermoListener : public esp32_ble_tracker::ESPBTDeviceListener, public Component {
 public:
  void set_hub(flamekeeper::FlamekeeperHub *hub) { this->hub_ = hub; }
  void dump_config() override;
  bool parse_device(const ble_device_base::ESPBTDevice &device) override;
  void loop() override;
  // Scan windows in 0.625 ms units: the usual one, and the wider one while an app searches.
  void set_windows(uint32_t normal, uint32_t search) {
    this->normal_window_ = normal;
    this->search_window_ = search;
  }

 protected:
  flamekeeper::FlamekeeperHub *hub_{nullptr};
  uint32_t normal_window_{0};
  uint32_t search_window_{0};
  bool searching_{false};
};

}  // namespace flamekeeper_thermo
}  // namespace esphome
