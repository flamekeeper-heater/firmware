// Golden vectors for app developers: the exact bytes this firmware produces (records, results, a
// Wi-Fi key sign-in and encrypted frames), printed as JSON, for a client's decoders, sign-in and frame
// cipher to be tested against. Built like the host tests (tests/run.sh), with mbedTLS:
//   dump_vectors <output.json>
#define MBEDTLS_ALLOW_PRIVATE_ACCESS  // the J-PAKE vectors read the app's scalars from mbedTLS's context
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../access.h"
#include "../automation.h"
#include "../ble_protocol.h"
#include "../crypto.h"
#include "../push.h"
#include "../relay.h"
#include "../wifi_session.h"
#include "fake_heater.h"
#include "mbedtls/ecjpake.h"
#include "mbedtls/sha256.h"

namespace w = fk::wire;
namespace cr = fk::crypto;
namespace ble = fk::ble;
using Bytes = std::vector<uint8_t>;

static uint32_t g_x = 0x12345678;
static void test_rng(uint8_t *out, size_t len) {
  for (size_t i = 0; i < len; i++) {
    g_x = g_x * 1664525u + 1013904223u;
    out[i] = uint8_t(g_x >> 24);
  }
}

static std::string hex(const uint8_t *p, size_t n) {
  static const char *d = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; i++) {
    s += d[p[i] >> 4];
    s += d[p[i] & 15];
  }
  return s;
}
static std::string hex(const Bytes &b) { return hex(b.data(), b.size()); }
static std::string q(const std::string &s) { return "\"" + s + "\""; }  // plain ASCII only
static std::string num(long long v) { return std::to_string(v); }

// {"k": v, ...} in insertion order.
struct Obj {
  std::string s;
  Obj &add(const char *k, const std::string &v) {
    s += (s.empty() ? "" : ", ") + q(k) + ": " + v;
    return *this;
  }
  std::string str() const { return "{" + s + "}"; }
};

static std::vector<std::string> g_records;
static void record(const char *name, const char *kind, const uint8_t *p, size_t n, const Obj &expect) {
  Obj o;
  o.add("name", q(name)).add("kind", q(kind)).add("hex", q(hex(p, n))).add("expect", expect.str());
  g_records.push_back("    " + o.str());
}

struct FakeLink : fk::Link {
  fk::Transport t = fk::Transport::BLUETOOTH;
  int slot = -1;
  fk::Transport transport() const override { return t; }
  int phone_slot() const override { return slot; }
  fk::Outcome forget_me() override { return fk::Outcome::ok(); }
  fk::Outcome issue_wifi_key(uint8_t id[16], uint8_t secret[32]) override {
    if (t != fk::Transport::BLUETOOTH)
      return fk::Link::issue_wifi_key(id, secret);
    for (int i = 0; i < 16; i++)
      id[i] = uint8_t(0xA0 + i);
    for (int i = 0; i < 32; i++)
      secret[i] = uint8_t(0x40 + i);
    return fk::Outcome::ok();
  }
};

// Answers the board firmware update commands as the board does at the given step.
struct FakeUpdater : fk::BoardUpdater {
  fk::UpdateStatus st;
  uint32_t resume_at = 0;
  fk::Outcome begin_result = fk::Outcome::ok(), finish_result = fk::Outcome::ok();
  fk::Outcome update_begin(uint32_t, const uint8_t *, uint32_t *offset) override {
    *offset = resume_at;
    return begin_result;
  }
  void update_status(fk::UpdateStatus &out) const override { out = st; }
  fk::Outcome update_finish() override { return finish_result; }
  void update_abort() override {}
};

// Keeps one phone's preferences and answers as PushEngine does (GET always shows bit 10).
struct FakePush : fk::PushControl {
  fk::PushPrefs p;
  bool prefs_get(int, fk::PushPrefs &out) const override {
    out = p;
    out.alerts |= 1u << fk::push::CO_BIT;
    return true;
  }
  fk::Outcome prefs_set(int, const fk::PushPrefs &in) override {
    p = in;
    return fk::Outcome::ok();
  }
};

struct FakeBoard : fk::BoardSettings {
  fk::BoardUpdater *upd = nullptr;
  fk::PushControl *psh = nullptr;
  fk::BoardUpdater *updater() override { return upd; }
  fk::PushControl *push() override { return psh; }
  fk::Outcome set_pin(uint32_t) override { return fk::Outcome::ok(); }
  fk::Outcome set_web_password(const uint8_t *, size_t, const uint8_t *, size_t) override { return fk::Outcome::ok(); }
  void wifi_status(fk::WifiStatus &) const override {}
  fk::Outcome wifi_set(const uint8_t *, size_t, const uint8_t *, size_t) override { return fk::Outcome::ok(); }
  fk::Outcome wifi_off() override { return fk::Outcome::ok(); }
  bool ha_switchable() const override { return true; }
  fk::HaState ha_state() const override { return fk::HaState::OFF; }
  bool ha_key_set() const override { return false; }
  fk::Outcome ha_set(bool) override { return fk::Outcome::ok(); }
  fk::Outcome ha_key(uint8_t out[32]) override {
    std::memset(out, 0, 32);
    return fk::Outcome::ok();
  }
  fk::Outcome ha_key_new(uint8_t out[32]) override {
    std::memset(out, 0, 32);
    return fk::Outcome::ok();
  }
};

struct Rig {
  FakeHeater h;
  fk::Controller c{&h};
  fk::Automation a{c};  // the automation commands and State's automation bytes
  FakeLink link;
  FakeBoard board;
  Bytes run(const Bytes &cmd) {
    uint8_t out[ble::RESULT_MAX];
    size_t n = ble::handle_command(cmd.data(), cmd.size(), c, link, board, out, sizeof(out));
    return Bytes(out, out + n);
  }
};

static Obj result_expect(const Bytes &r) {
  Obj o;
  o.add("requestId", num(r[0])).add("opcode", num(r[1])).add("code", num(r[2]))
      .add("data", q(hex(r.data() + 3, r.size() - 3)));
  return o;
}

