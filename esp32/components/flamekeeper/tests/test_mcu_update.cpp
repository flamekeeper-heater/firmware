// The safety-MCU updater against a simulated STM32G030 (sim_stm32.h): the AN3155 commands, a whole
// update through the link client, the refusals, a wrong chip, blocks that read back wrong, a power
// loss mid-write and the update after it (also after a restart of the ESP32), lost and garbled
// bytes, pages 30 and 31 never touched, and the option bytes.
#include <cstring>
#include <string>
#include <vector>

#include "../bytes.h"
#include "../link_client.h"
#include "../mcu_update.h"
#include "../stm32_boot.h"
#include "fake_heater.h"
#include "sim_stm32.h"

using fk::McuJob;
using fk::Result;
using Boot = fk::Stm32Boot;

static std::vector<uint8_t> make_image(uint32_t size, uint8_t minor, uint32_t seed) {
  std::vector<uint8_t> v(size);
  uint32_t x = seed;
  for (auto &b : v) {
    x = x * 1103515245u + 12345u;
    b = uint8_t(x >> 16);
  }
  fk::put_u32(v.data(), 0x20002000u);      // the initial stack pointer
  fk::put_u32(v.data() + 4, 0x080000C1u);  // the reset handler (Thumb)
  v[0x100] = 0;
  v[0x101] = minor;
  v[0x102] = 0;
  return v;
}

static const uint32_t CONFIGURED_OPTR = 0xDEFEE1AAu;  // WDG-1 on the factory value

struct Rig {
  SimStm32 mcu;
  fk::McuUpdate upd{&mcu};
  fk::LinkClient client{&mcu};
  std::vector<uint8_t> old_image = make_image(12 * 1024, 1, 1);
  std::vector<uint8_t> image;
  std::vector<int> saves;
  uint32_t t = 1000;

  explicit Rig(uint32_t size = 10 * 1024 + 104, uint32_t optr = CONFIGURED_OPTR) {
    mcu.fill_saved_pages();
    mcu.program_app(old_image);
    mcu.set_optr(optr);
    mcu.power_on();
    image = make_image(size, 2, 2);
    upd.set_image(image.data(), uint32_t(image.size()), image.data() + 0x100);
    upd.on_save([this](bool p) { saves.push_back(p ? 1 : 0); });
    client.set_mcu_updater(&upd);
    run(1000);
  }
  void run(uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) {
      t++;
      mcu.tick(t);
      client.loop(t);
    }
  }
  // Runs until the updater stops (at most ms); true when it did.
  bool finish(uint32_t ms = 120000) {
    for (uint32_t i = 0; i < ms; i++) {
      if (!upd.running())
        return true;
      run(1);
    }
    return !upd.running();
  }
  bool flash_matches() const {
    const uint32_t end = uint32_t((image.size() + 2047) / 2048 * 2048);
    for (uint32_t i = 0; i < end; i++)
      if (mcu.flash[i] != (i < image.size() ? image[i] : 0xFF))
        return false;
    return true;
  }
  fk::HeaterStatus st() {
    fk::HeaterStatus s;
    client.read_status(s);
    return s;
  }
  bool runs(uint8_t minor) const { return client.up() && client.info().version[1] == minor; }
  std::string text() const { return client.firmware_update_text(); }
};

static bool contains(const std::string &s, const char *what) { return s.find(what) != std::string::npos; }

// ---------------------------------------------------------------- the AN3155 commands

