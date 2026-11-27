// BLE records and the command handler, byte for byte against PROTOCOL.md.
#include "fake_heater.h"

#include "../ble_protocol.h"

namespace ble = fk::ble;
using fk::Mode;
using fk::Result;
using fk::Source;

struct FakeLink : fk::Link {
  fk::Transport t = fk::Transport::BLUETOOTH;
  int me = 0, keys = 0;
  bool web = false;
  fk::Transport transport() const override { return t; }
  bool browser() const override { return web; }
  fk::Outcome forget_me() override {
    me++;
    return fk::Outcome::ok();
  }
  fk::Outcome issue_wifi_key(uint8_t id[16], uint8_t secret[32]) override {
    if (t != fk::Transport::BLUETOOTH)
      return fk::Link::issue_wifi_key(id, secret);
    keys++;
    std::memset(id, 0x11, 16);
    std::memset(secret, 0x22, 32);
    return fk::Outcome::ok();
  }
};

struct FakeUpdater : fk::BoardUpdater {
  uint32_t size = 0, offset = 0;
  uint8_t sha[32]{};
  fk::Outcome begin_result = fk::Outcome::ok(), finish_result = fk::Outcome::ok();
  int finishes = 0, aborts = 0;
  fk::Outcome update_begin(uint32_t sz, const uint8_t h[32], uint32_t *off) override {
    if (!begin_result.accepted())
      return begin_result;
    size = sz;
    std::memcpy(sha, h, 32);
    *off = offset;
    return fk::Outcome::ok();
  }
  void update_status(fk::UpdateStatus &st) const override {
    st.state = fk::UpdateState::RECEIVING;
    st.size = size;
    st.received = offset;
    std::memcpy(st.sha256, sha, 32);
    st.last = fk::UpdateLast::ROLLED_BACK;
  }
  fk::Outcome update_finish() override {
    finishes++;
    return finish_result;
  }
  void update_abort() override { aborts++; }
};

struct FakeRelay : fk::RelayControl {
  bool on = false;
  std::string host;
  void relay_status(fk::RelayStatus &st, uint32_t) const override {
    st.on = on;
    st.state = on ? fk::RelayState::CONNECTED : fk::RelayState::OFF;
    st.has_id = true;
    for (int i = 0; i < 16; i++)
      st.id[i] = uint8_t(0x60 + i);
    st.since_s = on ? 0 : 0xFFFFFFFFu;
    std::snprintf(st.host, sizeof(st.host), "%s", host.c_str());
  }
  fk::Outcome relay_set(bool o, const uint8_t *h, size_t n) override {
    on = o;
    host.assign((const char *) h, n);
    return fk::Outcome::ok();
  }
};

struct FakeBoard : fk::BoardSettings {
  fk::BoardUpdater *upd = nullptr;
  fk::BoardUpdater *updater() override { return upd; }
  fk::RelayControl *rel = nullptr;
  fk::RelayControl *relay() override { return rel; }
  uint32_t pin = 0;
  std::string cur, web, ssid, wifi_pw;
  bool switchable = true, ha_on = false, key_set = false;
  int wifi_offs = 0, new_keys = 0;
  fk::Outcome set_pin(uint32_t p) override {
    pin = p;
    return fk::Outcome::ok();
  }
  fk::Outcome set_web_password(const uint8_t *c, size_t cl, const uint8_t *pw, size_t len) override {
    cur.assign((const char *) c, cl);
    web.assign((const char *) pw, len);
    return fk::Outcome::ok();
  }
  void wifi_status(fk::WifiStatus &w) const override {
    w.state = fk::WifiState::CONNECTED;
    w.rssi = -61;
    w.ip[0] = 192, w.ip[1] = 168, w.ip[2] = 1, w.ip[3] = 20;
    std::strcpy(w.ssid, "Home");
  }
  fk::Outcome wifi_set(const uint8_t *s, size_t sl, const uint8_t *pw, size_t len) override {
    ssid.assign((const char *) s, sl);
    wifi_pw.assign((const char *) pw, len);
    return fk::Outcome::ok();
  }
  fk::Outcome wifi_off() override {
    wifi_offs++;
    return fk::Outcome::ok();
  }
  bool ha_switchable() const override { return switchable; }
  fk::HaState ha_state() const override { return ha_on ? fk::HaState::WAITING : fk::HaState::OFF; }
  bool ha_key_set() const override { return key_set; }
  fk::Outcome ha_set(bool on) override {
    ha_on = on;
    return fk::Outcome::ok();
  }
  fk::Outcome ha_key(uint8_t out[32]) override {
    std::memset(out, 0x33, 32);
    return fk::Outcome::ok();
  }
  fk::Outcome ha_key_new(uint8_t out[32]) override {
    new_keys++;
    std::memset(out, 0x44, 32);
    return fk::Outcome::ok();
  }
};

static uint16_t u16(const uint8_t *p) { return uint16_t(p[0] | (p[1] << 8)); }
static int32_t i32(const uint8_t *p) { return int32_t(uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24)); }

struct Rig {
  FakeHeater h;
  fk::Controller c{&h};
  FakeLink link;
  FakeBoard board;
  uint8_t out[ble::RESULT_MAX];
  size_t n = 0;
  size_t cmd(std::initializer_list<uint8_t> bytes) { return cmdv(std::vector<uint8_t>(bytes)); }
  size_t cmdv(const std::vector<uint8_t> &in) {
    n = ble::handle_command(in.data(), in.size(), c, link, board, out, sizeof(out));
    return n;
  }
  Result res() const { return Result(out[2]); }
  std::string text() const { return std::string((const char *) out + 3, n - 3); }
};