static void records() {
  // --- Info: disjoint bit subsets, and every number distinct ---
  {
    struct V {
      const char *name;
      uint32_t mask;  // the capability bits as Info carries them (bits 6 and 8 are the board's; bit 4 is never set)
      bool window, pin, webpw;
      fk::WifiState wifi;
      fk::HaState ha;
      uint8_t ver[3];
      uint32_t build;
    };
    // Masks 0x2a, 0x4c, 0x60 give bits 1..6 each their own on/off pattern; flags bits 0-2 likewise.
    const V vs[] = {
        {"info, capabilities 0x16f, flags bits 0 and 1", 0x16F, true, true, false, fk::WifiState::CONNECTED,
         fk::HaState::WAITING, {1, 2, 3}, 1795000000u},
        {"info, capabilities 0x2a, flags bit 2", 0x2A, false, false, true, fk::WifiState::FAILED, fk::HaState::CONNECTED,
         {0, 10, 255}, 1},
        {"info, capabilities 0x14c, flags bits 0, 1 and 2", 0x14C, true, true, true, fk::WifiState::CONNECTING,
         fk::HaState::OFF, {255, 0, 7}, 0xFFFFFFFEu},
        {"info, capabilities 0x60, no flags", 0x60, false, false, false, fk::WifiState::OFF, fk::HaState::WAITING,
         {0, 0, 0}, 0},
        {"info, capabilities 0x801, the heater's fault log", 0x801, false, false, false, fk::WifiState::CONNECTED,
         fk::HaState::CONNECTED, {0, 2, 0}, 1795000000u},
    };
    for (const V &v : vs) {
      fk::BoardInfo p;
      p.window_open = v.window;
      p.wifi = v.wifi;
      p.pin_changed = v.pin;
      p.web_password_set = v.webpw;
      p.ha = v.ha;
      p.ha_switchable = (v.mask & 0x40) != 0;
      uint8_t out[ble::INFO_SIZE];
      p.can_update = (v.mask & 0x100) != 0;
      p.version[0] = v.ver[0], p.version[1] = v.ver[1], p.version[2] = v.ver[2];
      p.build_utc = v.build;
      // The heater also reports bit 4 (parameters): Info leaves it out.
      size_t n = ble::encode_info(ble::BOARD_FLAMEKEEPER_REV_A2,
                                  (v.mask & 0xBF) | 0x10 | ((v.mask & 0x800) ? fk::cap::FAULT_LOG : 0), p, out);
      CHECK_EQ(n, 14);
      uint32_t caps = v.mask;
      auto bit = [&](int i) { return std::string((caps >> i) & 1 ? "true" : "false"); };
      Obj cp;
      cp.add("fanOnly", bit(0)).add("temperature", bit(1)).add("prime", bit(2)).add("primeStop", bit(3))
          .add("roomSource", bit(5)).add("haSwitch", bit(6))
          .add("bluetoothRoom", bit(7)).add("boardUpdate", bit(8)).add("heaterLog", bit(11));
      Obj e;
      e.add("major", num(ble::PROTOCOL_MAJOR))
          .add("board", num(ble::BOARD_FLAMEKEEPER_REV_A2)).add("pairingOpen", v.window ? "true" : "false")
          .add("capabilities", cp.str()).add("wifi", num(int(v.wifi)))
          .add("pinChanged", v.pin ? "true" : "false").add("webPasswordSet", v.webpw ? "true" : "false")
          .add("homeAssistant", num(int(v.ha)))
          .add("firmwareVersion", q(num(v.ver[0]) + "." + num(v.ver[1]) + "." + num(v.ver[2])))
          .add("buildUtc", num(v.build));
      record(v.name, "info", out, n, e);
    }

  }

  // --- State, heating from a phone ---
  {
    Rig r;
    r.h.caps |= fk::cap::ROOM_SOURCE;
    CHECK(r.run({2, ble::op::SET_HEAT_MODE, 0})[2] == 0);
    CHECK(r.run({3, ble::op::SET_POWER, 45})[2] == 0);
    CHECK(r.run({4, ble::op::SET_TARGET, 0xD7, 0x00})[2] == 0);  // 21.5 C
    CHECK(r.run({5, ble::op::SET_MODE, 1})[2] == 0);
    r.h.st.state = fk::HeaterState::HEATING;
    r.h.st.answering = true;
    r.h.st.fault_log_seq = 7;
    r.c.room().set_source(fk::RoomSource::PROBE);
    r.c.room().probe(19.3f, 1000);
    r.c.tick(1000, false);
    const fk::Status &s = r.c.status();
    uint8_t out[ble::STATE_SIZE];
    size_t n = ble::encode_state(s, out);
    CHECK_EQ(n, 39);
    CHECK_EQ(s.fault_log_seq, 7);
    CHECK(s.run_source == fk::Source::PHONE);
    Obj e;
    e.add("heaterAnswering", "true").add("failsafeStopped", "false").add("roomFallback", "false")
        .add("updateRunning", "false").add("thermostatPaused", "false").add("requestedMode", num(int(fk::Mode::HEAT)))
        .add("heatMode", num(int(fk::HeatMode::POWER))).add("power", "45").add("targetC", "21.5")
        .add("roomC", "19.3").add("roomSource", num(int(fk::RoomSource::PROBE)))
        .add("heaterState", num(int(fk::HeaterState::HEATING)))
        .add("controllerStatus", num(int(s.control))).add("fault", "0").add("primeSecondsLeft", "0")
        .add("failsafeSecondsLeft", "null").add("runSource", num(int(fk::Source::PHONE)))
        .add("stagedChanges", "0").add("alerts", "0").add("automatic", "0").add("fuelLeft", "null")
        .add("burnoffMinutesLeft", "0").add("timerSlot", "null").add("runEndUtc", "null")
        .add("runId", num(s.run_id)).add("heaterLogSeq", "7").add("startAtUtc", "null").add("startAtMinutes", "0");
    CHECK(s.run_id == 1);
    record("state, heating from a phone", "state", out, n, e);
  }

  // --- State flags: masks 0x0f, 0x05, 0x0a, 0x03 ---
  // Each of bits 0-2 has its own pattern over the four records (0: 1101, 1: 1011, 2: 1100). Bit 3 is
  // set in the status but no longer sent (heater state 8 says the heater is updating).
  const uint8_t state_masks[4] = {0x0F, 0x05, 0x0A, 0x03};
  for (int i = 0; i < 4; i++) {
    uint8_t m = state_masks[i];
    bool odd = i % 2 == 1;
    fk::Status s;
    s.answering = m & 0x01;
    s.failsafe_stopped = m & 0x02;
    s.room_fallback = m & 0x04;
    s.update_running = m & 0x08;
    s.params_pending = uint8_t(i * 2);
    s.mode = odd ? fk::Mode::FAN : fk::Mode::HEAT;
    s.heat_mode = odd ? fk::HeatMode::TEMPERATURE : fk::HeatMode::POWER;
    s.power = odd ? 100 : 30;
    s.target_c = odd ? 35.0f : 5.0f;
    s.room_c = odd ? -3.2f : NAN;
    s.room_source = odd ? fk::RoomSource::PROBE : fk::RoomSource::HOME_ASSISTANT;
    s.state = odd ? fk::HeaterState::COOLING : fk::HeaterState::STARTING;
    s.control = odd ? fk::ControlStatus::STOPPING : fk::ControlStatus::NO_REPLY;
    s.fault = odd ? fk::Fault::FLAME_OUT : fk::Fault::NONE;
    s.prime_left_s = odd ? 25 : 0;
    s.failsafe_left_s = odd ? 600 : UINT32_MAX;
    s.run_source = odd ? fk::Source::WIFI_APP : fk::Source::BOARD;
    s.alerts = odd ? 0x0555 : 0x0AAA;
    s.automatic = odd ? 0x05 : 0x0A;
    s.fuel_left = odd ? 37 : 0xFF;
    s.burnoff_left_min = odd ? 17 : 0;
    s.timer_slot = odd ? 3 : 0xFF;
    s.run_end_utc = odd ? 1768199400u : 0;
    s.run_id = odd ? 0xA5C30007u : 0x00010001u;
    s.thermo_paused = odd;  // bit 6
    s.fault_log_seq = odd ? 201 : 0;
    s.start_at_utc = odd ? 1768195800u : 0;
    s.start_at_min = odd ? 45 : 0;
    uint8_t out[ble::STATE_SIZE];
    size_t n = ble::encode_state(s, out);
    CHECK_EQ(out[0], (m & 0x07) | (odd ? 0x40 : 0));
    auto b = [&](int bit) { return std::string((m >> bit) & 1 ? "true" : "false"); };
    Obj e;
    e.add("heaterAnswering", b(0)).add("failsafeStopped", b(1)).add("roomFallback", b(2))
        .add("thermostatPaused", odd ? "true" : "false").add("requestedMode", num(int(s.mode)))
        .add("heatMode", num(int(s.heat_mode))).add("power", num(s.power)).add("targetC", odd ? "35" : "5")
        .add("roomC", odd ? "-3.2" : "null").add("roomSource", num(int(s.room_source)))
        .add("heaterState", num(int(s.state))).add("controllerStatus", num(int(s.control)))
        .add("fault", num(int(s.fault))).add("primeSecondsLeft", num(s.prime_left_s))
        .add("failsafeSecondsLeft", odd ? "600" : "null").add("runSource", num(int(s.run_source)))
        .add("stagedChanges", num(s.params_pending)).add("alerts", num(s.alerts))
        .add("automatic", num(s.automatic)).add("fuelLeft", odd ? "37" : "null")
        .add("burnoffMinutesLeft", odd ? "17" : "0").add("timerSlot", odd ? "3" : "null")
        .add("runEndUtc", odd ? "1768199400" : "null")
        .add("runId", num(s.run_id)).add("heaterLogSeq", odd ? "201" : "0")
        .add("startAtUtc", odd ? "1768195800" : "null").add("startAtMinutes", odd ? "45" : "0");
    char name[48];
    std::snprintf(name, sizeof(name), "state, flags 0x%02x", m);
    record(name, "state", out, n, e);
  }

  // --- Telemetry ---
  {
    fk::Status s;
    s.heat_exchanger_c = 187.26f;
    s.supply_v = 12.34f;
    s.fan_rpm = 3412.0f;
    s.pump_hz = 2.8f;
    s.glow_w = 0.0f;
    s.power_pct = 55.4f;
    s.panel_fault = 8;
    s.intake_c = -12.0f;
    s.co_ppm = 3.0f;
    s.altitude_m = 1712.4f;
    s.run_time_s = 123456.7f;
    s.fan_v = 7.856f;
    s.glow_v = 9.5f;
    s.glow_a = 8.04f;
    s.pressure_hpa = 1013.27f;
    s.co_state = 3;  // ok
    s.run_s = 8040;  // this run: 2 h 14 min
    s.room_probe_c = 19.4f;
    s.room_ha_c = 21.06f;
    s.room_bt_c = -4.5f;
    s.altitude_trim_pct = -17;
    s.fuel_trim_pct = -12;
    s.altitude_smoothed_m = 1705.6f;
    uint8_t out[ble::TELEMETRY_SIZE];
    size_t n = ble::encode_telemetry(s, out);
    CHECK_EQ(n, 45);
    Obj e;
    e.add("exchangerC", "187.3").add("supplyV", "12.34").add("fanRpm", "3412").add("pumpHz", "2.8")
        .add("glowW", "0").add("powerPercent", "55").add("panelFault", "8").add("intakeC", "-12")
        .add("coPpm", "3").add("altitudeM", "1712").add("runningSeconds", "123456").add("fanV", "7.86")
        .add("glowV", "9.5").add("glowA", "8.04").add("pressureHpa", "1013.3").add("coState", "3")
        .add("runSeconds", "8040").add("roomProbeC", "19.4").add("roomHomeAssistantC", "21.1")
        .add("roomBluetoothC", "-4.5").add("altitudeTrimPercent", "-17").add("fuelTrimPercent", "-12")
        .add("altitudeSmoothedM", "1706");
    record("telemetry, every value", "telemetry", out, n, e);

    fk::Status u;  // everything unknown
    n = ble::encode_telemetry(u, out);
    Obj z;
    z.add("exchangerC", "null").add("supplyV", "null").add("fanRpm", "null").add("pumpHz", "null")
        .add("glowW", "null").add("powerPercent", "null").add("panelFault", "0").add("intakeC", "null")
        .add("coPpm", "null").add("altitudeM", "null").add("runningSeconds", "null").add("fanV", "null")
        .add("glowV", "null").add("glowA", "null").add("pressureHpa", "null").add("coState", "0")
        .add("runSeconds", "0").add("roomProbeC", "null").add("roomHomeAssistantC", "null")
        .add("roomBluetoothC", "null").add("altitudeTrimPercent", "null").add("fuelTrimPercent", "0")
        .add("altitudeSmoothedM", "null");
    record("telemetry, everything unknown", "telemetry", out, n, z);
  }

  // --- Results ---
  {
    Rig r;  // the heater is not answering: a start is refused with the backend's reason
    r.h.st.answering = false;
    Bytes refusal = r.run({7, ble::op::SET_MODE, 1});
    CHECK(refusal[2] == uint8_t(fk::Result::NO_REPLY));
    r.h.st.answering = true;
    Obj e = result_expect(refusal);
    e.add("text", q(std::string((const char *) refusal.data() + 3, refusal.size() - 3)));
    record("result, refusal (no reply)", "result", refusal.data(), refusal.size(), e);

    // An opcode nobody uses is refused as unknown.
    Bytes unknown = r.run({8, 0x05, 1});
    CHECK(unknown[2] == uint8_t(fk::Result::UNKNOWN_OPCODE));
    Obj e1 = result_expect(unknown);
    e1.add("text", q(std::string((const char *) unknown.data() + 3, unknown.size() - 3)));
    record("result, unknown opcode (05)", "result", unknown.data(), unknown.size(), e1);

    struct P {
      uint16_t id;
      uint8_t flags;
      fk::Unit unit;
      uint8_t decimals;
      int32_t min, max, step;
      const char *name;
      fk::ParamGroup group;
      fk::ParamLevel level;  // bit 3 of the flags (advanced) for anything above basic
    };
    using G = fk::ParamGroup;
    using L = fk::ParamLevel;
    const P ps[] = {
        {0x0105, fk::pflag::NEEDS_OFF, fk::Unit::CELSIUS, 1, -150, 2500, 5, "Start temperature", G::START_UP,
         L::ADVANCED},
        {0x0201, uint8_t(fk::pflag::READ_ONLY | fk::pflag::BOARD), fk::Unit::CELSIUS_DIFFERENCE, 1, 0, 100, 5,
         "Thermostat band", G::THERMOSTAT, L::BASIC},
        {0x0302, fk::pflag::BOARD, fk::Unit::SECONDS, 0, 0, 3600, 10, "Delay", G::SHUT_DOWN, L::EXPERT},
        {0x0403, fk::pflag::READ_ONLY, fk::Unit::RPM, 0, 0, 6000, 1, "Fan speed", G::FAN, L::ADVANCED},
    };
    for (const P &p : ps) {
      r.h.params.push_back({});
      FakeHeater::Param &hp = r.h.params.back();
      hp.info.id = p.id;
      hp.info.flags = p.flags;
      hp.info.unit = p.unit;
      hp.info.decimals = p.decimals;
      hp.info.min = p.min;
      hp.info.max = p.max;
      hp.info.step = p.step;
      hp.info.name = p.name;
      hp.info.group = p.group;
      hp.info.level = p.level;
    }
    r.h.params[0].help = "Shell temperature at which the start counts as done. Higher waits longer.";
    for (uint8_t i = 0; i < 4; i++) {
      const P &p = ps[i];
      Bytes d = r.run({uint8_t(9 + i), ble::op::PARAM_DESCRIBE, i});
      CHECK(d[2] == 0);
      Obj pd;
      pd.add("index", num(i)).add("count", num(d[4])).add("id", num(p.id))
          .add("needsHeaterOff", (p.flags & fk::pflag::NEEDS_OFF) ? "true" : "false")
          .add("boardSetting", (p.flags & fk::pflag::BOARD) ? "true" : "false")
          .add("readOnly", (p.flags & fk::pflag::READ_ONLY) ? "true" : "false")
          .add("unit", num(int(p.unit))).add("decimals", num(p.decimals)).add("group", num(int(p.group)))
          .add("level", num(int(p.level))).add("min", num(p.min)).add("max", num(p.max)).add("step", num(p.step))
          .add("name", q(p.name));
      Obj e2 = result_expect(d);
      e2.add("paramDescription", pd.str());
      static const char *const names[4] = {
          "result, PARAM_DESCRIBE flags 0x01 (needs heater off; start-up, advanced)",
          "result, PARAM_DESCRIBE flags 0x06 (read only, board setting; unit 10, thermostat, basic)",
          "result, PARAM_DESCRIBE flags 0x02 (board setting; shut-down, expert)",
          "result, PARAM_DESCRIBE flags 0x04 (read only; fan, advanced)"};
      record(names[i], "result", d.data(), d.size(), e2);
    }

    // PARAM_DESCRIBE_ID: the same answer, found by id (0x0302, index 2).
    {
      Bytes d = r.run({13, ble::op::PARAM_DESCRIBE_ID, 0x02, 0x03});
      CHECK(d[2] == 0 && d[3] == 2);
      const P &p = ps[2];
      Obj pd;
      pd.add("index", "2").add("count", num(d[4])).add("id", num(p.id)).add("needsHeaterOff", "false")
          .add("boardSetting", "true").add("readOnly", "false").add("unit", num(int(p.unit)))
          .add("decimals", num(p.decimals)).add("group", num(int(p.group))).add("level", num(int(p.level)))
          .add("min", num(p.min)).add("max", num(p.max)).add("step", num(p.step))
          .add("name", q(p.name));
      Obj e2 = result_expect(d);
      e2.add("paramDescription", pd.str());
      record("result, PARAM_DESCRIBE_ID 0x0302", "result", d.data(), d.size(), e2);
    }

    // PARAM_GROUP: the first group and the fourth, by name.
    for (uint8_t gi : {uint8_t(0), uint8_t(3)}) {
      Bytes g = r.run({uint8_t(20 + gi), ble::op::PARAM_GROUP, gi});
      CHECK(g[2] == 0);
      const uint8_t *p = g.data() + 3;
      const std::string name((const char *) p + 3, g.size() - 3 - 3);
      Obj pg;
      pg.add("index", num(p[0])).add("count", num(p[1])).add("group", num(p[2])).add("name", q(name));
      Obj e4 = result_expect(g);
      e4.add("paramGroup", pg.str());
      record(gi == 0 ? "result, PARAM_GROUP 0" : "result, PARAM_GROUP 3", "result", g.data(), g.size(), e4);
    }

    // PARAM_HELP: a heater parameter's description, and a board setting's.
    for (uint16_t id : {uint16_t(0x0105), uint16_t(fk::T_CYCLE)}) {
      Bytes hb = r.run({30, ble::op::PARAM_HELP, uint8_t(id), uint8_t(id >> 8)});
      CHECK(hb[2] == 0);
      Obj ph;
      ph.add("id", num(id)).add("text", q(std::string((const char *) hb.data() + 5, hb.size() - 5)));
      Obj e5 = result_expect(hb);
      e5.add("paramHelp", ph.str());
      record(id == 0x0105 ? "result, PARAM_HELP (heater parameter)" : "result, PARAM_HELP (board setting)", "result",
             hb.data(), hb.size(), e5);
    }

    Bytes k = r.run({4, ble::op::WIFI_KEY});
    CHECK(k.size() == 3 + 48 && k[2] == 0);
    Obj kk;
    kk.add("id", q(hex(k.data() + 3, 16))).add("secret", q(hex(k.data() + 19, 32)));
    Obj e3 = result_expect(k);
    e3.add("wifiKey", kk.str());
    record("result, WIFI_KEY", "result", k.data(), k.size(), e3);
  }

  // --- Event 05: Wi-Fi changed ---
  {
    fk::WifiStatus ws;
    ws.state = fk::WifiState::CONNECTED;
    ws.ip[0] = 192, ws.ip[1] = 168, ws.ip[2] = 1, ws.ip[3] = 20;
    std::strcpy(ws.ssid, "Home");
    uint8_t out[ble::RESULT_MAX];
    size_t n = ble::encode_wifi_event(&ws, nullptr, out, sizeof(out));
    Bytes b(out, out + n);
    Obj ch;
    ch.add("ok", "true").add("address", q("192.168.1.20")).add("ssid", q("Home"));
    Obj e = result_expect(b);
    e.add("wifiChanged", ch.str());
    record("event 05, Wi-Fi connected", "result", out, n, e);

    n = ble::encode_wifi_event(nullptr, "wrong password", out, sizeof(out));
    Bytes f(out, out + n);
    Obj ch2;
    ch2.add("ok", "false").add("reason", q("wrong password"));
    Obj e2 = result_expect(f);
    e2.add("wifiChanged", ch2.str());
    record("event 05, Wi-Fi failed", "result", out, n, e2);
  }

  // --- clock, meters, timers, thermometers, event 08 ---
  {
    Rig r;
    const uint32_t utc = 1768194000u;  // 2026-01-12 05:00 UTC, a Monday
    const char *tz = "CET-1CEST,M3.5.0,M10.5.0/3";
    CHECK(r.a.set_clock(utc, tz, fk::ClockSource::APP, 0).accepted());
    Bytes g = r.run({40, ble::op::CLOCK_GET});
    CHECK(g[2] == 0);
    Obj ck;
    ck.add("utc", num(utc)).add("source", "1").add("timeZone", q(tz));
    Obj e = result_expect(g);
    e.add("clock", ck.str());
    record("result, CLOCK_GET", "result", g.data(), g.size(), e);

    fk::FuelMeter m;  // an hour at 2 Hz, 22 mL / 1000 strokes, glow plug on for 90 s
    for (int i = 0; i < 3600; i++)
      m.add(1.0f, 2.0f, 0.022f, true, i < 90, utc - 3600 + uint32_t(i), int32_t((utc + 3600) / 86400));
    r.a.load_meter(m.saved());
    Bytes mt = r.run({41, ble::op::METERS});
    CHECK(mt.size() == 3 + 56);
    static const char *const KEYS[14] = {"runFuelMl",   "runBurnS",   "runGlowS",   "dayFuelMl", "dayBurnS",
                                         "weekFuelMl",  "weekBurnS",  "monthFuelMl", "monthBurnS", "lifeFuelMl",
                                         "lifeBurnS",   "lifeGlowS",  "tankUsedMl",  "lowPowerSinceBurnoffS"};
    Obj mo;
    for (int i = 0; i < 14; i++) {
      const uint8_t *p = mt.data() + 3 + 4 * i;
      uint32_t v = uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
      mo.add(KEYS[i], v == 0xFFFFFFFFu ? "null" : num(v));
    }
    Obj e1 = result_expect(mt);
    e1.add("meters", mo.str());
    record("result, METERS (an hour at 2 Hz)", "result", mt.data(), mt.size(), e1);

    fk::TimerSpec t;  // weekdays 06:30, 90 min, temperature 21.5 C
    t.flags = fk::TimerSpec::ENABLED;
    t.days = 0x1F;
    t.start_min = 390;
    t.duration_min = 90;
    t.mode = 1;
    t.heat_mode = 1;
    t.power = 50;
    t.target_x10 = 215;
    CHECK(r.a.timer_set(2, t).accepted());
    fk::TimerSpec o;  // a one-off: fan only at 40 % for 20 min
    o.flags = fk::TimerSpec::ENABLED | fk::TimerSpec::ONE_OFF;
    o.once_utc = utc + 86400;
    o.duration_min = 20;
    o.mode = 2;
    o.power = 40;
    CHECK(r.a.timer_set(5, o).accepted());
    for (uint8_t slot : {uint8_t(2), uint8_t(5), uint8_t(0)}) {
      Bytes tg = r.run({42, ble::op::TIMER_GET, slot});
      CHECK(tg[2] == 0);
      fk::TimerSpec x;
      r.a.timer_get(slot, x);
      Obj to;
      to.add("slot", num(slot)).add("slots", "8").add("enabled", x.enabled() ? "true" : "false")
          .add("oneOff", x.one_off() ? "true" : "false").add("days", num(x.days)).add("startMinute", num(x.start_min))
          .add("onceUtc", num(x.once_utc)).add("durationMinutes", num(x.duration_min)).add("mode", num(x.mode))
          .add("heatMode", num(x.heat_mode)).add("power", num(x.power));
      char tc[16];
      std::snprintf(tc, sizeof(tc), "%g", x.target_x10 / 10.0);
      to.add("targetC", tc);
      Obj e2 = result_expect(tg);
      e2.add("timer", to.str());
      record(slot == 2 ? "result, TIMER_GET (weekdays 06:30)" : slot == 5 ? "result, TIMER_GET (one-off, fan only)"
                                                                          : "result, TIMER_GET (empty slot)",
             "result", tg.data(), tg.size(), e2);
    }

    r.c.tick(1000, false);
    r.run({43, ble::op::SENSOR_SCAN, 30});
    const uint8_t addr[6] = {0xC0, 0xFF, 0xEE, 0x12, 0x34, 0x56};
    fk::ThermoReading rd;
    rd.kind = fk::ThermoKind::XIAOMI_CUSTOM;
    rd.temperature_c = -4.2f;
    rd.humidity_pct = 71.0f;
    rd.battery_pct = 93;
    r.a.sensors().advert(addr, -67, "ATC_123456", rd, 1000);
    Bytes sf = r.run({44, ble::op::SENSOR_FOUND, 0});
    CHECK(sf[2] == 0);
    Obj so;
    so.add("index", "0").add("count", "1").add("scanning", "true").add("chosen", "false")
        .add("address", q("C0:FF:EE:12:34:56")).add("kind", "2").add("rssi", "-67").add("temperatureC", "-4.2")
        .add("humidity", "71").add("battery", "93").add("name", q("ATC_123456"));
    Obj e3 = result_expect(sf);
    e3.add("sensorFound", so.str());
    record("result, SENSOR_FOUND", "result", sf.data(), sf.size(), e3);
    r.a.sensor_use(addr, fk::ThermoKind::XIAOMI_CUSTOM);
    Bytes ss = r.run({45, ble::op::SENSOR_STATUS});
    CHECK(ss[2] == 0);
    Obj sto;
    sto.add("address", q("C0:FF:EE:12:34:56")).add("kind", "2").add("rssi", "-67").add("temperatureC", "-4.2")
        .add("humidity", "71").add("battery", "93").add("ageSeconds", "0");
    Obj e4 = result_expect(ss);
    e4.add("sensorStatus", sto.str());
    record("result, SENSOR_STATUS", "result", ss.data(), ss.size(), e4);

    uint8_t out[ble::RESULT_MAX];
    size_t n = ble::encode_event(fk::Event::ALERT, fk::Result::OK, 0, "fuel low: about 14 % left", out, sizeof(out));
    CHECK(n == 3 + 1 + 25 && out[4] == 'f');
    Bytes ev(out, out + n);
    Obj al;
    al.add("bit", "0").add("text", q("fuel low: about 14 % left"));
    Obj e5 = result_expect(ev);
    e5.add("alert", al.str());
    record("event 08, alert (fuel low)", "result", out, n, e5);
  }

  // --- event log, glow plug health ---
  {
    Rig r;
    const uint32_t utc = 1768194000u;
    r.a.log().add(fk::LogKind::BOARD_STARTED, 0, 1, 0, 0);
    r.a.log().add(fk::LogKind::RUN_STARTED, uint8_t(fk::Source::PHONE), 1, 0, utc);
    r.a.log().add(fk::LogKind::LOW_BATTERY_STOP, 0, 0, 11300, utc + 3600);
    Bytes lg = r.run({50, ble::op::LOG_READ, 2, 0, 0, 0});
    CHECK(lg[2] == 0 && lg.size() == 3 + 5 + 2 * 16);
    Obj lo;
    std::string recs = "[";
    {
      Obj a;
      a.add("seq", "2").add("utc", num(utc)).add("kind", "1").add("source", "2").add("a", "1").add("b", "0");
      Obj b;
      b.add("seq", "3").add("utc", num(utc + 3600)).add("kind", "9").add("source", "0").add("a", "0")
          .add("b", "11300");
      recs += a.str() + ", " + b.str() + "]";
    }
    lo.add("newest", "3").add("records", recs);
    Obj e = result_expect(lg);
    e.add("log", lo.str());
    record("result, LOG_READ from 2", "result", lg.data(), lg.size(), e);

    fk::Automation::Maintenance m{};
    m.check = fk::Automation::MAINTENANCE_CHECK;
    m.glow_baseline_mohm = 1000;
    m.glow_starts = 4;
    m.glow_n = 4;
    m.glow_readings[0] = 1010;
    m.glow_readings[1] = 990;
    m.glow_readings[2] = 1000;
    m.glow_readings[3] = 1350;
    r.a.load_maintenance(m);
    Bytes gh = r.run({51, ble::op::GLOW_HEALTH});
    CHECK(gh[2] == 0 && gh.size() == 3 + 7 + 8);
    Obj go;
    go.add("baselineMohm", "1000").add("lastMohm", "1350").add("starts", "4").add("readingsMohm", "[1010, 990, 1000, 1350]");
    Obj e2 = result_expect(gh);
    e2.add("glowHealth", go.str());
    record("result, GLOW_HEALTH", "result", gh.data(), gh.size(), e2);
  }

  // --- the heater's fault log: HEATER_LOG pages, HEATER_LOG_CLEAR ---
  {
    Rig r;
    r.h.caps |= fk::cap::FAULT_LOG;
    auto entry = [](uint8_t kind, uint8_t code, uint8_t why, uint8_t step, uint8_t seq, int8_t board, int16_t hx,
                    uint16_t supply, uint32_t up) {
      fk::HeaterLogEntry e;
      e.kind = kind, e.code = code, e.why = why, e.step = step, e.seq = seq;
      e.board_c = board, e.hx_c10 = hx, e.supply_cv = supply, e.uptime_s = up;
      return e;
    };
    // Newest first: an overheat while running, a dead-man stop while warming up (heat exchanger and
    // board unknown), a watchdog reset that started a cooldown.
    r.h.log.push_back(entry(fk::HEATER_LOG_FAULT, 6, 5, 6, 12, 46, 2513, 2380, 12307));
    r.h.log.push_back(entry(fk::HEATER_LOG_FAULT, 0, 2, 5, 11, -128, INT16_MIN, 1190, 59));
    r.h.log.push_back(entry(fk::HEATER_LOG_RESET, 2, 0, 8, 10, -5, -45, 2410, 0));
    r.h.st.fault_log_count = 3;
    r.h.st.fault_log_seq = 12;
    auto expect_entry = [](const fk::HeaterLogEntry &e) {
      Obj o;
      o.add("kind", num(e.kind)).add("code", num(e.code)).add("why", num(e.why)).add("step", num(e.step))
          .add("seq", num(e.seq)).add("boardC", e.board_c == -128 ? "null" : num(e.board_c))
          .add("heatExchangerC", e.hx_c10 == INT16_MIN ? "null" : (e.hx_c10 < 0 ? "-" : "") +
                                     num(std::abs(e.hx_c10) / 10) + "." + num(std::abs(e.hx_c10) % 10))
          .add("supplyV", e.supply_cv == 0xFFFF ? "null" : num(e.supply_cv / 100) + "." +
                                                             (e.supply_cv % 100 < 10 ? "0" : "") + num(e.supply_cv % 100))
          .add("uptimeSeconds", num(e.uptime_s));
      return o.str();
    };
    Bytes hl = r.run({60, ble::op::HEATER_LOG, 0});
    CHECK(hl[2] == 0 && hl.size() == 3 + 1 + 3 * 14 && hl[3] == 3);
    Obj lo;
    lo.add("count", "3").add("entries", "[" + expect_entry(r.h.log[0]) + ", " + expect_entry(r.h.log[1]) + ", " +
                                            expect_entry(r.h.log[2]) + "]");
    Obj e = result_expect(hl);
    e.add("heaterLog", lo.str());
    record("result, HEATER_LOG from 0", "result", hl.data(), hl.size(), e);
    Bytes h2 = r.run({61, ble::op::HEATER_LOG, 2});
    CHECK(h2[2] == 0 && h2.size() == 3 + 1 + 14);
    Obj l2;
    l2.add("count", "3").add("entries", "[" + expect_entry(r.h.log[2]) + "]");
    Obj e2 = result_expect(h2);
    e2.add("heaterLog", l2.str());
    record("result, HEATER_LOG from 2", "result", h2.data(), h2.size(), e2);
    Bytes hc = r.run({62, ble::op::HEATER_LOG_CLEAR});
    CHECK(hc[2] == 0 && hc.size() == 3 && r.h.log.empty());
    record("result, HEATER_LOG_CLEAR", "result", hc.data(), hc.size(), result_expect(hc));
  }

  // --- RUN_UNTIL: the current run, whoever started it, stops at a UTC time (0 = no end) ---
  {
    Rig r;
    auto cmd_vec = [&](const char *name, const Bytes &cmd) {
      Obj e;
      e.add("requestId", num(cmd[0])).add("opcode", num(cmd[1])).add("args", q(hex(cmd.data() + 2, cmd.size() - 2)));
      record(name, "command", cmd.data(), cmd.size(), e);
    };
    auto res = [&](const char *name, const Bytes &cmd) {
      Bytes out = r.run(cmd);
      record(name, "result", out.data(), out.size(), result_expect(out));
      return out;
    };
    const uint32_t now_utc = 1768194000u;  // the board's clock
    const uint32_t end = now_utc + 45 * 60;  // "stop in 45 min"
    const Bytes until = {0x40, ble::op::RUN_UNTIL, uint8_t(end), uint8_t(end >> 8), uint8_t(end >> 16),
                         uint8_t(end >> 24)};
    const Bytes clear = {0x41, ble::op::RUN_UNTIL, 0, 0, 0, 0};
    cmd_vec("command RUN_UNTIL, 45 minutes from 1768194000", until);
    cmd_vec("command RUN_UNTIL 0, no end", clear);
    CHECK(res("result RUN_UNTIL refused 2f, the heater is not running", until)[2] == 0x2F);
    CHECK(r.a.set_clock(now_utc, "UTC0", fk::ClockSource::APP, 0).accepted());
    CHECK(r.run({0x42, ble::op::SET_MODE, 1})[2] == 0);
    r.h.st.state = fk::HeaterState::HEATING;
    r.h.st.answering = true;
    r.c.tick(1000, false);
    r.a.tick(1000);
    CHECK(res("result RUN_UNTIL ok", until)[2] == 0);
    CHECK_EQ(r.c.status().run_end_utc, end);
    {
      // State: a run a phone started, with no timer, that stops in 45 minutes.
      fk::Status st = r.c.status();
      st.target_c = 21.5f;
      st.room_c = NAN;
      st.room_source = fk::RoomSource::PROBE;
      st.run_id = 0x00010003u;
      uint8_t rec[ble::STATE_SIZE];
      size_t n = ble::encode_state(st, rec);
      Obj e;
      e.add("heaterAnswering", "true").add("failsafeStopped", "false").add("roomFallback", "false")
          .add("updateRunning", "false").add("thermostatPaused", "false").add("requestedMode", num(int(st.mode)))
          .add("heatMode", num(int(st.heat_mode))).add("power", num(st.power)).add("targetC", "21.5")
          .add("roomC", "null").add("roomSource", num(int(fk::RoomSource::PROBE)))
          .add("heaterState", num(int(fk::HeaterState::HEATING))).add("controllerStatus", num(int(st.control)))
          .add("fault", "0").add("primeSecondsLeft", "0").add("failsafeSecondsLeft", "null")
          .add("runSource", num(int(fk::Source::PHONE))).add("stagedChanges", "0").add("alerts", num(st.alerts))
          .add("automatic", num(st.automatic)).add("fuelLeft", "null").add("burnoffMinutesLeft", "0")
          .add("timerSlot", "null").add("runEndUtc", num(end)).add("runId", num(st.run_id))
          .add("heaterLogSeq", "0");
      CHECK(st.failsafe_left_s == UINT32_MAX && st.fuel_left == 0xFF && st.timer_slot == 0xFF && rec[0] == 0x01);
      record("state, a phone's run that stops in 45 minutes (RUN_UNTIL)", "state", rec, n, e);
    }
    const Bytes past = {0x43, ble::op::RUN_UNTIL, uint8_t(now_utc), uint8_t(now_utc >> 8), uint8_t(now_utc >> 16),
                        uint8_t(now_utc >> 24)};
    CHECK(res("result RUN_UNTIL out of range 12, the end has passed", past)[2] == 0x12);
    CHECK(res("result RUN_UNTIL 0 ok", clear)[2] == 0);
    CHECK_EQ(r.c.status().run_end_utc, 0);
  }

  // --- Event 06: the heater's answer to FAULT_RESET (the texts LinkClient sends) ---
  {
    struct V {
      const char *name;
      fk::Result r;
      const char *text;
    };
    const V vs[] = {
        {"event 06, fault cleared", fk::Result::OK, "fault cleared"},
        {"event 06, the heater kept the fault", fk::Result::REFUSED,
         "the heater kept the fault (its cause may still be there)"},
    };
    for (const V &v : vs) {
      uint8_t out[ble::RESULT_MAX];
      size_t n = ble::encode_event(fk::Event::FAULT_RESET, v.r, 0, v.text, out, sizeof(out));
      Bytes b(out, out + n);
      Obj e = result_expect(b);
      e.add("text", q(v.text));
      record(v.name, "result", out, n, e);
    }
  }
}

