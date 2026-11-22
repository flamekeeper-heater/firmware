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

#include "g030_map.h"
#include "link_client.h"

namespace esphome {
namespace flamekeeper {

// The STM32G030K8 as its ROM bootloader sees it (g030_map.h, shared with the safety MCU).
namespace g030 {
static const uint16_t CHIP_ID = G030_CHIP_ID;  // Get ID (product id)
static const uint32_t FLASH_BASE = G030_FLASH_BASE;
static const uint32_t PAGE_SIZE = G030_PAGE_SIZE;
static const uint16_t APP_PAGES = G030_APP_PAGES;  // pages 30 and 31 are the saved data: never erased or written here
static const uint32_t APP_END = G030_APP_END;      // 0x0800F000
static const uint32_t RAM_BASE = G030_RAM_BASE;
static const uint32_t RAM_END = G030_RAM_END;
// The option bytes: double words, the value in the low word and its complement in the high word.
// UNVERIFIED (WDG-13): that the bootloader reads and writes this area as such, and its length.
static const uint32_t OPTION_BASE = G030_OPTION_BASE;
static const uint16_t OPTION_SIZE = 128;
// FLASH_OPTR, the first option word.
static const uint32_t OPTR_RDP = G030_OPTR_RDP;
static const uint32_t RDP_LEVEL_0 = G030_RDP_LEVEL_0;
static const uint32_t OPTR_IWDG_SW = G030_OPTR_IWDG_SW;
// WDG-1's bits set and cleared, every other bit (RDP included) as it is.
inline uint32_t optr_required(uint32_t optr) { return g030_optr_required(optr); }
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
  // The AN3155 command codes this client sends.
  enum Cmd : uint8_t { GET = 0x00, GET_ID = 0x02, READ = 0x11, GO = 0x21, WRITE = 0x31, EXT_ERASE = 0x44 };
  static const uint16_t BLOCK = 256;  // Read and Write Memory: at most this many bytes
  // Timeouts per step (AN3155 gives none; these cover the transfer at 115200 8E1 and the flash).
  static const uint32_t SYNC_MS = 200;
  static const uint32_t ACK_MS = 1000;
  static const uint32_t ERASE_MS = 500;       // ...plus this per page (a G030 page takes up to 40 ms)
  static const uint32_t ERASE_PAGE_MS = 100;

  explicit Stm32Boot(BootPort *port) : port_(port) {}

  // 0x7F: the bootloader answers ACK (or NACK when it was already synced).
  void sync();
  // Get: reply() is the version byte, then the command codes.
  void get() { this->counted_(GET); }
  // Get ID: reply() is the product id, high byte first.
  void get_id() { this->counted_(GET_ID); }
  // Read Memory, 1..256 bytes into reply().
  void read(uint32_t address, uint16_t len);
  // Write Memory to the application's flash: 8-byte aligned, a multiple of 8, 8..256 bytes, below
  // page 30. False (and nothing sent) otherwise.
  bool write(uint32_t address, const uint8_t *data, uint16_t len);
  // Write Memory to the option bytes (the whole area, OPTION_SIZE bytes). The bootloader resets the
  // MCU after its ACK. UNVERIFIED (WDG-13).
  bool write_options(const uint8_t *data, uint16_t len);
  // Extended Erase of application pages (0..29), never a mass or bank erase. False otherwise.
  bool erase(const uint16_t *pages, uint8_t count);
  // Go: the bootloader jumps to the vector table at address.
  void go(uint32_t address);

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
  void counted_(uint8_t code);
  void address_(uint32_t address);
  void data_block_(const uint8_t *data, uint16_t len);
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
  bool have_count_{false};  // RECV_COUNTED: the count has arrived...
  uint16_t want_{0};        // ...and this many bytes are still due
  Status last_{Status::OK};
};

}  // namespace flamekeeper
}  // namespace esphome
