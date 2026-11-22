// The STM32 ROM bootloader's USART protocol (ST AN3155), as the ESP32 speaks it to the safety MCU
// on the link UART: one command at a time, started by a call and then driven by poll() from the
// loop until it is no longer BUSY. Nothing here waits.
//
// It knows the STM32G030K8's map and refuses, before a byte goes out, any erase or write of the
// last two flash pages (30 and 31: the saved parameters and the plug record) or outside the
// application's 60 KB. The option bytes have their own explicit call.
//
// Plain C++, host-tested in tests/test_mcu_update.cpp against a simulated bootloader.
#pragma once

#include <cstddef>
#include <cstdint>

#include "link_client.h"

namespace esphome {
namespace flamekeeper {

// The STM32G030K8 as its ROM bootloader sees it (stm32/hal/stm32g030k8.ld).
namespace g030 {
static const uint16_t CHIP_ID = 0x466;  // Get ID (product id)
static const uint32_t FLASH_BASE = 0x08000000;
static const uint32_t PAGE_SIZE = 2048;
static const uint16_t APP_PAGES = 30;  // pages 30 and 31 are the saved data: never erased or written here
static const uint32_t APP_END = FLASH_BASE + APP_PAGES * PAGE_SIZE;  // 0x0800F000
static const uint32_t RAM_BASE = 0x20000000;
static const uint32_t RAM_END = 0x20002000;
// The option bytes: double words, the value in the low word and its complement in the high word.
// UNVERIFIED (WDG-13): that the bootloader reads and writes this area as such, and its length.
static const uint32_t OPTION_BASE = 0x1FFF7800;
static const uint16_t OPTION_SIZE = 128;
// FLASH_OPTR, the first option word.
static const uint32_t OPTR_RDP = 0xFFu;  // 0xAA: level 0, no read-out protection
static const uint32_t RDP_LEVEL_0 = 0xAAu;
static const uint32_t OPTR_IWDG_SW = 1u << 16;
static const uint32_t OPTR_IWDG_STOP = 1u << 17;
static const uint32_t OPTR_IWDG_STDBY = 1u << 18;
static const uint32_t OPTR_NBOOT_SEL = 1u << 24;
static const uint32_t OPTR_NBOOT1 = 1u << 25;
static const uint32_t OPTR_NBOOT0 = 1u << 26;
// WDG-1: the watchdog in hardware and running in stop and standby, boot from the BOOT0 pin with
// main flash by default. Every other bit (RDP included) stays as it is.
inline uint32_t optr_required(uint32_t optr) {
  return (optr & ~(OPTR_IWDG_SW | OPTR_NBOOT_SEL)) | OPTR_IWDG_STOP | OPTR_IWDG_STDBY | OPTR_NBOOT1 | OPTR_NBOOT0;
}
}  // namespace g030

// The link UART plus the safety MCU's reset and boot lines. Only the safety-MCU updater uses the
// three extra calls, and only after the MCU agreed (DESIGN.md).
class BootPort : public LinkTransport {
 public:
  // UART framing: 8E1 for the ROM bootloader (true), 8N1 for the link (false).
  virtual void bootloader_framing(bool on) = 0;
  // STM_BOOT (BOOT0): driven high (true), or released to high-Z, where the MCU's pull-down holds it low.
  virtual void drive_boot(bool high) = 0;
  // STM_NRST: pulled low (true, open drain), or released.
  virtual void drive_reset(bool low) = 0;
};

class Stm32Boot {
 public:
  enum class Status : uint8_t { BUSY, OK, NACK, TIMEOUT, BAD_REPLY };
  static const uint8_t ACK = 0x79;
  static const uint8_t NACK = 0x1F;
  static const uint8_t SYNC = 0x7F;
  static const uint16_t BLOCK = 256;  // Read and Write Memory: at most this many bytes
  // Timeouts per step (AN3155 gives none; these cover the transfer at 115200 8E1 and the flash).
  static const uint32_t SYNC_MS = 200;
  static const uint32_t ACK_MS = 1000;
  static const uint32_t ERASE_MS = 500;       // ...plus this per page (a G030 page takes up to 40 ms)
  static const uint32_t ERASE_PAGE_MS = 100;

  explicit Stm32Boot(BootPort *port) : port_(port) {}

  // 0x7F: the bootloader answers ACK (or NACK when it was already synced).
  void sync(uint32_t now);
  // Get: reply() is the version byte, then the command codes.
  void get(uint32_t now);
  // Get ID: reply() is the product id, high byte first.
  void get_id(uint32_t now);
  // Read Memory, 1..256 bytes into reply().
  void read(uint32_t address, uint16_t len, uint32_t now);
  // Write Memory to the application's flash: 8-byte aligned, a multiple of 8, 8..256 bytes, below
  // page 30. False (and nothing sent) otherwise.
  bool write(uint32_t address, const uint8_t *data, uint16_t len, uint32_t now);
  // Write Memory to the option bytes (the whole area, OPTION_SIZE bytes). The bootloader resets the
  // MCU after its ACK. UNVERIFIED (WDG-13).
  bool write_options(const uint8_t *data, uint16_t len, uint32_t now);
  // Extended Erase of application pages (0..29), never a mass or bank erase. False otherwise.
  bool erase(const uint16_t *pages, uint8_t count, uint32_t now);
  // Go: the bootloader jumps to the vector table at address.
  void go(uint32_t address, uint32_t now);

  Status poll(uint32_t now);
  bool busy() const { return this->step_ < this->steps_; }
  const uint8_t *reply() const { return this->rx_; }
  size_t reply_len() const { return this->rx_len_; }

 protected:
  enum Kind : uint8_t { SEND_ACK, SEND_SYNC, RECV_COUNTED, RECV, RECV_ACK };
  struct Step {
    uint8_t kind;
    uint16_t off, len;  // bytes of tx_ sent at the step's start; for RECV, the bytes expected
    uint32_t timeout_ms;
  };
  void begin_();
  void add_(uint8_t kind, uint16_t len, uint32_t timeout_ms);
  void command_(uint8_t code);
  void address_(uint32_t address);
  Status finish_(Status s);

  BootPort *port_;
  uint8_t tx_[2 + 5 + 1 + BLOCK + 1];
  uint16_t tx_len_{0};
  uint8_t rx_[BLOCK + 2];
  uint16_t rx_len_{0};
  Step step_list_[5];
  uint8_t steps_{0};
  uint8_t step_{0};
  bool sent_{false};
  uint32_t since_{0};
  bool counted_{false};  // RECV_COUNTED: the count has arrived...
  uint16_t want_{0};     // ...and this many bytes are still due
  Status last_{Status::OK};
};

}  // namespace flamekeeper
}  // namespace esphome