static void test_uuid() {
  uint8_t b[16];
  ble::uuid_bytes(0x1002, b);
  // 81ce1002-024c-414c-9ca7-e0972fed52ee, least significant byte first
  const uint8_t want[16] = {0xee, 0x52, 0xed, 0x2f, 0x97, 0xe0, 0xa7, 0x9c,
                            0x4c, 0x41, 0x4c, 0x02, 0x02, 0x10, 0xce, 0x81};
  CHECK(std::memcmp(b, want, 16) == 0);
}

static void test_info() {
  uint8_t out[ble::INFO_SIZE];
  fk::BoardInfo p;
  p.window_open = true;
  p.wifi = fk::WifiState::CONNECTED;
  p.web_password_set = true;
  p.ha = fk::HaState::WAITING;
  p.version[0] = 1, p.version[1] = 2, p.version[2] = 3;
  p.build_utc = 1795000000u;
  CHECK_EQ(ble::encode_info(ble::BOARD_FLAMEKEEPER_REV_A2, 0xFFFFFFFFu, p, out), 14);
  CHECK_EQ(out[0], 1);
  CHECK_EQ(out[1], 2);
  CHECK_EQ(out[2], 0x05);  // the window open, a web password set
  // The heater's bits 0-3 and 5, bit 7 (Bluetooth thermometers), bit 10 (a Home Assistant room
  // sensor), bit 11 (the heater's fault log); bit 6 is the board's.
  CHECK_EQ(u16(out + 3), 0x0CAF);
  CHECK_EQ(out[5], 2);
  CHECK_EQ(out[6], 1);
  CHECK(out[7] == 1 && out[8] == 2 && out[9] == 3);
  CHECK_EQ(uint32_t(i32(out + 10)), 1795000000u);
  p.can_update = true;
  ble::encode_info(ble::BOARD_FLAMEKEEPER_REV_A2, 0, p, out);
  CHECK_EQ(u16(out + 3), 0x0100);  // bit 8: board firmware updates
  p.can_update = false;
  p.ha_switchable = true;
  p.pin_changed = true;
  ble::encode_info(ble::Board(1), fk::cap::FAN_ONLY, p, out);
  CHECK_EQ(u16(out + 3), 0x0041);
  CHECK_EQ(out[2], 0x07);
}

static void test_state_and_telemetry() {
  fk::Status s;
  uint8_t out[64];
  // Everything unknown.
  s.target_c = NAN;
  CHECK_EQ(ble::encode_state(s, out), 39);
  CHECK_EQ(out[32], 0);  // the heater's fault log: empty, or none
  CHECK(i32(out + 33) == 0 && u16(out + 37) == 0);  // no start to come
  CHECK_EQ(out[0], 0);
  CHECK_EQ(u16(out + 4), 0x8000);
  CHECK_EQ(u16(out + 6), 0x8000);
  CHECK_EQ(out[12], 0);       // not running
  CHECK_EQ(out[13], 0);       // nothing staged
  CHECK_EQ(u16(out + 16), 0xFFFF);
  CHECK_EQ(u16(out + 18), 0);  // no alerts, nothing automatic, no tank
  CHECK_EQ(out[20], 0);
  CHECK_EQ(out[21], 0xFF);
  CHECK_EQ(out[22], 0);       // no burn-off run
  CHECK_EQ(out[23], 0xFF);    // no timer run
  CHECK_EQ(i32(out + 24), 0);
  CHECK_EQ(i32(out + 28), 0);  // no run
  // Stopping or cooling down: State still names who started the run.
  s.ending_source = Source::TIMER;
  ble::encode_state(s, out);
  CHECK_EQ(out[12], uint8_t(Source::TIMER));
  s.ending_source = Source::NONE;
  CHECK_EQ(ble::encode_telemetry(s, out), 45);
  CHECK_EQ(u16(out + 0), 0x8000);
  CHECK_EQ(u16(out + 2), 0x8000);
  for (int at : {4, 6, 8, 10, 12, 14, 16, 20, 24})
    CHECK_EQ(u16(out + at), 0xFFFF);
  CHECK_EQ(out[18], 0xFF);
  CHECK_EQ(u16(out + 22), 0x8000);
  CHECK_EQ(uint32_t(i32(out + 26)), 0xFFFFFFFFu);
  CHECK_EQ(out[30], 0);  // CO sensor not fitted
  CHECK_EQ(i32(out + 31), 0);  // no run
  for (int at : {35, 37, 39})
    CHECK_EQ(u16(out + at), 0x8000);  // no room reading from any source
  s.room_probe_c = 18.25f;
  s.room_ha_c = -2.0f;
  ble::encode_telemetry(s, out);
  CHECK_EQ(int16_t(u16(out + 35)), 183);
  CHECK_EQ(int16_t(u16(out + 37)), -20);
  CHECK_EQ(u16(out + 39), 0x8000);
  s.room_probe_c = NAN;
  s.room_ha_c = NAN;
  // The fuel trim: the altitude's (off: 0x80), the one in effect, the smoothed altitude.
  ble::encode_telemetry(s, out);
  CHECK_EQ(out[41], 0x80);
  CHECK_EQ(out[42], 0);
  CHECK_EQ(u16(out + 43), 0x8000);
  s.altitude_trim_pct = -17;
  s.fuel_trim_pct = -12;
  s.altitude_smoothed_m = 1712.6f;
  ble::encode_telemetry(s, out);
  CHECK_EQ(int8_t(out[41]), -17);
  CHECK_EQ(int8_t(out[42]), -12);
  CHECK_EQ(int16_t(u16(out + 43)), 1713);
  s.altitude_trim_pct = INT8_MIN;
  s.fuel_trim_pct = 0;
  s.altitude_smoothed_m = NAN;

  s.answering = true;
  s.failsafe_stopped = true;
  s.params_pending = 3;
  s.room_fallback = true;
  s.update_running = true;
  s.mode = Mode::HEAT;
  s.heat_mode = fk::HeatMode::TEMPERATURE;
  s.power = 70;
  s.target_c = 21.5f;
  s.room_c = -3.24f;
  s.room_source = fk::RoomSource::PROBE;
  s.state = fk::HeaterState::HEATING;
  s.control = fk::ControlStatus::RUNNING;
  s.fault = fk::Fault::FLAME_OUT;
  s.prime_left_s = 12;
  s.failsafe_left_s = 600;
  s.run_source = Source::HOME_ASSISTANT;
  ble::encode_state(s, out);
  CHECK_EQ(out[0], 0x07);  // bit 3 is no longer set: heater state 8 says the heater is updating
  s.thermo_paused = true;
  ble::encode_state(s, out);
  CHECK_EQ(out[0], 0x47);  // bit 6: the thermostat holds the run off
  s.thermo_paused = false;
  ble::encode_state(s, out);
  CHECK_EQ(out[1], 1);
  CHECK_EQ(out[2], 1);
  CHECK_EQ(out[3], 70);
  CHECK_EQ(int16_t(u16(out + 4)), 215);
  CHECK_EQ(int16_t(u16(out + 6)), -32);
  CHECK_EQ(out[8], 0);
  CHECK_EQ(out[9], 3);
  CHECK_EQ(out[10], 2);  // running
  CHECK_EQ(out[11], 8);
  CHECK_EQ(out[12], 1);
  CHECK_EQ(out[13], 3);
  CHECK_EQ(u16(out + 14), 12);
  CHECK_EQ(u16(out + 16), 600);
  s.failsafe_left_s = 70000;  // held below the marker
  ble::encode_state(s, out);
  CHECK_EQ(u16(out + 16), 0xFFFF);
  s.fault_log_seq = 201;  // the heater's fault log's newest entry: read it again when this changes
  ble::encode_state(s, out);
  CHECK_EQ(out[32], 201);

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
  s.co_state = 3;
  ble::encode_telemetry(s, out);
  CHECK_EQ(int16_t(u16(out + 0)), 1873);
  CHECK_EQ(int16_t(u16(out + 2)), -120);
  CHECK_EQ(u16(out + 4), 1234);
  CHECK_EQ(u16(out + 6), 3412);
  CHECK_EQ(u16(out + 8), 786);
  CHECK_EQ(u16(out + 10), 280);
  CHECK_EQ(u16(out + 12), 0);
  CHECK_EQ(u16(out + 14), 950);
  CHECK_EQ(u16(out + 16), 804);
  CHECK_EQ(out[18], 55);
  CHECK_EQ(out[19], 8);
  CHECK_EQ(u16(out + 20), 3);
  CHECK_EQ(int16_t(u16(out + 22)), 1712);
  CHECK_EQ(u16(out + 24), 10133);
  CHECK_EQ(i32(out + 26), 123456);
  CHECK_EQ(out[30], 3);
  // Out of range values are held, never wrapped into a marker.
  s.supply_v = 900.0f;
  s.heat_exchanger_c = -5000.0f;
  ble::encode_telemetry(s, out);
  CHECK_EQ(u16(out + 4), 65534);
  CHECK_EQ(int16_t(u16(out + 0)), -32767);
}