static void test_commands() {
  SimStm32 m;
  m.fill_saved_pages();
  m.set_optr(CONFIGURED_OPTR);
  m.power_on();
  Boot b(&m);
  uint32_t t = 0;
  auto wait = [&]() {
    Boot::Status s;
    for (int i = 0; i < 5000; i++) {
      t++;
      m.tick(t);
      if ((s = b.poll(t)) != Boot::Status::BUSY)
        return s;
    }
    return Boot::Status::BUSY;
  };
  CHECK(m.mode == SimStm32::BOOTLOADER);  // blank: EMPTY
  m.bootloader_framing(true);
  b.sync();
  CHECK(wait() == Boot::Status::OK);
  b.get();
  CHECK(wait() == Boot::Status::OK);
  CHECK(b.reply_len() >= 5);
  CHECK(std::memchr(b.reply() + 1, 0x44, b.reply_len() - 1) != nullptr);
  b.get_id();
  CHECK(wait() == Boot::Status::OK);
  CHECK_EQ(b.reply_len(), 2);
  CHECK_EQ(b.reply()[0] << 8 | b.reply()[1], 0x466);

  // Write, read back, erase.
  uint8_t block[256];
  for (int i = 0; i < 256; i++)
    block[i] = uint8_t(i * 7);
  CHECK(b.write(0x08000100, block, 256));
  CHECK(wait() == Boot::Status::OK);
  b.read(0x08000100, 256);
  CHECK(wait() == Boot::Status::OK);
  CHECK_EQ(b.reply_len(), 256);
  CHECK(std::memcmp(b.reply(), block, 256) == 0);
  CHECK(b.write(0x08000100, block, 256));  // not erased: the bootloader refuses
  CHECK(wait() == Boot::Status::NACK);
  uint16_t page0 = 0;
  CHECK(b.erase(&page0, 1));
  CHECK(wait() == Boot::Status::OK);
  CHECK_EQ(m.flash[0x100], 0xFF);
  // The last block below page 30 is the application's.
  CHECK(b.write(0x0800EF00, block, 256));
  CHECK(wait() == Boot::Status::OK);
  b.read(0x1FFF7800, 8);
  CHECK(wait() == Boot::Status::OK);
  CHECK_EQ(fk::get_u32(b.reply()), CONFIGURED_OPTR);
  b.read(0x40000000, 4);  // not memory
  CHECK(wait() == Boot::Status::NACK);

  // Refused before a byte goes out: pages 30 and 31, beyond the application, odd sizes.
  const int sent = m.in_count;
  uint16_t saved[2] = {30, 31};
  CHECK(!b.erase(saved, 1));
  CHECK(!b.erase(saved + 1, 1));
  uint16_t mixed[2] = {5, 30};
  CHECK(!b.erase(mixed, 2));
  CHECK(!b.erase(mixed, 0));
  CHECK(!b.write(0x0800F000, block, 8));
  CHECK(!b.write(0x0800EF08, block, 256));  // runs into page 30
  CHECK(!b.write(0x0800FFF8, block, 8));
  CHECK(!b.write(0x07FFFF00, block, 256));
  CHECK(!b.write(0x08000004, block, 8));    // not on a double word
  CHECK(!b.write(0x08000000, block, 12));
  CHECK(!b.write(0x08000000, block, 0));
  CHECK(!b.write(0x08000000, block, 264));
  CHECK(!b.write(0x1FFF7800, block, 8));    // the option bytes have their own call
  CHECK(!b.write_options(block, 8));         // ...for the whole area only
  CHECK_EQ(m.in_count, sent);
  CHECK(!b.busy());
  CHECK(m.saved_pages_intact());

  // A mangled reply: a stray byte where an ACK belongs.
  m.out.push_back({0x55, true});
  b.get_id();
  CHECK(wait() == Boot::Status::OK);  // stale bytes are dropped before a command
  b.go(0x08000000);
  CHECK(wait() == Boot::Status::OK);
  CHECK_EQ(m.gos, 1);
}

// ---------------------------------------------------------------- a whole update

