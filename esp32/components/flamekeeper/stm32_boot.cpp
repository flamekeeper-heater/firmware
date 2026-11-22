#include "stm32_boot.h"

#include <cstring>

namespace esphome {
namespace flamekeeper {

// A command is a list of steps: send some bytes and wait for the ACK, or receive a reply.

void Stm32Boot::begin_() {
  while (this->port_->read() >= 0) {
  }  // whatever came before belongs to no command
  this->tx_len_ = 0;
  this->rx_len_ = 0;
  this->steps_ = 0;
  this->step_ = 0;
  this->sent_ = false;
  this->last_ = Status::BUSY;
}

void Stm32Boot::add_(uint8_t kind, uint16_t len, uint32_t timeout_ms) {
  Step &s = this->step_list_[this->steps_++];
  s.kind = kind;
  s.off = uint16_t(this->tx_len_ - (kind == SEND_ACK || kind == SEND_SYNC ? len : 0));
  s.len = len;
  s.timeout_ms = timeout_ms;
}

// The command code and its complement, ACKed.
void Stm32Boot::command_(uint8_t code) {
  this->tx_[this->tx_len_++] = code;
  this->tx_[this->tx_len_++] = uint8_t(~code);
  this->add_(SEND_ACK, 2, ACK_MS);
}

// Four address bytes, high first, and their XOR, ACKed.
void Stm32Boot::address_(uint32_t address) {
  uint8_t x = 0;
  for (int i = 3; i >= 0; i--) {
    const uint8_t b = uint8_t(address >> (8 * i));
    this->tx_[this->tx_len_++] = b;
    x ^= b;
  }
  this->tx_[this->tx_len_++] = x;
  this->add_(SEND_ACK, 5, ACK_MS);
}

void Stm32Boot::sync(uint32_t now) {
  (void) now;
  this->begin_();
  this->tx_[this->tx_len_++] = SYNC;
  this->add_(SEND_SYNC, 1, SYNC_MS);
}

void Stm32Boot::get(uint32_t now) {
  (void) now;
  this->begin_();
  this->command_(0x00);
  this->add_(RECV_COUNTED, 0, ACK_MS);
  this->add_(RECV_ACK, 0, ACK_MS);
}

void Stm32Boot::get_id(uint32_t now) {
  (void) now;
  this->begin_();
  this->command_(0x02);
  this->add_(RECV_COUNTED, 0, ACK_MS);
  this->add_(RECV_ACK, 0, ACK_MS);
}

void Stm32Boot::read(uint32_t address, uint16_t len, uint32_t now) {
  (void) now;
  if (len == 0 || len > BLOCK)
    len = len == 0 ? 1 : BLOCK;
  this->begin_();
  this->command_(0x11);
  this->address_(address);
  this->tx_[this->tx_len_++] = uint8_t(len - 1);
  this->tx_[this->tx_len_++] = uint8_t(~(len - 1));
  this->add_(SEND_ACK, 2, ACK_MS);
  this->add_(RECV, len, ACK_MS);
}

bool Stm32Boot::write(uint32_t address, const uint8_t *data, uint16_t len, uint32_t now) {
  (void) now;
  // The application's flash only, in whole double words (the G030 programs 64 bits at a time).
  if (len == 0 || len > BLOCK || (len & 7) != 0 || (address & 7) != 0)
    return false;
  if (address < g030::FLASH_BASE || address >= g030::APP_END || len > g030::APP_END - address)
    return false;
  this->begin_();
  this->command_(0x31);
  this->address_(address);
  uint8_t x = uint8_t(len - 1);
  this->tx_[this->tx_len_++] = x;
  for (uint16_t i = 0; i < len; i++) {
    this->tx_[this->tx_len_++] = data[i];
    x ^= data[i];
  }
  this->tx_[this->tx_len_++] = x;
  this->add_(SEND_ACK, uint16_t(len + 2), ACK_MS);
  return true;
}

bool Stm32Boot::write_options(const uint8_t *data, uint16_t len, uint32_t now) {
  (void) now;
  if (len != g030::OPTION_SIZE)
    return false;
  this->begin_();
  this->command_(0x31);
  this->address_(g030::OPTION_BASE);
  uint8_t x = uint8_t(len - 1);
  this->tx_[this->tx_len_++] = x;
  for (uint16_t i = 0; i < len; i++) {
    this->tx_[this->tx_len_++] = data[i];
    x ^= data[i];
  }
  this->tx_[this->tx_len_++] = x;
  this->add_(SEND_ACK, uint16_t(len + 2), ACK_MS);
  return true;
}

bool Stm32Boot::erase(const uint16_t *pages, uint8_t count, uint32_t now) {
  (void) now;
  if (count == 0 || count > g030::APP_PAGES)
    return false;
  for (uint8_t i = 0; i < count; i++)
    if (pages[i] >= g030::APP_PAGES)
      return false;  // pages 30 and 31 are never erased here
  this->begin_();
  this->command_(0x44);
  // The page count less one, then the pages, each high byte first, then the XOR of all of them.
  // (Counts from 0xFFF0 up are the special erases: never sent.)
  const uint16_t start = this->tx_len_;
  this->tx_[this->tx_len_++] = 0;
  this->tx_[this->tx_len_++] = uint8_t(count - 1);
  for (uint8_t i = 0; i < count; i++) {
    this->tx_[this->tx_len_++] = uint8_t(pages[i] >> 8);
    this->tx_[this->tx_len_++] = uint8_t(pages[i]);
  }
  uint8_t x = 0;
  for (uint16_t i = start; i < this->tx_len_; i++)
    x ^= this->tx_[i];
  this->tx_[this->tx_len_++] = x;
  this->add_(SEND_ACK, uint16_t(this->tx_len_ - start), ERASE_MS + ERASE_PAGE_MS * count);
  return true;
}

void Stm32Boot::go(uint32_t address, uint32_t now) {
  (void) now;
  this->begin_();
  this->command_(0x21);
  this->address_(address);
}

Stm32Boot::Status Stm32Boot::finish_(Status s) {
  this->step_ = this->steps_;
  this->last_ = s;
  return s;
}

Stm32Boot::Status Stm32Boot::poll(uint32_t now) {
  if (this->step_ >= this->steps_)
    return this->last_;
  while (this->step_ < this->steps_) {
    const Step &s = this->step_list_[this->step_];
    if (!this->sent_) {
      if ((s.kind == SEND_ACK || s.kind == SEND_SYNC) && s.len != 0)
        this->port_->send(this->tx_ + s.off, s.len);
      this->sent_ = true;
      this->since_ = now;
      this->counted_ = false;
      this->want_ = 0;
    }
    const int c = this->port_->read();
    if (c < 0) {
      if (uint32_t(now - this->since_) >= s.timeout_ms)
        return this->finish_(Status::TIMEOUT);
      return Status::BUSY;
    }
    bool next = false;
    switch (s.kind) {
      case SEND_ACK:
      case RECV_ACK:
        if (c == NACK)
          return this->finish_(Status::NACK);
        if (c != ACK)
          return this->finish_(Status::BAD_REPLY);
        next = true;
        break;
      case SEND_SYNC:
        // A bootloader synced already takes 0x7F for a command and NACKs it: it is listening.
        if (c != ACK && c != NACK)
          return this->finish_(Status::BAD_REPLY);
        next = true;
        break;
      case RECV_COUNTED:
        if (!this->counted_) {
          this->counted_ = true;
          this->want_ = uint16_t(c + 1);  // the count is the bytes that follow, less one
          break;
        }
        this->rx_[this->rx_len_++] = uint8_t(c);
        next = --this->want_ == 0;
        break;
      case RECV:
        this->rx_[this->rx_len_++] = uint8_t(c);
        next = this->rx_len_ >= s.len;
        break;
      default:
        return this->finish_(Status::BAD_REPLY);
    }
    if (next) {
      this->step_++;
      this->sent_ = false;
    }
  }
  this->last_ = Status::OK;
  return Status::OK;
}

}  // namespace flamekeeper
}  // namespace esphome