static void test_commands() {
  Rig r;
  // Malformed.
  CHECK_EQ(r.cmd({}), 0);
  CHECK_EQ(r.cmd({0, ble::op::SET_POWER, 3}), 0);  // id 0 is for events
  CHECK_EQ(r.cmd({9}), 3 + 9);
  CHECK(r.res() == Result::BAD_LENGTH);
  r.cmd({9, ble::op::SET_POWER});
  CHECK(r.res() == Result::BAD_LENGTH);
  CHECK_EQ(r.out[0], 9);
  CHECK_EQ(r.out[1], ble::op::SET_POWER);
  r.cmd({9, ble::op::SET_POWER, 3, 4});
  CHECK(r.res() == Result::BAD_LENGTH);
  r.cmd({9, 0x7F});
  CHECK(r.res() == Result::UNKNOWN_OPCODE);
  CHECK(r.h.calls.empty());

  // The heater not answering: a start is refused with the backend's reason, a power is kept.
  r.h.st.answering = false;
  r.cmd({1, ble::op::SET_MODE, 1});
  CHECK(r.res() == Result::NO_REPLY);
  CHECK(r.text() == "the heater does not answer");
  r.cmd({2, ble::op::SET_POWER, 60});
  CHECK(r.res() == Result::OK);
  CHECK_EQ(r.h.st.power, 60);
  r.cmd({2, ble::op::SET_POWER, 0});
  CHECK(r.res() == Result::OK);
  CHECK_EQ(r.h.st.power, 0);
  r.cmd({2, ble::op::SET_POWER, 101});
  CHECK(r.res() == Result::OUT_OF_RANGE);
  CHECK_EQ(r.h.st.power, 0);

  // An opcode nobody uses.
  r.cmd({3, 0x05, 1});
  CHECK(r.res() == Result::UNKNOWN_OPCODE);
  r.h.st.answering = true;

  // SET_MODE cannot prime: that needs PRIME_START with the confirmation.
  r.cmd({4, ble::op::SET_MODE, 3});
  CHECK(r.res() == Result::OUT_OF_RANGE);
  r.cmd({5, ble::op::PRIME_START, 0x00});
  CHECK(r.res() == Result::NOT_CONFIRMED);
  CHECK(r.h.st.mode == Mode::OFF);
  r.cmd({5, ble::op::PRIME_START, ble::PRIME_CONFIRM});
  CHECK(r.res() == Result::OK);
  CHECK(r.h.st.mode == Mode::PRIME);
  CHECK_EQ(int(r.c.status().run_source), int(Source::PHONE));
  r.cmd({6, ble::op::PRIME_STOP});
  CHECK(r.res() == Result::OK);
  CHECK(r.h.st.mode == Mode::OFF);

  r.cmd({7, ble::op::SET_MODE, 1});
  CHECK(r.res() == Result::OK);
  CHECK(r.h.st.mode == Mode::HEAT);
  r.cmd({8, ble::op::SET_HEAT_MODE, 1});
  CHECK(r.res() == Result::OK);
  CHECK(r.c.status().heat_mode == fk::HeatMode::TEMPERATURE);  // held by the controller
  r.cmd({8, ble::op::SET_HEAT_MODE, 2});
  CHECK(r.res() == Result::OUT_OF_RANGE);
  // 22.5 C = 225 = 0x00E1; -1 C = 0xFFF6 (out of range); the "unknown" marker.
  r.cmd({9, ble::op::SET_TARGET, 0xE1, 0x00});
  CHECK(r.res() == Result::OK);
  CHECK(r.h.st.target_c == 22.5f);
  r.cmd({9, ble::op::SET_TARGET, 0xF6, 0xFF});
  CHECK(r.res() == Result::OUT_OF_RANGE);
  r.cmd({9, ble::op::SET_TARGET, 0x00, 0x80});
  CHECK(r.res() == Result::OUT_OF_RANGE);
  r.cmd({10, ble::op::SET_ROOM_SOURCE, 0});
  CHECK(r.res() == Result::OK);
  CHECK(r.c.status().room_source == fk::RoomSource::PROBE);

  r.cmd({11, ble::op::FORGET_ME});
  CHECK_EQ(r.link.me, 1);
}