// The Wi-Fi key sign-in and encrypted frames.
// Board firmware updates (PROTOCOL.md): command results, the progress
// record, and State's flag bit 4.
static void update_records() {
  Rig r;
  FakeUpdater u;
  r.board.upd = &u;
  Bytes begin = {0x31, ble::op::UPDATE_BEGIN, 0xF4, 0xFF, 0x16, 0x00};  // 1507316 bytes
  for (int i = 0; i < 32; i++)
    begin.push_back(uint8_t(0xA0 + i));
  auto result = [&](const char *name, const Bytes &cmd) {
    Bytes out = r.run(cmd);
    Obj e = result_expect(out);
    record(name, "result", out.data(), out.size(), e);
    return out;
  };
  auto cmd_vec = [&](const char *name, const Bytes &cmd) {
    Obj e;
    e.add("requestId", num(cmd[0])).add("opcode", num(cmd[1])).add("args", q(hex(cmd.data() + 2, cmd.size() - 2)));
    record(name, "command", cmd.data(), cmd.size(), e);
  };
  cmd_vec("command UPDATE_BEGIN, 1507316 bytes, SHA-256 a0..bf", begin);
  // Over Bluetooth at MTU 247 (the fake link keeps the Wi-Fi chunk, 1000; the real one reports MTU - 7).
  u.resume_at = 0;
  CHECK(result("result UPDATE_BEGIN, new: offset 0, chunk 1000", begin).size() == 9);
  u.resume_at = 245760;
  CHECK(result("result UPDATE_BEGIN, resumed at 245760, chunk 1000", begin).size() == 9);
  u.begin_result = fk::Outcome::refuse(fk::Result::NOT_OFF, "the heater is not off yet (wait for the cooldown)");
  result("result UPDATE_BEGIN refused 20, heater not off", begin);
  u.begin_result = fk::Outcome::refuse(fk::Result::TOO_BIG, "the image does not fit the board's update slot");
  result("result UPDATE_BEGIN refused 2A, too big", begin);
  u.begin_result = fk::Outcome::refuse(fk::Result::BUSY, "another update is under way");
  result("result UPDATE_BEGIN refused 22, busy", begin);
  // Status, mid-way and after a rollback.
  u.st.state = fk::UpdateState::RECEIVING;
  u.st.size = 1507316;
  u.st.received = 245760;
  for (int i = 0; i < 32; i++)
    u.st.sha256[i] = uint8_t(0xA0 + i);
  u.st.last = fk::UpdateLast::NOTHING;
  CHECK(result("result UPDATE_STATUS, receiving 245760 of 1507316", {0x32, ble::op::UPDATE_STATUS}).size() == 45);
  u.st = fk::UpdateStatus{};
  u.st.last = fk::UpdateLast::ROLLED_BACK;
  result("result UPDATE_STATUS, none, last update rolled back", {0x33, ble::op::UPDATE_STATUS});
  // Finish: ok, and each refusal.
  result("result UPDATE_FINISH ok", {0x34, ble::op::UPDATE_FINISH});
  u.finish_result = fk::Outcome::refuse(fk::Result::HASH_MISMATCH, "not every byte has arrived yet");
  result("result UPDATE_FINISH refused 2B, bytes missing", {0x35, ble::op::UPDATE_FINISH});
  u.finish_result = fk::Outcome::refuse(fk::Result::BAD_IMAGE,
                                        "the image is not signed by a key this board trusts, or not valid");
  result("result UPDATE_FINISH refused 2C, bad signature", {0x36, ble::op::UPDATE_FINISH});
  result("result UPDATE_ABORT ok", {0x37, ble::op::UPDATE_ABORT});
  r.board.upd = nullptr;
  result("result UPDATE_STATUS refused 13, not in this build", {0x38, ble::op::UPDATE_STATUS});

  // Data and progress records (Bluetooth 1006 and WebSocket 06 carry the same bytes).
  Bytes data = {0x00, 0xC0, 0x03, 0x00, 0x11, 0x22, 0x33, 0x44};  // 4 image bytes at 245760
  Obj de;
  de.add("offset", "245760").add("bytes", q("11223344"));
  record("update data at 245760, 4 bytes", "updateData", data.data(), data.size(), de);
  struct P { const char *name; uint32_t next; fk::UpdateState state; fk::UpdateReport why; };
  const P ps[] = {{"update progress, receiving, next 249856", 249856, fk::UpdateState::RECEIVING, fk::UpdateReport::PROGRESS},
                  {"update progress, every byte received", 1507316, fk::UpdateState::RECEIVED, fk::UpdateReport::PROGRESS},
                  {"update progress, a repeat answered, next 249856", 249856, fk::UpdateState::RECEIVING, fk::UpdateReport::REPEAT},
                  {"update progress, a gap answered, next 249856", 249856, fk::UpdateState::RECEIVING, fk::UpdateReport::GAP},
                  {"update progress, none (dropped), next 0", 0, fk::UpdateState::NONE, fk::UpdateReport::REFUSED}};
  for (const P &p : ps) {
    uint8_t rec[fk::UPDATE_REPORT_SIZE];
    fk::encode_update_report(p.next, p.state, p.why, rec);
    Obj e;
    e.add("next", num(p.next)).add("state", num(int(p.state))).add("reason", num(int(p.why)));
    record(p.name, "updateProgress", rec, sizeof(rec), e);
  }
  // State flag bit 4.
  fk::Status s;
  s.answering = true;
  s.state = fk::HeaterState::OFF;
  s.board_updating = true;
  uint8_t st[ble::STATE_SIZE];
  ble::encode_state(s, st);
  CHECK((st[0] & 0x10) != 0);
  Obj e;
  e.add("heaterAnswering", "true").add("boardUpdateRunning", "true").add("flagsByte", num(st[0]));
  record("state flags, board firmware update running", "stateFlags", st, 1, e);
}

