// Board firmware updates from the app (PROTOCOL.md): offsets, resume,
// progress reports, the heater-off rule, the checks at the end, timeouts and rollback detection.
#include "fake_heater.h"

#include "../board_update.h"

using fk::Mode;
using fk::Result;
using fk::UpdateLast;
using fk::UpdateState;

struct FakeFlash : fk::UpdateFlash {
  uint32_t slot = 1 << 20;
  std::vector<uint8_t> written;
  bool began = false, aborted = false, restarted = false, fail_write = false;
  End end_result = End::OK;
  int ends = 0;
  uint32_t slot_size() const override { return slot; }
  bool begin(uint32_t) override {
    began = true;
    written.clear();
    return true;
  }
  bool write(const uint8_t *d, size_t n) override {
    if (fail_write)
      return false;
    written.insert(written.end(), d, d + n);
    return true;
  }
  End end() override {
    ends++;
    return end_result;
  }
  void abort() override { aborted = true; }
  void restart() override { restarted = true; }
};

static uint32_t u32(const uint8_t *p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

struct Rig {
  FakeHeater h;
  fk::Controller c{&h};
  FakeFlash flash;
  fk::BoardUpdate u{c, flash};
  uint32_t now = 1000;
  std::vector<uint8_t> image;
  uint8_t sha[32];
  int saves = 0;
  fk::BoardUpdate::Saved saved{};
  uint8_t report[fk::UPDATE_REPORT_SIZE];

  explicit Rig(size_t size = 10000) {
    for (size_t i = 0; i < size; i++)
      image.push_back(uint8_t(i * 7 + 3));
    rehash();
    u.on_save([this](const fk::BoardUpdate::Saved &s) {
      saves++;
      saved = s;
    });
    u.load(nullptr, 111);
    step(0);
  }
  void rehash() { fk::crypto::sha256({{image.data(), image.size()}}, sha); }
  void step(uint32_t ms) {
    now += ms;
    c.tick(now, false);
    u.loop(now);
  }
  fk::Outcome begin(uint32_t *off) { return u.update_begin(uint32_t(image.size()), sha, off); }
  // Sends image[at, at + n) as one data record; true if a report came back.
  bool send(uint32_t at, size_t n) {
    std::vector<uint8_t> rec = {uint8_t(at), uint8_t(at >> 8), uint8_t(at >> 16), uint8_t(at >> 24)};
    rec.insert(rec.end(), image.begin() + at, image.begin() + at + n);
    return u.data(rec.data(), rec.size(), report);
  }
  // Everything from `from`, in 240-byte chunks; returns the reports.
  int send_all(uint32_t from) {
    int reports = 0;
    for (uint32_t at = from; at < image.size(); at += 240)
      reports += send(at, std::min<size_t>(240, image.size() - at)) ? 1 : 0;
    return reports;
  }
};

static void test_full_update() {
  Rig r;
  uint32_t off = 99;
  CHECK(r.begin(&off).accepted() && off == 0);
  CHECK(r.u.running() && r.c.status().board_updating);
  // Starts are refused for every source while it runs.
  CHECK(r.c.set_mode(Mode::HEAT, fk::Source::HOME_ASSISTANT).result == Result::BUSY);
  CHECK(r.c.set_mode(Mode::FAN, fk::Source::PHONE).result == Result::BUSY);
  // 10000 bytes: reports at 4096+, 8192+, and the last byte.
  CHECK_EQ(r.send_all(0), 3);
  CHECK_EQ(u32(r.report), 10000u);
  CHECK_EQ(r.report[4], uint8_t(UpdateState::RECEIVED));
  CHECK_EQ(r.report[5], uint8_t(fk::UpdateReport::PROGRESS));
  CHECK(r.flash.written == r.image);
  fk::UpdateStatus st;
  r.u.update_status(st);
  CHECK(st.state == UpdateState::RECEIVED && st.size == 10000 && st.received == 10000 && st.sha256[0] == r.sha[0]);
  CHECK(r.u.update_finish().accepted());
  CHECK_EQ(r.flash.ends, 1);
  CHECK(r.saved.pending == 1 && r.saved.old_build == 111);
  CHECK(r.c.set_mode(Mode::HEAT, fk::Source::PHONE).result == Result::BUSY);  // until the restart
  r.step(500);
  CHECK(!r.flash.restarted);
  r.step(600);
  CHECK(r.flash.restarted);
}

static void test_offsets_and_resume() {
  Rig r;
  uint32_t off;
  CHECK(r.begin(&off).accepted());
  CHECK(!r.send(0, 240));
  // A repeat and a gap are ignored, and answered with the board's position and which it was.
  CHECK(r.send(0, 240));
  CHECK(u32(r.report) == 240u && r.report[5] == uint8_t(fk::UpdateReport::REPEAT));
  CHECK(!r.send(0, 240));    // another repeat within 250 ms: no report
  CHECK(r.send(480, 240));   // but a gap is still answered (its own limit)
  CHECK(u32(r.report) == 240u && r.report[5] == uint8_t(fk::UpdateReport::GAP));
  CHECK(!r.send(720, 240));  // the next gap at the same position within 250 ms: no report
  r.step(300);
  CHECK(r.send(480, 240));
  CHECK(u32(r.report) == 240u && r.report[5] == uint8_t(fk::UpdateReport::GAP));
  // The resend from the reported position is taken and answered at once (not 4096 bytes later).
  CHECK(r.send(240, 240));
  CHECK(u32(r.report) == 480u && r.report[5] == uint8_t(fk::UpdateReport::PROGRESS));
  CHECK(!r.send(480, 240));  // then the usual cadence
  // A gap at a new position is answered at once, even within 250 ms of the last.
  CHECK(r.send(1200, 240));
  CHECK(u32(r.report) == 720u && r.report[5] == uint8_t(fk::UpdateReport::GAP));
  r.send(720, 240);
  CHECK(r.flash.written.size() == 960);
  // Too short, or past the end: ignored.
  uint8_t tiny[3] = {0, 0, 0};
  r.step(300);
  CHECK(r.u.data(tiny, 3, r.report) && u32(r.report) == 960 && r.report[5] == uint8_t(fk::UpdateReport::REFUSED));
  // The link drops: UPDATE_BEGIN again with the same image resumes from 960, from any phone.
  r.step(20000);
  CHECK(r.begin(&off).accepted() && off == 960);
  r.send_all(960);
  CHECK(r.flash.written == r.image);
  CHECK(r.u.update_finish().accepted());
}

static void test_heater_rules() {
  Rig r;
  uint32_t off;
  // Running: refused.
  r.h.st.state = fk::HeaterState::HEATING;
  CHECK(r.begin(&off).result == Result::NOT_OFF);
  r.h.st.state = fk::HeaterState::COOLING;
  CHECK(r.begin(&off).result == Result::NOT_OFF);
  // The heater's own update: busy.
  r.h.st.state = fk::HeaterState::OFF;
  r.h.st.update_running = true;
  CHECK(r.begin(&off).result == Result::BUSY);
  r.h.st.update_running = false;
  // A run asked for (the thermostat may be holding the heater off): refused.
  CHECK(r.c.set_mode(Mode::FAN, fk::Source::PHONE).accepted());
  r.step(100);
  CHECK(r.begin(&off).result == Result::NOT_OFF);
  CHECK(r.c.set_mode(Mode::OFF, fk::Source::PHONE).accepted());
  r.step(100);
  // No reply from the heater at all: allowed.
  r.h.st.answering = false;
  r.h.st.state = fk::HeaterState::UNKNOWN;
  CHECK(r.begin(&off).accepted());
  r.u.update_abort();
  CHECK(!r.u.running() && !r.c.status().board_updating && r.flash.aborted);
  fk::UpdateStatus st;
  r.u.update_status(st);
  CHECK(st.state == UpdateState::NONE && st.last == UpdateLast::DROPPED);
  // Starts allowed again (the heater refuses for its own reason).
  r.h.st.answering = true;
  r.h.st.state = fk::HeaterState::OFF;
  CHECK(r.c.set_mode(Mode::FAN, fk::Source::PHONE).accepted());
}

static void test_sizes_and_takeover() {
  Rig r;
  uint32_t off;
  r.flash.slot = 5000;
  CHECK(r.begin(&off).result == Result::TOO_BIG);
  r.flash.slot = 1 << 20;
  CHECK(r.u.update_begin(0, r.sha, &off).result == Result::OUT_OF_RANGE);
  CHECK(r.begin(&off).accepted());
  r.send(0, 240);
  // A different image while this one is alive: busy; after 60 s without data: it takes over.
  uint8_t other[32] = {1};
  CHECK(r.u.update_begin(4000, other, &off).result == Result::BUSY);
  r.step(61000);
  CHECK(r.u.update_begin(4000, other, &off).accepted() && off == 0);
  fk::UpdateStatus st;
  r.u.update_status(st);
  CHECK(st.size == 4000 && st.received == 0 && st.sha256[0] == 1);
  // No data for 5 minutes: dropped, starts allowed.
  r.step(299000);
  CHECK(r.u.running());
  r.step(2000);
  CHECK(!r.u.running() && !r.c.status().board_updating);
  r.u.update_status(st);
  CHECK(st.last == UpdateLast::DROPPED);
}

static void test_finish_checks() {
  {
    Rig r;
    uint32_t off;
    CHECK(r.u.update_finish().result == Result::REFUSED);  // nothing under way
    CHECK(r.begin(&off).accepted());
    r.send(0, 240);
    CHECK(r.u.update_finish().result == Result::HASH_MISMATCH);  // bytes missing: kept
    CHECK(r.u.running());
    r.send_all(240);
    // The heater came back running (it did not answer at the start): refused, kept.
    r.h.st.state = fk::HeaterState::HEATING;
    CHECK(r.u.update_finish().result == Result::NOT_OFF);
    CHECK(r.u.running());
    r.h.st.state = fk::HeaterState::OFF;
    // Not signed by a trusted key: dropped.
    r.flash.end_result = fk::UpdateFlash::End::BAD_IMAGE;
    CHECK(r.u.update_finish().result == Result::BAD_IMAGE);
    CHECK(!r.u.running() && !r.c.status().board_updating);
    r.step(5000);
    CHECK(!r.flash.restarted);
  }
  {
    // The bytes are not those announced: dropped before the slot writer sees the end.
    Rig r;
    uint32_t off;
    uint8_t announced[32];
    std::memcpy(announced, r.sha, 32);
    CHECK(r.begin(&off).accepted());
    r.image[5000] ^= 0xFF;
    r.send_all(0);
    CHECK(r.u.update_finish().result == Result::HASH_MISMATCH);
    CHECK(!r.u.running() && r.flash.ends == 0 && r.flash.aborted);
    fk::UpdateStatus st;
    r.u.update_status(st);
    CHECK(st.last == UpdateLast::DROPPED);
  }
  {
    // A flash write fails: dropped at once, reported.
    Rig r;
    uint32_t off;
    CHECK(r.begin(&off).accepted());
    r.flash.fail_write = true;
    CHECK(r.send(0, 240));
    CHECK(r.report[4] == uint8_t(UpdateState::NONE) && !r.u.running());
  }
}

static void test_restart_outcome() {
  // Installed by build 111; build 222 comes up and confirms itself after 60 s.
  {
    Rig r;
    fk::BoardUpdate::Saved s = r.saved;
    s.check = fk::BoardUpdate::SAVED_CHECK;
    s.pending = 1;
    s.old_build = 111;
    FakeFlash f2;
    fk::BoardUpdate after{r.c, f2};
    fk::BoardUpdate::Saved got{};
    after.on_save([&](const fk::BoardUpdate::Saved &x) { got = x; });
    after.load(&s, 222);
    after.loop(5000);
    fk::UpdateStatus st;
    after.update_status(st);
    CHECK(st.last == UpdateLast::NOTHING);
    after.loop(65000);
    after.update_status(st);
    CHECK(st.last == UpdateLast::INSTALLED && got.pending == 0);
  }
  // Build 111 is running again: the new one did not come up.
  {
    Rig r;
    fk::BoardUpdate::Saved s{};
    s.check = fk::BoardUpdate::SAVED_CHECK;
    s.pending = 1;
    s.old_build = 111;
    FakeFlash f2;
    fk::BoardUpdate after{r.c, f2};
    fk::BoardUpdate::Saved got{};
    after.on_save([&](const fk::BoardUpdate::Saved &x) { got = x; });
    after.load(&s, 111);
    fk::UpdateStatus st;
    after.update_status(st);
    CHECK(st.last == UpdateLast::ROLLED_BACK && got.pending == 0 && got.last == uint8_t(UpdateLast::ROLLED_BACK));
    // A refused UPDATE_BEGIN leaves the last result alone; only an accepted one clears it.
    after.loop(1000);
    uint8_t sha[32] = {};
    uint32_t off;
    r.h.st.state = fk::HeaterState::HEATING;
    CHECK(after.update_begin(1000, sha, &off).result == fk::Result::NOT_OFF);
    f2.slot = 10;
    r.h.st.state = fk::HeaterState::OFF;
    CHECK(after.update_begin(1000, sha, &off).result == fk::Result::TOO_BIG);
    after.update_status(st);
    CHECK(st.last == UpdateLast::ROLLED_BACK);
    f2.slot = 1 << 20;
    CHECK(after.update_begin(1000, sha, &off).accepted());
    after.update_status(st);
    CHECK(st.last == UpdateLast::NOTHING);
  }
}

int main() {
  test_full_update();
  test_offsets_and_resume();
  test_heater_rules();
  test_sizes_and_takeover();
  test_finish_checks();
  test_restart_outcome();
  return report("board_update");
}