static void test_update() {
  Rig r;
  CHECK(r.runs(1));
  CHECK(contains(r.text(), "0.2.0"));  // offered
  CHECK(r.client.capabilities() & fk::cap::FIRMWARE_UPDATE);
  CHECK(r.client.start_firmware_update().accepted());
  CHECK(r.st().update_running);
  CHECK(r.st().control == fk::ControlStatus::UPDATING);
  CHECK(r.client.start_firmware_update().result == Result::BUSY);  // one at a time
  r.run(300);
  CHECK(r.st().update_running);
  CHECK(r.st().state == fk::HeaterState::UPDATING);
  CHECK(r.finish());
  CHECK(r.upd.done());
  CHECK(r.flash_matches());
  CHECK(r.mcu.saved_pages_intact());
  CHECK(!r.mcu.saved_touched);
  CHECK(!r.mcu.mass_erased);
  CHECK_EQ(r.mcu.prepares, 1);
  CHECK_EQ(r.mcu.gos, 1);
  CHECK(r.runs(2));
  CHECK(!r.st().update_running);
  CHECK(contains(r.text(), "0.2.0"));
  r.run(300);
  CHECK(r.st().answering);
  // Page 0 erased first and written last, after every other page.
  CHECK(!r.mcu.erase_log.empty() && r.mcu.erase_log[0] == 0);
  CHECK_EQ(r.mcu.erase_log.size(), 6);
  CHECK(!r.mcu.write_log.empty() && r.mcu.write_log.back() < 0x08000800);
  CHECK(r.mcu.write_log[0] >= 0x08000800);
  // The unfinished-update flag is set before the erase and cleared once HELLO confirmed it.
  CHECK(r.saves.size() == 2 && r.saves[0] == 1 && r.saves[1] == 0);
  CHECK(!r.upd.pending());
  CHECK(r.upd.optr_known());
  CHECK_EQ(r.upd.optr(), CONFIGURED_OPTR);
  CHECK(!r.mcu.reset_low);  // the lines released
  CHECK(!r.mcu.boot_high);
  CHECK(!r.mcu.host_8e1);
  // The MCU now refuses nothing: no update agreement left over.
  r.run(6000);
  CHECK(r.client.start_firmware_update().accepted());  // again: asks the MCU again
  CHECK(r.finish());
  CHECK_EQ(r.mcu.prepares, 2);
}

// Exactly 60 KB: every application page, and not one byte more.
static void test_whole_application() {
  Rig r(60 * 1024);
  CHECK(r.client.start_firmware_update().accepted());
  CHECK(r.finish());
  CHECK(r.upd.done());
  CHECK(r.flash_matches());
  CHECK_EQ(r.mcu.erase_log.size(), 30);
  CHECK(r.mcu.saved_pages_intact());
  CHECK(!r.mcu.saved_touched);
}

static void test_refusals() {
  {
    Rig r(60 * 1024 + 8);  // into page 30
    CHECK(r.client.start_firmware_update().result == Result::TOO_BIG);
    CHECK_EQ(r.mcu.prepares, 0);
  }
  {
    Rig r;
    r.image[4] = 0;  // no reset handler
    CHECK(r.client.start_firmware_update().result == Result::BAD_IMAGE);
  }
  {
    Rig r;
    r.mcu.app_state = 6;  // cooling down
    r.run(300);
    CHECK(r.client.start_firmware_update().result == Result::NOT_OFF);
    CHECK_EQ(r.mcu.prepares, 0);
  }
  {
    // Off, but the MCU refuses: still hot.
    Rig r;
    r.mcu.app_hot = true;
    CHECK(r.client.start_firmware_update().accepted());
    CHECK(r.finish(5000));
    CHECK(r.upd.failed());
    CHECK(contains(r.text(), "off and cold"));
    CHECK_EQ(r.mcu.prepares, 1);
    CHECK_EQ(r.mcu.boot_drives + r.mcu.reset_drives, 0);
    CHECK(r.runs(1));
    CHECK(!r.st().update_running);
  }
  {
    // A silent MCU this board never had agree: not touched.
    Rig r;
    r.mcu.power_off();
    r.run(10000);
    CHECK(r.client.start_firmware_update().result == Result::NO_REPLY);
    CHECK_EQ(r.mcu.boot_drives + r.mcu.reset_drives, 0);
  }
  {
    // No updater, or one without an image.
    SimStm32 m;
    fk::LinkClient c(&m);
    CHECK(c.start_firmware_update().result == Result::NOT_SUPPORTED);
    CHECK(!(c.capabilities() & fk::cap::FIRMWARE_UPDATE));
    fk::McuUpdate u(&m);
    c.set_mcu_updater(&u);
    CHECK(c.start_firmware_update().result == Result::NOT_SUPPORTED);
  }
  {
    // UPDATE_PREPARE unanswered (the MCU goes quiet just then).
    Rig r;
    CHECK(r.client.start_firmware_update().accepted());
    r.mcu.power_off();
    CHECK(r.finish(10000));
    CHECK(r.upd.failed());
    CHECK_EQ(r.mcu.boot_drives + r.mcu.reset_drives, 0);
  }
}

