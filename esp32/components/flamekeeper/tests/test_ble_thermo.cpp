// Host unit test for the Bluetooth thermometer advert decoders. Build and run with tests/run.sh.
//
// Vectors come from the decoders' own projects where they have one; the source is noted next to
// each. "constructed" marks a vector written by hand from the documented layout.
#include "../ble_thermo.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static int checks = 0, failures = 0;

#define CHECK(cond) \
  do { \
    checks++; \
    if (!(cond)) { \
      failures++; \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
  } while (0)

#define CHECK_NEAR(a, b, tol) \
  do { \
    checks++; \
    double va_ = (a), vb_ = (b); \
    if (!(std::fabs(va_ - vb_) <= (tol))) { \
      failures++; \
      printf("FAIL %s:%d: %s = %g, expected %s = %g (+-%g)\n", __FILE__, __LINE__, #a, va_, #b, vb_, double(tol)); \
    } \
  } while (0)

using namespace esphome::flamekeeper;

static std::vector<uint8_t> hex(const char *s) {
  std::vector<uint8_t> v;
  auto nib = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
  for (; s[0] && s[1]; s += 2)
    v.push_back(uint8_t(nib(s[0]) << 4 | nib(s[1])));
  return v;
}

static bool svc(uint16_t uuid, const char *h, ThermoReading &r) {
  auto v = hex(h);
  return parse_thermo_service_data(uuid, v.data(), v.size(), r);
}

static bool mfr(const char *h, ThermoReading &r) {
  auto v = hex(h);
  return parse_thermo_manufacturer_data(v.data(), v.size(), r);
}

static bool switchbot(const char *service, const char *manufacturer, ThermoReading &r) {
  auto s = hex(service), m = hex(manufacturer);
  return parse_switchbot_meter(s.empty() ? nullptr : s.data(), s.size(), m.empty() ? nullptr : m.data(),
                               m.size(), r);
}

// Expect a reading; humidity < 0 means "not in the advert".
static void expect(bool ok, const ThermoReading &r, ThermoKind kind, double t, double h, int battery, int line) {
  checks++;
  bool good = ok && r.kind == kind && std::fabs(r.temperature_c - t) <= 0.005 &&
              (h < 0 ? std::isnan(r.humidity_pct) : std::fabs(r.humidity_pct - h) <= 0.005) &&
              r.battery_pct == battery;
  if (!good) {
    failures++;
    printf("FAIL line %d: ok=%d kind=%d t=%g h=%g batt=%d, expected kind=%d t=%g h=%g batt=%d\n", line, ok,
           int(r.kind), r.temperature_c, r.humidity_pct, r.battery_pct, int(kind), t, h, battery);
  }
}
#define EXPECT(ok, r, kind, t, h, b) expect(ok, r, kind, t, h, b, __LINE__)

static void test_bthome() {
  ThermoReading r;
  // Theengs decoder tests/BLE/test_ble.cpp, LYWSD03MMC_PVVX_BTHOME_1: packet id, battery, temp, humidity.
  EXPECT(svc(0xFCD2, "4000840164021f0803141c", r), r, ThermoKind::BTHOME, 20.79, 71.88, 100);
  // bthome.io/format worked example: 0x02 C409 = 25.00 C, 0x03 BF13 = 50.55 %.
  r = {};
  EXPECT(svc(0xFCD2, "4002c40903bf13", r), r, ThermoKind::BTHOME, 25.00, 50.55, -1);
  // bthome.io/format table examples: 0x45 1101 = 27.3 C; 0x57 EA = -22 C; 0x58 EA = -7.7 C; 0x2E 23 = 35 %.
  r = {};
  EXPECT(svc(0xFCD2, "402e23451101", r), r, ThermoKind::BTHOME, 27.3, 35, -1);
  r = {};
  EXPECT(svc(0xFCD2, "4057ea", r), r, ThermoKind::BTHOME, -22.0, -1, -1);
  r = {};
  EXPECT(svc(0xFCD2, "4058ea", r), r, ThermoKind::BTHOME, -7.7, -1, -1);
  // constructed: 0x02 18FC = -10.00 C, battery 0x01 61 = 97 % (spec example value).
  r = {};
  EXPECT(svc(0xFCD2, "40016102" "18fc", r), r, ThermoKind::BTHOME, -10.00, -1, 97);

  // Unknown object id (0x7F) after the temperature: the walk stops there, the temperature counts,
  // the humidity behind it is not read. constructed
  r = {};
  EXPECT(svc(0xFCD2, "4002c4097f0103bf13", r), r, ThermoKind::BTHOME, 25.00, -1, -1);
  // Unknown id before the temperature: nothing to read. constructed
  r = {};
  CHECK(!svc(0xFCD2, "407f0102c409", r));

  // Variable-length objects are walked over: text 0x53 (bthome.io example "Hello World!"),
  // command 0x3B (bthome.io example 3B010305), raw 0x54 with length 0. constructed around them
  r = {};
  EXPECT(svc(0xFCD2, "4002c409" "530c48656c6c6f20576f726c6421" "3b010305" "5400" "03bf13", r), r,
         ThermoKind::BTHOME, 25.00, 50.55, -1);
  // Text whose length runs past the end: stop, keep the temperature. constructed
  r = {};
  EXPECT(svc(0xFCD2, "4002c4095320414243", r), r, ThermoKind::BTHOME, 25.00, -1, -1);
  // A first temperature wins over a later one. constructed
  r = {};
  EXPECT(svc(0xFCD2, "4002c409451101", r), r, ThermoKind::BTHOME, 25.00, -1, -1);

  // Encrypted (device info bit 0): Theengs test_ble.cpp, LYWSD03MMC_PVVX_BTHOME_1_ENCR.
  r = {};
  CHECK(!svc(0xFCD2, "41be07860de133b3422302000050dea27d", r));
  CHECK(r.kind == ThermoKind::NONE && std::isnan(r.temperature_c));  // untouched on refusal
  // No temperature: Theengs test_ble.cpp, LYWSD03MMC_PVVX_BTHOME_2 (voltage, power, opening).
  CHECK(!svc(0xFCD2, "4000810cda0b10011101", r));
  // Version 1 in the device info byte, an otherwise good advert. constructed
  CHECK(!svc(0xFCD2, "2002c409", r));
  // Too short / truncated object. constructed
  CHECK(!svc(0xFCD2, "", r));
  CHECK(!svc(0xFCD2, "40", r));
  CHECK(!svc(0xFCD2, "4002c4", r));
  // Impossible value: 0x02 2C2A = 107.96 C. constructed
  CHECK(!svc(0xFCD2, "40022c2a", r));
  CHECK(r.kind == ThermoKind::NONE);
}

static void test_xiaomi_custom() {
  ThermoReading r;
  // pvvx custom format, Theengs test_ble.cpp LYWSD03MMC_PVVX: 21.12 C, 50.53 %, 100 %.
  EXPECT(svc(0x181A, "5601cf38c1a44008bd13470c64cc0f", r), r, ThermoKind::XIAOMI_CUSTOM, 21.12, 50.53, 100);
  // Same source, negative: -6.70 C.
  r = {};
  EXPECT(svc(0x181A, "5601cf38c1a462fdbd13470c64cc0f", r), r, ThermoKind::XIAOMI_CUSTOM, -6.70, 50.53, 100);
  // ATC1441 format, Theengs test_ble.cpp LYWSD03MMC_ATC: 32.5 C, 62 %, 81 %.
  r = {};
  EXPECT(svc(0x181A, "a4c138d5d49801453e510b7b62", r), r, ThermoKind::XIAOMI_CUSTOM, 32.5, 62, 81);
  // Same source, negative: -4.5 C.
  r = {};
  EXPECT(svc(0x181A, "a4c138d5d498ffd33e510b7b62", r), r, ThermoKind::XIAOMI_CUSTOM, -4.5, 62, 81);
  // pvvx encrypted custom format (11 bytes), Theengs test_ble.cpp LYWSD03MMC_PVVX_ENCR: refused.
  CHECK(!svc(0x181A, "23ef56583dd42050fe8e4d", r));
  // Too short and one byte long. constructed
  CHECK(!svc(0x181A, "a4c138d5d49801453e510b7b", r));
  CHECK(!svc(0x181A, "5601cf38c1a44008bd13470c64cc0f00", r));
  // Impossible: ATC1441 temp 0x7FFF. constructed
  CHECK(!svc(0x181A, "a4c138d5d4987fff3e510b7b62", r));
}

static void test_govee() {
  ThermoReading r;
  // H5072/H5075, govee-ble tests/test_parser.py GVH5072_SERVICE_INFO (company 60552 = 0xEC88 put in
  // front): 21.6 C, 49.8 %, 100 %.
  EXPECT(mfr("88ec" "00034db26400", r), r, ThermoKind::GOVEE, 21.6, 49.8, 100);
  // govee-ble GVH5075_SERVICE_INFO_OTHER_VALUES: 20.0 C, 64.9 %, 53 %.
  r = {};
  EXPECT(mfr("88ec" "00030fc93500", r), r, ThermoKind::GOVEE, 20.0, 64.9, 53);
  // Negative, Theengs test_ble.cpp H5075 "88ec00811f096400": -7.3 C (govee-ble's rounding), 48.1 %.
  r = {};
  EXPECT(mfr("88ec00811f096400", r), r, ThermoKind::GOVEE, -7.3, 48.1, 100);
  // govee-ble GVH5075_SERVICE_INFO_NEGATIVE_VALUES: decodes to -393.2 C, which govee-ble reports as
  // an error; refused here by the range check.
  CHECK(!mfr("88ec" "00bc00043e27", r));
  // Error flag (battery bit 7) set on the 21.6 C advert. constructed
  CHECK(!mfr("88ec00034db2e400", r));

  // H5074, govee-ble GVH5074_SERVICE_INFO: 25.34 C, 47.96 %, 100 %.
  r = {};
  EXPECT(mfr("88ec" "00e609bc126402", r), r, ThermoKind::GOVEE, 25.34, 47.96, 100);
  // Negative, Theengs test_ble.cpp H5074 "88ec00a0facc176402": -13.76 C, 60.92 %.
  r = {};
  EXPECT(mfr("88ec00a0facc176402", r), r, ThermoKind::GOVEE, -13.76, 60.92, 100);

  // H5179, govee-ble GVH5179_SERVICE_INFO (company 34817 = 0x8801): 25.7 C, 17.0 %, 100 %.
  r = {};
  EXPECT(mfr("0188" "ec0001010a0aa40664", r), r, ThermoKind::GOVEE, 25.7, 17.0, 100);
  // Theengs test_ble.cpp H5179 "0188ec000101ee07581641": 20.3 C, 57.2 %, 65 %.
  r = {};
  EXPECT(mfr("0188ec000101ee07581641", r), r, ThermoKind::GOVEE, 20.3, 57.2, 65);
  // Negative: -5.00 C (0xFE0C). constructed from the layout above
  r = {};
  EXPECT(mfr("0188ec0001010cfe581641", r), r, ThermoKind::GOVEE, -5.00, 57.2, 65);

  // H5102, Theengs test_ble.cpp "0100010103590e64": 21.9 C, 40.6 %, 100 %.
  r = {};
  EXPECT(mfr("0100010103590e64", r), r, ThermoKind::GOVEE, 21.9, 40.6, 100);
  // H5177, govee-ble GVH5177_SERVICE_INFO (company 1, first 6 bytes): 21.0 C, 47.0 %, 100 %.
  r = {};
  EXPECT(mfr("0100" "010103362664", r), r, ThermoKind::GOVEE, 21.0, 47.0, 100);
  // H5174 negative, Theengs test_ble.cpp "01000101811a6764": -7.2 C, 29.5 %.
  r = {};
  EXPECT(mfr("01000101811a6764", r), r, ThermoKind::GOVEE, -7.2, 29.5, 100);
  // Company 0x0001 without the 01 01 Govee marker: refused. constructed
  CHECK(!mfr("0100020103590e64", r));

  // Unknown company: Apple, Theengs test_ble.cpp "4c0010050b182068dd".
  CHECK(!mfr("4c0010050b182068dd", r));
  // Govee id, wrong length (H5075 one byte short / one long; H5179 short). constructed
  CHECK(!mfr("88ec00034db264", r));
  CHECK(!mfr("88ec00034db2640000", r));  // 9 bytes is H5074's layout: 197.15 C, refused
  CHECK(!mfr("0188ec000101ee075816", r));
  // Too short. constructed
  CHECK(!mfr("", r));
  CHECK(!mfr("88", r));
  CHECK(!mfr("88ec", r));
  CHECK(!parse_thermo_manufacturer_data(nullptr, 8, r));
}

static void test_switchbot() {
  ThermoReading r;
  // Meter (T) on 0xFD3D, pySwitchbot tests/test_adv_parser.py test_wosensor_active: 24.6 C, 53 %, 100 %.
  EXPECT(svc(0xFD3D, "5400e4069835", r), r, ThermoKind::SWITCHBOT, 24.6, 53, 100);
  // Meter on 0x0D00, Theengs test_ble.cpp Switchbot_Meter "540054459938": 25.5 C, 56 %, 84 %.
  r = {};
  EXPECT(svc(0x0D00, "540054459938", r), r, ThermoKind::SWITCHBOT, 25.5, 56, 84);
  // Meter Plus (i), Theengs test_ble.cpp Switchbot_Meter_Plus "6900ba18993b": 25.8 C, 59 %, 58 %.
  r = {};
  EXPECT(svc(0xFD3D, "6900ba18993b", r), r, ThermoKind::SWITCHBOT, 25.8, 59, 58);
  // Negative, same source "6900ba031938": -25.3 C, 56 %.
  r = {};
  EXPECT(svc(0xFD3D, "6900ba031938", r), r, ThermoKind::SWITCHBOT, -25.3, 56, 58);

  // Outdoor Meter (w): reading in the manufacturer data. Theengs test_ble.cpp "SwitchBot Outdoor Meter":
  r = {};
  EXPECT(switchbot("770064", "6909aabbccddeeff8b0305993200", r), r, ThermoKind::SWITCHBOT, 25.5, 50, 100);
  r = {};
  EXPECT(switchbot("770041", "6909aabbccddeeffe30f090f2a00", r), r, ThermoKind::SWITCHBOT, -15.9, 42, 65);
  // pySwitchbot test_woiosensor_passive_and_active (company 2409 = 0x0969 put in front): 24.6 C, 53 %.
  r = {};
  EXPECT(switchbot("7700e4", "6909" "aabbccddeeffe00f06983500", r), r, ThermoKind::SWITCHBOT, 24.6, 53, 100);
  // Meter Pro (4), Theengs test_ble.cpp "SwitchBot Meter Pro (NO CO2)": 21.8 C, 61 %, 100 %.
  r = {};
  EXPECT(switchbot("340064", "6909b0e9fe9a0166886408953d001a", r), r, ThermoKind::SWITCHBOT, 21.8, 61, 100);
  // Meter Pro CO2 (5), pySwitchbot test_meter_pro_c_active: 27.7 C, 36 %, 100 %.
  r = {};
  EXPECT(switchbot("350064", "6909" "b0e9fe543215b7e4079ba4003702d500", r), r, ThermoKind::SWITCHBOT, 27.7, 36, 100);
  // Meter, both parts, pySwitchbot test_wosensor_passive_and_active: the manufacturer data wins.
  r = {};
  EXPECT(switchbot("5400e4069835", "6909" "d7c17d5deb43de03069835", r), r, ThermoKind::SWITCHBOT, 24.6, 53, 100);

  // Outdoor Meter service data alone has no reading: Theengs test_ble.cpp "770041".
  r = {};
  CHECK(!svc(0xFD3D, "770041", r));
  // SwitchBot manufacturer data alone does not say the sender is a meter: refused.
  CHECK(!mfr("6909aabbccddeeff8b0305993200", r));
  CHECK(!switchbot("", "6909aabbccddeeff8b0305993200", r));
  // Not a meter: Curtain (c), pySwitchbot test_parse_advertisement_data_curtain, both parts.
  CHECK(!svc(0xFD3D, "63c058001104", r));
  CHECK(!switchbot("63c058001104", "6909" "e7ab46ac8f927c0f001104", r));
  // All zero (no measurement yet), pySwitchbot test_wosensor_active_zero_data.
  CHECK(!svc(0xFD3D, "540000000000", r));
  // Too short. constructed
  CHECK(!svc(0xFD3D, "", r));
  CHECK(!svc(0xFD3D, "5400e40698", r));
  // Manufacturer data too short falls back to the service data, which has no reading. constructed
  CHECK(!switchbot("770064", "6909aabbccddeeff8b0305", r));
  // Other company id in the manufacturer slot is ignored. constructed
  CHECK(!switchbot("770064", "6a09aabbccddeeff8b0305993200", r));
  CHECK(r.kind == ThermoKind::NONE);
}

// Every positive vector, cut to each shorter length in an exact-size buffer: the sanitizers catch any
// read past the end.
static void test_ruuvi() {
  ThermoReading r;
  // Data format 5, Ruuvi's documented example "0512FC5394C37C0004FFFC040CAC364200CDCBB8334C884F"
  // (company 0x0499 put in front): 24.3 C, 53.49 %, 2977 mV (95 % on the 2.5-3.0 V scale).
  EXPECT(mfr("9904" "0512FC5394C37C0004FFFC040CAC364200CDCBB8334C884F", r), r, ThermoKind::RUUVI, 24.3, 53.49, 95);
  // Negative: -10.0 C is -2000 x 0.005 = 0xF830. constructed
  r = {};
  EXPECT(mfr("9904" "05F8305394C37C0004FFFC040CAC364200CDCBB8334C884F", r), r, ThermoKind::RUUVI, -10.0, 53.49, 95);
  // Temperature not available (0x8000): no reading.
  CHECK(!mfr("9904" "0580005394C37C0004FFFC040CAC364200CDCBB8334C884F", r));
  // Data format 3, Ruuvi's documented example "03291A1ECE1EFC18F94202CA0B53": 26.3 C, 20.5 %,
  // 2899 mV (79 %).
  r = {};
  EXPECT(mfr("9904" "03291A1ECE1EFC18F94202CA0B53", r), r, ThermoKind::RUUVI, 26.3, 20.5, 79);
  // Format 3 below zero (sign bit): -1.25 C. constructed
  r = {};
  EXPECT(mfr("9904" "03298119CE1EFC18F94202CA0B53", r), r, ThermoKind::RUUVI, -1.25, 20.5, 79);
  // Format 6 (Ruuvi's air sensors): temperature and humidity as in format 5. constructed
  r = {};
  EXPECT(mfr("9904" "0612FC5394C37C0004FFFC040CAC364200CDCBB8", r), r, ThermoKind::RUUVI, 24.3, 53.49, -1);
  // Weather station mode, Eddystone URL https://ruu.vi/#BEAVAMFKe (Ruuvi's format 4 example):
  // 21 C, 32 percent.
  r = {};
  EXPECT(svc(0xFEAA, "10f8037275752e76692f2342454156414d464b65", r), r, ThermoKind::RUUVI, 21.0, 32.0, -1);
  // Format 2, -5 C, 40 percent. constructed
  r = {};
  EXPECT(svc(0xFEAA, "10f8037275752e76692f23416c4346414d464b", r), r, ThermoKind::RUUVI, -5.0, 40.0, -1);
  // Another site's Eddystone URL, or a cut one: nothing.
  CHECK(!svc(0xFEAA, "10f8036578616d706c652e636f6d2f2342454156414d464b", r));
  CHECK(!svc(0xFEAA, "1003" "727575", r));
  // A Ruuvi advert cut short: nothing.
  CHECK(!mfr("9904" "0612FC5394C37C0004FFFC", r));
  CHECK(!mfr("9904" "0512FC5394", r));
}

static void test_qingping_and_mibeacon() {
  ThermoReading r;
  // Qingping CGG1: frame control 0x88, product 0x16, MAC, then 01 04 (temp 0x00EB = 23.5 C, humidity
  // 0x01C2 = 45.0 %) and 02 01 (battery 87 %). constructed
  EXPECT(svc(0xFDCD, "8816" "aabbccddeeff" "0104eb00c201" "020157", r), r, ThermoKind::QINGPING, 23.5, 45.0, 87);
  // Below zero (-3.2 C = 0xFFE0), no battery entry, an unknown entry first (skipped). constructed
  r = {};
  EXPECT(svc(0xFDCD, "8816" "aabbccddeeff" "0f0101" "0104e0ffc201", r), r, ThermoKind::QINGPING, -3.2, 45.0, -1);
  // Only a battery: no reading.
  CHECK(!svc(0xFDCD, "8816" "aabbccddeeff" "020157", r));

  // MiBeacon v2 (LYWSDCGQ): frame control 0x2050 (object, MAC), product 0x01AA, counter, MAC,
  // object 0x100D: 21.7 C (0x00D9), 53.0 % (0x0212). constructed
  r = {};
  EXPECT(svc(0xFE95, "5020" "aa01" "07" "aabbccddeeff" "0d1004" "d9001202", r), r, ThermoKind::XIAOMI, 21.7, 53.0, -1);
  // Temperature alone (0x1004): -0.5 C.
  r = {};
  EXPECT(svc(0xFE95, "5020" "aa01" "08" "aabbccddeeff" "041002" "fbff", r), r, ThermoKind::XIAOMI, -0.5, -1, -1);
  // Humidity or battery alone: no reading; encrypted (bit 3): refused.
  CHECK(!svc(0xFE95, "5020" "aa01" "09" "aabbccddeeff" "061002" "1202", r));
  CHECK(!svc(0xFE95, "5820" "aa01" "07" "aabbccddeeff" "0d1004" "d9001202", r));
  // With a capability byte (bit 5) before the object. constructed
  r = {};
  EXPECT(svc(0xFE95, "7020" "aa01" "07" "aabbccddeeff" "08" "0d1004" "d9001202", r), r, ThermoKind::XIAOMI, 21.7, 53.0, -1);
}

static void test_truncation() {
  struct Svc {
    uint16_t uuid;
    const char *hex;
  };
  const Svc svcs[] = {
      {0xFCD2, "4000840164021f0803141c"},
      {0xFCD2, "4002c409" "530c48656c6c6f20576f726c6421" "3b010305" "5400" "03bf13"},
      {0x181A, "5601cf38c1a44008bd13470c64cc0f"},
      {0x181A, "a4c138d5d49801453e510b7b62"},
      {0xFD3D, "5400e4069835"},
      {0xFDCD, "8816aabbccddeeff0104eb00c201020157"},
      {0xFEAA, "10f8037275752e76692f2342454156414d464b65"},
      {0xFE95, "7020aa0107aabbccddeeff080d1004d9001202"},
  };
  const char *mfrs[] = {"99040512FC5394C37C0004FFFC040CAC364200CDCBB8334C884F", "990403291A1ECE1EFC18F94202CA0B53", "88ec00034db26400", "88ec00e609bc126402", "0188ec000101ee07581641", "0100010103590e64"};
  ThermoReading r;
  for (const auto &s : svcs) {
    auto full = hex(s.hex);
    for (size_t n = 0; n <= full.size(); n++) {
      std::vector<uint8_t> cut(full.begin(), full.begin() + n);
      parse_thermo_service_data(s.uuid, cut.data(), cut.size(), r);
    }
  }
  for (const char *m : mfrs) {
    auto full = hex(m);
    for (size_t n = 0; n <= full.size(); n++) {
      std::vector<uint8_t> cut(full.begin(), full.begin() + n);
      parse_thermo_manufacturer_data(cut.data(), cut.size(), r);
    }
  }
  auto s = hex("770064"), m = hex("6909aabbccddeeff8b0305993200");
  for (size_t a = 0; a <= s.size(); a++)
    for (size_t b = 0; b <= m.size(); b++) {
      std::vector<uint8_t> cs(s.begin(), s.begin() + a), cm(m.begin(), m.begin() + b);
      parse_switchbot_meter(cs.data(), cs.size(), cm.data(), cm.size(), r);
    }
  // Every BTHome object id after a temperature, with a short tail: the walk never overruns.
  for (int id = 0; id < 256; id++) {
    uint8_t buf[] = {0x40, 0x02, 0xC4, 0x09, uint8_t(id), 0x1F};
    for (size_t n = 5; n <= sizeof(buf); n++) {
      std::vector<uint8_t> cut(buf, buf + n);
      ThermoReading t;
      CHECK(parse_thermo_service_data(0xFCD2, cut.data(), cut.size(), t) && std::fabs(t.temperature_c - 25.0f) < 0.001f);
    }
  }
}

static void test_names() {
  CHECK(strcmp(thermo_kind_name(ThermoKind::BTHOME), "BTHome") == 0);
  CHECK(strcmp(thermo_kind_name(ThermoKind::XIAOMI_CUSTOM), "Xiaomi (custom firmware)") == 0);
  CHECK(strcmp(thermo_kind_name(ThermoKind::GOVEE), "Govee") == 0);
  CHECK(strcmp(thermo_kind_name(ThermoKind::SWITCHBOT), "SwitchBot") == 0);
  CHECK(strcmp(thermo_kind_name(ThermoKind::RUUVI), "RuuviTag") == 0);
  CHECK(strcmp(thermo_kind_name(ThermoKind::QINGPING), "Qingping") == 0);
  CHECK(strcmp(thermo_kind_name(ThermoKind::XIAOMI), "Xiaomi") == 0);
  CHECK(strcmp(thermo_kind_name(ThermoKind::NONE), "none") == 0);
}

// Whole adverts, AD structures as on air (length, type, data), read in place.
static bool advert(const char *h, ThermoReading &r, char *name, size_t name_size) {
  std::vector<uint8_t> v = hex(h);
  return parse_thermo_advert(v.data(), v.size(), r, name, name_size);
}

static void test_advert() {
  ThermoReading r;
  char name[21];
  // Flags, BTHome service data, then the complete name.
  EXPECT(advert("020106" "0e16d2fc" "4000840164021f0803141c" "0709" "4c6f756e6765", r, name, sizeof(name)), r,
         ThermoKind::BTHOME, 20.79, 71.88, 100);
  CHECK(strcmp(name, "Lounge") == 0);
  // The name: complete over short, cut to fit.
  r = {};
  CHECK(advert("0408" "616263" "0616d2fc" "4057ea" "0709" "4c6f756e6765", r, name, sizeof(name)));
  CHECK(strcmp(name, "Lounge") == 0);
  CHECK_NEAR(r.temperature_c, -22.0, 0.01);
  char small[4];
  CHECK(advert("0709" "4c6f756e6765" "0616d2fc" "4057ea", r, small, sizeof(small)));
  CHECK(strcmp(small, "Lou") == 0);
  // Govee manufacturer data.
  r = {};
  EXPECT(advert("020106" "09ff" "88ec00034db26400", r, name, sizeof(name)), r, ThermoKind::GOVEE, 21.6, 49.8, 100);
  CHECK(name[0] == '\0');
  // SwitchBot: the service data (here after the manufacturer data) names the meter, the
  // manufacturer data carries the reading.
  r = {};
  EXPECT(advert("0fff" "6909aabbccddeeff8b0305993200" "06163dfd" "770064", r, name, sizeof(name)), r,
         ThermoKind::SWITCHBOT, 25.5, 50, 100);
  // Not a thermometer; zero padding ends the data; a structure cut short is not read.
  r = {};
  CHECK(!advert("020106" "03ff" "4c00", r, name, sizeof(name)));
  CHECK(std::isnan(r.temperature_c));
  CHECK(!advert("020106" "0000" "0616d2fc4057ea", r, name, sizeof(name)));
  CHECK(!advert("0716d2fc4057ea", r, name, sizeof(name)));
  CHECK(!parse_thermo_advert(nullptr, 5, r, name, sizeof(name)));
  // Every cut of a good advert is safe.
  std::vector<uint8_t> v = hex("0fff6909aabbccddeeff8b0305993200" "06163dfd770064" "0709" "4c6f756e6765");
  for (size_t n = 0; n <= v.size(); n++) {
    std::vector<uint8_t> cut(v.begin(), v.begin() + n);
    parse_thermo_advert(cut.data(), cut.size(), r, name, sizeof(name));
  }
}

int main() {
  test_bthome();
  test_xiaomi_custom();
  test_govee();
  test_switchbot();
  test_ruuvi();
  test_qingping_and_mibeacon();
  test_truncation();
  test_names();
  test_advert();
  printf("ble_thermo: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
