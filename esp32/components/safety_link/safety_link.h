// The heater backend of the Flamekeeper board: the link to the STM32 safety MCU over UART1
// (DESIGN.md). The protocol is flamekeeper/link_client.* on the shared C codec; this
// file only moves its bytes over the UART and runs it from ESPHome's loop.
//
// It never touches STM_NRST or STM_BOOT: only the safety-MCU updater may drive them, and only after
// UPDATE_PREPARE. The UART is 115200 8N1 (flamekeeper-rev-a2.yaml).
#pragma once

#include "esphome/components/flamekeeper/link_client.h"
#include "esphome/components/uart/uart.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace safety_link {

namespace fk = ::esphome::flamekeeper;

class SafetyLink : public Component, public uart::UARTDevice, public fk::LinkTransport, public fk::LinkClient {
 public:
  SafetyLink() : fk::LinkClient(this) {}

  void loop() override { fk::LinkClient::loop(millis()); }
  float get_setup_priority() const override { return setup_priority::DATA; }
  void dump_config() override {
    ESP_LOGCONFIG("safety_link", "Safety MCU link (protocol %u.%u) on UART", LINK_MAJOR, LINK_MINOR);
    if (this->up())
      ESP_LOGCONFIG("safety_link", "  MCU: %s", this->info().text);
    else
      ESP_LOGCONFIG("safety_link", "  MCU: %s", this->incompatible() ? "unknown link version" : "not answering");
  }

  // LinkTransport
  void send(const uint8_t *data, size_t len) override { this->write_array(data, len); }
  int read() override {
    uint8_t c;
    if (this->available() == 0 || !this->read_byte(&c))
      return -1;
    return c;
  }
};

}  // namespace safety_link
}  // namespace esphome
