// Push notifications, board side (PROTOCOL.md): who gets what, coalescing,
// hold-off, CO repeats, pushes held while the relay is down, and the blob each phone decrypts.
#include "fake_heater.h"

#include <cmath>

#include "../automation.h"
#include "../crypto.h"
#include "../push.h"
#include "../relay.h"
#include "../relay_client.h"

using fk::HeaterState;
using fk::Mode;
using fk::Source;
namespace rl = fk::relay;

static uint32_t g_x = 7;
static const rl::Random RNG = [](uint8_t *out, size_t n) {
  for (size_t i = 0; i < n; i++) {
    g_x = g_x * 1103515245u + 12345u;
    out[i] = uint8_t(g_x >> 16);
  }
};
static const uint32_t SEC = 1000;

struct Rig {
  FakeHeater h;
  fk::Controller c{&h};
  fk::Automation a{c};
  fk::Access access{RNG};
  fk::RelayClient relay{access, RNG};
  fk::PushEngine push{c, a, access, relay, RNG};
  std::vector<std::vector<uint8_t>> sent;  // everything the relay socket got
  bool link_ok = true;
  uint32_t now = 1000;
  int saves = 0;
  fk::PushEngine::Saved last_saved{};
  const fk::WifiKey *k0 = nullptr, *k1 = nullptr;

  Rig() {
    relay.set_io({[](const char *, const char *) {}, []() {},
                  [this](const uint8_t *d, size_t n) {
                    if (!link_ok)
                      return false;
                    sent.emplace_back(d, d + n);
                    return true;
                  }});
    push.on_save([this](const fk::PushEngine::Saved &s) {
      saves++;
      last_saved = s;
    });
    const uint8_t p0[6] = {1, 1, 1, 1, 1, 1}, p1[6] = {2, 2, 2, 2, 2, 2};
    k0 = access.issue_app_key(p0);
    k1 = access.issue_app_key(p1);
    relay.set(true, nullptr, 0);
    step(0);
  }
  // The relay connects and the board signs in: ready at once.
  void relay_up() {
    relay.connected(now);
    std::vector<uint8_t> ch = {0, 1};
    ch.resize(34, 9);
    relay.message(ch.data(), ch.size(), now);
  }
  // The LIVE messages sent since index `from`.
  std::vector<std::vector<uint8_t>> lives(size_t from = 0) const {
    std::vector<std::vector<uint8_t>> out;
    for (size_t i = from; i < sent.size(); i++)
      if (sent[i].size() >= 9 && sent[i][0] == 0 && sent[i][1] == rl::op::LIVE)
        out.push_back(sent[i]);
    return out;
  }
  // The slots a LIVE is for, in order (its slot mask).
  static std::vector<int> live_slots(const std::vector<uint8_t> &m) {
    std::vector<int> out;
    for (int i = 0; i < 8; i++)
      if (m[8] & (1u << i))
        out.push_back(i);
    return out;
  }
  // A LIVE's plain record, if it is for slot `slot`; empty if not.
  static std::vector<uint8_t> live_record(const std::vector<uint8_t> &m, int slot) {
    if (m.size() != 9 + 28 || !(m[8] & (1u << slot)))
      return {};
    return std::vector<uint8_t>(m.begin() + 9, m.end());
  }
  // A start from the phone in key slot `slot` (as the command handler does it), the heater following.
  void phone_start(int slot, Mode mode = Mode::HEAT) {
    c.set_start_slot(slot);
    CHECK(c.set_mode(mode, Source::PHONE).accepted());
    c.set_start_slot(-1);
    h.st.state = mode == Mode::FAN ? HeaterState::FAN_ONLY : HeaterState::HEATING;
  }
  void heater_off() {
    CHECK(c.set_mode(Mode::OFF, Source::PHONE).accepted());
    h.st.state = HeaterState::OFF;
  }
  void step(uint32_t ms) {
    uint32_t end = now + ms;
    do {
      c.tick(now, false);
      a.tick(now);
      relay.loop(now, true);
      push.loop(now);
      now += 250;
    } while (int32_t(end - now) > 0);
  }
  // The PUSH messages sent since index `from`.
  std::vector<std::vector<uint8_t>> pushes(size_t from = 0) const {
    std::vector<std::vector<uint8_t>> out;
    for (size_t i = from; i < sent.size(); i++)
      if (sent[i].size() > 2 && sent[i][0] == 0 && sent[i][1] == rl::op::PUSH)
        out.push_back(sent[i]);
    return out;
  }
  // Opens slot `slot`'s blob in a PUSH with that phone's key; empty if absent or it fails.
  std::vector<uint8_t> open(const std::vector<uint8_t> &m, int slot, const fk::WifiKey *k) const {
    uint8_t cls = m[2], n = m[3];
    size_t at = 4;
    for (uint8_t i = 0; i < n; i++) {
      uint8_t s = m[at], len = m[at + 1];
      const uint8_t *blob = m.data() + at + 2;
      if (s == slot) {
        uint8_t key[16];
        rl::push_key(k->secret, 32, key);
        uint8_t ad[17];
        std::memcpy(ad, relay.identity().id(), 16);
        ad[16] = cls;
        std::vector<uint8_t> pt(len - 12 - 16);
        if (!fk::crypto::gcm_open(key, blob, ad, 17, blob + 12, len - 12, pt.data()))
          return {};
        return pt;
      }
      at += 2 + len;
    }
    return {};
  }
};

