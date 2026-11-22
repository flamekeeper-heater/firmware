#include "mcu_update.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "bytes.h"

namespace esphome {
namespace flamekeeper {

void McuUpdate::set_text_(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vsnprintf(this->text_, sizeof(this->text_), fmt, args);
  va_end(args);
}

void McuUpdate::load(bool pending) {
  this->pending_ = pending;
  if (pending)
    this->set_text_("An update is unfinished: the heater stays off until it is installed");
}

void McuUpdate::set_image(const uint8_t *data, uint32_t size, const uint8_t version[3]) {
  this->image_ = data;
  this->size_ = size;
  std::memcpy(this->version_, version, 3);
}

Outcome McuUpdate::check_image() const {
  if (!this->available())
    return Outcome::refuse(Result::NOT_SUPPORTED, "this build carries no safety MCU firmware");
  if (this->size_ > uint32_t(g030::APP_PAGES) * g030::PAGE_SIZE)
    return Outcome::refuse(Result::TOO_BIG, "the safety MCU firmware would overwrite its saved data");
  // The vector table: a stack in RAM and a reset handler (Thumb) inside the image.
  const uint32_t sp = this->size_ >= 8 ? get_u32(this->image_) : 0;
  const uint32_t pc = this->size_ >= 8 ? get_u32(this->image_ + 4) : 0;
  if (sp <= g030::RAM_BASE || sp > g030::RAM_END || !(pc & 1u) || pc < g030::FLASH_BASE + 8 ||
      pc >= g030::FLASH_BASE + this->size_)
    return Outcome::refuse(Result::BAD_IMAGE, "the safety MCU firmware has no valid vector table");
  return Outcome::ok();
}

bool McuUpdate::may_take(uint32_t silent_ms) const {
  return (this->agreed_ || this->pending_) && silent_ms >= SILENT_MS;
}

bool McuUpdate::running() const {
  return this->phase_ != Phase::IDLE && this->phase_ != Phase::DONE && this->phase_ != Phase::FAILED;
}

bool McuUpdate::owns_uart() const {
  switch (this->phase_) {
    case Phase::ENTER:
    case Phase::SYNC:
    case Phase::COMMAND:
    case Phase::OPTION_WAIT:
    case Phase::LEAVE:
      return true;
    default:
      return false;
  }
}

float McuUpdate::progress() const {
  if (this->job_ != McuJob::UPDATE || this->size_ == 0)
    return this->phase_ == Phase::DONE ? 100.0f : 0.0f;
  if (this->phase_ == Phase::DONE)
    return 100.0f;
  const uint32_t blocks = (this->size_ + Stm32Boot::BLOCK - 1) / Stm32Boot::BLOCK;
  return 100.0f * float(this->blocks_done_) / float(blocks);
}

void McuUpdate::save_pending_(bool pending) {
  if (this->pending_ == pending)
    return;
  this->pending_ = pending;
  if (this->save_cb_)
    this->save_cb_(pending);
}

// ---------------------------------------------------------------- starting and ending

Outcome McuUpdate::begin(McuJob job, bool prepare, uint32_t now) {
  if (this->running())
    return Outcome::refuse(Result::BUSY, "the heater firmware is being updated");
  if (job == McuJob::UPDATE) {
    Outcome o = this->check_image();
    if (!o.accepted())
      return o;
  }
  this->job_ = job;
  this->now_ = now;
  this->step_ = Step::GET;
  this->entries_ = this->tries_ = this->reentries_ = 0;
  this->page_again_ = this->touched_ = this->failing_ = this->reset_tried_ = false;
  this->blocks_done_ = 0;
  if (prepare) {
    this->phase_ = Phase::PREPARE;
    this->phase_ms_ = now;
    this->set_text_("Asking the heater");
  } else {
    this->enter_(now);  // the MCU agreed before and has been silent since
  }
  return Outcome::ok();
}

void McuUpdate::prepared(uint8_t result, const uint8_t *payload, size_t len) {
  if (this->phase_ != Phase::PREPARE)
    return;
  const char *why = nullptr;
  if (result == LINK_NOT_OFF)
    why = "the heater must be off and cold";
  else if (result == LINK_NOT_SUPPORTED)
    why = "the heater's firmware cannot be updated from this board";
  else if (result != LINK_OK)
    why = "the heater refused the update";
  else if (len < 1 || payload[0] != 1)
    why = "the heater asks for an update method this board does not have";
  if (why != nullptr) {
    this->set_text_("Failed: %s", why);
    this->phase_ = Phase::FAILED;
    return;
  }
  this->agreed_ = true;  // it refuses starts until it resets
  this->enter_(this->now_);
}

void McuUpdate::on_link(bool up, const uint8_t version[3]) {
  if (!up)
    return;
  if (this->phase_ == Phase::CONFIRM) {
    this->agreed_ = false;
    if (this->job_ != McuJob::UPDATE) {
      this->phase_ = Phase::DONE;  // the text says what was read or written
      return;
    }
    this->save_pending_(false);  // a firmware runs
    if (std::memcmp(version, this->version_, 3) != 0) {
      this->fail_(this->now_, "the heater runs %u.%u.%u after the update, not %u.%u.%u", version[0], version[1],
                  version[2], this->version_[0], this->version_[1], this->version_[2]);
      return;
    }
    this->phase_ = Phase::DONE;
    this->set_text_("Installed %u.%u.%u", version[0], version[1], version[2]);
    return;
  }
  if (!this->running()) {
    // Its firmware runs: any agreement ended with the reset, and no update is left unfinished.
    this->agreed_ = false;
    this->save_pending_(false);
  }
}

void McuUpdate::fail_(uint32_t now, const char *fmt, ...) {
  char why[sizeof(this->text_)];
  va_list args;
  va_start(args, fmt);
  vsnprintf(why, sizeof(why), fmt, args);
  va_end(args);
  if (this->phase_ == Phase::PREPARE || this->phase_ == Phase::CONFIRM || !this->owns_uart()) {
    this->set_text_("Failed: %s", why);
    this->phase_ = Phase::FAILED;
    return;
  }
  if (!this->touched_) {
    // Nothing erased: the old firmware is whole, reset into it.
    this->set_text_("Failed: %s", why);
    this->failing_ = true;
    this->leave_by_reset_(now);
    return;
  }
  // The flash is half done: the MCU stays where it is (in its bootloader, or silent without a vector
  // table), every output off, until the next attempt.
  this->set_text_("Failed: %s. The heater stays off until the update is done again", why);
  this->port_->drive_boot(false);
  this->port_->drive_reset(false);
  this->port_->bootloader_framing(false);
  this->phase_ = Phase::FAILED;
}

void McuUpdate::enter_(uint32_t now) {
  if (this->step_ == Step::GET)
    this->set_text_("Starting the heater's bootloader");
  this->phase_ = Phase::ENTER;
  this->sub_ = 0;
  this->phase_ms_ = now;
}

void McuUpdate::leave_by_reset_(uint32_t now) {
  this->port_->drive_boot(false);
  this->port_->drive_reset(true);
  this->phase_ = Phase::LEAVE;
  this->phase_ms_ = now;
}

void McuUpdate::leave_by_go_(uint32_t now) {
  this->tries_ = 0;
  this->issue_(Step::GO, now);
}

// ---------------------------------------------------------------- the loop

void McuUpdate::loop(uint32_t now) {
  this->now_ = now;
  switch (this->phase_) {
    case Phase::PREPARE:
      if (uint32_t(now - this->phase_ms_) >= PREPARE_MS)
        this->fail_(now, "the heater did not answer the update request");
      return;
    case Phase::ENTER:
      // LNK-6: STM_BOOT high, STM_NRST pulsed, STM_BOOT released 10 ms after, then 0x7F at 8E1.
      if (this->sub_ == 0) {
        this->port_->bootloader_framing(true);
        this->port_->drive_boot(true);
        this->port_->drive_reset(true);
        this->sub_ = 1;
        this->phase_ms_ = now;
      } else if (this->sub_ == 1 && uint32_t(now - this->phase_ms_) >= RESET_MS) {
        this->port_->drive_reset(false);
        this->sub_ = 2;
        this->phase_ms_ = now;
      } else if (this->sub_ == 2 && uint32_t(now - this->phase_ms_) >= BOOT_HOLD_MS) {
        this->port_->drive_boot(false);
        this->phase_ = Phase::SYNC;
        this->syncs_ = 0;
        this->boot_.sync();
      }
      return;
    case Phase::SYNC: {
      const Stm32Boot::Status st = this->boot_.poll(now);
      if (st == Stm32Boot::Status::BUSY)
        return;
      if (st == Stm32Boot::Status::OK) {
        this->entries_ = 0;
        this->phase_ = Phase::COMMAND;
        this->tries_ = 0;
        this->issue_(now);
        return;
      }
      if (++this->syncs_ < SYNC_TRIES) {
        this->boot_.sync();
        return;
      }
      if (++this->entries_ < ENTRY_TRIES) {
        this->enter_(now);
        return;
      }
      this->fail_(now, this->touched_ || this->step_ != Step::GET
                           ? "the heater's bootloader stopped answering"
                           : "the heater's bootloader does not answer (its option bytes need nBOOT_SEL = 0)");
      return;
    }
    case Phase::COMMAND: {
      const Stm32Boot::Status st = this->boot_.poll(now);
      if (st == Stm32Boot::Status::BUSY)
        return;
      if (st == Stm32Boot::Status::OK) {
        this->tries_ = 0;
        this->command_ok_(now);
      } else {
        this->command_failed_(st, now);
      }
      return;
    }
    case Phase::OPTION_WAIT:
      if (uint32_t(now - this->phase_ms_) >= OPTION_RESET_MS)
        this->enter_(now);  // then CHECK_OPTIONS
      return;
    case Phase::LEAVE:
      if (uint32_t(now - this->phase_ms_) >= RESET_MS) {
        this->port_->drive_reset(false);
        this->port_->bootloader_framing(false);
        if (this->failing_) {
          this->failing_ = false;
          this->phase_ = Phase::FAILED;
        } else {
          this->phase_ = Phase::CONFIRM;
          this->phase_ms_ = now;
        }
      }
      return;
    case Phase::CONFIRM:
      if (uint32_t(now - this->phase_ms_) >= CONFIRM_MS) {
        if (!this->reset_tried_) {
          this->reset_tried_ = true;
          this->leave_by_reset_(now);
        } else {
          this->fail_(now, "the heater did not start after the update: switch the board off and on");
        }
      }
      return;
    default:
      return;
  }
}

// ---------------------------------------------------------------- commands

uint16_t McuUpdate::block_len_() const {
  uint32_t n = this->size_ - this->block_offset_();
  if (n > Stm32Boot::BLOCK)
    n = Stm32Boot::BLOCK;
  return uint16_t((n + 7) & ~7u);  // whole double words, padded with erased bytes
}

void McuUpdate::block_bytes_(uint8_t *out) const {
  const uint32_t off = this->block_offset_();
  const uint16_t len = this->block_len_();
  for (uint16_t i = 0; i < len; i++)
    out[i] = off + i < this->size_ ? this->image_[off + i] : 0xFF;
}

void McuUpdate::issue_(uint32_t now) {
  this->phase_ = Phase::COMMAND;
  bool ok = true;
  switch (this->step_) {
    case Step::GET:
      this->boot_.get();
      break;
    case Step::GET_ID:
      this->boot_.get_id();
      break;
    case Step::READ_OPTIONS:
    case Step::CHECK_OPTIONS:
      this->boot_.read(g030::OPTION_BASE, g030::OPTION_SIZE);
      break;
    case Step::ERASE: {
      const uint16_t page = this->page_again_ ? this->page_ : this->erase_index_;
      this->touched_ = true;
      ok = this->boot_.erase(&page, 1);
      break;
    }
    case Step::WRITE: {
      uint8_t block[Stm32Boot::BLOCK];
      this->block_bytes_(block);
      ok = this->boot_.write(this->block_address_(), block, this->block_len_());
      break;
    }
    case Step::VERIFY:
      this->boot_.read(this->block_address_(), this->block_len_());
      break;
    case Step::WRITE_OPTIONS:
      ok = this->boot_.write_options(this->options_, g030::OPTION_SIZE);
      break;
    case Step::GO:
      this->boot_.go(g030::FLASH_BASE);
      break;
  }
  if (!ok)
    this->fail_(now, "a write outside the application was refused");
}

void McuUpdate::start_page_(uint32_t now) {
  this->block_ = 0;
  this->issue_(Step::WRITE, now);
}

// The current page again from its start (the block may be half-written, or read back wrong): its
// blocks no longer count, and it is erased first.
void McuUpdate::redo_page_() {
  if (!this->page_again_)
    this->blocks_done_ -= this->block_;
  this->page_again_ = true;
  this->step_ = Step::ERASE;
}

// Pages 1, 2, ... then page 0, whose vector table makes the image runnable.
void McuUpdate::next_page_(uint32_t now) {
  this->page_tries_ = 0;
  if (this->page_ == 0) {
    this->set_text_("Starting the new firmware");
    this->leave_by_go_(now);
    return;
  }
  this->page_ = uint16_t(this->page_ + 1 < this->pages_() ? this->page_ + 1 : 0);
  this->start_page_(now);
}

void McuUpdate::command_ok_(uint32_t now) {
  const uint8_t *r = this->boot_.reply();
  const size_t n = this->boot_.reply_len();
  switch (this->step_) {
    case Step::GET: {
      // The version byte, then the commands: Read, Go, Write and Extended Erase are needed.
      static const uint8_t NEEDED[] = {Stm32Boot::READ, Stm32Boot::GO, Stm32Boot::WRITE, Stm32Boot::EXT_ERASE};
      for (uint8_t c : NEEDED) {
        if (n < 2 || std::memchr(r + 1, c, n - 1) == nullptr) {
          this->fail_(now, "the heater's bootloader lacks command 0x%02X", c);
          return;
        }
      }
      this->issue_(Step::GET_ID, now);
      return;
    }
    case Step::GET_ID: {
      this->chip_id_ = n >= 2 ? uint16_t(r[0] << 8 | r[1]) : 0;
      if (this->chip_id_ != g030::CHIP_ID) {
        this->fail_(now, "the safety MCU is not an STM32G030 (chip id 0x%03X)", unsigned(this->chip_id_));
        return;
      }
      this->issue_(Step::READ_OPTIONS, now);
      return;
    }
    case Step::READ_OPTIONS: {
      std::memcpy(this->options_, r, g030::OPTION_SIZE);
      this->optr_ = get_u32(this->options_);
      this->optr_known_ = true;
      const bool as_required = g030::optr_required(this->optr_) == this->optr_;
      if (this->job_ == McuJob::UPDATE) {
        this->save_pending_(true);  // before the first erase
        this->erase_index_ = 0;
        this->set_text_("Erasing");
        this->issue_(Step::ERASE, now);
        return;
      }
      if (this->job_ == McuJob::READ_OPTIONS) {
        this->set_text_("Option bytes 0x%08X: %s", unsigned(this->optr_),
                        as_required ? "as required" : "not as required (WDG-1)");
        this->leave_by_go_(now);
        return;
      }
      // WRITE_OPTIONS. UNVERIFIED (WDG-13): the area as value and complement per double word.
      if ((this->optr_ & g030::OPTR_RDP) != g030::RDP_LEVEL_0) {
        this->fail_(now, "the chip's read-out protection is on: option bytes not written");
        return;
      }
      if (get_u32(this->options_ + 4) != ~this->optr_) {
        this->fail_(now, "the option bytes do not read as value and complement: not written");
        return;
      }
      this->optr_wanted_ = g030::optr_required(this->optr_);
      if (as_required) {
        this->set_text_("Option bytes 0x%08X: already as required", unsigned(this->optr_));
        this->leave_by_go_(now);
        return;
      }
      put_u32(this->options_, this->optr_wanted_);
      put_u32(this->options_ + 4, ~this->optr_wanted_);
      this->set_text_("Writing the option bytes");
      this->issue_(Step::WRITE_OPTIONS, now);
      return;
    }
    case Step::ERASE:
      if (this->page_again_) {
        this->page_again_ = false;
        this->start_page_(now);
        return;
      }
      if (++this->erase_index_ < this->pages_()) {
        this->issue_(now);
        return;
      }
      this->page_ = this->pages_() > 1 ? 1 : 0;
      this->page_tries_ = 0;
      this->set_text_("Writing 0 %%");
      this->start_page_(now);
      return;
    case Step::WRITE:
      this->issue_(Step::VERIFY, now);
      return;
    case Step::VERIFY: {
      uint8_t block[Stm32Boot::BLOCK];
      this->block_bytes_(block);
      const uint16_t len = this->block_len_();
      if (n != len || std::memcmp(r, block, len) != 0) {
        if (++this->page_tries_ >= PAGE_TRIES) {
          this->fail_(now, "the block at 0x%08X does not read back as written", unsigned(this->block_address_()));
          return;
        }
        this->redo_page_();
        this->issue_(now);
        return;
      }
      this->blocks_done_++;
      this->set_text_("Writing %u %%", unsigned(this->progress()));
      this->block_++;
      uint32_t in_page = this->size_ - uint32_t(this->page_) * g030::PAGE_SIZE;
      if (in_page > g030::PAGE_SIZE)
        in_page = g030::PAGE_SIZE;
      if (uint32_t(this->block_) * Stm32Boot::BLOCK >= in_page) {
        this->next_page_(now);
        return;
      }
      this->issue_(Step::WRITE, now);
      return;
    }
    case Step::WRITE_OPTIONS:
      // ACKed: the bootloader resets the MCU, which loads the new option bytes. Back in, to read them.
      this->step_ = Step::CHECK_OPTIONS;
      this->phase_ = Phase::OPTION_WAIT;
      this->phase_ms_ = now;
      return;
    case Step::CHECK_OPTIONS: {
      this->optr_ = get_u32(r);
      if (this->optr_ != this->optr_wanted_ || get_u32(r + 4) != ~this->optr_wanted_) {
        this->fail_(now, "the option bytes read back 0x%08X, not 0x%08X", unsigned(this->optr_),
                    unsigned(this->optr_wanted_));
        return;
      }
      this->set_text_("Option bytes written and read back: 0x%08X", unsigned(this->optr_));
      this->leave_by_go_(now);
      return;
    }
    case Step::GO:
      this->port_->bootloader_framing(false);
      this->phase_ = Phase::CONFIRM;
      this->phase_ms_ = now;
      return;
  }
}

void McuUpdate::command_failed_(Stm32Boot::Status st, uint32_t now) {
  if (st == Stm32Boot::Status::NACK) {
    // Refused (a byte garbled on the way, or the bootloader's own refusal): the same again.
    if (++this->tries_ < COMMAND_TRIES) {
      this->issue_(now);
      return;
    }
    this->tries_ = 0;
    switch (this->step_) {
      case Step::WRITE:
        // Refused every time (a double word not erased?): the page again from its start.
        if (++this->page_tries_ < PAGE_TRIES) {
          this->redo_page_();
          this->issue_(now);
          return;
        }
        break;
      case Step::GO:
        // A reset pulse with BOOT0 low starts the firmware too (unless the EMPTY flag is stale).
        this->reset_tried_ = true;
        this->leave_by_reset_(now);
        return;
      case Step::READ_OPTIONS:
      case Step::CHECK_OPTIONS:
        this->fail_(now, "the heater's bootloader refuses reads (read-out protection?)");
        return;
      default:
        break;
    }
    this->fail_(now, "the heater's bootloader refused a command");
    return;
  }
  // No reply, or a garbled one: enter the bootloader again by the pins.
  if (++this->reentries_ > REENTRY_MAX) {
    this->fail_(now, "the link to the heater's bootloader keeps failing");
    return;
  }
  if (this->step_ == Step::WRITE || this->step_ == Step::VERIFY ||
      (this->step_ == Step::ERASE && this->page_again_)) {
    this->redo_page_();  // the block may be half-written
  } else if (this->step_ == Step::WRITE_OPTIONS) {
    this->step_ = Step::CHECK_OPTIONS;  // it may have been written, and the MCU reset
  }
  this->enter_(now);
}

}  // namespace flamekeeper
}  // namespace esphome
