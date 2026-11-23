#include "ble_protocol.h"

#include "automation.h"
#include "bytes.h"

#include <cstring>

namespace esphome {
namespace flamekeeper {
namespace ble {

namespace {

const uint8_t BASE_UUID[16] = {0x81, 0xce, 0x00, 0x00, 0x02, 0x4c, 0x41, 0x4c,
                               0x9c, 0xa7, 0xe0, 0x97, 0x2f, 0xed, 0x52, 0xee};

size_t result(uint8_t req, uint8_t opcode, Result r, const char *text, uint8_t *out, size_t cap) {
  out[0] = req;
  out[1] = opcode;
  out[2] = uint8_t(r);
  size_t n = 3;
  if (text != nullptr) {
    size_t len = strlen(text);
    if (len > cap - n)
      len = cap - n;
    memcpy(out + n, text, len);
    n += len;
  }
  return n;
}

size_t outcome(uint8_t req, uint8_t opcode, const Outcome &o, uint8_t *out, size_t cap) {
  return result(req, opcode, o.result, o.result == Result::OK ? nullptr : o.reason, out, cap);
}


// PARAM_GROUP: the index-th group that has parameters, in menu order (PARAM_GROUP_ORDER): its code and
// name. Which parameters are in it comes with each PARAM_DESCRIBE.
size_t param_group(uint8_t req, uint8_t opcode, uint8_t index, const Controller &c, uint8_t *out, size_t cap) {
  const size_t total = c.param_count();
  uint8_t groups = 0;
  int found = -1;
  for (ParamGroup g : PARAM_GROUP_ORDER) {
    bool any = false;
    ParamInfo info;
    for (size_t i = 0; i < total && !any; i++)
      any = c.param_info(i, info) && info.group == g;
    if (!any)
      continue;
    if (groups == index)
      found = int(g);
    groups++;
  }
  if (found < 0)
    return result(req, opcode, Result::OUT_OF_RANGE, "no such group index", out, cap);
  size_t k = result(req, opcode, Result::OK, nullptr, out, cap);
  out[k] = index;
  out[k + 1] = groups;
  out[k + 2] = uint8_t(found);
  k += 3;
  const char *name = param_group_name(ParamGroup(found));
  size_t len = strlen(name);
  if (len > cap - k)
    len = cap - k;
  memcpy(out + k, name, len);
  return k + len;
}


uint8_t fixed_u8_or(float v, uint8_t unknown) {
  if (std::isnan(v) || v < 0.0f)
    return unknown;
  return v >= 254.5f ? 254 : uint8_t(std::lround(v));
}

void put_timer(uint8_t *p, const TimerSpec &t) {
  p[0] = t.flags;
  p[1] = t.days;
  put_u16(p + 2, t.start_min);
  put_u32(p + 4, t.once_utc);
  put_u16(p + 8, t.duration_min);
  p[10] = t.mode;
  p[11] = t.heat_mode;
  p[12] = t.power;
  put_u16(p + 13, uint16_t(t.target_x10));
}

TimerSpec get_timer(const uint8_t *p) {
  TimerSpec t;
  t.flags = p[0];
  t.days = p[1];
  t.start_min = get_u16(p + 2);
  t.once_utc = uint32_t(get_i32(p + 4));
  t.duration_min = get_u16(p + 8);
  t.mode = p[10];
  t.heat_mode = p[11];
  t.power = p[12];
  t.target_x10 = int16_t(get_u16(p + 13));
  return t;
}

// A thermometer as SENSOR_FOUND / SENSOR_STATUS send it, from offset `p` on: address, kind, RSSI,
// temperature, humidity, battery.
size_t put_sensor(uint8_t *p, const FoundSensor &f) {
  memcpy(p, f.address, 6);
  p[6] = uint8_t(f.kind);
  p[7] = uint8_t(f.rssi);
  put_fixed_i16(p + 8, f.temperature_c, 10.0f);
  p[10] = fixed_u8_or(f.humidity_pct, 0xFF);
  p[11] = f.battery_pct < 0 ? 0xFF : uint8_t(f.battery_pct);
  return 12;
}

// The automation commands (PROTOCOL.md).
size_t automation_command(uint8_t req, uint8_t opcode, const uint8_t *arg, size_t n, Automation &a, Controller &c,
                          Source source, uint8_t *out, size_t cap) {
  auto bad = [&]() { return result(req, opcode, Result::BAD_LENGTH, "wrong argument length", out, cap); };
  const uint32_t now = c.now();
  switch (opcode) {
    case op::CLOCK_SET: {
      if (n < 5 || n != size_t(5 + arg[4]) || arg[4] > Automation::TZ_MAX)
        return bad();
      char tz[Automation::TZ_MAX + 1];
      memcpy(tz, arg + 5, arg[4]);
      tz[arg[4]] = '\0';
      return outcome(req, opcode, a.set_clock(uint32_t(get_i32(arg)), tz, ClockSource::APP, now), out, cap);
    }
    case op::CLOCK_GET: {
      if (n != 0)
        return bad();
      size_t k = result(req, opcode, Result::OK, nullptr, out, cap);
      put_u32(out + k, a.utc(now));
      out[k + 4] = uint8_t(a.utc(now) == 0 ? ClockSource::NONE : a.clock_source());
      k += 5;
      size_t len = a.time_zone().size();
      memcpy(out + k, a.time_zone().data(), len);
      return k + len;
    }
    case op::METERS: {
      if (n != 0)
        return bad();
      FuelMeter::Totals t = a.meters(now);
      const uint32_t v[14] = {t.run_fuel,  t.run_burn,  t.run_glow,  t.day_fuel,   t.day_burn,
                              t.week_fuel, t.week_burn, t.month_fuel, t.month_burn, t.life_fuel,
                              t.life_burn, t.life_glow, t.tank_used,
                              a.maintenance().low_power_s};  // low-power time since the last burn-off
      size_t k = result(req, opcode, Result::OK, nullptr, out, cap);
      for (uint32_t x : v) {
        put_u32(out + k, x);
        k += 4;
      }
      return k;
    }
    case op::TANK_SET:
      if (n != 1)
        return bad();
      return outcome(req, opcode, a.tank_set(arg[0], source), out, cap);
    case op::ALERTS_CLEAR:
      if (n != 2)
        return bad();
      return outcome(req, opcode, a.clear_alerts(get_u16(arg)), out, cap);
    case op::AUTO_REARM:
      if (n != 0)
        return bad();
      return outcome(req, opcode, a.rearm(source), out, cap);
    case op::TIMER_GET: {
      if (n != 1)
        return bad();
      TimerSpec t;
      if (!a.timer_get(arg[0], t))
        return result(req, opcode, Result::OUT_OF_RANGE, "no such timer slot", out, cap);
      size_t k = result(req, opcode, Result::OK, nullptr, out, cap);
      out[k] = arg[0];
      out[k + 1] = uint8_t(Automation::TIMERS);
      put_timer(out + k + 2, t);
      return k + 2 + TIMER_RECORD_SIZE;
    }
    case op::TIMER_SET:
      if (n != 1 + TIMER_RECORD_SIZE)
        return bad();
      return outcome(req, opcode, a.timer_set(arg[0], get_timer(arg + 1)), out, cap);
    case op::TIMER_DELETE:
      if (n != 1)
        return bad();
      return outcome(req, opcode, a.timer_delete(arg[0]), out, cap);
    case op::SENSOR_SCAN:
      if (n != 1)
        return bad();
      if (arg[0] < 5 || arg[0] > 60)
        return result(req, opcode, Result::OUT_OF_RANGE, "5 to 60 seconds", out, cap);
      a.sensors().scan(arg[0], now);
      return result(req, opcode, Result::OK, nullptr, out, cap);
    case op::SENSOR_FOUND: {
      if (n != 1)
        return bad();
      RoomSensors &rs = a.sensors();
      const size_t count = rs.found_count();
      const bool scanning = rs.scanning(now);
      if (arg[0] >= count && !(arg[0] == 0 && count == 0))
        return result(req, opcode, Result::OUT_OF_RANGE, "no thermometer at this index", out, cap);
      size_t k = result(req, opcode, Result::OK, nullptr, out, cap);
      out[k] = arg[0];
      out[k + 1] = uint8_t(count);
      if (count == 0) {
        out[k + 2] = scanning ? 0x01 : 0;
        return k + 3;
      }
      const FoundSensor &f = rs.found(arg[0]);
      out[k + 2] = uint8_t((scanning ? 0x01 : 0) | (rs.is_chosen(f.address) ? 0x02 : 0));
      k += 3;
      k += put_sensor(out + k, f);
      size_t len = strnlen(f.name, SENSOR_NAME_MAX);
      memcpy(out + k, f.name, len);
      return k + len;
    }
    case op::SENSOR_USE: {
      if (n != 7)
        return bad();
      if (arg[6] > uint8_t(THERMO_KIND_LAST))
        return result(req, opcode, Result::OUT_OF_RANGE, "unknown thermometer kind", out, cap);
      a.sensor_use(arg, ThermoKind(arg[6]));
      return result(req, opcode, Result::OK, nullptr, out, cap);
    }
    case op::LOG_READ: {
      if (n != 4)
        return bad();
      LogRecord recs[7];
      const size_t got = a.log().read(uint32_t(get_i32(arg)), recs, 7);
      size_t k = result(req, opcode, Result::OK, nullptr, out, cap);
      put_u32(out + k, a.log().newest());
      out[k + 4] = uint8_t(got);
      k += 5;
      for (size_t i = 0; i < got; i++) {
        put_u32(out + k, recs[i].seq);
        put_u32(out + k + 4, recs[i].utc);
        out[k + 8] = recs[i].kind;
        out[k + 9] = recs[i].source;
        put_u16(out + k + 10, recs[i].a);
        put_u32(out + k + 12, recs[i].b);
        k += 16;
      }
      return k;
    }
    case op::GLOW_HEALTH: {
      if (n != 0)
        return bad();
      const Automation::Maintenance &m = a.maintenance();
      size_t k = result(req, opcode, Result::OK, nullptr, out, cap);
      put_u16(out + k, m.glow_baseline_mohm);
      put_u16(out + k + 2, m.glow_n == 0 ? 0xFFFF : m.glow_readings[m.glow_n - 1]);
      put_u16(out + k + 4, m.glow_starts);
      out[k + 6] = m.glow_n;
      k += 7;
      for (size_t i = 0; i < m.glow_n; i++, k += 2)
        put_u16(out + k, m.glow_readings[i]);
      return k;
    }
    case op::GLOW_NEW_PLUG:
      if (n != 0)
        return bad();
      return outcome(req, opcode, a.glow_new_plug(), out, cap);
    case op::BURNOFF_START:
      if (n != 1)
        return bad();
      return outcome(req, opcode, a.burnoff_start(arg[0], source), out, cap);
    case op::RUN_UNTIL:
      if (n != 4)
        return bad();
      return outcome(req, opcode, a.run_until(get_u32(arg), source), out, cap);
    case op::SENSOR_STATUS: {
      if (n != 0)
        return bad();
      const RoomSensors &rs = a.sensors();
      size_t k = result(req, opcode, Result::OK, nullptr, out, cap);
      FoundSensor f = rs.chosen();
      if (!rs.has_chosen())
        f = FoundSensor{};
      k += put_sensor(out + k, f);
      put_u16(out + k, rs.age_s(now));
      return k + 2;
    }
    default:
      return result(req, opcode, Result::UNKNOWN_OPCODE, "unknown opcode", out, cap);
  }
}

}  // namespace

void uuid_bytes(uint16_t short_id, uint8_t out[16]) {
  // The string form is big-endian; Bluetooth sends 128-bit UUIDs least significant byte first.
  uint8_t be[16];
  memcpy(be, BASE_UUID, 16);
  be[2] = uint8_t(short_id >> 8);
  be[3] = uint8_t(short_id);
  for (int i = 0; i < 16; i++)
    out[i] = be[15 - i];
}

size_t encode_info(uint8_t board, uint32_t heater_capabilities, const BoardInfo &info, uint8_t *out) {
  auto nibbles = [](uint8_t low, uint8_t high) {
    return uint8_t((low > 15 ? 15 : low) | ((high > 15 ? 15 : high) << 4));
  };
  out[0] = PROTOCOL_MAJOR;
  out[1] = PROTOCOL_MINOR;
  out[2] = board;
  out[3] = (info.window_open ? 0x01 : 0) | (info.bonds_full ? 0x02 : 0);
  put_u16(out + 4, info.window_left_s);
  // Bits 0-5 come from the heater; bit 6 from the board.
  put_u32(out + 6, (heater_capabilities & 0x3Fu) | (info.ha_switchable ? CAP_HA_SWITCH : 0) |
                       ((heater_capabilities & cap::BLUETOOTH_ROOM) ? CAP_BLUETOOTH_ROOM : 0) |
                       (info.can_update ? CAP_BOARD_UPDATE : 0) | (info.has_relay ? CAP_RELAY : 0) |
                       ((heater_capabilities & cap::HA_ROOM) ? CAP_HA_ROOM : 0) |
                       ((heater_capabilities & cap::FAULT_LOG) ? CAP_HEATER_LOG : 0));
  out[10] = info.bonds;
  out[11] = info.max_bonds;
  out[12] = nibbles(info.app_keys, info.browser_keys);
  out[13] = nibbles(info.max_app_keys, info.max_browser_keys);
  out[14] = uint8_t(info.wifi);
  out[15] = (info.pin_changed ? 0x01 : 0) | (info.web_password_set ? 0x02 : 0);
  out[16] = uint8_t(info.ha);
  out[17] = info.version[0];
  out[18] = info.version[1];
  out[19] = info.version[2];
  put_u32(out + 20, info.build_utc);
  return INFO_SIZE;
}

size_t encode_state(const Status &s, uint8_t *out) {
  out[0] = (s.answering ? 0x01 : 0) | (s.failsafe_stopped ? 0x02 : 0) | (s.room_fallback ? 0x04 : 0) |
           (s.update_running ? 0x08 : 0) | (s.board_updating ? 0x10 : 0) | (s.relay_phones != 0 ? 0x20 : 0) |
           (s.thermo_paused ? 0x40 : 0);
  out[1] = uint8_t(s.mode);
  out[2] = uint8_t(s.heat_mode);
  out[3] = s.power;
  put_fixed_i16(out + 4, s.target_c, 10.0f);
  put_fixed_i16(out + 6, s.room_c, 10.0f);
  out[8] = uint8_t(s.room_source);
  out[9] = uint8_t(s.state);
  out[10] = uint8_t(s.control);
  out[11] = uint8_t(s.fault);
  out[12] = uint8_t(s.run_source != Source::NONE ? s.run_source : s.ending_source);
  out[13] = s.params_pending;
  put_u16(out + 14, s.prime_left_s);
  put_u16(out + 16, s.failsafe_left_s >= 0xFFFF ? 0xFFFF : uint16_t(s.failsafe_left_s));
  put_u16(out + 18, s.alerts);
  out[20] = s.automatic;
  out[21] = s.fuel_left;
  out[22] = s.burnoff_left_min;
  out[23] = s.timer_slot;
  put_u32(out + 24, s.run_end_utc);  // any run's end: its timer's, or RUN_UNTIL's
  put_u32(out + 28, s.run_id);
  out[32] = s.fault_log_seq;  // the heater's fault log: its newest entry (0 none); read it when this changes
  return STATE_SIZE;
}

size_t encode_telemetry(const Status &s, uint8_t *out) {
  put_fixed_i16(out + 0, s.heat_exchanger_c, 10.0f);
  put_fixed_i16(out + 2, s.intake_c, 10.0f);
  put_fixed_u16(out + 4, s.supply_v, 100.0f);
  put_fixed_u16(out + 6, s.fan_rpm, 1.0f);
  put_fixed_u16(out + 8, s.fan_v, 100.0f);
  put_fixed_u16(out + 10, s.pump_hz, 100.0f);
  put_fixed_u16(out + 12, s.glow_w, 1.0f);
  put_fixed_u16(out + 14, s.glow_v, 100.0f);
  put_fixed_u16(out + 16, s.glow_a, 100.0f);
  out[18] = fixed_u8(s.power_pct);
  out[19] = s.panel_fault;
  put_fixed_u16(out + 20, s.co_ppm, 1.0f);
  put_fixed_i16(out + 22, s.altitude_m, 1.0f);
  put_fixed_u16(out + 24, s.pressure_hpa, 10.0f);
  if (std::isnan(s.run_time_s) || s.run_time_s < 0.0f) {
    put_u32(out + 26, 0xFFFFFFFFu);
  } else {
    double t = std::floor(double(s.run_time_s));
    put_u32(out + 26, t >= 4294967294.0 ? 0xFFFFFFFEu : uint32_t(t));
  }
  out[30] = s.co_state;
  put_u32(out + 31, s.run_s);  // in Telemetry, not State: it changes every second
  put_fixed_i16(out + 35, s.room_probe_c, 10.0f);  // every room source's reading, the one in use or not
  put_fixed_i16(out + 37, s.room_ha_c, 10.0f);
  put_fixed_i16(out + 39, s.room_bt_c, 10.0f);
  out[41] = uint8_t(s.altitude_trim_pct);  // 0x80: altitude trim off, or no altitude yet
  out[42] = uint8_t(s.fuel_trim_pct);      // the fuel trim in effect (set + altitude), % signed
  put_fixed_i16(out + 43, s.altitude_smoothed_m, 1.0f);
  return TELEMETRY_SIZE;
}

size_t encode_event(Event event, Result r, uint16_t id, const char *text, uint8_t *out, size_t cap, uint32_t seq) {
  if (event == Event::PARAM_CHANGED) {
    size_t n = result(0, uint8_t(event), r, nullptr, out, cap);
    put_u16(out + n, id);
    return n + 2;
  }
  if (event == Event::ALERT) {  // u8 alert bit, u32 the log record's sequence number, then the text
    size_t n = result(0, uint8_t(event), r, nullptr, out, cap);
    out[n++] = uint8_t(id);
    put_u32(out + n, seq);
    n += 4;
    size_t len = text != nullptr ? strlen(text) : 0;
    if (len > cap - n)
      len = cap - n;
    memcpy(out + n, text, len);
    return n + len;
  }
  return result(0, uint8_t(event), r, text, out, cap);
}

size_t encode_wifi_event(const WifiStatus *connected, const char *failure, uint8_t *out, size_t cap) {
  if (connected == nullptr)
    return result(0, EVENT_WIFI_CHANGED, Result::REFUSED, failure, out, cap);
  size_t n = result(0, EVENT_WIFI_CHANGED, Result::OK, nullptr, out, cap);
  memcpy(out + n, connected->ip, 4);
  n += 4;
  size_t len = strnlen(connected->ssid, SSID_MAX);
  if (len > cap - n)
    len = cap - n;
  memcpy(out + n, connected->ssid, len);
  return n + len;
}

size_t handle_command(const uint8_t *in, size_t len, Controller &c, Link &link, BoardSettings &board, uint8_t *out,
                      size_t cap) {
  if (len == 0 || in[0] == 0)
    return 0;
  uint8_t req = in[0];
  if (len < 2)
    return result(req, 0, Result::BAD_LENGTH, "no opcode", out, cap);
  uint8_t opcode = in[1];
  const uint8_t *arg = in + 2;
  size_t n = len - 2;
  auto need = [&](size_t want) { return n == want; };
  const Source PHONE = link.transport() == Transport::BLUETOOTH ? Source::PHONE
                       : link.transport() == Transport::RELAY   ? Source::RELAY_APP
                       : link.browser()                         ? Source::BROWSER
                                                                : Source::WIFI_APP;
  // "u8 length, bytes" fields; false if they run past the end.
  auto field = [&](size_t at, const uint8_t **p, size_t *flen) {
    if (at >= n || at + 1 + arg[at] > n)
      return false;
    *flen = arg[at];
    *p = arg + at + 1;
    return true;
  };

  switch (opcode) {
    case op::SET_MODE:
      if (!need(1))
        break;
      if (arg[0] > uint8_t(Mode::FAN))  // priming has its own command (it needs the confirmation)
        return result(req, opcode, Result::OUT_OF_RANGE, "mode is 0 off, 1 heat, 2 fan only", out, cap);
    {
      c.set_start_slot(link.phone_slot());  // the live card's starter, if this starts a run
      const Outcome o = c.set_mode(Mode(arg[0]), PHONE);
      c.set_start_slot(-1);
      return outcome(req, opcode, o, out, cap);
    }
    case op::SET_HEAT_MODE:
      if (!need(1))
        break;
      return outcome(req, opcode, c.set_heat_mode(HeatMode(arg[0]), PHONE), out, cap);
    case op::SET_POWER:
      if (!need(1))
        break;
      return outcome(req, opcode, c.set_power(arg[0], PHONE), out, cap);
    case op::SET_TARGET:
      if (!need(2))
        break;
      if (get_u16(arg) == 0x8000)
        return result(req, opcode, Result::OUT_OF_RANGE, "no target given", out, cap);
      return outcome(req, opcode, c.set_target(int16_t(get_u16(arg)) / 10.0f, PHONE), out, cap);
    case op::SET_ROOM_SOURCE:
      if (!need(1))
        break;
      return outcome(req, opcode, c.set_room_source(RoomSource(arg[0]), PHONE), out, cap);
    case op::PRIME_START:
      if (!need(1))
        break;
      if (arg[0] != PRIME_CONFIRM)
        return result(req, opcode, Result::NOT_CONFIRMED, "ask the user to confirm they are at the heater", out,
                      cap);
      return outcome(req, opcode, c.start_prime(PHONE), out, cap);
    case op::PRIME_STOP:
      if (!need(0))
        break;
      return outcome(req, opcode, c.stop_prime(PHONE), out, cap);
    case op::FAULT_RESET:
      if (!need(0))
        break;
      return outcome(req, opcode, c.reset_fault(PHONE), out, cap);
    case op::HEATER_LOG: {
      if (!need(1))
        break;
      if (!(c.heater()->capabilities() & cap::FAULT_LOG))
        return result(req, opcode, Result::NOT_SUPPORTED, "this heater has no fault log", out, cap);
      // The entry count, then up to 8 entries from the arg-th newest, as the safety MCU's link has them.
      HeaterLogEntry e[HEATER_LOG_PAGE];
      const size_t got = c.fault_log(arg[0], e, HEATER_LOG_PAGE);
      HeaterStatus hs;
      c.heater()->read_status(hs);
      size_t k = result(req, opcode, Result::OK, nullptr, out, cap);
      out[k++] = hs.fault_log_count;
      for (size_t i = 0; i < got; i++, k += HEATER_LOG_ENTRY_SIZE) {
        uint8_t *p = out + k;
        p[0] = e[i].kind;
        p[1] = e[i].code;
        p[2] = e[i].why;
        p[3] = e[i].step;
        p[4] = e[i].seq;
        p[5] = uint8_t(e[i].board_c);
        put_u16(p + 6, uint16_t(e[i].hx_c10));
        put_u16(p + 8, e[i].supply_cv);
        put_u32(p + 10, e[i].uptime_s);
      }
      return k;
    }
    case op::HEATER_LOG_CLEAR:
      if (!need(0))
        break;
      return outcome(req, opcode, c.clear_fault_log(PHONE), out, cap);

    case op::PARAM_DESCRIBE:
    case op::PARAM_DESCRIBE_ID: {
      if (!need(opcode == op::PARAM_DESCRIBE ? 1 : 2))
        break;
      size_t count = c.param_count();  // the heater's parameters, then the board's own
      ParamInfo info;
      size_t index = arg[0];
      if (opcode == op::PARAM_DESCRIBE_ID) {  // the same answer, found by id
        const uint16_t id = get_u16(arg);
        for (index = 0; index < count; index++)
          if (c.param_info(index, info) && info.id == id)
            break;
        if (index >= count)
          return result(req, opcode, Result::OUT_OF_RANGE, "no such parameter", out, cap);
      }
      if (index >= count || !c.param_info(index, info))
        return result(req, opcode, Result::OUT_OF_RANGE, "no such parameter index", out, cap);
      size_t k = result(req, opcode, Result::OK, nullptr, out, cap);
      uint8_t *p = out + k;
      p[0] = uint8_t(index > 255 ? 255 : index);
      p[1] = uint8_t(count > 255 ? 255 : count);
      put_u16(p + 2, info.id);
      p[4] = info.flags;
      p[5] = uint8_t(info.unit);
      p[6] = info.decimals;
      p[7] = uint8_t(info.group);
      p[8] = uint8_t(info.level);
      put_u32(p + 9, uint32_t(info.min));
      put_u32(p + 13, uint32_t(info.max));
      put_u32(p + 17, uint32_t(info.step));
      k += PARAM_DESC_FIXED;
      size_t name = strlen(info.name);
      if (name > PARAM_NAME_MAX)
        name = PARAM_NAME_MAX;
      if (name > cap - k)
        name = cap - k;
      memcpy(out + k, info.name, name);
      return k + name;
    }
    case op::PARAM_GROUP: {
      if (!need(1))
        break;
      return param_group(req, opcode, arg[0], c, out, cap);
    }
    case op::PARAM_HELP: {
      if (!need(2))
        break;
      uint16_t id = get_u16(arg);
      const char *help = c.param_help(id);
      if (help == nullptr)
        return result(req, opcode, Result::OUT_OF_RANGE, "no such parameter", out, cap);
      size_t k = result(req, opcode, Result::OK, nullptr, out, cap);
      put_u16(out + k, id);
      k += 2;
      size_t n = strlen(help);
      if (n > PARAM_HELP_MAX)
        n = PARAM_HELP_MAX;
      if (n > cap - k)
        n = cap - k;
      memcpy(out + k, help, n);
      return k + n;
    }
    case op::PARAM_GET: {
      if (!need(2))
        break;
      uint16_t id = get_u16(arg);
      ParamValue v;
      if (!c.param_get(id, v))
        return result(req, opcode, Result::OUT_OF_RANGE, "no such parameter", out, cap);
      size_t k = result(req, opcode, Result::OK, nullptr, out, cap);
      put_u16(out + k, id);
      out[k + 2] = (v.known ? 0x01 : 0) | (v.staged ? 0x02 : 0);
      put_u32(out + k + 3, uint32_t(v.value));
      put_u32(out + k + 7, uint32_t(v.staged ? v.staged_value : v.value));
      return k + 11;
    }
    case op::PARAM_SET:
      if (!need(6))
        break;
      return outcome(req, opcode, c.param_set(get_u16(arg), get_i32(arg + 2), PHONE), out, cap);
    case op::PARAM_APPLY:
      if (!need(1))
        break;
      if (arg[0] > 1)
        return result(req, opcode, Result::OUT_OF_RANGE, "0 apply, 1 apply and save", out, cap);
      return outcome(req, opcode, c.param_apply(arg[0] == 1, PHONE), out, cap);
    case op::PARAM_DISCARD:
      if (!need(0))
        break;
      return outcome(req, opcode, c.param_discard(PHONE), out, cap);

    case op::FORGET_ME:
      if (!need(0))
        break;
      return outcome(req, opcode, link.forget_me(), out, cap);
    case op::WIFI_KEY: {
      if (!need(0))
        break;
      uint8_t id[KEY_ID_SIZE], secret[KEY_SECRET_SIZE];
      Outcome o = link.issue_wifi_key(id, secret);
      if (o.result != Result::OK)
        return outcome(req, opcode, o, out, cap);
      size_t k = result(req, opcode, Result::OK, nullptr, out, cap);
      memcpy(out + k, id, KEY_ID_SIZE);
      memcpy(out + k + KEY_ID_SIZE, secret, KEY_SECRET_SIZE);
      memset(secret, 0, sizeof(secret));
      return k + KEY_ID_SIZE + KEY_SECRET_SIZE;
    }

    case op::PIN_SET: {
      if (!need(4))
        break;
      uint32_t pin = uint32_t(get_i32(arg));
      if (pin > 999999)
        return result(req, opcode, Result::OUT_OF_RANGE, "the PIN has six digits", out, cap);
      return outcome(req, opcode, board.set_pin(pin), out, cap);
    }
    case op::WEB_PASSWORD_SET: {
      const uint8_t *cur, *pw;
      size_t cur_len, pw_len;
      if (!field(0, &cur, &cur_len) || !field(1 + cur_len, &pw, &pw_len) || 2 + cur_len + pw_len != n)
        break;
      if (pw_len != 0 && (pw_len < WEB_PASSWORD_MIN || pw_len > WEB_PASSWORD_MAX))
        return result(req, opcode, Result::OUT_OF_RANGE, "the web password has 8 to 64 bytes", out, cap);
      return outcome(req, opcode, board.set_web_password(cur, cur_len, pw, pw_len), out, cap);
    }

    case op::WIFI_STATUS: {
      if (!need(0))
        break;
      WifiStatus w;
      board.wifi_status(w);
      size_t k = result(req, opcode, Result::OK, nullptr, out, cap);
      out[k] = uint8_t(w.state);
      out[k + 1] = uint8_t(w.rssi);
      memcpy(out + k + 2, w.ip, 4);
      k += 6;
      size_t sl = strnlen(w.ssid, SSID_MAX);
      memcpy(out + k, w.ssid, sl);
      return k + sl;
    }
    case op::WIFI_SET: {
      const uint8_t *ssid, *pw;
      size_t ssid_len, pw_len;
      if (!field(0, &ssid, &ssid_len) || !field(1 + ssid_len, &pw, &pw_len) || 2 + ssid_len + pw_len != n)
        break;
      if (ssid_len == 0 || ssid_len > SSID_MAX)
        return result(req, opcode, Result::OUT_OF_RANGE, "the network name has 1 to 32 bytes", out, cap);
      if (pw_len != 0 && (pw_len < WIFI_PASSWORD_MIN || pw_len > WIFI_PASSWORD_MAX))
        return result(req, opcode, Result::OUT_OF_RANGE, "the Wi-Fi password has 8 to 63 characters, or none",
                      out, cap);
      return outcome(req, opcode, board.wifi_set(ssid, ssid_len, pw, pw_len), out, cap);
    }
    case op::WIFI_OFF:
      if (!need(0))
        break;
      return outcome(req, opcode, board.wifi_off(), out, cap);

    case op::HA_STATUS: {
      if (!need(0))
        break;
      size_t k = result(req, opcode, Result::OK, nullptr, out, cap);
      out[k] = uint8_t(board.ha_state());
      out[k + 1] = board.ha_key_set() ? 1 : 0;
      return k + 2;
    }
    case op::HA_SET: {
      if (!need(1))
        break;
      if (arg[0] > 1)
        return result(req, opcode, Result::OUT_OF_RANGE, "0 off, 1 on", out, cap);
      if (!board.ha_switchable())
        return result(req, opcode, Result::NOT_SUPPORTED, "Home Assistant is always on on this board", out, cap);
      // Switching restarts the board, and a restart never resumes a run: only with the heater off.
      const Status &st = c.status();
      if (st.mode != Mode::OFF || (st.state != HeaterState::OFF && st.state != HeaterState::UNKNOWN))
        return result(req, opcode, Result::NOT_OFF, "switching Home Assistant restarts the board: turn the heater off first",
                      out, cap);
      return outcome(req, opcode, board.ha_set(arg[0] == 1), out, cap);
    }
    case op::HA_KEY:
    case op::HA_KEY_NEW: {
      if (!need(0))
        break;
      if (!board.ha_switchable())
        return result(req, opcode, Result::NOT_SUPPORTED, "this board's Home Assistant key is set in its configuration",
                      out, cap);
      uint8_t key[HA_KEY_SIZE];
      Outcome o = opcode == op::HA_KEY ? board.ha_key(key) : board.ha_key_new(key);
      if (o.result != Result::OK)
        return outcome(req, opcode, o, out, cap);
      size_t k = result(req, opcode, Result::OK, nullptr, out, cap);
      memcpy(out + k, key, HA_KEY_SIZE);
      memset(key, 0, sizeof(key));
      return k + HA_KEY_SIZE;
    }


    // ---------------------------------------------------------------- the internet relay
    case op::RELAY_STATUS:
    case op::RELAY_SET: {
      RelayControl *rc = board.relay();
      if (rc == nullptr)
        return result(req, opcode, Result::NOT_SUPPORTED, "this build has no internet relay", out, cap);
      if (opcode == op::RELAY_SET) {
        if (n < 2 || arg[0] > 1 || n != size_t(2 + arg[1]))
          break;
        if (arg[1] != 0 && !relay_host_ok(arg + 2, arg[1]))
          return result(req, opcode, Result::OUT_OF_RANGE, "not a host name or address", out, cap);
        return outcome(req, opcode, rc->relay_set(arg[0] == 1, arg + 2, arg[1]), out, cap);
      }
      if (!need(0))
        break;
      RelayStatus st;
      rc->relay_status(st, c.now());
      size_t k = result(req, opcode, Result::OK, nullptr, out, cap);
      out[k++] = st.on ? 1 : 0;
      out[k++] = uint8_t(st.state);
      if (st.has_id)
        memcpy(out + k, st.id, 16);
      else
        memset(out + k, 0, 16);
      k += 16;
      put_u32(out + k, st.since_s);
      k += 4;
      out[k++] = c.status().relay_phones;
      const size_t hl = strlen(st.host);
      out[k++] = uint8_t(hl);
      memcpy(out + k, st.host, hl);
      return k + hl;
    }

    // ---------------------------------------------------------------- push notifications
    case op::PUSH_PREFS:
    case op::PUSH_PREFS_GET:
    case op::LOCAL_ALERTS: {
      PushControl *pc = board.push();
      if (pc == nullptr)
        return result(req, opcode, Result::NOT_SUPPORTED, "this build sends no push notifications", out, cap);
      const int slot = link.phone_slot();
      if (opcode == op::PUSH_PREFS) {
        if (!need(PUSH_PREFS_SIZE))
          break;
        PushPrefs p;
        p.alerts = get_u16(arg);
        p.events = get_u16(arg + 2);
        p.flags = arg[4];
        return outcome(req, opcode, pc->prefs_set(slot, p), out, cap);
      }
      if (opcode == op::LOCAL_ALERTS) {
        if (!need(1))
          break;
        if (arg[0] > 3)
          return result(req, opcode, Result::OUT_OF_RANGE, "bit 0 alerts, bit 1 live card: 0-3", out, cap);
        if (link.transport() != Transport::BLUETOOTH)
          return result(req, opcode, Result::BLUETOOTH_ONLY, "alerts shown by the phone itself: over Bluetooth only",
                        out, cap);
        return outcome(req, opcode, pc->local_alerts(slot, arg[0]), out, cap);
      }
      if (!need(0))
        break;
      PushPrefs p;
      if (!pc->prefs_get(slot, p))
        return result(req, opcode, Result::KEY_UNKNOWN, "this phone has no Wi-Fi key yet: ask for one first", out, cap);
      size_t k = result(req, opcode, Result::OK, nullptr, out, cap);
      put_u16(out + k, p.alerts);
      put_u16(out + k + 2, p.events);
      out[k + 4] = p.flags;
      out[k + 5] = uint8_t(slot);
      return k + 6;
    }

    // ---------------------------------------------------------------- board firmware updates
    case op::UPDATE_BEGIN:
    case op::UPDATE_STATUS:
    case op::UPDATE_FINISH:
    case op::UPDATE_ABORT: {
      BoardUpdater *u = board.updater();
      if (u == nullptr)
        return result(req, opcode, Result::NOT_SUPPORTED, "this build takes no firmware updates from the app", out,
                      cap);
      if (opcode == op::UPDATE_BEGIN) {
        if (!need(4 + UPDATE_HASH_SIZE))
          break;
        uint32_t offset = 0;
        Outcome o = u->update_begin(uint32_t(get_i32(arg)), arg + 4, &offset);
        if (o.result != Result::OK)
          return outcome(req, opcode, o, out, cap);
        size_t k = result(req, opcode, Result::OK, nullptr, out, cap);
        put_u32(out + k, offset);
        put_u16(out + k + 4, link.update_chunk_max());
        return k + 6;
      }
      if (!need(0))
        break;
      if (opcode == op::UPDATE_STATUS) {
        UpdateStatus st;
        u->update_status(st);
        size_t k = result(req, opcode, Result::OK, nullptr, out, cap);
        out[k] = uint8_t(st.state);
        put_u32(out + k + 1, st.size);
        put_u32(out + k + 5, st.received);
        memcpy(out + k + 9, st.sha256, UPDATE_HASH_SIZE);
        out[k + 9 + UPDATE_HASH_SIZE] = uint8_t(st.last);
        return k + 10 + UPDATE_HASH_SIZE;
      }
      if (opcode == op::UPDATE_FINISH)
        return outcome(req, opcode, u->update_finish(), out, cap);
      u->update_abort();
      return result(req, opcode, Result::OK, nullptr, out, cap);
    }

    // ---------------------------------------------------------------- automation
    case op::CLOCK_SET:
    case op::CLOCK_GET:
    case op::METERS:
    case op::TANK_SET:
    case op::ALERTS_CLEAR:
    case op::AUTO_REARM:
    case op::TIMER_GET:
    case op::TIMER_SET:
    case op::TIMER_DELETE:
    case op::SENSOR_SCAN:
    case op::SENSOR_FOUND:
    case op::SENSOR_USE:
    case op::SENSOR_STATUS:
    case op::LOG_READ:
    case op::GLOW_HEALTH:
    case op::GLOW_NEW_PLUG:
    case op::BURNOFF_START:
    case op::RUN_UNTIL: {
      Automation *a = c.automation();
      if (a == nullptr)
        return result(req, opcode, Result::NOT_SUPPORTED, "not on this board", out, cap);
      c.set_start_slot(opcode == op::BURNOFF_START ? link.phone_slot() : -1);  // a burn-off run's starter
      const size_t k = automation_command(req, opcode, arg, n, *a, c, PHONE, out, cap);
      c.set_start_slot(-1);
      return k;
    }

    default:
      return result(req, opcode, Result::UNKNOWN_OPCODE, "unknown opcode", out, cap);
  }
  return result(req, opcode, Result::BAD_LENGTH, "wrong argument length", out, cap);
}

}  // namespace ble
}  // namespace flamekeeper
}  // namespace esphome