static uint16_t u16(const uint8_t *p) { return uint16_t(p[0] | (p[1] << 8)); }
static uint32_t u32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

static void test_alert_reaches_each_phone() {
  Rig r;
  r.relay_up();
  r.step(SEC);
  size_t mark = r.sent.size();
  r.h.st.co_state = uint8_t(fk::CoState::ALARM);  // CO alarm: bit 10, class 3
  r.step(2 * SEC);
  CHECK(r.pushes(mark).empty());  // still coalescing
  r.step(2 * SEC);
  auto ps = r.pushes(mark);
  CHECK(ps.size() == 1);
  if (ps.empty())
    return;
  const auto &m = ps[0];
  CHECK(m[2] == 3 && m[3] == 2);  // class 3, two phones
  auto p0 = r.open(m, 0, r.k0), p1 = r.open(m, 1, r.k1);
  CHECK(!p0.empty() && !p1.empty());
  CHECK(r.open(m, 0, r.k1).empty());  // phone 1's key does not open phone 0's blob
  // Payload: the alert's log record, class, alert 10, alerts active.
  CHECK(p0[8] == 3 && p0[9] == 0 && p0[10] == 10);
  CHECK(u32(p0.data() + 0) <= r.a.log().newest());
  CHECK(u16(p0.data() + 12) & (1u << 10));
  CHECK(p0[26] > 0 && p0.size() == 27u + p0[26]);  // the alert's text
  // A relay that changes the class breaks the tag.
  auto forged = m;
  forged[2] = 0;
  CHECK(r.open(forged, 0, r.k0).empty());
}

static void test_prefs_and_filters() {
  Rig r;
  r.relay_up();
  // Phone 1 turns notifications off; phone 0 drops the burn alert (bit 2).
  fk::PushPrefs off;
  off.flags = 0;
  CHECK(r.push.prefs_set(1, off).accepted() && r.saves == 1);
  fk::PushPrefs no_burn;
  no_burn.alerts = fk::push::DEFAULT_ALERTS & ~(1u << 2);
  CHECK(r.push.prefs_set(0, no_burn).accepted());
  fk::PushPrefs got;
  CHECK(r.push.prefs_get(0, got) && got.alerts == no_burn.alerts && got.flags == fk::push::DEFAULT_FLAGS);
  CHECK(fk::push::DEFAULT_FLAGS == 0x07);
  // Flags bits 1-3 (the live card) are kept and returned; bits 4-7 are refused.
  const uint8_t p2[6] = {3, 3, 3, 3, 3, 3};
  CHECK(r.access.issue_app_key(p2) != nullptr && r.access.app_key(2) != nullptr);
  fk::PushPrefs card;
  card.alerts = 0x0001;  // only fuel low: GET still shows the CO alarm (bit 10) on
  card.flags = 0x0B;
  CHECK(r.push.prefs_set(2, card).accepted());
  CHECK(r.push.prefs_get(2, got) && got.alerts == 0x0401 && got.flags == 0x0B);
  card.flags = 0x10;
  CHECK(r.push.prefs_set(2, card).result == fk::Result::OUT_OF_RANGE);
  CHECK(r.push.prefs_get(2, got) && got.flags == 0x0B);
  CHECK(!r.push.prefs_get(5, got));  // no key there
  CHECK(r.push.prefs_set(5, off).result == fk::Result::KEY_UNKNOWN);
  r.step(SEC);
  size_t mark = r.sent.size();
  r.h.st.fault = fk::Fault::OVERHEAT;  // a heater fault: burn problem, bit 2
  r.h.st.answering = true;
  r.step(5 * SEC);
  CHECK(r.pushes(mark).empty());  // nobody wants it
  // The CO alarm still goes to every phone with a key, all three (the relay drops those without a
  // device token).
  r.h.st.fault = fk::Fault::NONE;
  r.h.st.co_state = uint8_t(fk::CoState::ALARM);
  r.step(5 * SEC);
  auto ps = r.pushes(mark);
  CHECK(ps.size() == 1 && ps[0][3] == 3);
  // A new key for phone 0 starts from the defaults.
  const uint8_t p0[6] = {1, 1, 1, 1, 1, 1};
  r.access.issue_app_key(p0);
  CHECK(r.push.prefs_get(0, got) && got.alerts == (fk::push::DEFAULT_ALERTS | 0x0400) && got.events == 0 &&
        got.flags == fk::push::DEFAULT_FLAGS);
}