// Not a G030: nothing erased, and the old firmware back.
static void test_wrong_chip() {
  Rig r;
  r.mcu.pid = 0x460;
  CHECK(r.client.start_firmware_update().accepted());
  CHECK(r.finish());
  CHECK(r.upd.failed());
  CHECK(contains(r.text(), "0x460"));
  CHECK_EQ(r.mcu.erases + r.mcu.writes, 0);
  CHECK(r.saves.empty());
  r.run(1000);
  CHECK(r.runs(1));
  CHECK(!r.st().update_running);
}

static void test_verify() {
  {
    // One block reads back wrong: its page is erased and written again.
    Rig r;
    r.mcu.corrupt_write_at = 3;
    CHECK(r.client.start_firmware_update().accepted());
    CHECK(r.finish());
    CHECK(r.upd.done());
    CHECK(r.flash_matches());
    CHECK_EQ(r.mcu.erase_log.size(), 7);
    CHECK(r.runs(2));
  }
  {
    // A page that never reads back right: given up, the heater left off and silent.
    Rig r;
    r.mcu.bad_page = 2;
    CHECK(r.client.start_firmware_update().accepted());
    CHECK(r.finish());
    CHECK(r.upd.failed());
    CHECK(contains(r.text(), "0x08001"));
    CHECK(r.upd.pending());
    CHECK(!r.st().update_running);
    CHECK(!r.st().answering);
    CHECK(r.mcu.mode != SimStm32::APP);
    CHECK(!r.mcu.reset_low);
    CHECK(!r.mcu.boot_high);
    CHECK(!r.mcu.host_8e1);
    CHECK(r.mcu.saved_pages_intact());
    // Not again at once: the MCU must have been silent 5 s.
    CHECK(r.client.start_firmware_update().result == Result::NO_REPLY);
    // Fixed (a flaky contact, say): this board's own unfinished update is taken up again.
    r.mcu.bad_page = -1;
    r.run(6000);
    const int prepares = r.mcu.prepares;
    CHECK(r.client.start_firmware_update().accepted());
    CHECK(r.finish());
    CHECK(r.upd.done());
    CHECK_EQ(r.mcu.prepares, prepares);  // nothing to ask: no firmware runs
    CHECK(r.flash_matches());
    CHECK(r.runs(2));
    CHECK(!r.upd.pending());
  }
}

static void test_power_loss() {
  {
    // The MCU loses power mid-write and comes back at once: the update carries on.
    Rig r;
    r.mcu.power_loss_write_at = 9;
    CHECK(r.client.start_firmware_update().accepted());
    for (int i = 0; i < 20000 && r.mcu.powered; i++)
      r.run(1);
    CHECK(!r.mcu.powered);
    r.run(50);
    r.mcu.power_on();  // page 0 is erased: EMPTY, so the bootloader runs
    CHECK(r.mcu.mode == SimStm32::BOOTLOADER);
    CHECK(r.finish());
    CHECK(r.upd.done());
    CHECK(r.flash_matches());
    CHECK(r.runs(2));
  }
  {
    // The whole board loses power mid-write: the ESP32 starts again with the update unfinished.
    Rig r;
    r.mcu.power_loss_write_at = 20;
    CHECK(r.client.start_firmware_update().accepted());
    for (int i = 0; i < 20000 && r.mcu.powered; i++)
      r.run(1);
    CHECK(!r.mcu.powered);
    CHECK(r.saves.size() == 1 && r.saves[0] == 1);
    // A new ESP32 start: the saved flag, a new link client.
    fk::McuUpdate upd(&r.mcu);
    upd.set_image(r.image.data(), uint32_t(r.image.size()), r.image.data() + 0x100);
    upd.load(true);
    fk::LinkClient client(&r.mcu);
    client.set_mcu_updater(&upd);
    r.mcu.drive_reset(false);
    r.mcu.drive_boot(false);
    r.mcu.bootloader_framing(false);
    r.mcu.power_on();
    CHECK(r.mcu.mode == SimStm32::BOOTLOADER);  // the half-written image cannot run
    uint32_t t = r.t + 100000;
    auto run = [&](uint32_t ms) {
      for (uint32_t i = 0; i < ms; i++) {
        t++;
        r.mcu.tick(t);
        client.loop(t);
      }
    };
    run(1000);
    CHECK(!client.up());
    CHECK(std::string(client.firmware_update_text()).find("unfinished") != std::string::npos);
    CHECK(client.start_firmware_update().result == Result::NO_REPLY);  // not silent 5 s yet
    run(5000);
    CHECK(client.start_firmware_update().accepted());
    for (int i = 0; i < 120000 && upd.running(); i++)
      run(1);
    CHECK(upd.done());
    CHECK(r.flash_matches());
    CHECK(client.up() && client.info().version[1] == 2);
    CHECK(!upd.pending());
    CHECK(r.mcu.saved_pages_intact());
  }
}

