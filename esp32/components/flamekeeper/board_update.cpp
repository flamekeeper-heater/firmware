#include "board_update.h"
#include "bytes.h"

#include <cstring>

namespace esphome {
namespace flamekeeper {


void BoardUpdate::load(const Saved *saved, uint32_t running_build) {
  this->running_build_ = running_build;
  if (saved == nullptr || saved->check != SAVED_CHECK)
    return;
  this->saved_ = *saved;
  // Installed by the build that is running again: the new one did not come up (rollback).
  if (this->saved_.pending && running_build == this->saved_.old_build)
    this->set_last_(UpdateLast::ROLLED_BACK, false);
}

void BoardUpdate::set_last_(UpdateLast last, bool pending) {
  this->saved_.check = SAVED_CHECK;
  this->saved_.last = uint8_t(last);
  this->saved_.pending = pending ? 1 : 0;
  if (pending)
    this->saved_.old_build = this->running_build_;
  if (this->save_cb_)
    this->save_cb_(this->saved_);
}

void BoardUpdate::loop(uint32_t now) {
  this->now_ = now;
  if (!this->booted_) {
    this->booted_ = true;
    this->boot_ms_ = now;
  }
  // A new build that has run this long confirms itself (ESPHome's safe_mode marks it valid).
  if (this->saved_.pending && uint32_t(now - this->boot_ms_) >= CONFIRM_MS)
    this->set_last_(UpdateLast::INSTALLED, false);
  if ((this->state_ == UpdateState::RECEIVING || this->state_ == UpdateState::RECEIVED) &&
      uint32_t(now - this->last_data_ms_) >= IDLE_DROP_MS)
    this->drop_(UpdateLast::DROPPED);
  if (this->restart_due_ && int32_t(now - this->restart_at_ms_) >= 0) {
    this->restart_due_ = false;
    this->flash_.restart();
  }
}

Outcome BoardUpdate::heater_check_() const {
  HeaterStatus hs;  // the heater's own, now
  this->c_.heater()->read_status(hs);
  if (hs.update_running)
    return Outcome::refuse(Result::BUSY, "the heater firmware is being updated");
  if (this->c_.status().mode != Mode::OFF)
    return Outcome::refuse(Result::NOT_OFF, "turn the heater off first");
  // A heater that does not answer cannot be running on this board's say: the update may go ahead.
  if (hs.answering && hs.state != HeaterState::OFF)
    return Outcome::refuse(Result::NOT_OFF, "the heater is not off yet (wait for the cooldown)");
  return Outcome::ok();
}

void BoardUpdate::drop_(UpdateLast last) {
  if (this->state_ == UpdateState::RECEIVING || this->state_ == UpdateState::RECEIVED)
    this->flash_.abort();
  this->state_ = UpdateState::NONE;
  this->size_ = this->received_ = 0;
  this->c_.set_board_updating(false);
  this->set_last_(last, false);
}

Outcome BoardUpdate::update_begin(uint32_t size, const uint8_t sha256[UPDATE_HASH_SIZE], uint32_t *offset) {
  *offset = 0;
  if (this->state_ == UpdateState::INSTALLING)
    return Outcome::refuse(Result::BUSY, "an update is being installed");
  const bool same = this->state_ != UpdateState::NONE && size == this->size_ &&
                    std::memcmp(sha256, this->want_, UPDATE_HASH_SIZE) == 0;
  if (same) {  // resume, from any app
    *offset = this->received_;
    this->last_data_ms_ = this->now_;
    return Outcome::ok();
  }
  Outcome h = this->heater_check_();
  if (!h.accepted())
    return h;
  if (size == 0)
    return Outcome::refuse(Result::OUT_OF_RANGE, "the image is empty");
  if (size > this->flash_.slot_size())
    return Outcome::refuse(Result::TOO_BIG, "the image does not fit the board's update slot");
  if (this->state_ != UpdateState::NONE) {
    if (uint32_t(this->now_ - this->last_data_ms_) < TAKEOVER_MS)
      return Outcome::refuse(Result::BUSY, "another update is under way");
    this->drop_(UpdateLast::DROPPED);
  }
  if (!this->flash_.begin(size))  // refused: the last result stays as it was
    return Outcome::refuse(Result::REFUSED, "the update slot could not be prepared");
  this->state_ = UpdateState::RECEIVING;
  this->size_ = size;
  this->received_ = 0;
  std::memcpy(this->want_, sha256, UPDATE_HASH_SIZE);
  this->sha_.start();
  this->last_data_ms_ = this->now_;
  this->reported_at_ = 0;
  std::memset(this->reported_once_, 0, sizeof(this->reported_once_));
  this->gap_at_ = 0xFFFFFFFFu;
  this->answer_next_ = false;
  this->c_.set_board_updating(true);
  this->set_last_(UpdateLast::NOTHING, false);
  return Outcome::ok();
}

void BoardUpdate::report_(uint8_t report[UPDATE_REPORT_SIZE], UpdateReport why) {
  encode_update_report(this->received_, this->state_, why, report);
  if (why == UpdateReport::PROGRESS)
    this->reported_at_ = this->received_;  // the every-4096 cadence counts progress reports only
}

bool BoardUpdate::data(const uint8_t *in, size_t len, uint8_t report[UPDATE_REPORT_SIZE]) {
  // Anything not taken gets the board's own position back and why, at most 4 times a second per reason
  // (so a repeat answered a moment ago never hides a gap).
  auto ignored = [&](UpdateReport why) {
    const uint8_t i = uint8_t(why);
    const bool new_gap = why == UpdateReport::GAP && this->gap_at_ != this->received_;
    if (!new_gap && this->reported_once_[i] && uint32_t(this->now_ - this->report_ms_[i]) < REPORT_MIN_MS)
      return false;
    this->reported_once_[i] = true;
    this->report_ms_[i] = this->now_;
    if (why == UpdateReport::GAP)
      this->gap_at_ = this->received_;
    if (why == UpdateReport::REPEAT || why == UpdateReport::GAP)
      this->answer_next_ = true;
    this->report_(report, why);
    return true;
  };
  if (this->state_ != UpdateState::RECEIVING || len < UPDATE_HEADER_SIZE)
    return ignored(UpdateReport::REFUSED);
  const uint32_t offset = get_u32(in);
  const size_t n = len - UPDATE_HEADER_SIZE;
  if (offset < this->received_)
    return ignored(UpdateReport::REPEAT);
  if (offset > this->received_)
    return ignored(UpdateReport::GAP);
  if (n == 0 || n > this->size_ - this->received_)
    return ignored(UpdateReport::REFUSED);
  if (!this->flash_.write(in + UPDATE_HEADER_SIZE, n)) {
    this->drop_(UpdateLast::DROPPED);
    this->report_(report, UpdateReport::REFUSED);
    return true;
  }
  this->sha_.add(in + UPDATE_HEADER_SIZE, n);
  this->received_ += uint32_t(n);
  this->last_data_ms_ = this->now_;
  if (this->received_ == this->size_) {
    this->state_ = UpdateState::RECEIVED;
    this->report_(report);
    return true;
  }
  // Every 4096 bytes, and the first chunk taken after a repeat or a gap was answered (the app's
  // probe, or the resend after a gap, is then confirmed at once).
  if (this->answer_next_ || this->received_ - this->reported_at_ >= REPORT_EVERY) {
    this->answer_next_ = false;
    this->report_(report);
    return true;
  }
  return false;
}

void BoardUpdate::update_status(UpdateStatus &out) const {
  out.state = this->state_;
  out.size = this->size_;
  out.received = this->received_;
  std::memcpy(out.sha256, this->want_, UPDATE_HASH_SIZE);
  if (this->state_ == UpdateState::NONE)
    std::memset(out.sha256, 0, UPDATE_HASH_SIZE);
  out.last = UpdateLast(this->saved_.last);
}

Outcome BoardUpdate::update_finish() {
  if (this->state_ == UpdateState::NONE)
    return Outcome::refuse(Result::REFUSED, "no update is under way");
  if (this->state_ == UpdateState::INSTALLING)
    return Outcome::refuse(Result::BUSY, "the update is being installed");
  if (this->state_ == UpdateState::RECEIVING)
    return Outcome::refuse(Result::HASH_MISMATCH, "not every byte has arrived yet");
  Outcome h = this->heater_check_();
  if (!h.accepted())
    return h;
  uint8_t got[UPDATE_HASH_SIZE];
  this->sha_.finish(got);
  if (std::memcmp(got, this->want_, UPDATE_HASH_SIZE) != 0) {
    this->drop_(UpdateLast::DROPPED);
    return Outcome::refuse(Result::HASH_MISMATCH, "the image received does not match its SHA-256");
  }
  this->state_ = UpdateState::INSTALLING;
  const UpdateFlash::End end = this->flash_.end();
  if (end != UpdateFlash::End::OK) {
    this->state_ = UpdateState::NONE;  // the writer has given the slot up
    this->size_ = this->received_ = 0;
    this->c_.set_board_updating(false);
    this->set_last_(UpdateLast::DROPPED, false);
    if (end == UpdateFlash::End::BAD_IMAGE)
      return Outcome::refuse(Result::BAD_IMAGE, "the image is not signed by a key this board trusts, or not valid");
    return Outcome::refuse(Result::REFUSED, "the image could not be installed");
  }
  this->set_last_(UpdateLast::NOTHING, true);
  this->restart_due_ = true;
  this->restart_at_ms_ = this->now_ + RESTART_DELAY_MS;
  return Outcome::ok();  // stays INSTALLING (starts refused) until the restart
}

void BoardUpdate::update_abort() {
  if (this->state_ == UpdateState::RECEIVING || this->state_ == UpdateState::RECEIVED)
    this->drop_(UpdateLast::DROPPED);
}

}  // namespace flamekeeper
}  // namespace esphome