static void test_coalescing_and_hold_off() {
  Rig r;
  r.relay_up();
  r.step(SEC);
  size_t mark = r.sent.size();
  // Two alerts within 3 s: one push, led by the more urgent.
  r.h.st.co_state = uint8_t(fk::CoState::SENSOR_FAULT);  // bit 11, class 2
  r.step(SEC);
  r.h.st.fault = fk::Fault::OVERHEAT;  // bit 2, class 2 (first of equals leads: 11)
  r.step(5 * SEC);
  auto ps = r.pushes(mark);
  CHECK(ps.size() == 1);
  auto p = r.open(ps[0], 0, r.k0);
  CHECK(!p.empty() && p[10] == 11);  // the CO sensor fault leads
  // The burn alert clears and rises again within the hour: held off.
  r.h.st.fault = fk::Fault::NONE;
  r.a.clear_alerts(fk::alert::BURN);
  r.step(2 * SEC);
  mark = r.sent.size();
  r.h.st.fault = fk::Fault::FAN;
  r.step(5 * SEC);
  CHECK(r.pushes(mark).empty());
}

static void test_co_repeats() {
  Rig r;
  r.relay_up();
  r.step(SEC);
  size_t mark = r.sent.size();
  r.h.st.co_state = uint8_t(fk::CoState::ALARM);
  r.step(10 * SEC);
  CHECK(r.pushes(mark).size() == 1);
  r.step(5 * 60 * SEC);
  auto ps = r.pushes(mark);
  CHECK(ps.size() == 2);
  if (ps.size() == 2) {
    auto p = r.open(ps[1], 0, r.k0);
    CHECK(!p.empty() && p[10] == 10 && ((p[14] >> 1) & 3) == 1);  // repeat 1
  }
  // Someone signs in: no third.
  r.push.app_seen();
  r.step(6 * 60 * SEC);
  CHECK(r.pushes(mark).size() == 2);
}

static void test_held_while_relay_down() {
  Rig r;
  r.step(SEC);  // relay never up
  r.h.st.fault = fk::Fault::OVERHEAT;
  r.step(10 * SEC);
  CHECK(r.pushes().empty());
  // Back: the held alert goes out, flagged delayed, with its original record.
  uint32_t seq_at = r.a.log().newest();
  r.relay_up();
  r.step(SEC);
  auto ps = r.pushes();
  CHECK(ps.size() == 1);
  if (!ps.empty()) {
    auto p = r.open(ps[0], 0, r.k0);
    CHECK(!p.empty() && p[10] == 2 && (p[14] & 1) && u32(p.data() + 0) <= seq_at);
  }
  // An alert that cleared while the relay was down is not sent.
  Rig q;
  q.step(SEC);
  q.h.st.co_state = uint8_t(fk::CoState::SENSOR_FAULT);
  q.step(10 * SEC);
  q.h.st.co_state = uint8_t(fk::CoState::OK);
  q.step(2 * SEC);
  q.relay_up();
  q.step(SEC);
  CHECK(q.pushes().empty());
}

