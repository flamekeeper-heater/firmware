// The board's event log (PROTOCOL.md): its last 128 events, kept over restarts, each
// with a sequence number that only grows so an app reads only what is new. Plain C++ (host-tested
// in tests/test_automation.cpp).
#pragma once

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace flamekeeper {

enum class LogKind : uint8_t {
  RUN_STARTED = 1,     // a = mode
  STOP_ASKED = 2,
  STOPPED_BY_ITSELF = 3,  // a = fault
  FAULT = 4,           // a = fault, b = the heater's own code
  ALERT = 5,           // a = alert bit
  BOARD_STARTED = 6,   // a = reset reason
  DISARMED = 7,
  REARMED = 8,
  LOW_BATTERY_STOP = 9,  // b = supply, mV
  BURNOFF = 10,          // a = 0 started, 1 finished; b = minutes
  GLOW_READING = 11,     // b = mOhm
  TANK_SET = 12,         // a = %
  CLOCK_SET = 13,        // a = clock source
  RELAY_SESSION = 14,    // a = phone key slot, b = 0 opened, 1 closed
};

struct LogRecord {
  uint32_t seq;
  uint32_t utc;  // 0 = the time was not known
  uint8_t kind;
  uint8_t source;
  uint16_t a;
  uint32_t b;
};

class EventLog {
 public:
  static const size_t SIZE = 128;
  static const uint32_t SAVED_CHECK = 0x45564C31UL;  // "EVL1"

  struct Saved {
    uint32_t check;
    uint32_t next_seq;
    LogRecord records[SIZE];  // records[seq % SIZE]
  };

  EventLog() { this->clear(); }
  void clear() {
    this->s_ = Saved{};
    this->s_.check = SAVED_CHECK;
    this->s_.next_seq = 1;
  }
  bool load(const Saved &in) {
    if (in.check != SAVED_CHECK || in.next_seq == 0)
      return false;
    this->s_ = in;
    return true;
  }
  const Saved &saved() const { return this->s_; }
  bool dirty() const { return this->dirty_; }
  void saved_now() { this->dirty_ = false; }

  void add(LogKind kind, uint8_t source, uint16_t a, uint32_t b, uint32_t utc) {
    const uint32_t seq = this->s_.next_seq++;
    this->s_.records[seq % SIZE] = LogRecord{seq, utc, uint8_t(kind), source, a, b};
    this->dirty_ = true;
  }

  // The newest sequence number (0: empty).
  uint32_t newest() const { return this->s_.next_seq - 1; }
  uint32_t oldest() const {
    const uint32_t n = this->newest();
    return n == 0 ? 0 : (n >= SIZE ? n - SIZE + 1 : 1);
  }
  // Up to `max` records from `first` on (or the oldest kept); returns how many.
  size_t read(uint32_t first, LogRecord *out, size_t max) const {
    if (this->newest() == 0)
      return 0;
    uint32_t seq = first < this->oldest() ? this->oldest() : first;
    size_t n = 0;
    while (n < max && seq <= this->newest()) {
      out[n++] = this->s_.records[seq % SIZE];
      seq++;
    }
    return n;
  }

 protected:
  Saved s_;
  bool dirty_{false};
};

}  // namespace flamekeeper
}  // namespace esphome