static std::vector<uint8_t> with_fields(uint8_t req, uint8_t opcode, const std::string &a, const std::string &b) {
  std::vector<uint8_t> v = {req, opcode, uint8_t(a.size())};
  v.insert(v.end(), a.begin(), a.end());
  v.push_back(uint8_t(b.size()));
  v.insert(v.end(), b.begin(), b.end());
  return v;
}

static void test_board_commands() {
  Rig r;
  // Commands over Wi-Fi run with their own source.
  r.link.t = fk::Transport::WIFI;
  r.cmd({2, ble::op::SET_MODE, 1});
  CHECK_EQ(int(r.c.status().run_source), int(Source::WIFI_APP));
  r.cmd({3, ble::op::SET_MODE, 0});
  // The board's web page: its own source, so a run shows as started from a web browser.
  r.link.web = true;
  r.cmd({4, ble::op::SET_MODE, 1});
  CHECK_EQ(int(r.c.status().run_source), int(Source::BROWSER));
  CHECK(std::strcmp(fk::source_name(Source::BROWSER), "Web browser") == 0);
  r.cmd({5, ble::op::SET_MODE, 0});
  r.link.web = false;

  // WIFI_KEY: Bluetooth only.
  r.cmd({4, ble::op::WIFI_KEY});
  CHECK(r.res() == Result::BLUETOOTH_ONLY);
  r.link.t = fk::Transport::BLUETOOTH;
  CHECK_EQ(r.cmd({4, ble::op::WIFI_KEY}), 3 + 48);
  CHECK(r.res() == Result::OK);
  CHECK_EQ(r.out[3], 0x11);
  CHECK_EQ(r.out[3 + 16], 0x22);

  // PIN_SET
  r.cmd({5, ble::op::PIN_SET, 0x3F, 0x42, 0x0F, 0x00});  // 999999
  CHECK(r.res() == Result::OK);
  CHECK_EQ(r.board.pin, 999999);
  r.cmd({5, ble::op::PIN_SET, 0x40, 0x42, 0x0F, 0x00});  // 1000000
  CHECK(r.res() == Result::OUT_OF_RANGE);
  r.cmd({5, ble::op::PIN_SET, 1, 2, 3});
  CHECK(r.res() == Result::BAD_LENGTH);

  // WEB_PASSWORD_SET: two length-prefixed fields; the new one 8-64 bytes or empty.
  r.cmdv(with_fields(6, ble::op::WEB_PASSWORD_SET, "", "correct horse"));
  CHECK(r.res() == Result::OK);
  CHECK(r.board.web == "correct horse");
  r.cmdv(with_fields(6, ble::op::WEB_PASSWORD_SET, "correct horse", ""));
  CHECK(r.res() == Result::OK);
  CHECK(r.board.cur == "correct horse");
  r.cmdv(with_fields(6, ble::op::WEB_PASSWORD_SET, "", "short"));
  CHECK(r.res() == Result::OUT_OF_RANGE);
  r.cmdv(with_fields(6, ble::op::WEB_PASSWORD_SET, "", std::string(65, 'x')));
  CHECK(r.res() == Result::OUT_OF_RANGE);
  std::vector<uint8_t> bad = with_fields(6, ble::op::WEB_PASSWORD_SET, "a", "abcdefgh");
  bad.push_back(0);  // a trailing byte
  r.cmdv(bad);
  CHECK(r.res() == Result::BAD_LENGTH);
  r.cmd({6, ble::op::WEB_PASSWORD_SET, 5, 'a'});  // the field runs past the end
  CHECK(r.res() == Result::BAD_LENGTH);

  // Wi-Fi
  CHECK_EQ(r.cmd({7, ble::op::WIFI_STATUS}), 3 + 6 + 4);
  CHECK_EQ(r.out[3], 2);
  CHECK_EQ(int8_t(r.out[4]), -61);
  CHECK_EQ(r.out[5], 192);
  CHECK(std::string((const char *) r.out + 9, 4) == "Home");
  r.cmdv(with_fields(8, ble::op::WIFI_SET, "Mobius", "password1"));
  CHECK(r.res() == Result::OK);
  CHECK(r.board.ssid == "Mobius" && r.board.wifi_pw == "password1");
  r.cmdv(with_fields(8, ble::op::WIFI_SET, "Open", ""));
  CHECK(r.res() == Result::OK);
  r.cmdv(with_fields(8, ble::op::WIFI_SET, "", "password1"));
  CHECK(r.res() == Result::OUT_OF_RANGE);
  r.cmdv(with_fields(8, ble::op::WIFI_SET, std::string(33, 's'), "password1"));
  CHECK(r.res() == Result::OUT_OF_RANGE);
  r.cmdv(with_fields(8, ble::op::WIFI_SET, "Mobius", "1234567"));
  CHECK(r.res() == Result::OUT_OF_RANGE);
  r.cmdv(with_fields(8, ble::op::WIFI_SET, "Mobius", std::string(64, 'p')));
  CHECK(r.res() == Result::OUT_OF_RANGE);
  r.cmd({9, ble::op::WIFI_OFF});
  CHECK_EQ(r.board.wifi_offs, 1);

  // Home Assistant
  CHECK_EQ(r.cmd({10, ble::op::HA_STATUS}), 4);
  CHECK_EQ(r.out[3], 0);
  r.cmd({11, ble::op::HA_SET, 1});
  CHECK(r.res() == Result::OK);
  CHECK(r.board.ha_on);
  r.cmd({11, ble::op::HA_SET, 2});
  CHECK(r.res() == Result::OUT_OF_RANGE);
  CHECK_EQ(r.cmd({12, ble::op::HA_KEY}), 3 + 32);
  CHECK_EQ(r.out[3], 0x33);
  CHECK_EQ(r.cmd({13, ble::op::HA_KEY_NEW}), 3 + 32);
  CHECK_EQ(r.out[3], 0x44);
  CHECK_EQ(r.board.new_keys, 1);
  // Switching restarts the board: refused while the heater runs.
  r.cmd({14, ble::op::SET_MODE, 1});
  r.cmd({15, ble::op::HA_SET, 0});
  CHECK(r.res() == Result::NOT_OFF);
  CHECK(r.board.ha_on);
  r.cmd({16, ble::op::SET_MODE, 0});
  r.cmd({17, ble::op::HA_SET, 0});
  CHECK(r.res() == Result::OK);
  // Not on a board where Home Assistant is always on.
  r.board.switchable = false;
  r.cmd({18, ble::op::HA_SET, 1});
  CHECK(r.res() == Result::NOT_SUPPORTED);
  r.cmd({19, ble::op::HA_KEY});
  CHECK(r.res() == Result::NOT_SUPPORTED);
}

