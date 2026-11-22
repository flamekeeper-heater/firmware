// Bluetooth thermometers (PROTOCOL.md): the list a scan finds, the chosen
// one and its last reading. The board's Bluetooth code feeds it every decoded advert
// (ble_thermo.h); the controller takes the chosen one's temperature as a room source. Plain C++
// (host-tested in tests/test_automation.cpp).
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

#include "ble_thermo.h"

namespace esphome {
namespace flamekeeper {

struct FoundSensor {
  uint8_t address[6]{};  // as printed: [0] is the first pair
  ThermoKind kind{ThermoKind::NONE};
  int8_t rssi{0};
  float temperature_c{NAN};
  float humidity_pct{NAN};
  int battery_pct{-1};
  char name[21]{};
  uint32_t heard_ms{0};
};

class RoomSensors {
 public:
  static const size_t MAX_FOUND = 16;
  static const uint32_t STALE_MS = 10 * 60 * 1000;

  // Look for thermometers for this long (5-60 s); the list starts empty.
  void scan(uint8_t seconds, uint32_t now) {
    this->found_count_ = 0;
    this->scan_until_ms_ = now + uint32_t(seconds) * 1000;
    this->scanning_ = true;
  }
  bool scanning(uint32_t now) {
    if (this->scanning_ && int32_t(now - this->scan_until_ms_) >= 0)
      this->scanning_ = false;
    return this->scanning_;
  }
  // Whether the radio should listen at all: while scanning, or with a sensor chosen.
  bool listening(uint32_t now) { return this->scanning(now) || this->has_chosen(); }

  // One decoded advert. name may be null or empty.
  void advert(const uint8_t address[6], int8_t rssi, const char *name, const ThermoReading &r, uint32_t now) {
    if (r.kind == ThermoKind::NONE || std::isnan(r.temperature_c))
      return;
    if (this->has_chosen() && std::memcmp(address, this->chosen_.address, 6) == 0) {
      this->fill_(this->chosen_, address, rssi, name, r, now);
    }
    if (!this->scanning(now))
      return;
    for (size_t i = 0; i < this->found_count_; i++) {
      if (std::memcmp(this->found_[i].address, address, 6) == 0) {
        this->fill_(this->found_[i], address, rssi, name, r, now);
        return;
      }
    }
    if (this->found_count_ < MAX_FOUND)
      this->fill_(this->found_[this->found_count_++], address, rssi, name, r, now);
  }

  size_t found_count() const { return this->found_count_; }
  const FoundSensor &found(size_t i) const { return this->found_[i]; }

  // Choose one (an all-zero address forgets it).
  void use(const uint8_t address[6], ThermoKind kind) {
    this->chosen_ = FoundSensor{};
    std::memcpy(this->chosen_.address, address, 6);
    this->chosen_.kind = this->has_chosen() ? kind : ThermoKind::NONE;
    for (size_t i = 0; i < this->found_count_; i++)  // a fresh reading from the scan, if any
      if (std::memcmp(this->found_[i].address, address, 6) == 0)
        this->chosen_ = this->found_[i];
  }
  bool has_chosen() const {
    static const uint8_t ZERO[6] = {0, 0, 0, 0, 0, 0};
    return std::memcmp(this->chosen_.address, ZERO, 6) != 0;
  }
  bool is_chosen(const uint8_t address[6]) const {
    return this->has_chosen() && std::memcmp(address, this->chosen_.address, 6) == 0;
  }
  const FoundSensor &chosen() const { return this->chosen_; }
  // The chosen one's temperature, NAN when none is chosen or it has not been heard for STALE_MS.
  float temperature(uint32_t now) const {
    if (!this->has_chosen() || this->chosen_.heard_ms == 0 || uint32_t(now - this->chosen_.heard_ms) >= STALE_MS)
      return NAN;
    return this->chosen_.temperature_c;
  }
  // Seconds since the chosen one was heard, 0xFFFF never (or none chosen).
  uint16_t age_s(uint32_t now) const {
    if (!this->has_chosen() || this->chosen_.heard_ms == 0)
      return 0xFFFF;
    uint32_t s = uint32_t(now - this->chosen_.heard_ms) / 1000;
    return s >= 0xFFFF ? 0xFFFE : uint16_t(s);
  }

 protected:
  static void fill_(FoundSensor &f, const uint8_t address[6], int8_t rssi, const char *name, const ThermoReading &r,
                    uint32_t now) {
    std::memcpy(f.address, address, 6);
    f.kind = r.kind;
    f.rssi = rssi;
    f.temperature_c = r.temperature_c;
    if (!std::isnan(r.humidity_pct))
      f.humidity_pct = r.humidity_pct;
    if (r.battery_pct >= 0)
      f.battery_pct = r.battery_pct;
    if (name != nullptr && name[0] != '\0') {
      std::strncpy(f.name, name, sizeof(f.name) - 1);
      f.name[sizeof(f.name) - 1] = '\0';
    }
    f.heard_ms = now == 0 ? 1 : now;
  }

  FoundSensor found_[MAX_FOUND];
  size_t found_count_{0};
  FoundSensor chosen_;
  uint32_t scan_until_ms_{0};
  bool scanning_{false};
};

}  // namespace flamekeeper
}  // namespace esphome
