#include "ble_thermo.h"
#include "bytes.h"

#include <cstring>

namespace esphome {
namespace flamekeeper {

namespace {

constexpr float MIN_TEMP_C = -60.0f;
constexpr float MAX_TEMP_C = 100.0f;

int16_t s16_le(const uint8_t *p) { return int16_t(get_u16(p)); }
int16_t s16_be(const uint8_t *p) { return int16_t(uint16_t((p[0] << 8) | p[1])); }

// Copies a decoded reading to out when it is plausible; impossible values are noise, not a reading.
bool accept(ThermoKind kind, float t, float h, int battery, ThermoReading &out) {
  if (!(t >= MIN_TEMP_C && t <= MAX_TEMP_C))
    return false;
  out.kind = kind;
  out.temperature_c = t;
  out.humidity_pct = (h >= 0.0f && h <= 100.0f) ? h : NAN;
  out.battery_pct = (battery >= 0 && battery <= 100) ? battery : -1;
  return true;
}

// ---- BTHome v2 (https://bthome.io/format/) ----

// Bytes of value after a BTHome object id, from the spec's tables (sensor, binary sensor, events,
// device information); -1 for an id the spec does not list. Text (0x53), raw (0x54) and command
// (0x3B) carry their own length and are handled by the caller.
int bthome_value_len(uint8_t id) {
  switch (id) {
    case 0x00:  // packet id
    case 0x01:  // battery
    case 0x09:  // count uint8
    case 0x2E:  // humidity uint8
    case 0x2F:  // moisture uint8
    case 0x3A:  // button event
    case 0x46:  // UV index
    case 0x57:  // temperature sint8
    case 0x58:  // temperature sint8 x0.35
    case 0x59:  // count sint8
    case 0x60:  // channel
    case 0x64:  // light level
    case 0x65:  // settings revision
      return 1;
    case 0x02: case 0x03: case 0x06: case 0x07: case 0x08: case 0x0C: case 0x0D: case 0x0E:
    case 0x12: case 0x13: case 0x14: case 0x3C: case 0x3D: case 0x3F: case 0x40: case 0x41:
    case 0x43: case 0x44: case 0x45: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x51:
    case 0x52: case 0x56: case 0x5A: case 0x5D: case 0x5E: case 0x5F: case 0x61: case 0xF0:
      return 2;
    case 0x04: case 0x05: case 0x0A: case 0x0B: case 0x42: case 0x4B: case 0xF2:
      return 3;
    case 0x3E: case 0x4C: case 0x4D: case 0x4E: case 0x4F: case 0x50: case 0x55: case 0x5B:
    case 0x5C: case 0x62: case 0x63: case 0xF1:
      return 4;
    default:
      // Binary sensors 0x0F..0x11 and 0x15..0x2D are one byte each.
      if ((id >= 0x0F && id <= 0x11) || (id >= 0x15 && id <= 0x2D))
        return 1;
      return -1;
  }
}

// Device info byte, then objects. The spec says a receiver stops at the first id it does not know;
// whatever was read before it still counts. The first temperature, humidity and battery win.
bool parse_bthome(const uint8_t *d, size_t len, ThermoReading &out) {
  if (len < 1)
    return false;
  uint8_t info = d[0];
  if (info & 0x01)  // encrypted: no key, so nothing to read
    return false;
  if ((info >> 5) != 2)
    return false;
  float t = NAN, h = NAN;
  int battery = -1;
  size_t i = 1;
  while (i < len) {
    uint8_t id = d[i++];
    size_t n;
    if (id == 0x53 || id == 0x54) {  // text, raw: length byte, then that many bytes
      if (i >= len)
        break;
      n = 1 + size_t(d[i]);
    } else if (id == 0x3B) {  // command: length byte (low 5 bits = argument bytes), opcode, arguments
      if (i >= len)
        break;
      n = 2 + size_t(d[i] & 0x1F);
    } else {
      int v = bthome_value_len(id);
      if (v < 0)
        break;
      n = size_t(v);
    }
    if (n > len - i)  // truncated object
      break;
    const uint8_t *p = d + i;
    switch (id) {
      case 0x01:
        if (battery < 0)
          battery = p[0];
        break;
      case 0x02:
        if (std::isnan(t))
          t = s16_le(p) * 0.01f;
        break;
      case 0x45:
        if (std::isnan(t))
          t = s16_le(p) * 0.1f;
        break;
      case 0x57:
        if (std::isnan(t))
          t = float(int8_t(p[0]));
        break;
      case 0x58:
        if (std::isnan(t))
          t = float(int8_t(p[0])) * 0.35f;
        break;
      case 0x03:
        if (std::isnan(h))
          h = get_u16(p) * 0.01f;
        break;
      case 0x2E:
        if (std::isnan(h))
          h = float(p[0]);
        break;
      default:
        break;
    }
    i += n;
  }
  return accept(ThermoKind::BTHOME, t, h, battery, out);
}

// ---- LYWSD03MMC custom firmware on 0x181A (https://github.com/pvvx/ATC_MiThermometer) ----

// pvvx "custom", 15 bytes, little-endian: MAC[6] (reversed), int16 temp x0.01, uint16 humidity
// x0.01, uint16 battery mV, uint8 battery %, uint8 counter, uint8 flags.
// ATC1441, 13 bytes, big-endian: MAC[6], int16 temp x0.1, uint8 humidity %, uint8 battery %,
// uint16 battery mV, uint8 counter.
// The encrypted variants on the same UUID are 8 and 11 bytes and so are refused by length.
bool parse_xiaomi_custom(const uint8_t *d, size_t len, ThermoReading &out) {
  if (len == 15)
    return accept(ThermoKind::XIAOMI_CUSTOM, s16_le(d + 6) * 0.01f, get_u16(d + 8) * 0.01f, d[12], out);
  if (len == 13)
    return accept(ThermoKind::XIAOMI_CUSTOM, s16_be(d + 6) * 0.1f, float(d[8]), d[9], out);
  return false;
}

// ---- SwitchBot (pySwitchbot adv_parsers/meter.py and _sensor_th.py; Theengs SBMT_json.h) ----

// Service data byte 0 (bit 7 masked off) names the model. These are the meters whose reading is the
// 3-byte block below: Meter (T), Meter Plus (i), Outdoor Meter (w), Meter Pro (4), Meter Pro CO2 (5),
// each also seen with bit 5 flipped.
bool switchbot_is_meter(uint8_t model_byte) {
  switch (model_byte & 0x7F) {
    case 'T': case 't': case 'i': case 'I': case 'w': case 'W':
    case '4': case 0x14: case '5': case 0x15:
      return true;
    default:
      return false;
  }
}

// 3 bytes: [0] bits 3..0 = tenths of a degree; [1] bit 7 = sign (1 = positive), bits 6..0 = whole
// degrees; [2] bits 6..0 = humidity % (bit 7 = the display shows Fahrenheit).
bool switchbot_th(const uint8_t *p, int battery, ThermoReading &out) {
  float t = float(p[1] & 0x7F) + float(p[0] & 0x0F) / 10.0f;
  if (!(p[1] & 0x80))
    t = -t;
  float h = float(p[2] & 0x7F);
  if (t == 0.0f && h == 0.0f)  // all zero: the meter has no measurement yet (as pySwitchbot)
    return false;
  return accept(ThermoKind::SWITCHBOT, t, h, battery, out);
}

// ---- Govee (govee-ble parser.py; Theengs H5072/H5074/H5102/H5179_json.h) ----

// 3 bytes big-endian: bit 23 = negative, the rest = temp_tenths * 1000 + humidity_tenths.
bool govee_packed(const uint8_t *p, uint8_t battery_byte, ThermoReading &out) {
  if (battery_byte & 0x80)  // the sensor's own error flag
    return false;
  uint32_t v = (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
  bool negative = v & 0x800000;
  v &= 0x7FFFFF;
  float t = float(v / 1000) / 10.0f;
  if (negative)
    t = -t;
  float h = float(v % 1000) / 10.0f;
  return accept(ThermoKind::GOVEE, t, h, battery_byte & 0x7F, out);
}

// Little-endian int16 temp x0.01, uint16 humidity x0.01, uint8 battery %.
bool govee_le(const uint8_t *p, ThermoReading &out) {
  return accept(ThermoKind::GOVEE, s16_le(p) * 0.01f, get_u16(p + 2) * 0.01f, p[4], out);
}

bool parse_govee(uint16_t company, const uint8_t *d, size_t len, ThermoReading &out) {
  // Lengths are the whole entry, company id included.
  if (company == COMPANY_GOVEE && len == 8)  // H5072/H5075: 88 EC 00 [t t t] batt 00
    return govee_packed(d + 3, d[6], out);
  if (company == COMPANY_GOVEE && len == 9)  // H5074: 88 EC 00 [tL tH hL hH] batt 02
    return govee_le(d + 3, out);
  if (company == COMPANY_GOVEE_H5179 && len == 11)  // H5179: 01 88 EC 00 01 01 [tL tH hL hH] batt
    return govee_le(d + 6, out);
  // H5101/H5102/H5104/H5174/H5177 (and the newer GV5179): 01 00 01 01 [t t t] batt. Company 0x0001
  // is not Govee's own and others use it, so the 01 01 that every known Govee advert carries is
  // required too; the caller may also check the name ("GVH5...").
  if (company == COMPANY_GOVEE_H510X && len == 8 && d[2] == 0x01 && d[3] == 0x01)
    return govee_packed(d + 4, d[7], out);
  return false;
}

}  // namespace

bool parse_switchbot_meter(const uint8_t *service, size_t service_len, const uint8_t *manufacturer,
                           size_t manufacturer_len, ThermoReading &out) {
  if (service == nullptr || service_len < 1 || !switchbot_is_meter(service[0]))
    return false;
  int battery = service_len >= 3 ? (service[2] & 0x7F) : -1;
  // Newer firmware: company id, MAC[6], two bytes, then the reading at 10..12.
  if (manufacturer != nullptr && manufacturer_len >= 13 && get_u16(manufacturer) == COMPANY_SWITCHBOT)
    return switchbot_th(manufacturer + 10, battery, out);
  // Meter / Meter Plus: model, flags, battery, then the reading at 3..5.
  if (service_len >= 6)
    return switchbot_th(service + 3, battery, out);
  return false;
}

namespace {

// ---- RuuviTag "weather station" mode: Eddystone URL https://ruu.vi/#<base64> (formats 2 and 4) ----
// Frame: u8 0x10 (URL), i8 TX power, u8 scheme, then the URL text. After '#': 8 base64url characters
// for 6 bytes (format 4 adds one character, a tag id): format, humidity 0.5 %, temperature as
// sign-and-magnitude degrees, a byte the format leaves 0, pressure.
int b64url(uint8_t c) {
  if (c >= 'A' && c <= 'Z')
    return c - 'A';
  if (c >= 'a' && c <= 'z')
    return c - 'a' + 26;
  if (c >= '0' && c <= '9')
    return c - '0' + 52;
  if (c == '-' || c == '+')
    return 62;
  if (c == '_' || c == '/')
    return 63;
  return -1;
}

bool parse_ruuvi_url(const uint8_t *d, size_t n, ThermoReading &out) {
  static const char HOST[] = "ruu.vi/#";
  const size_t host = sizeof(HOST) - 1;
  if (n < 3 + host + 8 || d[0] != 0x10)
    return false;
  if (std::memcmp(d + 3, HOST, host) != 0)
    return false;
  const uint8_t *p = d + 3 + host;
  uint8_t b[6];
  for (int g = 0; g < 2; g++) {  // two groups of 4 characters -> 3 bytes
    int v[4];
    for (int k = 0; k < 4; k++)
      if ((v[k] = b64url(p[g * 4 + k])) < 0)
        return false;
    b[g * 3] = uint8_t((v[0] << 2) | (v[1] >> 4));
    b[g * 3 + 1] = uint8_t(((v[1] & 15) << 4) | (v[2] >> 2));
    b[g * 3 + 2] = uint8_t(((v[2] & 3) << 6) | v[3]);
  }
  if (b[0] != 2 && b[0] != 4)
    return false;
  const float t = float(b[2] & 0x7F) * ((b[2] & 0x80) ? -1.0f : 1.0f);
  return accept(ThermoKind::RUUVI, t, b[1] * 0.5f, -1, out);
}

// ---- Qingping (service data 0xFDCD) ----
// u8 frame control, u8 product id, u8[6] MAC, then entries of u8 type, u8 length, value:
// 0x01 (4 bytes) temperature i16 LE 0.1 C and humidity u16 LE 0.1 %, 0x02 (1 byte) battery %.
bool parse_qingping(const uint8_t *d, size_t n, ThermoReading &out) {
  if (n < 8)
    return false;
  float t = NAN, h = NAN;
  int battery = -1;
  for (size_t i = 8; i + 2 <= n;) {
    const uint8_t type = d[i], len = d[i + 1];
    if (i + 2 + len > n)
      break;
    const uint8_t *v = d + i + 2;
    if (type == 0x01 && len == 4) {
      t = s16_le(v) * 0.1f;
      h = get_u16(v + 2) * 0.1f;
    } else if (type == 0x02 && len == 1) {
      battery = v[0];
    }
    i += 2 + len;
  }
  return accept(ThermoKind::QINGPING, t, h, battery, out);
}

// ---- Xiaomi MiBeacon (service data 0xFE95), unencrypted only ----
// u16 frame control (bit 3 encrypted, bit 4 MAC present, bit 5 capability present, bit 6 object
// present, bits 12-15 version), u16 product id, u8 frame counter, [u8[6] MAC], [u8 capability, and 2
// more bytes when its bit 5 is set on version 5], then one object: u16 id, u8 length, value.
bool parse_mibeacon(const uint8_t *d, size_t n, ThermoReading &out) {
  if (n < 5)
    return false;
  const uint16_t ctrl = get_u16(d);
  if ((ctrl & 0x0008) || !(ctrl & 0x0040))
    return false;  // encrypted (needs the device's key), or no reading in this frame
  size_t i = 5;
  if (ctrl & 0x0010)
    i += 6;
  if (ctrl & 0x0020) {
    if (i >= n)
      return false;
    const uint8_t cap = d[i++];
    if ((ctrl >> 12) >= 5 && (cap & 0x20))
      i += 2;
  }
  if (i + 3 > n)
    return false;
  const uint16_t id = get_u16(d + i);
  const uint8_t len = d[i + 2];
  const uint8_t *v = d + i + 3;
  if (i + 3 + len > n)
    return false;
  // Temperature and humidity come together (0x100D) or one per frame (0x1004, 0x1006, 0x100A):
  // a frame without a temperature is no reading here.
  if (id == 0x100D && len == 4)
    return accept(ThermoKind::XIAOMI, s16_le(v) * 0.1f, get_u16(v + 2) * 0.1f, -1, out);
  if (id == 0x1004 && len == 2)
    return accept(ThermoKind::XIAOMI, s16_le(v) * 0.1f, NAN, -1, out);
  return false;
}

}  // namespace

bool parse_thermo_service_data(uint16_t uuid, const uint8_t *data, size_t len, ThermoReading &out) {
  if (data == nullptr)
    return false;
  switch (uuid) {
    case UUID_BTHOME:
      return parse_bthome(data, len, out);
    case UUID_ENVIRONMENTAL:
      return parse_xiaomi_custom(data, len, out);
    case UUID_SWITCHBOT:
    case UUID_SWITCHBOT_OLD:
      return parse_switchbot_meter(data, len, nullptr, 0, out);
    case UUID_QINGPING:
      return parse_qingping(data, len, out);
    case UUID_MIBEACON:
      return parse_mibeacon(data, len, out);
    case UUID_EDDYSTONE:
      return parse_ruuvi_url(data, len, out);
    default:
      return false;
  }
}

namespace {

// ---- RuuviTag (https://docs.ruuvi.com/communication/bluetooth-advertisements) ----
// After the company id: the data format byte, then big-endian fields. Battery from the cell voltage,
// 2.5 V (Ruuvi's "replace") to 3.0 V (a new CR2477), as a rough percentage.
int ruuvi_battery(int mv) {
  if (mv <= 0)
    return -1;
  const int pct = (mv - 2500) / 5;
  return pct < 0 ? 0 : (pct > 100 ? 100 : pct);
}

bool parse_ruuvi(const uint8_t *d, size_t n, ThermoReading &out) {
  if (n < 1)
    return false;
  if (d[0] == 0x05) {  // RAWv2: 24 bytes
    if (n < 24)
      return false;
    const int16_t t = s16_be(d + 1);
    const uint16_t h = uint16_t((d[3] << 8) | d[4]);
    const uint16_t power = uint16_t((d[13] << 8) | d[14]);
    if (t == int16_t(0x8000))
      return false;  // not available
    const int mv = (power >> 5) == 0x7FF ? 0 : (power >> 5) + 1600;
    return accept(ThermoKind::RUUVI, t * 0.005f, h == 0xFFFF ? NAN : h * 0.0025f, ruuvi_battery(mv), out);
  }
  if (d[0] == 0x06) {  // format 6 (Ruuvi's air sensors): temperature and humidity as in format 5
    if (n < 20)
      return false;
    const int16_t t = s16_be(d + 1);
    const uint16_t h = uint16_t((d[3] << 8) | d[4]);
    if (t == int16_t(0x8000))
      return false;
    return accept(ThermoKind::RUUVI, t * 0.005f, h == 0xFFFF ? NAN : h * 0.0025f, -1, out);
  }
  if (d[0] == 0x03) {  // RAWv1: 14 bytes; temperature as sign-and-magnitude degrees plus hundredths
    if (n < 14 || d[3] > 99)
      return false;
    const float mag = float(d[2] & 0x7F) + d[3] / 100.0f;
    const int mv = (d[12] << 8) | d[13];
    return accept(ThermoKind::RUUVI, (d[2] & 0x80) ? -mag : mag, d[1] * 0.5f, ruuvi_battery(mv), out);
  }
  return false;  // other formats: no plain temperature reading this board knows
}

}  // namespace

bool parse_thermo_manufacturer_data(const uint8_t *data, size_t len, ThermoReading &out) {
  if (data == nullptr || len < 2)
    return false;
  if (get_u16(data) == COMPANY_RUUVI)
    return parse_ruuvi(data + 2, len - 2, out);
  return parse_govee(get_u16(data), data, len, out);
}

bool parse_thermo_advert(const uint8_t *ad, size_t len, ThermoReading &out, char *name, size_t name_size) {
  if (name_size > 0)
    name[0] = '\0';
  if (ad == nullptr)
    return false;
  ThermoReading reading;
  bool ok = false;
  bool complete_name = false;
  // SwitchBot: the service data names the model, the manufacturer data may carry the reading.
  const uint8_t *switchbot = nullptr;
  size_t switchbot_len = 0;
  // Two passes, as the decoders rank them: service data first, then manufacturer data.
  for (int pass = 0; pass < 2; pass++) {
    for (size_t i = 0; i < len;) {
      const size_t l = ad[i];
      if (l == 0 || i + 1 + l > len)
        break;  // the end (zero padding), or cut short
      const uint8_t type = ad[i + 1];
      const uint8_t *d = ad + i + 2;
      const size_t n = l - 1;
      i += 1 + l;
      if (pass == 0 && type == 0x16 && n >= 2) {  // service data, 16-bit UUID
        const uint16_t uuid = uint16_t(d[0] | d[1] << 8);
        if (uuid == 0xFD3D || uuid == 0x0D00) {
          switchbot = d + 2;
          switchbot_len = n - 2;
        }
        if (!ok)
          ok = parse_thermo_service_data(uuid, d + 2, n - 2, reading);
      } else if (pass == 0 && (type == 0x09 || (type == 0x08 && !complete_name)) && name_size > 0) {
        const size_t k = n < name_size - 1 ? n : name_size - 1;
        std::memcpy(name, d, k);
        name[k] = '\0';
        complete_name = complete_name || type == 0x09;
      } else if (pass == 1 && type == 0xFF && n >= 2) {  // manufacturer data, company id first
        const uint16_t company = uint16_t(d[0] | d[1] << 8);
        if (company == 0x0969 && switchbot != nullptr) {
          ThermoReading s;
          if (parse_switchbot_meter(switchbot, switchbot_len, d, n, s)) {
            reading = s;
            ok = true;
          }
        } else if (!ok) {
          ok = parse_thermo_manufacturer_data(d, n, reading);
        }
      }
    }
  }
  if (ok)
    out = reading;
  return ok;
}

const char *thermo_kind_name(ThermoKind k) {
  switch (k) {
    case ThermoKind::BTHOME:
      return "BTHome";
    case ThermoKind::XIAOMI_CUSTOM:
      return "Xiaomi (custom firmware)";
    case ThermoKind::GOVEE:
      return "Govee";
    case ThermoKind::SWITCHBOT:
      return "SwitchBot";
    case ThermoKind::RUUVI:
      return "RuuviTag";
    case ThermoKind::QINGPING:
      return "Qingping";
    case ThermoKind::XIAOMI:
      return "Xiaomi";
    default:
      return "none";
  }
}

}  // namespace flamekeeper
}  // namespace esphome