// The internet relay and push notifications (PROTOCOL.md).
static std::string relay_vectors() {
  namespace rl = fk::relay;
  uint8_t secret[32];
  std::memset(secret, 0x09, 32);
  uint8_t t[32], th[32], p[16];
  rl::relay_token(secret, 32, t);
  cr::sha256({{t, 32}}, th);
  rl::push_key(secret, 32, p);
  // The board whose private key is 1: its public key is G.
  uint8_t one[32] = {};
  one[31] = 1;
  rl::Identity g;
  CHECK(g.load(one, [](uint8_t *o, size_t n) { test_rng(o, n); }));
  char id[27];
  rl::id_text(g.id(), id);
  uint8_t ch[32];
  std::memset(ch, 0x01, 32);
  uint8_t sb[160];
  size_t sbn = rl::auth_signed_bytes(ch, "relay.flamekeeper.app", sb, sizeof(sb));
  rl::RosterEntry e{2, {}};
  std::memcpy(e.hash, th, 32);
  uint8_t roster[64];
  size_t rn = rl::encode_roster(&e, 1, roster, sizeof(roster));
  uint8_t close[5];
  rl::encode_close(1, 4001, close);
  const uint8_t open[4] = {0, 4, 1, 2};

  // A push: fuel low (bit 0, class 1), coalesced with "no clock" (bit 5), to the phone in slot 2.
  fk::Status s;
  s.alerts = 0x0021;
  s.fuel_left = 12;
  s.supply_v = 12.34f;
  s.fault = fk::Fault::NONE;
  s.panel_fault = 0;
  s.co_ppm = NAN;
  s.room_c = 4.8f;
  s.target_c = 21.0f;
  s.run_end_utc = 0;
  fk::PushEngine::Item lead{0, 0, 0, 1234, 1795000000u};
  uint8_t payload[100];
  size_t pn = fk::PushEngine::encode_payload(lead, 0, s, "fuel low: 12 % left", payload, sizeof(payload));
  uint8_t nonce[12];
  for (int i = 0; i < 12; i++)
    nonce[i] = uint8_t(0xC0 + i);
  uint8_t blob[200];
  size_t bn = fk::PushEngine::seal_blob(p, nonce, g.id(), 1, payload, pn, blob, sizeof(blob));
  std::vector<uint8_t> push = {0, 6, 1, 1, 2, uint8_t(bn)};
  push.insert(push.end(), blob, blob + bn);

  // The preference commands, from the phone in slot 2 over Bluetooth.
  Rig r;
  FakePush fp;
  r.board.psh = &fp;
  r.link.slot = 2;
  const Bytes prefs_cmd = {0x61, ble::op::PUSH_PREFS, 0xEF, 0x0F, 0x05, 0x00, 0x03};
  const Bytes prefs_res = r.run(prefs_cmd);
  CHECK(prefs_res.size() == 3 && prefs_res[2] == 0);
  const Bytes get_cmd = {0x62, ble::op::PUSH_PREFS_GET};
  const Bytes get_res = r.run(get_cmd);
  CHECK(get_res.size() == 9 && get_res[8] == 2);

  // The live card (LIVE): a run a phone in slot 2 started, heating by temperature, ending at 21:45.
  fk::PushEngine::LiveContent lc;
  lc.state = uint8_t(fk::HeaterState::HEATING);
  lc.kind = 0;
  lc.source = uint8_t(fk::Source::PHONE);
  lc.fault = 0;
  lc.alerts = 0x0001;           // fuel low
  lc.room_c10 = 184;            // 18.4 C
  lc.target_c10 = 215;          // 21.5 C
  lc.power = 60;
  lc.run_end_utc = 1795003500u;
  lc.burnoff_end_utc = 0;
  lc.started_utc = 1794999600u;
  lc.fuel_left = 37;
  uint8_t live_rec[fk::push::LIVE_RECORD_SIZE];
  fk::PushEngine::encode_live_record(1795000000u, lc, live_rec);
  fk::PushEngine::LiveContent lu;  // everything unknown or none
  lu.state = uint8_t(fk::HeaterState::UNKNOWN);
  uint8_t live_unknown[fk::push::LIVE_RECORD_SIZE];
  fk::PushEngine::encode_live_record(0, lu, live_unknown);
  // Whole frames: start, update, end, for the phone in slot 2 (run id 42, which it started).
  auto live_frame = [&](uint8_t event, uint8_t flags) {
    std::vector<uint8_t> m(fk::PushEngine::LIVE_HEADER_SIZE);
    fk::PushEngine::encode_live_header(event, flags, 42, 0x04, m.data());
    m.insert(m.end(), live_rec, live_rec + sizeof(live_rec));
    return m;
  };
  const std::vector<uint8_t> live_start = live_frame(fk::push::LIVE_START, fk::push::LIVE_IMPORTANT);
  const std::vector<uint8_t> live_update = live_frame(fk::push::LIVE_UPDATE, 0);
  const std::vector<uint8_t> live_end = live_frame(fk::push::LIVE_END, fk::push::LIVE_IMPORTANT | fk::push::LIVE_FAULT_END);
  CHECK(live_start.size() == 9 + 28);

  // Three more pushes to the phone in slot 2, each as payload, nonce and blob.
  struct PV {
    const char *name;
    fk::PushEngine::Item lead;
    uint8_t flags;
    fk::Status s;
    const char *text;
  };
  std::vector<PV> pv(3);
  // A CO alarm (bit 10, class 3), the first repeat, delayed (the relay was down when it rose).
  pv[0].name = "CO alarm, repeat 1, delayed";
  pv[0].lead = {0, 10, 0, 5678, 1795001234u};
  pv[0].flags = 0x01 | (1 << 1);
  pv[0].s.alerts = 0x0400;
  pv[0].s.co_ppm = 212;
  pv[0].s.fuel_left = 64;
  pv[0].s.supply_v = 12.71f;
  pv[0].s.room_c = 18.5f;
  pv[0].s.target_c = 21.0f;
  pv[0].text = "CO 212 ppm: heater stopped";
  // A timer started the heater (event 2, source 6), with the timer's run end.
  pv[1].name = "timer started the heater";
  pv[1].lead = {1, 2, 6, 5690, 1795003600u};
  pv[1].s.alerts = 0;
  pv[1].s.co_ppm = 0;
  pv[1].s.fuel_left = 63;
  pv[1].s.supply_v = 12.62f;
  pv[1].s.room_c = 9.4f;
  pv[1].s.target_c = 19.5f;
  pv[1].s.run_end_utc = 1795010800u;
  pv[1].text = nullptr;
  // Every value unknown: no clock (record time 0), no tank size, no supply, CO, room or target
  // reading, no timer, no text; leading alert bit 9.
  pv[2].name = "every value unknown";
  pv[2].lead = {0, 9, 0, 5700, 0};
  pv[2].s.alerts = 0x0200;
  pv[2].s.panel_fault = 0xFF;
  pv[2].s.target_c = NAN;
  pv[2].text = nullptr;
  std::string pushes = "[";
  for (size_t i = 0; i < pv.size(); i++) {
    const PV &v = pv[i];
    uint8_t pl[100];
    const size_t pln = fk::PushEngine::encode_payload(v.lead, v.flags, v.s, v.text, pl, sizeof(pl));
    CHECK(pln >= fk::push::PAYLOAD_FIXED);
    uint8_t nc[12];
    for (int j = 0; j < 12; j++)
      nc[j] = uint8_t(0xD0 + 0x10 * i + j);
    const uint8_t cls = pl[8];
    uint8_t bl[200];
    const size_t bln = fk::PushEngine::seal_blob(p, nc, g.id(), cls, pl, pln, bl, sizeof(bl));
    CHECK(bln == pln + 28);
    Obj po;
    po.add("name", q(v.name)).add("class", num(cls)).add("slot", "2").add("payload", q(hex(pl, pln)))
        .add("nonce", q(hex(nc, 12))).add("blob", q(hex(bl, bln)));
    pushes += (i ? ",\n      " : "\n      ") + po.str();
  }
  pushes += "\n    ]";
  CHECK(pv[0].lead.code == fk::push::CO_BIT && fk::push::alert_class(10) == 3);

  Obj o;
  o.add("keySecret", q(hex(secret, 32))).add("relayToken", q(hex(t, 32))).add("relayTokenHash", q(hex(th, 32)))
      .add("pushKey", q(hex(p, 16))).add("boardPublicKey", q(hex(g.public_key(), 65)))
      .add("boardIdHex", q(hex(g.id(), 16))).add("boardId", q(id))
      .add("authSignedBytes", q(hex(sb, sbn)))
      .add("roster", q(hex(roster, rn))).add("open", q(hex(open, 4))).add("close", q(hex(close, 5)))
      .add("pushPayload", q(hex(payload, pn))).add("pushNonce", q(hex(nonce, 12)))
      .add("pushClass", "1").add("pushBlob", q(hex(blob, bn))).add("pushMessage", q(hex(push)))
      .add("pushPrefsCommand", q(hex(prefs_cmd))).add("pushPrefsResult", q(hex(prefs_res)))
      .add("pushPrefsGetCommand", q(hex(get_cmd))).add("pushPrefsGetResult", q(hex(get_res)))
      .add("liveRecord", q(hex(live_rec, sizeof(live_rec))))
      .add("liveRecordFields", "{\"utc\": 1795000000, \"state\": 3, \"kind\": 0, "
                               "\"source\": 2, \"fault\": 0, \"alerts\": 1, \"roomC\": 18.4, \"targetC\": 21.5, "
                               "\"power\": 60, \"runEndUtc\": 1795003500, \"burnoffEndUtc\": 0, "
                               "\"startedUtc\": 1794999600, \"fuelLeft\": 37}")
      .add("liveRecordUnknown", q(hex(live_unknown, sizeof(live_unknown))))
      .add("liveRecordUnknownFields", "{\"utc\": 0, \"state\": 0, \"kind\": 0, "
                                      "\"source\": 0, \"fault\": 0, \"alerts\": 0, \"roomC\": null, \"targetC\": null, "
                                      "\"power\": null, \"runEndUtc\": 0, \"burnoffEndUtc\": 0, \"startedUtc\": 0, "
                                      "\"fuelLeft\": null}")
      .add("liveStart", q(hex(live_start))).add("liveUpdate", q(hex(live_update))).add("liveEnd", q(hex(live_end)))
      .add("pushes", pushes);
  return o.str();
}

