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
  const fk::WifiKey *k0 = nullptr, *k1 = nullptr;

  Rig() {
    relay.set_io({[](const char *, const char *) {}, []() {},
                  [this](const uint8_t *d, size_t n) {
                    if (!link_ok)
                      return false;
                    sent.emplace_back(d, d + n);
                    return true;
                  }});
    push.on_save([this](const fk::PushEngine::Saved &) { saves++; });
    const uint8_t p0[6] = {1, 1, 1, 1, 1, 1}, p1[6] = {2, 2, 2, 2, 2, 2};
    k0 = access.issue_app_key(p0);
    k1 = access.issue_app_key(p1);
    relay.set(true, nullptr, 0);
    step(0);
  }
  void relay_up(uint8_t want) {
    relay.connected(now);
    std::vector<uint8_t> ch = {0, 1};
    ch.resize(34, 9);
    relay.message(ch.data(), ch.size(), now);
    const uint8_t w[4] = {0, 8, want, 0};
    relay.message(w, 4, now);
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
  r.relay_up(0x03);  // both phones registered
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
  // Payload: version 1, the alert's log record, class, alert 10, alerts active.
  CHECK(p0[0] == 1 && p0[9] == 3 && p0[10] == 0 && p0[11] == 10);
  CHECK(u32(p0.data() + 1) == r.a.log().newest() || u32(p0.data() + 1) <= r.a.log().newest());
  CHECK(u16(p0.data() + 13) & (1u << 10));
  CHECK(p0[35] > 0 && p0.size() == 36u + p0[35]);  // the alert's text
  // A relay that changes the class breaks the tag.
  auto forged = m;
  forged[2] = 0;
  CHECK(r.open(forged, 0, r.k0).empty());
}

static void test_prefs_and_filters() {
  Rig r;
  r.relay_up(0x03);
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
  // The CO alarm still goes to both.
  r.h.st.fault = fk::Fault::NONE;
  r.h.st.co_state = uint8_t(fk::CoState::ALARM);
  r.step(5 * SEC);
  auto ps = r.pushes(mark);
  CHECK(ps.size() == 1 && ps[0][3] == 2);
  // A phone that shows alerts itself over Bluetooth gets none but the CO alarm.
  CHECK(r.push.local_alerts(0, true).accepted());
  r.push.bluetooth_gone(0);
  // A new key for phone 0 starts from the defaults.
  const uint8_t p0[6] = {1, 1, 1, 1, 1, 1};
  r.access.issue_app_key(p0);
  CHECK(r.push.prefs_get(0, got) && got.alerts == (fk::push::DEFAULT_ALERTS | 0x0400) && got.events == 0 &&
        got.flags == fk::push::DEFAULT_FLAGS);
}

static void test_coalescing_and_hold_off() {
  Rig r;
  r.relay_up(0x01);
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
  CHECK(!p.empty() && p[11] == 11 && (u16(p.data() + 15) & (1u << 2)));
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
  r.relay_up(0x01);
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
    CHECK(!p.empty() && p[11] == 10 && ((p[19] >> 1) & 3) == 1);  // repeat 1
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
  r.relay_up(0x01);
  r.step(SEC);
  auto ps = r.pushes();
  CHECK(ps.size() == 1);
  if (!ps.empty()) {
    auto p = r.open(ps[0], 0, r.k0);
    CHECK(!p.empty() && p[11] == 2 && (p[19] & 1) && u32(p.data() + 1) <= seq_at);
  }
  // An alert that cleared while the relay was down is not sent.
  Rig q;
  q.step(SEC);
  q.h.st.co_state = uint8_t(fk::CoState::SENSOR_FAULT);
  q.step(10 * SEC);
  q.h.st.co_state = uint8_t(fk::CoState::OK);
  q.step(2 * SEC);
  q.relay_up(0x01);
  q.step(SEC);
  CHECK(q.pushes().empty());
}

static void test_events() {
  Rig r;
  r.relay_up(0x01);
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
    CHECK(!pl.empty() && pl[9] == 0 && pl[10] == 1 && pl[11] == fk::push::EV_STARTED_BY_OTHER &&
          pl[12] == uint8_t(Source::HOME_ASSISTANT));
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
  s.timer_end_utc = 0;
  fk::PushEngine::Item lead{0, 0, 0, 77, 1790000000u};
  uint8_t out[100];
  size_t n = fk::PushEngine::encode_payload(lead, 0x0008, 0x0010, 0x01, s, "fuel low: 12 % left", out, sizeof(out));
  CHECK(n == 36 + 19);
  CHECK(out[0] == 1 && u32(out + 1) == 77 && u32(out + 5) == 1790000000u && out[9] == 1 && out[10] == 0 && out[11] == 0);
  CHECK(u16(out + 13) == 0x0401 && u16(out + 15) == 0x0008 && u16(out + 17) == 0x0010 && out[19] == 1);
  CHECK(out[20] == 12 && u16(out + 21) == 1234 && u16(out + 25) == 0xFFFF && u16(out + 27) == 48 && u16(out + 29) == 0x8000);
  CHECK(out[35] == 19 && std::memcmp(out + 36, "fuel low: 12 % left", 19) == 0);
  // Classes.
  CHECK(fk::push::alert_class(10) == 3 && fk::push::alert_class(11) == 2 && fk::push::alert_class(0) == 1 &&
        fk::push::alert_class(8) == 0 && fk::push::alert_class(4) == 0);
}

int main() {
  test_payload_layout();
  test_alert_reaches_each_phone();
  test_prefs_and_filters();
  test_coalescing_and_hold_off();
  test_co_repeats();
  test_held_while_relay_down();
  test_events();
  return report("push");
}