static void test_events() {
  Rig r;
  r.relay_up();
  fk::PushPrefs p;
  p.events = (1u << fk::push::EV_STARTED_BY_OTHER) | (1u << fk::push::EV_STOPPED_BY_OTHER);
  CHECK(r.push.prefs_set(0, p).accepted());
  r.step(SEC);
  size_t mark = r.sent.size();
  CHECK(r.c.set_mode(Mode::FAN, Source::HOME_ASSISTANT).accepted());
  r.h.st.state = HeaterState::FAN_ONLY;
  r.step(5 * SEC);
  auto ps = r.pushes(mark);
  CHECK(ps.size() == 1);
  if (!ps.empty()) {
    auto pl = r.open(ps[0], 0, r.k0);
    CHECK(!pl.empty() && pl[8] == 0 && pl[9] == 1 && pl[10] == fk::push::EV_STARTED_BY_OTHER &&
          pl[11] == uint8_t(Source::HOME_ASSISTANT));
  }
  // Phone 1 (no events wanted) got nothing.
  CHECK(ps.empty() || ps[0][3] == 1);
}

static void test_payload_layout() {
  fk::Status s;
  s.alerts = 0x0401;
  s.fuel_left = 12;
  s.supply_v = 12.34f;
  s.fault = fk::Fault::NONE;
  s.panel_fault = 0;
  s.co_ppm = NAN;
  s.room_c = 4.8f;
  s.target_c = NAN;
  s.run_end_utc = 0;
  fk::PushEngine::Item lead{0, 0, 0, 77, 1790000000u};
  uint8_t out[100];
  size_t n = fk::PushEngine::encode_payload(lead, 0x01, s, "fuel low: 12 % left", out, sizeof(out));
  CHECK(n == 27 + 19);
  CHECK(u32(out + 0) == 77 && u32(out + 4) == 1790000000u && out[8] == 1 && out[9] == 0 && out[10] == 0);
  CHECK(u16(out + 12) == 0x0401 && out[14] == 1);
  CHECK(out[15] == 12 && u16(out + 16) == 1234 && u16(out + 18) == 48 && u16(out + 20) == 0x8000 && u32(out + 22) == 0);
  CHECK(out[26] == 19 && std::memcmp(out + 27, "fuel low: 12 % left", 19) == 0);
  // Classes.
  CHECK(fk::push::alert_class(10) == 3 && fk::push::alert_class(11) == 2 && fk::push::alert_class(0) == 1 &&
        fk::push::alert_class(8) == 0 && fk::push::alert_class(4) == 0);
}

// A fan stall the safety MCU retries is not pushed; the lockout after the retries is, once, as an
// urgent burn problem carrying the fault (7) and the reason (7).
static void test_fan_retry_not_pushed() {
  Rig r;
  r.relay_up();
  r.step(SEC);
  CHECK(r.c.set_mode(Mode::FAN, Source::PHONE).accepted());
  r.h.st.state = HeaterState::FAN_ONLY;
  r.step(SEC);
  size_t mark = r.sent.size();
  r.h.st.fault = fk::Fault::FAN;
  r.h.st.panel_fault = uint8_t(fk::Reason::FAN_STALL);
  r.step(5 * SEC);
  r.h.st.fault = fk::Fault::NONE;
  r.h.st.panel_fault = 0;
  r.step(5 * SEC);
  CHECK(r.pushes(mark).empty());
  r.h.st.fault = fk::Fault::FAN;
  r.h.st.panel_fault = uint8_t(fk::Reason::FAN_STALL);
  r.h.st.state = HeaterState::OFF;
  r.h.st.mode = Mode::OFF;
  r.step(10 * SEC);
  auto ps = r.pushes(mark);
  CHECK_EQ(ps.size(), 1);
  if (ps.size() == 1) {
    CHECK_EQ(ps[0][2], 2);  // urgent
    auto p = r.open(ps[0], 0, r.k0);
    CHECK(!p.empty() && p[9] == 0 && p[10] == 2);  // the burn problem (its text says the fan stalled)
  }
}