static std::string sign_in() {
  fk::Access access{test_rng};
  fk::WifiSession board{access};
  std::vector<Bytes> to_app;
  fk::WifiSession::Send send = [&](const uint8_t *p, size_t n) { to_app.emplace_back(p, p + n); };
  uint8_t cmd[w::FRAME_MAX];
  size_t cmd_len = 0;
  auto feed = [&](const Bytes &f) {
    return board.receive(f.data(), f.size(), 1000, send, cmd, &cmd_len, sizeof(cmd));
  };

  const uint8_t owner[6] = {1, 2, 3, 4, 5, 6};
  const fk::WifiKey *k = access.issue_app_key(owner);
  Bytes id(k->id, k->id + 16), secret(k->secret, k->secret + 32);

  board.start(send);
  CHECK(to_app.size() == 1 && to_app[0].size() == 17 && to_app[0][0] == w::HELLO);
  Bytes hello = to_app[0];
  Bytes board_nonce(hello.begin() + 1, hello.end());

  Bytes app_nonce(16);
  test_rng(app_nonce.data(), 16);
  Bytes sign = {w::SIGN_IN, 0};
  sign.insert(sign.end(), id.begin(), id.end());
  sign.insert(sign.end(), app_nonce.begin(), app_nonce.end());
  uint8_t mac[32];
  const uint8_t label[] = {'f', 'k', '1', ' ', 'a', 'p', 'p'};
  cr::hmac_sha256(secret.data(), 32, {{label, 7}, {board_nonce.data(), 16}, {app_nonce.data(), 16}}, mac);
  sign.insert(sign.end(), mac, mac + 32);
  CHECK(feed(sign) == fk::WifiSession::Step::CONTINUE);
  CHECK(board.signed_in());
  CHECK(to_app.size() == 2);
  Bytes signed_in = to_app[1];
  CHECK(signed_in.size() == 34 && signed_in[0] == w::SIGNED_IN && signed_in[1] == 0);

  // The app's keys, as the protocol derives them (app -> board: first key and third salt).
  Bytes salt = board_nonce;
  salt.insert(salt.end(), app_nonce.begin(), app_nonce.end());
  uint8_t okm[40];
  cr::hkdf_sha256(salt.data(), 32, secret.data(), 32, "fk1 session", okm, 40);
  const uint8_t *k_to_board = okm, *s_to_board = okm + 32;
  uint64_t sent = 0;
  auto app_frame = [&](const Bytes &rec, uint8_t type = w::COMMAND) {
    Bytes f(1 + rec.size() + 16);
    f[0] = type;
    uint8_t n[12];
    std::memcpy(n, s_to_board, 4);
    for (int i = 0; i < 8; i++)
      n[4 + i] = uint8_t(sent >> (8 * i));
    sent++;
    CHECK(cr::gcm_seal(k_to_board, n, f.data(), 1, rec.data(), rec.size(), f.data() + 1));
    return f;
  };

  // The conversation: SET_POWER in, its Result out, SET_TARGET in, then a State out.
  Rig rig;
  rig.link.t = fk::Transport::WIFI;
  CHECK(rig.run({2, ble::op::SET_MODE, 1})[2] == 0);
  Bytes cmd1 = {3, ble::op::SET_POWER, 45};
  Bytes cmd2 = {4, ble::op::SET_TARGET, 0xE1, 0x00};  // 22.5 C
  Bytes f1 = app_frame(cmd1);
  CHECK(feed(f1) == fk::WifiSession::Step::COMMAND);
  CHECK(cmd_len == cmd1.size() && std::memcmp(cmd, cmd1.data(), cmd_len) == 0);
  Bytes result_rec = rig.run(Bytes(cmd, cmd + cmd_len));
  CHECK(result_rec.size() == 3 && result_rec[2] == 0);
  Bytes f2 = app_frame(cmd2);
  CHECK(feed(f2) == fk::WifiSession::Step::COMMAND);
  CHECK(cmd_len == cmd2.size() && std::memcmp(cmd, cmd2.data(), cmd_len) == 0);
  CHECK(rig.run(Bytes(cmd, cmd + cmd_len))[2] == 0);
  // Board firmware data (frame 06): offset 0, the first 8 bytes of an image.
  Bytes data3 = {0x00, 0x00, 0x00, 0x00, 0xE9, 0x06, 0x02, 0x20, 0x00, 0x00, 0x08, 0x40};
  Bytes f3 = app_frame(data3, w::UPDATE);
  CHECK(feed(f3) == fk::WifiSession::Step::UPDATE_DATA);
  CHECK(cmd_len == data3.size() && std::memcmp(cmd, data3.data(), cmd_len) == 0);
  rig.h.st.state = fk::HeaterState::HEATING;
  rig.h.st.answering = true;
  rig.c.tick(1000, false);
  uint8_t state_rec[ble::STATE_SIZE];
  CHECK_EQ(ble::encode_state(rig.c.status(), state_rec), 39);

  uint8_t sealed[64];
  size_t n = board.seal(w::STATE, state_rec, sizeof(state_rec), sealed, sizeof(sealed));
  CHECK_EQ(n, ble::STATE_SIZE + w::SEAL_OVERHEAD);
  Bytes board_state(sealed, sealed + n);
  n = board.seal(w::RESULT, result_rec.data(), result_rec.size(), sealed, sizeof(sealed));
  CHECK_EQ(n, 3 + w::SEAL_OVERHEAD);
  Bytes board_result(sealed, sealed + n);
  uint8_t progress_rec[fk::UPDATE_REPORT_SIZE];
  fk::encode_update_report(4096, fk::UpdateState::RECEIVING, fk::UpdateReport::PROGRESS, progress_rec);
  n = board.seal(w::UPDATE, progress_rec, sizeof(progress_rec), sealed, sizeof(sealed));
  CHECK_EQ(n, fk::UPDATE_REPORT_SIZE + w::SEAL_OVERHEAD);
  Bytes board_progress(sealed, sealed + n);

  auto frame = [](const char *dir, uint8_t type, const Bytes &rec, const Bytes &f) {
    Obj o;
    o.add("dir", q(dir)).add("type", num(type)).add("recordHex", q(hex(rec))).add("frameHex", q(hex(f)));
    return "      " + o.str();
  };
  Bytes st(state_rec, state_rec + sizeof(state_rec));
  std::string frames = frame("fromBoard", w::STATE, st, board_state) + ",\n" +
                       frame("fromBoard", w::RESULT, result_rec, board_result) + ",\n" +
                       frame("fromBoard", w::UPDATE, Bytes(progress_rec, progress_rec + sizeof(progress_rec)),
                             board_progress) + ",\n" +
                       frame("toBoard", w::COMMAND, cmd1, f1) + ",\n" + frame("toBoard", w::COMMAND, cmd2, f2) +
                       ",\n" + frame("toBoard", w::UPDATE, data3, f3);
  Obj o;
  o.add("keyId", q(hex(id))).add("secret", q(hex(secret))).add("boardNonce", q(hex(board_nonce)))
      .add("appNonce", q(hex(app_nonce))).add("helloFrame", q(hex(hello))).add("signInFrame", q(hex(sign)))
      .add("signedInFrame", q(hex(signed_in))).add("frames", "[\n" + frames + "\n    ]");
  return o.str();
}