// Lost, garbled and refused bytes on the way: the update still completes.
static void test_link_faults() {
  struct Case {
    const char *name;
    void (*set)(SimStm32 &);
  };
  const Case cases[] = {
      {"first sync byte lost", [](SimStm32 &m) { m.drop_in_at = 1; }},
      {"byte to the MCU lost", [](SimStm32 &m) { m.drop_in_at = 60; }},
      {"byte from the MCU lost", [](SimStm32 &m) { m.drop_out_at = 80; }},
      {"write NACKed", [](SimStm32 &m) { m.nack_write_at = 4; }},
      {"bootloader hangs", [](SimStm32 &m) { m.hang_at_command = 9; }},
      {"go NACKed", [](SimStm32 &m) { m.go_nack = true; }},
  };
  for (const Case &c : cases) {
    Rig r;
    c.set(r.mcu);
    CHECK(r.client.start_firmware_update().accepted());
    CHECK(r.finish());
    if (!r.upd.done())
      std::printf("  %s: %s\n", c.name, r.text().c_str());
    CHECK(r.upd.done());
    CHECK(r.flash_matches());
    CHECK(r.runs(2));
    CHECK(r.mcu.saved_pages_intact());
  }
  {
    // Go refused and the EMPTY flag stale from a power-up with page 0 erased: a reset pulse brings
    // the bootloader back, not the new firmware. Failed, with the way out in the text.
    Rig r;
    r.mcu.go_nack = true;
    r.mcu.empty = true;
    CHECK(r.client.start_firmware_update().accepted());
    CHECK(r.finish());
    CHECK(r.upd.failed());
    CHECK(contains(r.text(), "off and on"));
    CHECK(r.flash_matches());
    r.mcu.power_off();
    r.mcu.power_on();
    r.run(1000);
    CHECK(r.runs(2));
    CHECK(!r.upd.pending());
  }
}

// The image's version and what HELLO reports disagree.
static void test_version_mismatch() {
  Rig r;
  r.image[0x101] = 3;  // the image says 0.3.0, the build declared 0.2.0
  uint8_t declared[3] = {0, 2, 0};
  r.upd.set_image(r.image.data(), uint32_t(r.image.size()), declared);
  CHECK(r.client.start_firmware_update().accepted());
  CHECK(r.finish());
  CHECK(r.upd.failed());
  CHECK(contains(r.text(), "0.3.0"));
  CHECK(!r.upd.pending());  // a firmware runs
}