static void test_update_commands() {
  Rig r;
  // No updater in this build: refused, capability bit 8 off.
  r.cmd({1, ble::op::UPDATE_STATUS});
  CHECK(r.res() == Result::NOT_SUPPORTED);
  FakeUpdater u;
  r.board.upd = &u;
  std::vector<uint8_t> begin = {2, ble::op::UPDATE_BEGIN, 0x40, 0x42, 0x0F, 0x00};  // 1000000 bytes
  for (int i = 0; i < 32; i++)
    begin.push_back(uint8_t(i));
  u.offset = 8192;  // a resumed update
  CHECK_EQ(r.cmdv(begin), 9);
  CHECK(r.res() == Result::OK && u.size == 1000000 && u.sha[31] == 31);
  CHECK_EQ(uint32_t(i32(r.out + 3)), 8192u);
  CHECK_EQ(u16(r.out + 7), fk::UPDATE_CHUNK_WIFI);  // FakeLink keeps the default
  begin.pop_back();
  r.cmdv(begin);
  CHECK(r.res() == Result::BAD_LENGTH);
  begin.push_back(31);
  u.begin_result = fk::Outcome::refuse(Result::NOT_OFF, "the heater is not off yet");
  r.cmdv(begin);
  CHECK(r.res() == Result::NOT_OFF && r.text() == "the heater is not off yet");
  // Status: state, size, received, SHA-256, last result.
  CHECK_EQ(r.cmd({3, ble::op::UPDATE_STATUS}), 3u + 42u);
  CHECK(r.out[3] == 1 && uint32_t(i32(r.out + 4)) == 1000000u && uint32_t(i32(r.out + 8)) == 8192u);
  CHECK(r.out[12] == 0 && r.out[43] == 31 && r.out[44] == 2);
  u.finish_result = fk::Outcome::refuse(Result::BAD_IMAGE, "not signed");
  r.cmd({4, ble::op::UPDATE_FINISH});
  CHECK(r.res() == Result::BAD_IMAGE && u.finishes == 1);
  r.cmd({5, ble::op::UPDATE_ABORT});
  CHECK(r.res() == Result::OK && u.aborts == 1);
  r.cmd({6, ble::op::UPDATE_ABORT, 0});
  CHECK(r.res() == Result::BAD_LENGTH);
  // State flag bit 4 while it runs.
  fk::Status s;
  s.board_updating = true;
  uint8_t st[ble::STATE_SIZE];
  ble::encode_state(s, st);
  CHECK(st[0] & 0x10);
}