// The live card: a run's start, its updates (what the user asked for 5 s after the last frame,
// other changes 20 s after, else every 120 s) and its end, to the phones whose preferences take the run, one plain record with the phones it is for.
static void test_live_card() {
  Rig r;
  r.relay_up();
  r.step(6 * SEC);
  // A run from Home Assistant: others' runs are off by default, so no card.
  size_t mark = r.sent.size();
  CHECK(r.c.set_mode(Mode::HEAT, Source::HOME_ASSISTANT).accepted());
  r.h.st.state = HeaterState::HEATING;
  r.step(30 * SEC);
  CHECK(r.lives(mark).empty());
  r.heater_off();
  r.step(30 * SEC);
  CHECK(r.lives(mark).empty());

  // A run the phone in slot 0 starts: its card, for it only (slot 1 takes no others' runs).
  mark = r.sent.size();
  r.phone_start(0);
  r.step(SEC);
  auto ls = r.lives(mark);
  CHECK_EQ(ls.size(), 1);
  if (ls.size() != 1)
    return;
  CHECK(ls[0][2] == fk::push::LIVE_START && ls[0][3] == fk::push::LIVE_IMPORTANT);
  const uint32_t run = u32(ls[0].data() + 4);  // the controller's run id
  CHECK(run != 0 && run == r.c.status().run_id);
  CHECK(Rig::live_slots(ls[0]) == std::vector<int>({0}));
  auto rec = Rig::live_record(ls[0], 0);
  CHECK_EQ(rec.size(), 28);
  if (rec.size() == 28) {
    CHECK(rec[4] == uint8_t(HeaterState::HEATING) && rec[6] == uint8_t(Source::PHONE));
    CHECK(rec[5] == 1);  // heat by power (the controller's default)
  }
  CHECK(Rig::live_record(ls[0], 1).empty());

  // A power change 1 s after the start goes 5 s after it, not important.
  mark = r.sent.size();
  CHECK(r.c.set_power(70, Source::PHONE).accepted());
  r.step(3 * SEC);
  CHECK(r.lives(mark).empty());
  r.step(3 * SEC);
  ls = r.lives(mark);
  CHECK_EQ(ls.size(), 1);
  if (ls.size() == 1) {
    CHECK(ls[0][2] == fk::push::LIVE_UPDATE && ls[0][3] == 0 && u32(ls[0].data() + 4) == run);
    rec = Rig::live_record(ls[0], 0);
    CHECK(rec.size() == 28 && rec[14] == 70);
  }
  // A room change waits for the 20 s window.
  mark = r.sent.size();
  r.c.room().probe(18.0f, r.now);
  r.step(10 * SEC);
  CHECK(r.lives(mark).empty());
  r.step(11 * SEC);
  ls = r.lives(mark);
  CHECK_EQ(ls.size(), 1);
  if (ls.size() == 1) {
    rec = Rig::live_record(ls[0], 0);
    CHECK(rec.size() == 28 && int16_t(u16(rec.data() + 10)) == 180);
  }
  // Nothing changes: a heartbeat every 120 s.
  mark = r.sent.size();
  r.step(110 * SEC);  // the update went out 3 s before the mark
  CHECK(r.lives(mark).empty());
  r.step(10 * SEC);
  CHECK_EQ(r.lives(mark).size(), 1);
  // The end: at off, not after a fault.
  r.step(125 * SEC);
  mark = r.sent.size();
  r.heater_off();
  r.step(SEC);
  ls = r.lives(mark);
  CHECK_EQ(ls.size(), 1);
  if (ls.size() == 1) {
    CHECK(ls[0][2] == fk::push::LIVE_END && ls[0][3] == fk::push::LIVE_IMPORTANT && u32(ls[0].data() + 4) == run);
    rec = Rig::live_record(ls[0], 0);
    CHECK(rec.size() == 28 && rec[4] == uint8_t(HeaterState::OFF) && rec[6] == uint8_t(Source::PHONE));
  }
  mark = r.sent.size();
  r.step(200 * SEC);
  CHECK(r.lives(mark).empty());  // no card: nothing more
  // A run that follows its end at once: its start 5 s after the end.
  r.phone_start(0);
  r.step(30 * SEC);
  r.heater_off();
  mark = r.sent.size();
  r.step(SEC);
  r.phone_start(0);
  r.step(3 * SEC);
  ls = r.lives(mark);
  CHECK(ls.size() == 1 && ls[0][2] == fk::push::LIVE_END);
  r.step(2 * SEC);
  ls = r.lives(mark);
  CHECK(ls.size() == 2 && ls.back()[2] == fk::push::LIVE_START);
}