static void test_option_bytes() {
  {
    Rig r;
    CHECK(r.client.start_mcu_job(McuJob::READ_OPTIONS).accepted());
    CHECK(r.finish());
    CHECK(r.upd.done());
    CHECK_EQ(r.upd.optr(), CONFIGURED_OPTR);
    CHECK(contains(r.text(), "DEFEE1AA"));
    CHECK(contains(r.text(), "as required"));
    CHECK_EQ(r.mcu.erases + r.mcu.writes + r.mcu.option_writes, 0);
    CHECK(r.runs(1));
  }
  {
    // The watchdog in software: written, the chip resets, read back.
    Rig r(10 * 1024, CONFIGURED_OPTR | fk::g030::OPTR_IWDG_SW);
    CHECK(r.client.start_mcu_job(McuJob::WRITE_OPTIONS).accepted());
    CHECK(r.finish());
    if (!r.upd.done())
      std::printf("  options: %s\n", r.text().c_str());
    CHECK(r.upd.done());
    CHECK_EQ(r.mcu.option_writes, 1);
    CHECK_EQ(r.mcu.optr, CONFIGURED_OPTR);
    CHECK(!r.mcu.optverr);
    CHECK_EQ(fk::get_u32(r.mcu.opt + 4), ~CONFIGURED_OPTR);
    CHECK_EQ(fk::get_u32(r.mcu.opt + 8), 0x000000FFu);  // the other words kept
    CHECK(contains(r.text(), "DEFEE1AA"));
    CHECK_EQ(r.mcu.erases + r.mcu.writes, 0);
    CHECK(r.runs(1));
    CHECK(r.mcu.saved_pages_intact());
  }
  {
    // Already right: nothing written.
    Rig r;
    CHECK(r.client.start_mcu_job(McuJob::WRITE_OPTIONS).accepted());
    CHECK(r.finish());
    CHECK(r.upd.done());
    CHECK_EQ(r.mcu.option_writes, 0);
    CHECK(r.runs(1));
  }
  {
    // ACKed but not taken: the read-back says so.
    Rig r(10 * 1024, CONFIGURED_OPTR | fk::g030::OPTR_IWDG_SW);
    r.mcu.option_write_ignored = true;
    CHECK(r.client.start_mcu_job(McuJob::WRITE_OPTIONS).accepted());
    CHECK(r.finish());
    CHECK(r.upd.failed());
    CHECK(contains(r.text(), "read back"));
    r.run(1000);
    CHECK(r.runs(1));
  }
  {
    // Read-out protection: the bootloader refuses reads; nothing is touched.
    Rig r(10 * 1024, (CONFIGURED_OPTR & ~0xFFu) | 0xBB);
    CHECK(r.client.start_mcu_job(McuJob::WRITE_OPTIONS).accepted());
    CHECK(r.finish());
    CHECK(r.upd.failed());
    CHECK(contains(r.text(), "protect"));
    CHECK_EQ(r.mcu.option_writes, 0);
    r.run(1000);
    CHECK(r.runs(1));
  }
  {
    // Factory option bytes (nBOOT_SEL = 1): the BOOT0 pin is ignored and the bootloader cannot be
    // reached while a firmware runs. Failed; the old firmware back.
    Rig r(10 * 1024, SimStm32::FACTORY_OPTR);
    CHECK(r.client.start_firmware_update().accepted());
    CHECK(r.finish());
    CHECK(r.upd.failed());
    CHECK(contains(r.text(), "nBOOT_SEL"));
    CHECK_EQ(r.mcu.erases + r.mcu.writes, 0);
    r.run(1000);
    CHECK(r.runs(1));
  }
}

static void test_controller() {
  Rig r;
  fk::Controller c(&r.client);
  c.tick(r.t, true);
  c.set_board_updating(true);
  CHECK(c.start_firmware_update(fk::Source::HOME_ASSISTANT).result == Result::BUSY);
  c.set_board_updating(false);
  CHECK(c.start_firmware_update(fk::Source::HOME_ASSISTANT).accepted());
  r.run(200);
  c.tick(r.t, true);
  CHECK(c.set_mode(fk::Mode::HEAT, fk::Source::PHONE).result == Result::BUSY);
  CHECK(r.finish());
  r.run(300);
  c.tick(r.t, true);
  CHECK(r.upd.done());
  CHECK(c.set_mode(fk::Mode::HEAT, fk::Source::PHONE).accepted());
}

int main() {
  test_commands();
  test_update();
  test_whole_application();
  test_refusals();
  test_wrong_chip();
  test_verify();
  test_power_loss();
  test_link_faults();
  test_version_mismatch();
  test_option_bytes();
  test_controller();
  return report("mcu_update");
}
