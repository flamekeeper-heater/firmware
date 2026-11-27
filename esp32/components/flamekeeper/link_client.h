// The ESP32 side of the link to the safety MCU (DESIGN.md), as a heater backend:
// everything above heater.h (the controller, Home Assistant, the apps) talks to it like any other
// heater, and it talks to whatever safety MCU answers on the transport: the Flamekeeper STM32 over
// UART1, or an emulator in memory that a backend for another heater provides.
//
// Plain C++ on the shared C codec (link.h), host-tested in tests/test_link_client.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "heater.h"
#include "link.h"

namespace esphome {
namespace flamekeeper {

// Bytes to and from the safety MCU.
class LinkTransport {
 public:
  virtual ~LinkTransport() = default;
  virtual void send(const uint8_t *data, size_t len) = 0;
  // One received byte, or -1 when there is none.
  virtual int read() = 0;
};

// What the safety-MCU updater does (mcu_update.h).
enum class McuJob : uint8_t {
  UPDATE,         // install the firmware image this build carries
  READ_OPTIONS,   // read the option bytes
  WRITE_OPTIONS,  // write the option bits WDG-1 asks for (UNVERIFIED, WDG-13)
};

// The safety-MCU updater, as the link client sees it: the link client asks the MCU (UPDATE_PREPARE)
// and lends it the UART while owns_uart().
class McuUpdater {
 public:
  // The refusal of anything else while a job runs.
  static constexpr const char *BUSY_TEXT = "the heater firmware is being updated";

  virtual ~McuUpdater() = default;
  virtual bool available() const = 0;              // an image to install
  virtual const uint8_t *image_version() const = 0;  // ...and its version (major, minor, patch)
  // Whether the MCU may be taken without asking: see mcu_update.h.
  virtual bool may_take(uint32_t silent_ms) const = 0;
  // prepare: UPDATE_PREPARE has been queued, and prepared() brings its answer.
  virtual Outcome begin(McuJob job, bool prepare, uint32_t now) = 0;
  virtual void prepared(uint8_t result, const uint8_t *payload, size_t len) = 0;
  // Every loop while the link has the UART: whether it is up, and the version HELLO reported.
  virtual void on_link(bool up, const uint8_t version[3]) = 0;
  virtual void loop(uint32_t now) = 0;
  virtual bool owns_uart() const = 0;
  virtual bool running() const = 0;  // from begin() until done or failed
  virtual float progress() const = 0;  // %
  virtual const char *text() const = 0;
};

class LinkClient : public Heater {
 public:
  static const uint32_t POLL_MS = 100;
  static const uint32_t DETAIL_MS = 1000;
  static const uint32_t REPLY_TIMEOUT_MS = 50;
  static const uint32_t HELLO_MS = 500;
  static const uint32_t INFO_REFRESH_MS = 30000;  // HELLO again while up
  static const uint32_t PARAM_REFRESH_MS = 250;  // one PARAM_GET this often
  static const uint8_t MISSES_DOWN = 3;
  static const size_t QUEUE_MAX = 16;  // commands waiting for the link

  explicit LinkClient(LinkTransport *transport);

  // Call often (every loop).
  void loop(uint32_t now);

  bool up() const { return this->up_; }
  bool incompatible() const { return this->incompatible_; }
  const link_info &info() const { return this->info_; }
  const link_status &raw_status() const { return this->status_; }
  const link_detail &raw_detail() const { return this->detail_; }
  uint32_t frames_bad() const { return this->parser_.bad; }

  // The fuel and fan trims the ESP32 adds (altitude trim, the user's live trims).
  void set_trims(int fuel_pct, int fan_rpm) override;
  // Whether a PARAM_APPLY reply raises the applied / saved event (default). An emulator that
  // syncs parameters with its heater turns it off: its sync raises them itself once the heater has the values.
  void set_apply_events(bool on) { this->apply_events_ = on; }
  // Clears a latched fault on the MCU (a user reset).
  Outcome reset_fault() override;
  Outcome glow_plug_fitted() override;
  // The MCU's fault log (LINK_CAP_FAULT_LOG): read whole after connecting and whenever Status's log
  // number changes, then served from here.
  size_t fault_log(size_t from, HeaterLogEntry *out, size_t max) const override;
  Outcome clear_fault_log() override;
  // The safety-MCU updater, if this board has one (the board's reset and boot lines, and an image).
  void set_mcu_updater(McuUpdater *updater) { this->mcu_ = updater; }
  // Starts a job of the updater: with the heater off, after UPDATE_PREPARE (see mcu_update.h).
  Outcome start_mcu_job(McuJob job);