static void test_relay_commands() {
  Rig r;
  r.cmd({1, ble::op::RELAY_STATUS});
  CHECK(r.res() == Result::NOT_SUPPORTED);
  FakeRelay fr;
  r.board.rel = &fr;
  // Status, off: on 0, state 0, id, since never, phones 0, default host.
  CHECK_EQ(r.cmd({2, ble::op::RELAY_STATUS}), 3u + 1 + 1 + 16 + 4 + 1 + 1);
  CHECK(r.out[3] == 0 && r.out[4] == 0 && r.out[5] == 0x60 && r.out[20] == 0x6F);
  CHECK(uint32_t(i32(r.out + 21)) == 0xFFFFFFFFu && r.out[25] == 0 && r.out[26] == 0);
  // On with a host.
  std::vector<uint8_t> set = {3, ble::op::RELAY_SET, 1, 9};
  for (char ch : std::string("relay.lan"))
    set.push_back(uint8_t(ch));
  r.cmdv(set);
  CHECK(r.res() == Result::OK && fr.on && fr.host == "relay.lan");
  CHECK_EQ(r.cmd({4, ble::op::RELAY_STATUS}), 3u + 24 + 9);
  CHECK(r.out[3] == 1 && r.out[4] == 2 && r.out[26] == 9 && std::memcmp(r.out + 27, "relay.lan", 9) == 0);
  // Default host; bad host; bad lengths; a value other than 0 or 1.
  r.cmd({5, ble::op::RELAY_SET, 1, 0});
  CHECK(r.res() == Result::OK && fr.host.empty());
  r.cmd({6, ble::op::RELAY_SET, 1, 3, 'a', ' ', 'b'});
  CHECK(r.res() == Result::OUT_OF_RANGE);
  r.cmd({7, ble::op::RELAY_SET, 1, 3, 'a'});
  CHECK(r.res() == Result::BAD_LENGTH);
  r.cmd({8, ble::op::RELAY_SET, 2, 0});
  CHECK(r.res() == Result::BAD_LENGTH);
  r.cmd({9, ble::op::RELAY_SET, 0, 0});
  CHECK(r.res() == Result::OK && !fr.on);
  // Capability bit 9 and State flag bit 5.
  fk::BoardInfo p;
  p.has_relay = true;
  uint8_t info[ble::INFO_SIZE];
  ble::encode_info(ble::Board(1), 0, p, info);
  CHECK(u16(info + 3) == 0x0200);
  fk::Status s;
  s.relay_phones = 1;
  uint8_t st[ble::STATE_SIZE];
  ble::encode_state(s, st);
  CHECK(st[0] & 0x20);
}

static void test_unsupported() {
  Rig r;
  r.h.caps = 0;
  r.cmd({1, ble::op::SET_MODE, 2});
  CHECK(r.res() == Result::NOT_SUPPORTED);
  r.cmd({1, ble::op::PRIME_START, ble::PRIME_CONFIRM});
  CHECK(r.res() == Result::NOT_SUPPORTED);
  // No heater parameters: the table is the board's own (fuel trim first).
  r.cmd({1, ble::op::PARAM_DESCRIBE, 0});
  CHECK(r.res() == Result::OK);
  CHECK_EQ(u16(r.out + 3 + 2), fk::T_FUEL_TRIM);
  CHECK_EQ(r.out[3 + 1], fk::Tuning::count());
  r.cmd({1, ble::op::PARAM_DESCRIBE, uint8_t(fk::Tuning::count())});
  CHECK(r.res() == Result::OUT_OF_RANGE);
  r.cmd({1, ble::op::PARAM_APPLY});
  CHECK(r.res() == Result::NOT_SUPPORTED);
}

static void test_heater_log() {
  Rig r;
  // No fault log in this heater: not supported, either way.
  r.cmd({1, ble::op::HEATER_LOG, 0});
  CHECK(r.res() == Result::NOT_SUPPORTED);
  r.cmd({2, ble::op::HEATER_LOG_CLEAR});
  CHECK(r.res() == Result::NOT_SUPPORTED);
  CHECK_EQ(r.h.log_clears, 0);
  // Ten entries, newest first: 8 then 2, each the link's 14 bytes.
  r.h.caps |= fk::cap::FAULT_LOG;
  for (int i = 10; i >= 1; i--) {
    fk::HeaterLogEntry e;
    e.kind = i == 1 ? fk::HEATER_LOG_RESET : fk::HEATER_LOG_FAULT;
    e.code = uint8_t(i == 1 ? 2 : 6);
    e.why = 5;
    e.step = 6;
    e.seq = uint8_t(i);
    e.board_c = -12;
    e.hx_c10 = 2513;
    e.supply_cv = 2380;
    e.uptime_s = 0x01020304u;
    r.h.log.push_back(e);
  }
  r.h.st.fault_log_count = 10;
  r.h.st.fault_log_seq = 10;
  r.cmd({3, ble::op::HEATER_LOG, 0});
  CHECK(r.res() == Result::OK);
  CHECK_EQ(r.n, 3 + 1 + 8 * 14);
  CHECK(r.n <= ble::RESULT_MAX);
  CHECK_EQ(r.out[3], 10);  // entries in the log
  const uint8_t first[14] = {1, 6, 5, 6, 10, 0xF4, 0xD1, 0x09, 0x4C, 0x09, 4, 3, 2, 1};
  CHECK(std::memcmp(r.out + 4, first, 14) == 0);
  CHECK_EQ(r.out[4 + 7 * 14 + 4], 3);  // the eighth: seq 3
  r.cmd({4, ble::op::HEATER_LOG, 8});
  CHECK_EQ(r.n, 3 + 1 + 2 * 14);
  CHECK(r.out[4 + 14] == 2 && r.out[4 + 14 + 1] == 2 && r.out[4 + 14 + 4] == 1);  // a reset, the watchdog
  r.cmd({5, ble::op::HEATER_LOG, 10});
  CHECK(r.res() == Result::OK);
  CHECK_EQ(r.n, 3 + 1);
  r.cmd({6, ble::op::HEATER_LOG});
  CHECK(r.res() == Result::BAD_LENGTH);
  // Clearing goes through the controller to the heater.
  r.cmd({7, ble::op::HEATER_LOG_CLEAR, 0});
  CHECK(r.res() == Result::BAD_LENGTH);
  r.cmd({8, ble::op::HEATER_LOG_CLEAR});
  CHECK(r.res() == Result::OK);
  CHECK_EQ(r.h.log_clears, 1);
  r.cmd({9, ble::op::HEATER_LOG, 0});
  CHECK_EQ(r.n, 3 + 1);
  CHECK_EQ(r.out[3], 0);
}

