// The heater backend of the Flamekeeper board: the link to the STM32 safety MCU over UART1
// (DESIGN.md). The protocol is flamekeeper/link_client.* on the shared C codec; this
// file only moves its bytes over the UART and runs it from ESPHome's loop.
//
// It never touches STM_NRST or STM_BOOT itself: only the safety-MCU updater (flamekeeper/mcu_update.*)
// drives them, through the calls below, and only after UPDATE_PREPARE. Until then both are inputs
// with no pull, as if not configured. The UART is 115200 8N1 (flamekeeper-rev-a2.yaml), 8E1 while
// the updater talks to the ROM bootloader.
#pragma once

#include <cstring>

#include "esphome/components/flamekeeper/link_client.h"
#include "esphome/components/flamekeeper/mcu_update.h"
#include "esphome/components/uart/uart.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/preferences.h"

namespace esphome {
namespace safety_link {

namespace fk = ::esphome::flamekeeper;

static const char *const TAG = "safety_link";

class SafetyLink : public Component, public uart::UARTDevice, public fk::BootPort, public fk::LinkClient {
 public:
  SafetyLink() : fk::LinkClient(this), updater_(this) {}

  void set_reset_pin(InternalGPIOPin *pin) { this->reset_pin_ = pin; }
  void set_boot_pin(InternalGPIOPin *pin) { this->boot_pin_ = pin; }
  void set_mcu_image(const uint8_t *data, uint32_t size, uint8_t major, uint8_t minor, uint8_t patch) {
    const uint8_t version[3] = {major, minor, patch};
    this->updater_.set_image(data, size, version);
  }
  // For a YAML lambda at bring-up: the option bytes (WDG-1). The write is UNVERIFIED (WDG-13).
  fk::Outcome read_option_bytes() { return this->start_mcu_job(fk::McuJob::READ_OPTIONS); }
  fk::Outcome write_option_bytes() { return this->start_mcu_job(fk::McuJob::WRITE_OPTIONS); }

  void setup() override {
    if (this->reset_pin_ == nullptr || this->boot_pin_ == nullptr)
      return;  // no updater on this board
    this->reset_pin_->setup();  // inputs with no pull: released
    this->boot_pin_->setup();
    // An update that erased the MCU's flash and was not confirmed, kept over restarts.
    this->pref_ = global_preferences->make_preference<uint8_t>(fnv1_hash("safety_link_mcu_update"));
    uint8_t pending = 0;
    if (this->pref_.load(&pending))
      this->updater_.load(pending != 0);
    this->updater_.on_save([this](bool p) {
      uint8_t v = p ? 1 : 0;
      this->pref_.save(&v);
      global_preferences->sync();  // a power loss is just what it is for
    });
    this->set_mcu_updater(&this->updater_);
  }
  void loop() override {
    fk::LinkClient::loop(millis());
    // The bootloader exchange goes faster with the loop at full speed (start() and stop() only
    // count a change).
    if (this->updater_.running())
      this->high_freq_.start();
    else
      this->high_freq_.stop();
    const char *text = this->updater_.text();
    if (std::strcmp(text, this->logged_) != 0 && std::strncmp(text, "Writing", 7) != 0) {
      ESP_LOGI(TAG, "Heater firmware update: %s", text);
      std::strncpy(this->logged_, text, sizeof(this->logged_) - 1);
    }
  }
  float get_setup_priority() const override { return setup_priority::DATA; }
  void dump_config() override {
    ESP_LOGCONFIG(TAG, "Safety MCU link (protocol %u.%u) on UART", LINK_MAJOR, LINK_MINOR);
    if (this->up())
      ESP_LOGCONFIG(TAG, "  MCU: %s", this->info().text);
    else
      ESP_LOGCONFIG(TAG, "  MCU: %s", this->incompatible() ? "unknown link version" : "not answering");
    if (this->reset_pin_ != nullptr) {
      LOG_PIN("  Reset pin: ", this->reset_pin_);
      LOG_PIN("  Boot pin: ", this->boot_pin_);
      ESP_LOGCONFIG(TAG, "  Heater firmware update: %s", this->firmware_update_text());
    }
  }

  // LinkTransport
  void send(const uint8_t *data, size_t len) override { this->write_array(data, len); }
  int read() override {
    uint8_t c;
    if (this->available() == 0 || !this->read_byte(&c))
      return -1;
    return c;
  }

  // BootPort: only the updater calls these.
  void bootloader_framing(bool on) override {
    this->parent_->set_parity(on ? uart::UART_CONFIG_PARITY_EVEN : uart::UART_CONFIG_PARITY_NONE);
#if defined(USE_ESP32) || defined(USE_ESP8266)
    this->parent_->load_settings(false);  // reinstalls the driver: whatever was in flight is lost
#endif
  }
  void drive_boot(bool high) override {
    if (high) {
      this->boot_pin_->digital_write(true);
      this->boot_pin_->pin_mode(gpio::FLAG_OUTPUT);
    } else {
      this->boot_pin_->pin_mode(gpio::FLAG_INPUT);  // the MCU's pull-down holds BOOT0 low
    }
  }
  void drive_reset(bool low) override {
    if (low) {
      this->reset_pin_->digital_write(false);
      this->reset_pin_->pin_mode(gpio::FLAG_OUTPUT | gpio::FLAG_OPEN_DRAIN);  // LNK-3
    } else {
      this->reset_pin_->pin_mode(gpio::FLAG_INPUT);
    }
  }

 protected:
  fk::McuUpdate updater_;
  InternalGPIOPin *reset_pin_{nullptr};
  InternalGPIOPin *boot_pin_{nullptr};
  ESPPreferenceObject pref_;
  HighFrequencyLoopRequester high_freq_;
  char logged_[96]{};
};

}  // namespace safety_link
}  // namespace esphome
