// Updates the safety MCU (STM32G030K8) through its ROM bootloader on the link UART (DESIGN.md),
// with the firmware image this board's build carries.
//
// The link client asks the MCU first (UPDATE_PREPARE: only with the heater off and cold; the MCU
// then refuses starts until it resets) and lends this updater the UART. The updater then:
//
//   enters the bootloader: STM_BOOT high, STM_NRST pulsed, STM_BOOT released 10 ms later (LNK-6),
//     the UART at 8E1, 0x7F;
//   checks it: Get (the commands it needs), Get ID (0x466), the option bytes (no read-out
//     protection);
//   erases the image's pages, page 0 first, never pages 30 and 31;
//   writes 256-byte blocks, reading each back, page 0 last: until then the MCU has no vector table
//     and cannot run a half-written image (a reset leaves it silent or in the bootloader, with
//     every output off);
//   starts it (Go 0x08000000; a reset pulse if Go fails) and hands the UART back; the update is done
//     when HELLO reports the image's version.
//
// A NACK repeats the command; a lost or garbled reply re-enters the bootloader by the pins (a block
// that may be half-written gets its page erased again); a block that reads back wrong gets its page
// erased and written again. Limits on each. A failure before anything was erased restarts the old
// firmware; after that the MCU stays in the bootloader, the heater off, and the next attempt may
// take the MCU without UPDATE_PREPARE once it has been silent for 5 s (LNK-4): this board's own
// unfinished update (kept over a restart of the ESP32, on_save) is the only case.
//
// The option bytes (WDG-1, WDG-13) are a separate, explicit job, not part of an update:
// READ_OPTIONS reads FLASH_OPTR; WRITE_OPTIONS writes the bits WDG-1 asks for and reads them back.
// UNVERIFIED until proven on the first board: that the bootloader takes Write Memory at
// 0x1FFF7800, the area's layout (value and complement per double word) and length, and the reset it
// makes after the write.
//
// Plain C++, host-tested in tests/test_mcu_update.cpp against a simulated STM32 bootloader.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "link_client.h"
#include "stm32_boot.h"

namespace esphome {
namespace flamekeeper {

class McuUpdate : public McuUpdater {
 public:
  static const uint32_t PREPARE_MS = 2000;    // UPDATE_PREPARE's answer
  static const uint32_t RESET_MS = 5;         // STM_NRST held low
  static const uint32_t BOOT_HOLD_MS = 10;    // STM_BOOT held high after STM_NRST is released (LNK-6)
  static const uint32_t OPTION_RESET_MS = 50; // after an option write: the MCU reloads them and restarts
  static const uint32_t SILENT_MS = 5000;     // LNK-4
  static const uint32_t CONFIRM_MS = 5000;    // HELLO from the started firmware
  static const uint8_t SYNC_TRIES = 5;        // 0x7F per entry
  static const uint8_t ENTRY_TRIES = 3;       // pin entries in a row without a sync
  static const uint8_t COMMAND_TRIES = 3;     // NACKs in a row on one command
  static const uint8_t PAGE_TRIES = 3;        // writes of one page that read back wrong
  static const uint8_t REENTRY_MAX = 8;       // re-entries after a lost reply, per job

  explicit McuUpdate(BootPort *port) : port_(port), boot_(port) {}

  // The image this build carries (the STM32's flash from 0x08000000) and the version its HELLO
  // reports. The data stays the caller's.
  void set_image(const uint8_t *data, uint32_t size, const uint8_t version[3]);
  // Why this image cannot be installed (an empty, oversized or implausible image), or OK.
  Outcome check_image() const;

  // Kept over restarts of the ESP32: an update erased the MCU's flash and has not confirmed the
  // new firmware yet.
  void load(bool pending);
  void on_save(std::function<void(bool pending)> &&cb) { this->save_cb_ = std::move(cb); }
  bool pending() const { return this->pending_; }

  // The option word last read (READ_OPTIONS, WRITE_OPTIONS or an update).
  bool optr_known() const { return this->optr_known_; }
  uint32_t optr() const { return this->optr_; }
  uint16_t chip_id() const { return this->chip_id_; }

