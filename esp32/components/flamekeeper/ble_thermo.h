// Room temperature from Bluetooth LE thermometers: decoders for the advertisements of the
// thermometers the heater can follow. Plain C++ with no ESPHome or ESP-IDF headers, so it is
// unit-tested on the host (tests/).
//
// Adverts are untrusted radio input: every read is bounds-checked, nothing is allocated, and a
// reading only counts when it is unencrypted, has a temperature, and the temperature is within
// -60..+100 C. On false the output is left as it was.
//
// Supported:
//   BTHome v2           service data 0xFCD2 (any BTHome sensor, also LYWSD03MMC on pvvx firmware)
//   Xiaomi custom fw    service data 0x181A: pvvx "custom" (15 bytes) and ATC1441 (13 bytes)
//   Govee               manufacturer data: H5072/H5075, H5074, H5179, H5101/H5102/H5104/H5174/H5177
//   SwitchBot           service data 0x0D00/0xFD3D: Meter, Meter Plus (6-byte form); with the
//                       manufacturer data (company 0x0969) also the Outdoor Meter and Meter Pro,
//                       through parse_switchbot_meter()
//   RuuviTag            manufacturer data 0x0499: data formats 5 (RAWv2), 3 (RAWv1) and 6; service
//                       data 0xFEAA (Eddystone URL "ruu.vi/#..."): formats 2 and 4, the older tags'
//                       default "weather station" mode
//   Qingping            service data 0xFDCD (CGG1, CGDK2, CGP1W and the other Qingping sensors)
//   Xiaomi stock fw     service data 0xFE95, MiBeacon without encryption (Mi Temperature and
//                       Humidity Monitor LYWSDCGQ, and others that send unencrypted readings);
//                       encrypted MiBeacons (LYWSD03MMC on stock firmware) are refused
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace esphome {
namespace flamekeeper {

enum class ThermoKind : uint8_t { NONE = 0, BTHOME = 1, XIAOMI_CUSTOM = 2, GOVEE = 3, SWITCHBOT = 4, RUUVI = 5,
                                  QINGPING = 6, XIAOMI = 7 };
static constexpr ThermoKind THERMO_KIND_LAST = ThermoKind::XIAOMI;

struct ThermoReading {
  ThermoKind kind{ThermoKind::NONE};
  float temperature_c{NAN};
  float humidity_pct{NAN};
  int battery_pct{-1};  // -1 = not in this advert
};

// Service UUIDs and company ids the decoders know.
static constexpr uint16_t UUID_BTHOME = 0xFCD2;
static constexpr uint16_t UUID_ENVIRONMENTAL = 0x181A;  // pvvx / ATC1441 custom firmware
static constexpr uint16_t UUID_SWITCHBOT = 0xFD3D;
static constexpr uint16_t UUID_SWITCHBOT_OLD = 0x0D00;
static constexpr uint16_t COMPANY_SWITCHBOT = 0x0969;
static constexpr uint16_t COMPANY_GOVEE = 0xEC88;        // H5072/H5075, H5074
static constexpr uint16_t COMPANY_GOVEE_H5179 = 0x8801;
static constexpr uint16_t COMPANY_GOVEE_H510X = 0x0001;  // shared id; see the note in the .cpp
static constexpr uint16_t COMPANY_RUUVI = 0x0499;
static constexpr uint16_t UUID_QINGPING = 0xFDCD;
static constexpr uint16_t UUID_MIBEACON = 0xFE95;
static constexpr uint16_t UUID_EDDYSTONE = 0xFEAA;

// One service-data entry of an advert (16-bit service UUID; data without the UUID). True when it is a
// supported, unencrypted thermometer reading with a temperature.
bool parse_thermo_service_data(uint16_t uuid, const uint8_t *data, size_t len, ThermoReading &out);

// One manufacturer-specific data entry, as on air: the 2-byte company id (little-endian) first.
// SwitchBot (0x0969) is refused here: its manufacturer data does not say which device sent it, so it
// is only read together with the service data, by parse_switchbot_meter().
bool parse_thermo_manufacturer_data(const uint8_t *data, size_t len, ThermoReading &out);

// A SwitchBot meter from both parts of one advert: the service data of UUID 0xFD3D or 0x0D00 (names
// the model; may carry the reading) and the manufacturer data as on air (company 0x0969 first;
// carries the reading on newer firmware, the Outdoor Meter and the Meter Pro). Either may be null.
// The service data is required, since only it says the device is a meter.
bool parse_switchbot_meter(const uint8_t *service, size_t service_len, const uint8_t *manufacturer,
                           size_t manufacturer_len, ThermoReading &out);

// A whole advert as received (the advertising data, then any scan response), its AD structures read
// in place: the first supported reading among its service data (16-bit UUIDs), then its
// manufacturer data, a SwitchBot meter from both. name gets the local name (the complete one over a
// short one), cut to name_size - 1 characters, "" when there is none.
bool parse_thermo_advert(const uint8_t *ad, size_t len, ThermoReading &out, char *name, size_t name_size);

// "BTHome", "Xiaomi (custom firmware)", "Govee", "SwitchBot", "RuuviTag", "Qingping", "Xiaomi";
// "none" for NONE.
const char *thermo_kind_name(ThermoKind k);

}  // namespace flamekeeper
}  // namespace esphome