static void test_params() {
  Rig r;
  r.h.params.push_back({{14, fk::pflag::NEEDS_OFF, fk::Unit::SECONDS, 1, 20, 150, 5, "Self-test time",
                         fk::ParamGroup::START_UP, fk::ParamLevel::EXPERT},
                        {true, false, 50, 0},
                        "How long the fan runs its check at start-up."});
  r.h.params.push_back({{0x0A01, fk::pflag::BOARD, fk::Unit::RPM, 0, -1000, 1000, 50,
                         "A parameter name that is much longer than forty bytes in all"},
                        {false, false, 0, 0}});
  r.cmd({1, ble::op::PARAM_DESCRIBE, 0});
  CHECK(r.res() == Result::OK);
  const uint8_t *p = r.out + 3;
  CHECK_EQ(p[0], 0);
  CHECK_EQ(p[1], 2 + fk::Tuning::count());  // the heater's two, then the board's own
  CHECK_EQ(u16(p + 2), 14);
  CHECK_EQ(p[4], fk::pflag::NEEDS_OFF);
  CHECK_EQ(p[5], int(fk::Unit::SECONDS));
  CHECK_EQ(p[6], 1);
  CHECK_EQ(p[7], int(fk::ParamGroup::START_UP));
  CHECK_EQ(p[8], int(fk::ParamLevel::EXPERT));
  CHECK_EQ(i32(p + 9), 20);
  CHECK_EQ(i32(p + 13), 150);
  CHECK_EQ(i32(p + 17), 5);
  CHECK(std::string((const char *) p + 21, r.n - 3 - 21) == "Self-test time");
  r.cmd({2, ble::op::PARAM_DESCRIBE, 1});
  CHECK_EQ(r.n, 3 + ble::PARAM_DESC_FIXED + ble::PARAM_NAME_MAX);
  CHECK_EQ(r.out[3 + 4], fk::pflag::BOARD);
  CHECK_EQ(r.out[3 + 7], int(fk::ParamGroup::OTHER));
  CHECK_EQ(i32(r.out + 3 + 9), -1000);
  r.cmd({3, ble::op::PARAM_DESCRIBE, 2});
  CHECK(r.res() == Result::OK);
  CHECK_EQ(u16(r.out + 3 + 2), fk::T_FUEL_TRIM);
  r.cmd({3, ble::op::PARAM_DESCRIBE, uint8_t(2 + fk::Tuning::count())});
  CHECK(r.res() == Result::OUT_OF_RANGE);

  r.cmd({4, ble::op::PARAM_GET, 14, 0});
  CHECK(r.res() == Result::OK);
  CHECK_EQ(r.n, 14);
  CHECK_EQ(u16(r.out + 3), 14);
  CHECK_EQ(r.out[5], 0x01);
  CHECK_EQ(i32(r.out + 6), 50);
  CHECK_EQ(i32(r.out + 10), 50);
  // 70 = 0x46; -500 = 0xFFFFFE0C
  r.cmd({5, ble::op::PARAM_SET, 14, 0, 0x46, 0, 0, 0});
  CHECK(r.res() == Result::OK);
  r.cmd({5, ble::op::PARAM_SET, 0x01, 0x0A, 0x0C, 0xFE, 0xFF, 0xFF});
  CHECK(r.res() == Result::OK);
  CHECK_EQ(r.h.params[1].value.staged_value, -500);
  r.cmd({6, ble::op::PARAM_GET, 14, 0});
  CHECK_EQ(r.out[5], 0x03);
  CHECK_EQ(i32(r.out + 10), 70);
  r.cmd({7, ble::op::PARAM_SET, 14, 0, 0xFF, 0, 0, 0});
  CHECK(r.res() == Result::OUT_OF_RANGE);
  r.cmd({8, ble::op::PARAM_GET, 99, 0});
  CHECK(r.res() == Result::OUT_OF_RANGE);
  r.cmd({9, ble::op::PARAM_APPLY, 1});
  CHECK(r.res() == Result::BAD_LENGTH);  // no argument any more: every Apply saves
  r.cmd({9, ble::op::PARAM_APPLY});
  CHECK(r.res() == Result::OK);
  CHECK_EQ(r.h.params[0].value.value, 70);
  r.cmd({10, ble::op::PARAM_DISCARD});
  CHECK(r.res() == Result::OK);

  // PARAM_DESCRIBE_ID: the same answer as PARAM_DESCRIBE, by id.
  r.cmd({2, ble::op::PARAM_DESCRIBE, 1});
  std::vector<uint8_t> by_index(r.out + 3, r.out + r.n);
  r.cmd({2, ble::op::PARAM_DESCRIBE_ID, 0x01, 0x0A});
  CHECK(r.res() == Result::OK);
  CHECK(std::vector<uint8_t>(r.out + 3, r.out + r.n) == by_index);
  CHECK_EQ(r.out[3], 1);  // its index
  r.cmd({3, ble::op::PARAM_DESCRIBE_ID, uint8_t(fk::T_MAX_POWER), uint8_t(fk::T_MAX_POWER >> 8)});
  CHECK(r.res() == Result::OK);
  CHECK_EQ(u16(r.out + 3 + 2), fk::T_MAX_POWER);
  CHECK_EQ(i32(r.out + 3 + 13), 100);
  r.cmd({3, ble::op::PARAM_DESCRIBE_ID, 99, 0});
  CHECK(r.res() == Result::OUT_OF_RANGE);
  r.cmd({3, ble::op::PARAM_DESCRIBE_ID, 99});
  CHECK(r.res() == Result::BAD_LENGTH);

  // PARAM_HELP: the heater's text, the board's own, none, unknown.
  r.cmd({11, ble::op::PARAM_HELP, 14, 0});
  CHECK(r.res() == Result::OK);
  CHECK_EQ(u16(r.out + 3), 14);
  CHECK(std::string((const char *) r.out + 5, r.n - 5) == "How long the fan runs its check at start-up.");
  r.cmd({12, ble::op::PARAM_HELP, 0x01, 0x0A});
  CHECK(r.res() == Result::OK);
  CHECK_EQ(r.n, 5);  // a parameter with no description
  r.cmd({13, ble::op::PARAM_HELP, uint8_t(fk::T_CYCLE), uint8_t(fk::T_CYCLE >> 8)});
  CHECK(r.res() == Result::OK);
  CHECK(r.n > 5 + 20);
  CHECK(r.n <= 5 + fk::PARAM_HELP_MAX);
  r.cmd({14, ble::op::PARAM_HELP, 99, 0});
  CHECK(r.res() == Result::OUT_OF_RANGE);
  r.cmd({15, ble::op::PARAM_HELP, 14});
  CHECK(r.res() == Result::BAD_LENGTH);
}

