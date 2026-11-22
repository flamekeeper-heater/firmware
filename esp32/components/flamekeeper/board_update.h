// A board firmware update from the app (PROTOCOL.md): the image arrives in
// chunks over Bluetooth or the WebSocket, is written to the other app slot as it comes, and is
// checked (SHA-256 here, then the signature and the image by the slot writer) before the board
// switches to it. Only with the heater off; every start is refused while it runs.
//
// Plain C++ on the Controller and a slot writer (UpdateFlash), host-tested in
// tests/test_board_update.cpp. On the board the slot writer is ESPHome's OTA backend, which checks
// the signature (esp32: signed_ota_verification) and selects the new build; ESP-IDF's app rollback
// brings the old one back if the new one does not confirm itself.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "board.h"
#include "controller.h"
#include "crypto.h"

namespace esphome {
namespace flamekeeper {

// The app slot the new build goes into.
class UpdateFlash {
 public:
  enum class End : uint8_t { OK, BAD_IMAGE, FAILED };
  virtual ~UpdateFlash() = default;
  virtual uint32_t slot_size() const = 0;
  virtual bool begin(uint32_t size) = 0;
  virtual bool write(const uint8_t *data, size_t len) = 0;
  // Checks the image (and its signature) and selects it for the next start. Takes seconds.
  virtual End end() = 0;
  virtual void abort() = 0;
  virtual void restart() = 0;
};

class BoardUpdate : public BoardUpdater {
 public:
  static const uint32_t IDLE_DROP_MS = 5 * 60 * 1000;  // no data this long: dropped
  static const uint32_t TAKEOVER_MS = 60 * 1000;       // ...and another image may replace it after this
  static const uint32_t REPORT_EVERY = 4096;           // progress after this many bytes taken
  static const uint32_t REPORT_MIN_MS = 250;           // ...and at most this often for ignored chunks
  static const uint32_t RESTART_DELAY_MS = 1000;       // UPDATE_FINISH's result goes out first
  static const uint32_t CONFIRM_MS = 60 * 1000;        // as ESPHome's safe_mode boot_is_good_after

  // Kept over restarts: the last update's result, and whether one waits to be confirmed.
  struct Saved {
    uint32_t check;
    uint8_t last;       // UpdateLast
    uint8_t pending;    // 1: installed, the restart into it not yet confirmed
    uint8_t reserved[2];
    uint32_t old_build;  // the build that installed it
  };
  static const uint32_t SAVED_CHECK = 0x46575531UL;  // "FWU1"

  BoardUpdate(Controller &controller, UpdateFlash &flash) : c_(controller), flash_(flash) {}

  // At start-up: what was saved (nullptr: nothing), and when this build was made.
  void load(const Saved *saved, uint32_t running_build);
  void on_save(std::function<void(const Saved &)> &&cb) { this->save_cb_ = std::move(cb); }
  void loop(uint32_t now);

  // State flag bit 4: from UPDATE_BEGIN until the update ends, is dropped or the board restarts.
  bool running() const { return this->state_ != UpdateState::NONE; }

  // One data record (u32 offset, image bytes). True when a progress report should go back to the
  // sender, written to report.
  bool data(const uint8_t *in, size_t len, uint8_t report[UPDATE_REPORT_SIZE]);

  // BoardUpdater
  Outcome update_begin(uint32_t size, const uint8_t sha256[UPDATE_HASH_SIZE], uint32_t *offset) override;
  void update_status(UpdateStatus &out) const override;
  Outcome update_finish() override;
  void update_abort() override;

 protected:
  Outcome heater_check_() const;
  void drop_(UpdateLast last);
  void set_last_(UpdateLast last, bool pending);
  void report_(uint8_t report[UPDATE_REPORT_SIZE], UpdateReport why = UpdateReport::PROGRESS);

  Controller &c_;
  UpdateFlash &flash_;
  std::function<void(const Saved &)> save_cb_;
  crypto::Sha256 sha_;
  UpdateState state_{UpdateState::NONE};
  uint32_t size_{0};
  uint32_t received_{0};
  uint8_t want_[UPDATE_HASH_SIZE]{};
  uint32_t now_{0};
  uint32_t last_data_ms_{0};
  uint32_t reported_at_{0};    // bytes received at the last report
  uint32_t report_ms_[4]{};    // ...and when a report for an ignored chunk last went out, per reason
  bool reported_once_[4]{};
  uint32_t gap_at_{0xFFFFFFFFu};  // the position the last gap report gave: a gap at a new one is always answered
  bool answer_next_{false};       // a repeat or gap was answered: the next chunk taken is answered too
  bool restart_due_{false};
  uint32_t restart_at_ms_{0};
  bool booted_{false};
  uint32_t boot_ms_{0};
  uint32_t running_build_{0};
  Saved saved_{SAVED_CHECK, 0, 0, {0, 0}, 0};
};

}  // namespace flamekeeper
}  // namespace esphome