// ------------------------------------------------------------------ web password sign-in (EC J-PAKE)
//
// The app is mbedTLS's J-PAKE client here, with a recorded random source. mbedTLS draws its scalars
// (x1, x2 and each proof's v) as 32 bytes read big-endian, between draws that only blind the point
// maths, so each v is found as the draw whose multiple of the proof's base is the proof's V. With
// those scalars an app reproduces its round one and round two byte for byte.

struct Draws {
  std::vector<Bytes> all;
};
// For maths whose blinding never shows in a message: its own stream, so the vectors above stay put.
static int mbed_dummy_rng(void *, unsigned char *out, size_t len) {
  static uint32_t x = 0x9E3779B9u;
  for (size_t i = 0; i < len; i++) {
    x = x * 1664525u + 1013904223u;
    out[i] = uint8_t(x >> 24);
  }
  return 0;
}
static int recorded_rng(void *p, unsigned char *out, size_t len) {
  test_rng(out, len);
  static_cast<Draws *>(p)->all.emplace_back(out, out + len);
  return 0;
}

static Bytes mpi32(const mbedtls_mpi &m) {
  Bytes b(32);
  CHECK(mbedtls_mpi_write_binary(&m, b.data(), 32) == 0);
  return b;
}
static Bytes point(mbedtls_ecp_group &g, const mbedtls_ecp_point &pt) {
  Bytes b(65);
  size_t n = 0;
  CHECK(mbedtls_ecp_point_write_binary(&g, &pt, MBEDTLS_ECP_PF_UNCOMPRESSED, &n, b.data(), b.size()) == 0 && n == 65);
  return b;
}