// PARAM_GROUP: the groups that have parameters, in menu order, with their names; which parameters
// are in each comes with PARAM_DESCRIBE.
static void test_param_groups() {
  Rig r;
  r.h.params.push_back({{14, fk::pflag::NEEDS_OFF, fk::Unit::SECONDS, 1, 20, 150, 5, "Self-test time",
                         fk::ParamGroup::START_UP, fk::ParamLevel::EXPERT},
                        {true, false, 50, 0}});
  r.h.params.push_back({{2, 0, fk::Unit::UNITLESS, 0, 0, 1, 1, "Mystery"}, {true, false, 0, 0}});  // no group
  // In menu order: thermostat, frost protection, room temperature, fuel use, battery, fuel pump (fuel
  // trim), fan (fan trim), the heater's start-up, altitude, maintenance, then Other.
  const uint8_t want_codes[] = {1, 9, 10, 8, 11, 2, 3, 4, 7, 12, 0};
  const size_t groups = sizeof(want_codes);
  for (size_t i = 0; i < groups; i++) {
    r.cmd({1, ble::op::PARAM_GROUP, uint8_t(i)});
    CHECK(r.res() == Result::OK);
    const uint8_t *p = r.out + 3;
    CHECK_EQ(p[0], i);
    CHECK_EQ(p[1], groups);
    CHECK_EQ(p[2], want_codes[i]);
    std::string name((const char *) p + 3, r.n - 3 - 3);
    CHECK(name == fk::param_group_name(fk::ParamGroup(want_codes[i])));
  }
  r.cmd({2, ble::op::PARAM_GROUP, uint8_t(groups)});
  CHECK(r.res() == Result::OUT_OF_RANGE);
  r.cmd({3, ble::op::PARAM_GROUP});
  CHECK(r.res() == Result::BAD_LENGTH);
  // Every parameter's group is one PARAM_GROUP lists.
  for (size_t i = 0; i < r.c.param_count(); i++) {
    r.cmd({4, ble::op::PARAM_DESCRIBE, uint8_t(i)});
    bool listed = false;
    for (uint8_t code : want_codes)
      listed = listed || r.out[3 + 7] == code;
    CHECK(listed);
  }
}

static void test_events() {
  uint8_t out[ble::RESULT_MAX];
  size_t n = ble::encode_event(fk::Event::PARAM_CHANGED, Result::OK, 0x0102, nullptr, out, sizeof(out));
  CHECK_EQ(n, 5);
  CHECK_EQ(out[0], 0);
  CHECK_EQ(out[1], 3);
  CHECK_EQ(u16(out + 3), 0x0102);
  n = ble::encode_event(fk::Event::FAILSAFE_STOP, Result::OK, 0, "gone", out, sizeof(out));
  CHECK_EQ(n, 7);
  CHECK(std::string((const char *) out + 3, 4) == "gone");
  // Long texts are cut to the record size.
  std::string long_text(200, 'x');
  n = ble::encode_event(fk::Event::PARAMS_APPLIED, Result::REFUSED, 0, long_text.c_str(), out, sizeof(out));
  CHECK_EQ(n, ble::RESULT_MAX);

  fk::WifiStatus w;
  w.ip[0] = 192, w.ip[1] = 168, w.ip[2] = 1, w.ip[3] = 20;
  std::strcpy(w.ssid, "Van");
  n = ble::encode_wifi_event(&w, nullptr, out, sizeof(out));
  CHECK_EQ(n, 3 + 4 + 3);
  CHECK_EQ(out[1], 5);
  CHECK_EQ(out[2], 0);
  CHECK_EQ(out[3], 192);
  n = ble::encode_wifi_event(nullptr, "no answer", out, sizeof(out));
  CHECK_EQ(out[2], 0x2F);
  CHECK(std::string((const char *) out + 3, n - 3) == "no answer");
}

int main() {
  test_uuid();
  test_info();
  test_state_and_telemetry();
  test_commands();
  test_board_commands();
  test_update_commands();
  test_relay_commands();
  test_unsupported();
  test_heater_log();
  test_params();
  test_param_groups();
  test_events();
  return report("ble_protocol");
}