  // McuUpdater
  bool available() const override { return this->image_ != nullptr && this->size_ != 0; }
  const uint8_t *image_version() const override { return this->version_; }
  bool may_take(uint32_t silent_ms) const override;
  Outcome begin(McuJob job, bool prepare, uint32_t now) override;
  void prepared(uint8_t result, const uint8_t *payload, size_t len) override;
  void on_link(bool up, const uint8_t version[3]) override;
  void loop(uint32_t now) override;
  bool owns_uart() const override;
  bool running() const override;
  float progress() const override;
  const char *text() const override { return this->text_; }

  bool done() const { return this->phase_ == Phase::DONE; }
  bool failed() const { return this->phase_ == Phase::FAILED; }

 protected:
  enum class Phase : uint8_t {
    IDLE,
    PREPARE,    // UPDATE_PREPARE sent: waiting for the link client's answer
    ENTER,      // the pins (sub_ counts the steps)
    SYNC,
    COMMAND,    // one bootloader command (step_)
    OPTION_WAIT,  // after an option write, before re-entering
    LEAVE,      // a reset pulse into the application
    CONFIRM,    // the UART is the link's again: waiting for HELLO
    DONE,
    FAILED,
  };
  enum class Step : uint8_t {
    GET,
    GET_ID,
    READ_OPTIONS,
    ERASE,        // erase_order_[index_]
    WRITE,        // the block at block_
    VERIFY,       // ...read back
    WRITE_OPTIONS,
    CHECK_OPTIONS,  // the option word read back after a write
    GO,
  };

  void set_text_(const char *fmt, ...);
  void enter_(uint32_t now);
  void issue_(uint32_t now);
  void command_ok_(uint32_t now);
  void command_failed_(Stm32Boot::Status st, uint32_t now);
  void next_page_(uint32_t now);
  void start_page_(uint32_t now);
  void leave_by_reset_(uint32_t now);
  void leave_by_go_(uint32_t now);
  void fail_(uint32_t now, const char *why);
  void finish_failed_();
  void save_pending_(bool pending);
  uint32_t block_address_() const { return g030::FLASH_BASE + this->page_ * g030::PAGE_SIZE + this->block_ * Stm32Boot::BLOCK; }
  uint16_t block_len_() const;
  void block_bytes_(uint8_t *out) const;
  uint16_t pages_() const { return uint16_t((this->size_ + g030::PAGE_SIZE - 1) / g030::PAGE_SIZE); }

  BootPort *port_;
  Stm32Boot boot_;
  const uint8_t *image_{nullptr};
  uint32_t size_{0};
  uint8_t version_[3]{};
  std::function<void(bool)> save_cb_;
  bool pending_{false};
  bool agreed_{false};  // UPDATE_PREPARE answered OK, and the MCU has not run its firmware since

  McuJob job_{McuJob::UPDATE};
  Phase phase_{Phase::IDLE};
  Step step_{Step::GET};
  uint8_t sub_{0};
  uint32_t phase_ms_{0};
  uint32_t now_{0};
  uint8_t syncs_{0};
  uint8_t entries_{0};
  uint8_t tries_{0};
  uint8_t reentries_{0};
  bool redo_page_{false};  // after a re-entry: erase the current page again, then write it from its start
  bool touched_{false};    // something was erased or written: the old firmware is gone
  bool failing_{false};    // leaving towards FAILED (the old firmware restarted)
  bool reset_tried_{false};  // CONFIRM: the reset pulse after a silent start was tried
  uint16_t erase_index_{0};
  uint16_t page_{0};   // the page being written: 1, 2, ... then 0
  uint16_t block_{0};  // ...and the block in it
  uint8_t page_tries_{0};
  uint32_t blocks_done_{0};
  uint8_t options_[g030::OPTION_SIZE]{};
  bool optr_known_{false};
  uint32_t optr_{0};
  uint32_t optr_wanted_{0};
  uint16_t chip_id_{0};
  char text_[96]{};
};

}  // namespace flamekeeper
}  // namespace esphome