static void test_live_card_rules() {
  Rig r;
  r.relay_up();
  r.step(6 * SEC);
  // A timer's run: the board's runs are on by default, so both phones.
  size_t mark = r.sent.size();
  CHECK(r.c.set_mode(Mode::HEAT, Source::TIMER).accepted());
  r.h.st.state = HeaterState::STARTING;
  r.step(SEC);
  auto ls = r.lives(mark);
  CHECK(ls.size() == 1 && Rig::live_slots(ls[0]) == std::vector<int>({0, 1}));
  // Starting to heating: a state change is important, and goes 5 s after the last frame.
  r.step(3 * SEC);
  mark = r.sent.size();
  r.h.st.state = HeaterState::HEATING;
  r.step(SEC);
  CHECK(r.lives(mark).empty());
  r.step(SEC);
  ls = r.lives(mark);
  CHECK(ls.size() == 1 && ls[0][3] == fk::push::LIVE_IMPORTANT);
  // The phone in slot 1 keeps its card but drops the board's runs (flags 0x03): slot 0 only.
  fk::PushPrefs p;
  p.flags = fk::push::FLAG_ON | fk::push::FLAG_LIVE;
  CHECK(r.push.prefs_set(1, p).accepted());
  r.step(125 * SEC);
  mark = r.sent.size();
  r.step(125 * SEC);
  ls = r.lives(mark);
  CHECK(ls.size() == 1 && Rig::live_slots(ls[0]) == std::vector<int>({0}));
  // A fault stop: the end carries flags bit 1 (the relay keeps such a card longer).
  r.step(25 * SEC);
  mark = r.sent.size();
  r.h.st.fault = fk::Fault::FLAME_OUT;
  r.h.st.state = HeaterState::OFF;
  r.h.st.mode = Mode::OFF;
  r.step(2 * SEC);
  ls = r.lives(mark);
  bool end = false;
  for (auto &m : ls)
    if (m[2] == fk::push::LIVE_END) {
      end = true;
      CHECK(m[3] == (fk::push::LIVE_IMPORTANT | fk::push::LIVE_FAULT_END));
    }
  CHECK(end);
  // Priming never gets a card.
  r.h.st.fault = fk::Fault::NONE;
  r.step(30 * SEC);
  mark = r.sent.size();
  r.h.st.state = HeaterState::PRIMING;
  r.step(30 * SEC);
  CHECK(r.lives(mark).empty());
}

static void test_live_card_relay_down() {
  Rig r;
  r.relay_up();
  r.step(6 * SEC);
  r.phone_start(0);
  r.step(SEC);
  CHECK_EQ(r.lives().size(), 1);
  // The relay goes away: nothing queued; back, an important update at once.
  r.link_ok = false;
  r.relay.closed(0, r.now);
  r.step(300 * SEC);
  r.link_ok = true;
  size_t mark = r.sent.size();
  r.relay_up();
  r.step(SEC);
  auto ls = r.lives(mark);
  CHECK(ls.size() == 1 && ls[0][2] == fk::push::LIVE_UPDATE && ls[0][3] == fk::push::LIVE_IMPORTANT);
  // Down again, and the run ends meanwhile: the end goes when the relay is back.
  r.link_ok = false;
  r.relay.closed(0, r.now);
  r.step(30 * SEC);
  r.heater_off();
  r.step(30 * SEC);
  r.link_ok = true;
  mark = r.sent.size();
  r.relay_up();
  r.step(SEC);
  ls = r.lives(mark);
  CHECK(ls.size() == 1 && ls[0][2] == fk::push::LIVE_END);
}

int main() {
  test_payload_layout();
  test_alert_reaches_each_phone();
  test_prefs_and_filters();
  test_coalescing_and_hold_off();
  test_co_repeats();
  test_held_while_relay_down();
  test_events();
  test_fan_retry_not_pushed();
  test_live_card();
  test_live_card_rules();
  test_live_card_relay_down();
  return report("push");
}