  // --- Heater ---
  uint32_t capabilities() const override;
  void read_status(HeaterStatus &out) const override;
  const char *firmware_text() const override { return this->info_.text; }
  Outcome start_firmware_update() override { return this->start_mcu_job(McuJob::UPDATE); }
  const char *firmware_update_text() const override;
  Outcome set_mode(Mode mode) override;
  Outcome set_power_demand(uint8_t percent) override;
  Outcome set_target(float celsius) override;
  Outcome stop_prime() override;
  void set_room_temperature(float celsius) override { this->room_known_ = !std::isnan(celsius); }
  size_t param_count() const override;
  bool param_info(size_t index, ParamInfo &out) const override;
  bool param_get(uint16_t id, ParamValue &out) const override;
  const char *param_help(uint16_t id) const override;
  Outcome param_set(uint16_t id, int32_t value) override;
  Outcome param_apply() override;
  Outcome param_discard() override;
  const char *param_status_text() const override;
  Outcome reset_parameters() override;

 protected:
  struct Request {
    uint8_t op;
    uint8_t payload[LINK_REQUEST_PAYLOAD_MAX];
    uint8_t len;
    uint16_t id;  // the parameter, for PARAM_SET
  };
  struct Param {
    link_param_desc desc;
    bool described{false};
    link_param_value value{};
    bool staged_here{false};  // set here, not yet confirmed by a PARAM_GET
    int32_t staged_value{0};
    std::string help;          // fetched in pieces after the table is described
    bool help_done{false};
  };

  void send_(uint8_t op, const uint8_t *payload, size_t len, uint16_t id, uint32_t now);
  void send_next_(uint32_t now);
  void on_reply_(const uint8_t *frame);
  void down_(const char *why);
  void relink_();
  Param *param_(uint16_t id);
  const Param *param_(uint16_t id) const;
  bool params_ready_() const;
  Outcome queue_(uint8_t op, const uint8_t *payload, size_t len, uint16_t id = 0);
  bool queue_full_() const { return this->queue_cmds_.size() >= QUEUE_MAX; }
  bool mcu_available_() const { return this->mcu_ != nullptr && this->mcu_->available(); }
  bool mcu_running_() const { return this->mcu_ != nullptr && this->mcu_->running(); }

  LinkTransport *transport_;
  link_parser parser_;

  // link state
  bool up_{false};
  bool incompatible_{false};
  bool have_status_{false};
  bool need_baseline_{true};
  uint8_t misses_{0};
  bool outstanding_{false};
  uint8_t out_seq_{0};
  uint8_t out_op_{0};
  uint16_t out_id_{0};
  uint32_t out_ms_{0};
  uint8_t seq_{0};
  uint32_t last_hello_ms_{0};
  uint32_t last_poll_ms_{0};
  uint32_t last_detail_ms_{0};
  uint32_t last_param_ms_{0};
  bool polled_once_{false};
  std::deque<Request> queue_cmds_;
  uint32_t heard_ms_{0};  // the last valid reply (or the first loop, before any)
  bool started_{false};

  // the safety-MCU updater
  McuUpdater *mcu_{nullptr};
  bool lent_{false};  // the UART is the updater's
  mutable char update_text_[96]{};

  // what the MCU told us
  link_info info_{};
  link_status status_{};
  link_detail detail_{};
  std::vector<Param> params_;
  uint8_t param_count_{0};
  size_t refresh_index_{0};

  // the MCU's fault log, newest first
  std::vector<link_fault_entry> log_;
  std::vector<link_fault_entry> log_reading_;  // the read under way
  bool log_known_{false};  // log_ is the MCU's log since this link came up
  bool log_busy_{false};   // a read is under way
  bool has_log_() const { return this->up_ && (this->info_.capabilities & LINK_CAP_FAULT_LOG) != 0; }
  uint8_t log_newest_() const { return this->log_.empty() ? 0 : this->log_.front().seq; }

  // what we want
  uint8_t start_{0};
  bool seen_running_{false};  // the MCU reported our start as under way
  uint8_t mode_{LINK_MODE_OFF};
  uint8_t power_{0};
  int8_t fuel_trim_{0};
  int16_t fan_trim_{0};
  bool room_known_{false};
  float target_c_{20.0f};
  uint32_t now_{0};
  mutable char status_text_[48]{};
  bool apply_events_{true};
};

}  // namespace flamekeeper
}  // namespace esphome
