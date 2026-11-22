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

class LinkClient : public Heater {
 public:
  static const uint32_t POLL_MS = 100;
  static const uint32_t DETAIL_MS = 1000;
  static const uint32_t REPLY_TIMEOUT_MS = 50;
  static const uint32_t HELLO_MS = 500;
  static const uint32_t INFO_REFRESH_MS = 30000;  // HELLO again while up
  static const uint32_t PARAM_REFRESH_MS = 250;  // one PARAM_GET this often
  static const uint8_t MISSES_DOWN = 3;

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

  // --- Heater ---
  uint32_t capabilities() const override;
  void read_status(HeaterStatus &out) const override;
  const char *firmware_text() const override { return this->info_.text; }
  Outcome set_mode(Mode mode) override;
  Outcome set_heat_mode(HeatMode mode) override;
  Outcome set_power_demand(uint8_t percent) override;
  Outcome set_target(float celsius) override;
  Outcome stop_prime() override;
  void set_room_temperature(float celsius) override { this->room_known_ = !std::isnan(celsius); }
  size_t param_count() const override;
  bool param_info(size_t index, ParamInfo &out) const override;
  bool param_get(uint16_t id, ParamValue &out) const override;
  const char *param_help(uint16_t id) const override;
  Outcome param_set(uint16_t id, int32_t value) override;
  Outcome param_apply(bool save) override;
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
  Param *param_(uint16_t id);
  const Param *param_(uint16_t id) const;
  bool params_ready_() const;
  Outcome queue_(uint8_t op, const uint8_t *payload, size_t len, uint16_t id = 0);

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

  // what the MCU told us
  link_info info_{};
  link_status status_{};
  link_detail detail_{};
  std::vector<Param> params_;
  uint8_t param_count_{0};
  size_t refresh_index_{0};

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
