// A board firmware update from the app (PROTOCOL.md): the image arrives in
// chunks over Bluetooth (stop and wait: the app asks UPDATE_STATUS after each block), is written to
// the other app slot as it comes, and is
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
  // Where that slot starts in flash: the board knows the new build ran by running from there.
  virtual uint32_t update_slot() const = 0;
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
  static const uint32_t RESTART_DELAY_MS = 1000;       // UPDATE_FINISH's result goes out first
  static const uint32_t CONFIRM_MS = 60 * 1000;        // as ESPHome's safe_mode boot_is_good_after

  // Kept over restarts: the last update's result, and whether one waits to be confirmed.
  struct Saved {
    uint32_t check;
    uint8_t last;       // UpdateLast
    uint8_t pending;    // 1: installed, the restart into it not yet confirmed
    uint8_t reserved[2];
    uint32_t new_slot;  // the app slot it was written to (its flash address)
  };
  static const uint32_t SAVED_CHECK = 0x46575532UL;  // "FWU2"

  BoardUpdate(Controller &controller, UpdateFlash &flash) : c_(controller), flash_(flash) {}

  // At start-up: what was saved (nullptr: nothing), and the app slot this build runs from. Not the
  // build time: a re-install of the same build, which starts and confirms, is installed too.
  void load(const Saved *saved, uint32_t running_slot);
  void on_save(std::function<void(const Saved &)> &&cb) { this->save_cb_ = std::move(cb); }
  void loop(uint32_t now);

  // State flag bit 4: from UPDATE_BEGIN until the update ends, is dropped or the board restarts.
  bool running() const { return this->state_ != UpdateState::NONE; }

  // One data record (u32 offset, image bytes): taken only while receiving, at exactly the offset
  // expected next, with at least one byte and not past the size; anything else is ignored. Nothing
  // is answered: the app asks UPDATE_STATUS after each block.
  void data(const uint8_t *in, size_t len);

  // BoardUpdater
  Outcome update_begin(uint32_t size, const uint8_t sha256[UPDATE_HASH_SIZE], uint32_t *offset) override;
  void update_status(UpdateStatus &out) const override;
  Outcome update_finish() override;
  void update_abort() override;

 protected:
  Outcome heater_check_() const;
  void drop_(UpdateLast last);
  void set_last_(UpdateLast last, bool pending);

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
  bool restart_due_{false};
  uint32_t restart_at_ms_{0};
  bool booted_{false};
  uint32_t boot_ms_{0};
  Saved saved_{SAVED_CHECK, 0, 0, {0, 0}, 0};
};

}  // namespace flamekeeper
}  // namespace esphome