// The draw d with d x base == V, as 32 bytes (empty if none).
static Bytes find_scalar(mbedtls_ecp_group &g, const mbedtls_ecp_point &base, const Bytes &v_point, const Draws &d) {
  for (const Bytes &draw : d.all) {
    if (draw.size() != 32)
      continue;
    mbedtls_mpi k;
    mbedtls_ecp_point r;
    mbedtls_mpi_init(&k);
    mbedtls_ecp_point_init(&r);
    bool hit = mbedtls_mpi_read_binary(&k, draw.data(), 32) == 0 && mbedtls_mpi_cmp_int(&k, 0) > 0 &&
               mbedtls_mpi_cmp_mpi(&k, &g.N) < 0 && mbedtls_ecp_mul(&g, &r, &k, &base, mbed_dummy_rng, nullptr) == 0 &&
               point(g, r) == v_point;
    mbedtls_mpi_free(&k);
    mbedtls_ecp_point_free(&r);
    if (hit)
      return draw;
  }
  return {};
}

// mbedTLS's ECJPAKEKeyKP list: per key, ECPoint X (u8 length, point), ECPoint V, then r (u8 length, r).
struct Kp {
  Bytes x, v, r;
};
static size_t read_kp(const uint8_t *p, size_t n, Kp &out) {
  size_t at = 0;
  auto field = [&](Bytes &f) {
    CHECK(at < n && at + 1 + p[at] <= n);
    f.assign(p + at + 1, p + at + 1 + p[at]);
    at += 1 + p[at];
  };
  field(out.x);
  field(out.v);
  field(out.r);
  return at;
}

static std::string jpake_exchange(const char *board_password, const char *app_password, bool expect_ok) {
  fk::Access access{test_rng};
  CHECK(access.change_web_password(nullptr, 0, (const uint8_t *) board_password, strlen(board_password)).accepted());
  fk::WifiSession board{access};
  std::vector<Bytes> to_app;
  fk::WifiSession::Send send = [&](const uint8_t *p, size_t n) { to_app.emplace_back(p, p + n); };
  uint8_t cmd[w::FRAME_MAX];
  size_t cmd_len = 0;
  auto feed = [&](const Bytes &f) { return board.receive(f.data(), f.size(), 1000, send, cmd, &cmd_len, sizeof(cmd)); };

  board.start(send);
  CHECK(to_app.size() == 1 && to_app[0].size() == 17);
  Bytes hello = to_app[0];
  Bytes board_nonce(hello.begin() + 1, hello.end());

  // Round one, in the sign-in frame.
  mbedtls_ecjpake_context jp;
  mbedtls_ecjpake_init(&jp);
  CHECK(mbedtls_ecjpake_setup(&jp, MBEDTLS_ECJPAKE_CLIENT, MBEDTLS_MD_SHA256, MBEDTLS_ECP_DP_SECP256R1,
                              (const unsigned char *) app_password, strlen(app_password)) == 0);
  Draws d1;
  uint8_t r1[512];
  size_t r1n = 0;
  CHECK(mbedtls_ecjpake_write_round_one(&jp, r1, sizeof(r1), &r1n, recorded_rng, &d1) == 0);
  Bytes sign = {w::SIGN_IN, 1, 1};  // method 1 (the web password), remember
  sign.insert(sign.end(), r1, r1 + r1n);
  CHECK(feed(sign) == fk::WifiSession::Step::CONTINUE);
  CHECK(to_app.size() == 2 && to_app[1][0] == w::JPAKE_BOARD);
  Bytes board_rounds = to_app[1];

  // The board's round one and round two, then the app's round two.
  const size_t b1 = board_rounds[1] | (board_rounds[2] << 8);
  CHECK(mbedtls_ecjpake_read_round_one(&jp, board_rounds.data() + 3, b1) == 0);
  CHECK(mbedtls_ecjpake_read_round_two(&jp, board_rounds.data() + 3 + b1, board_rounds.size() - 3 - b1) == 0);
  Draws d2;
  uint8_t r2[512];
  size_t r2n = 0;
  CHECK(mbedtls_ecjpake_write_round_two(&jp, r2, sizeof(r2), &r2n, recorded_rng, &d2) == 0);
  Bytes round_two = {w::JPAKE_APP};
  round_two.insert(round_two.end(), r2, r2 + r2n);
  CHECK(feed(round_two) == fk::WifiSession::Step::CONTINUE);
  CHECK(to_app.size() == 2);  // nothing back until the confirmation

  uint8_t pms[32];
  size_t olen = 0;
  CHECK(mbedtls_ecjpake_derive_secret(&jp, pms, sizeof(pms), &olen, mbed_dummy_rng, nullptr) == 0 && olen == 32);
  uint8_t k[32];
  CHECK(cr::hkdf_sha256(board_nonce.data(), 16, pms, 32, "fk1 password", k, 32));
  uint8_t h[32];
  CHECK(cr::sha256({{hello.data(), hello.size()}, {sign.data(), sign.size()}, {board_rounds.data(), board_rounds.size()},
                    {round_two.data(), round_two.size()}},
                   h));
  const uint8_t app_label[] = {'f', 'k', '1', ' ', 'a', 'p', 'p', ' ', 'c', 'o', 'n', 'f', 'i', 'r', 'm'};
  Bytes confirm = {w::CONFIRM};
  confirm.resize(33);
  CHECK(cr::hmac_sha256(k, 32, {{app_label, sizeof(app_label)}, {h, 32}}, confirm.data() + 1));
  feed(confirm);
  CHECK(to_app.size() == 3);
  Bytes signed_in = to_app[2];

  // The app's scalars: x1, x2 from the context; each v from the draws (round two's base is
  // X1 + X3 + X4: the app's first key and both of the board's).
  mbedtls_ecp_group &g = jp.grp;
  Kp k1, k2, k3;
  size_t at = read_kp(r1, r1n, k1);
  read_kp(r1 + at, r1n - at, k2);
  read_kp(r2, r2n, k3);
  mbedtls_ecp_point base2;
  mbedtls_ecp_point_init(&base2);
  mbedtls_mpi one;
  mbedtls_mpi_init(&one);
  CHECK(mbedtls_mpi_lset(&one, 1) == 0);
  CHECK(mbedtls_ecp_muladd(&g, &base2, &one, &jp.Xm1, &one, &jp.Xp1) == 0);
  CHECK(mbedtls_ecp_muladd(&g, &base2, &one, &base2, &one, &jp.Xp2) == 0);
  Bytes v1 = find_scalar(g, g.G, k1.v, d1), v2 = find_scalar(g, g.G, k2.v, d1), v3 = find_scalar(g, base2, k3.v, d2);
  CHECK(v1.size() == 32 && v2.size() == 32 && v3.size() == 32);
  Obj scalars;
  scalars.add("x1", q(hex(mpi32(jp.xm1)))).add("x2", q(hex(mpi32(jp.xm2)))).add("v1", q(hex(v1)))
      .add("v2", q(hex(v2))).add("v3", q(hex(v3)));
  mbedtls_ecp_point_free(&base2);
  mbedtls_mpi_free(&one);
  mbedtls_ecjpake_free(&jp);

  Obj o;
  o.add("password", q(app_password)).add("boardPassword", q(board_password))
      .add("remember", "true").add("boardNonce", q(hex(board_nonce))).add("appScalars", scalars.str())
      .add("helloFrame", q(hex(hello))).add("signInFrame", q(hex(sign))).add("boardRoundsFrame", q(hex(board_rounds)))
      .add("appRoundTwoFrame", q(hex(round_two))).add("premasterSecret", q(hex(pms, 32))).add("k", q(hex(k, 32)))
      .add("transcriptHash", q(hex(h, 32))).add("confirmFrame", q(hex(confirm)))
      .add("signedInFrame", q(hex(signed_in)));
  if (expect_ok) {
    CHECK(signed_in.size() == 2 + 32 + 48 + 16 && signed_in[0] == w::SIGNED_IN && signed_in[1] == 0);
    const uint8_t board_label[] = {'f', 'k', '1', ' ', 'b', 'o', 'a', 'r', 'd', ' ', 'c', 'o', 'n', 'f', 'i', 'r', 'm'};
    uint8_t mac[32];
    CHECK(cr::hmac_sha256(k, 32, {{board_label, sizeof(board_label)}, {h, 32}}, mac));
    CHECK(std::memcmp(mac, signed_in.data() + 2, 32) == 0);
    static const uint8_t zero_nonce[12] = {};
    uint8_t key[48];
    CHECK(cr::gcm_open(k, zero_nonce, nullptr, 0, signed_in.data() + 34, 48 + 16, key));
    const fk::WifiKey *issued = access.find_key(key);
    CHECK(issued != nullptr && issued->kind == fk::KeyKind::BROWSER && std::memcmp(issued->secret, key + 16, 32) == 0);
    o.add("boardConfirm", q(hex(mac, 32))).add("browserKeyId", q(hex(key, 16))).add("browserKeySecret", q(hex(key + 16, 32)));
  } else {
    CHECK(signed_in.size() == 2 && signed_in[0] == w::SIGNED_IN && signed_in[1] == uint8_t(fk::Result::WRONG_PASSWORD));
    CHECK(access.lockout().fails() == 1);
  }
  return o.str();
}

static std::string jpake_vectors() {
  Obj o;
  o.add("curve", q("P-256")).add("hash", q("SHA-256")).add("appRole", q("client")).add("boardRole", q("server"));
  o.add("ok", jpake_exchange("correct horse battery", "correct horse battery", true));
  o.add("wrongPassword", jpake_exchange("correct horse battery", "correct horse batterz", false));
  return o.str();
}

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: dump_vectors <output.json>\n");
    return 2;
  }
  records();
  update_records();
  std::string signin = sign_in();
  std::string json = "{\n  \"records\": [\n";
  for (size_t i = 0; i < g_records.size(); i++)
    json += g_records[i] + (i + 1 < g_records.size() ? ",\n" : "\n");
  json += "  ],\n  \"signIn\": " + signin + ",\n  \"relay\": " + relay_vectors() + ",\n  \"jpake\": " + jpake_vectors() + "\n}\n";
  FILE *f = std::fopen(argv[1], "w");
  if (!f) {
    std::perror(argv[1]);
    return 2;
  }
  std::fputs(json.c_str(), f);
  std::fclose(f);
  return report("dump_vectors");
}
