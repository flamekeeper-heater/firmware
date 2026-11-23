// The board's automation (automation.h): fuel meters and tank, frost protection, timers, automatic
// starts disarmed on doubt, alerts, the clock, Bluetooth thermometers as a room source, and the 1.3
// protocol commands that reach them.
#include "fake_heater.h"

#include "../automation.h"
#include "../ble_protocol.h"

using fk::Mode;
using fk::Result;
using fk::Source;
using fk::HeaterState;
namespace al = fk::alert;
namespace ble = fk::ble;

static const uint32_t SEC = 1000;
static const uint32_t MIN = 60 * SEC;
// 2026-01-12 (a Monday) 05:00:00 UTC = 06:00 in Berlin (CET, UTC+1).
static const uint32_t MON_0600_CET = 1768194000u;
static const char *const BERLIN = "CET-1CEST,M3.5.0,M10.5.0/3";

struct Rig {
  FakeHeater h;
  fk::Controller c{&h};
  fk::Automation a{c};
  uint32_t now = 1000;
  float probe = NAN;
  std::vector<std::pair<int, std::string>> alerts;  // event 08: bit, text
  int saves = 0, meter_saves = 0;
  fk::Automation::Saved last_saved{};

  Rig() {
    c.on_event([this](fk::Event e, Result, uint16_t id, const char *text) {
      if (e == fk::Event::ALERT)
        alerts.push_back({int(id), text});
    });
    a.on_save([this](const fk::Automation::Saved &s) {
      saves++;
      last_saved = s;
    });
    a.on_meter_save([this](const fk::FuelMeter::Saved &) { meter_saves++; });
    c.room().set_source(fk::RoomSource::PROBE);
    c.set_bluetooth_room(true);
    step(0);
  }
  void step(uint32_t ms) {
    uint32_t end = now + ms;
    do {
      if (!std::isnan(probe))
        c.room().probe(probe, now);
      c.tick(now, false);
      a.tick(now);
      now += 500;
    } while (int32_t(end - now) > 0);
  }
  void set(uint16_t id, int32_t v) { CHECK(c.param_set(id, v, Source::PHONE).accepted()); }
  bool alerted(int bit) const {
    for (auto &x : alerts)
      if (x.first == bit)
        return true;
    return false;
  }
  // The heater does what it was asked (the fake only records the mode).
  void heater_follows() {
    if (h.st.mode == Mode::HEAT && h.st.state == HeaterState::OFF)
      h.st.state = HeaterState::HEATING;
    if (h.st.mode == Mode::FAN && h.st.state == HeaterState::OFF)
      h.st.state = HeaterState::FAN_ONLY;
    if (h.st.mode == Mode::OFF && h.st.state != HeaterState::OFF)
      h.st.state = HeaterState::OFF;
  }
};

static void test_fuel_meter() {
  fk::FuelMeter m;
  // No clock: the run, lifetime and tank count; the periods are unknown.
  m.add(1.0f, 2.0f, 0.022f, true, true, 0, 0);
  fk::FuelMeter::Totals t = m.totals(0, 0);
  CHECK_EQ(t.day_fuel, fk::FuelMeter::UNKNOWN);
  CHECK_EQ(t.run_burn, 1);
  CHECK_EQ(t.run_glow, 1);
  // An hour at 2 Hz with a 22 mL / 1000 pump: 158.4 mL.
  m.new_run();
  const uint32_t utc = 1768194000u;
  const int32_t day = int32_t(utc / 86400);
  for (int i = 0; i < 3600; i++)
    m.add(1.0f, 2.0f, 0.022f, true, false, utc + i, day);
  t = m.totals(utc + 3600, day);
  CHECK_EQ(t.run_fuel, 158);
  CHECK_EQ(t.run_burn, 3600);
  CHECK_EQ(t.run_glow, 0);
  CHECK_EQ(t.day_fuel, 158);
  CHECK_EQ(t.day_burn, 3600);
  CHECK_EQ(t.week_fuel, 158);
  CHECK_EQ(t.month_fuel, 158);
  CHECK_EQ(t.life_fuel, 158);  // 158.4 + 0.044
  CHECK_EQ(t.life_burn, 3601);
  // A day later the 24 hours are empty, the week and month keep it; 8 days later only the month.
  t = m.totals(utc + 86400 + 3600, day + 1);
  CHECK_EQ(t.day_fuel, 0);
  CHECK_EQ(t.week_fuel, 158);
  t = m.totals(utc + 8 * 86400, day + 8);
  CHECK_EQ(t.week_fuel, 0);
  CHECK_EQ(t.month_fuel, 158);
  t = m.totals(utc + 31 * 86400, day + 31);
  CHECK_EQ(t.month_fuel, 0);
  CHECK_EQ(t.life_fuel, 158);
  // A stall (60 s or more between calls) is not fuel; neither is a silly pump rate.
  m.add(120.0f, 2.0f, 0.022f, true, false, 0, 0);
  m.add(1.0f, 500.0f, 0.022f, true, false, 0, 0);
  CHECK_EQ(m.totals(0, 0).life_fuel, 158);
  // Kept over a restart.
  fk::FuelMeter m2;
  CHECK(m2.load(m.saved()));
  CHECK_EQ(m2.totals(utc + 3600, day).day_fuel, 158);
  fk::FuelMeter::Saved bad = m.saved();
  bad.check = 0;
  CHECK(!m2.load(bad));
}

static void test_tank_and_low_fuel() {
  Rig r;
  // No tank size: no estimate, TANK_SET refused.
  CHECK_EQ(r.c.status().fuel_left, 0xFF);
  CHECK(r.a.tank_set(100, Source::PHONE).result == Result::REFUSED);
  // Defaults: alert at 20 %, stop at 10 %.
  CHECK_EQ(r.c.tuning().extra(fk::F_LOW_ALERT), 20);
  CHECK_EQ(r.c.tuning().extra(fk::F_STOP_AT), 10);
  r.set(fk::F_TANK, 10);       // 1.0 L
  r.set(fk::F_LOW_ALERT, 15);
  r.set(fk::F_STOP_AT, 5);
  CHECK(r.a.tank_set(100, Source::PHONE).accepted());
  CHECK(r.a.tank_set(101, Source::PHONE).result == Result::OUT_OF_RANGE);
  r.step(2 * SEC);
  CHECK_EQ(r.c.status().fuel_left, 100);
  // Burn at 5 Hz (0.11 mL/s): 85 % used after about 2.1 h; run it down to the alert.
  CHECK(r.c.set_mode(Mode::HEAT, Source::PHONE).accepted());
  r.heater_follows();
  r.h.st.pump_hz = 5.0f;
  r.step(7800 * SEC);  // 858 mL
  CHECK(r.c.status().fuel_left <= 15);
  CHECK(r.c.status().alerts & al::FUEL_LOW);
  CHECK(r.alerted(0));
  CHECK(!(r.c.status().alerts & al::FUEL_STOP));
  CHECK(r.c.status().mode == Mode::HEAT);
  // Down to 5 %: stopped, and starts refused for every source.
  r.step(1000 * SEC);
  CHECK(r.c.status().fuel_left <= 5);
  CHECK(r.c.status().alerts & al::FUEL_STOP);
  CHECK(r.c.status().mode == Mode::OFF);
  r.heater_follows();
  r.h.st.pump_hz = 0.0f;
  r.step(2 * SEC);
  CHECK(!(r.c.status().alerts & al::BURN));  // asked to stop: not "stopped by itself"
  CHECK(r.c.set_mode(Mode::HEAT, Source::HOME_ASSISTANT).result == Result::REFUSED);
  CHECK(r.c.set_mode(Mode::FAN, Source::PHONE).result == Result::REFUSED);
  // Filled up: the alerts clear, starts work.
  CHECK(r.a.tank_set(100, Source::PHONE).accepted());
  r.step(2 * SEC);
  CHECK_EQ(r.c.status().alerts & (al::FUEL_LOW | al::FUEL_STOP), 0);
  CHECK(r.c.set_mode(Mode::HEAT, Source::PHONE).accepted());
  // Half full.
  CHECK(r.a.tank_set(50, Source::PHONE).accepted());
  r.step(2 * SEC);
  CHECK(r.c.status().fuel_left >= 49 && r.c.status().fuel_left <= 50);
}

// Priming is no burn: stopping it, or its time running out, raises no "burn problem".
static void test_priming_no_burn_alert() {
  {
    Rig r;
    r.h.st.answering = true;
    CHECK(r.c.set_mode(Mode::PRIME, Source::PHONE).accepted());
    r.h.st.state = HeaterState::PRIMING;
    r.step(5 * SEC);
    CHECK(r.c.stop_prime(Source::PHONE).accepted());
    r.h.st.state = HeaterState::OFF;
    r.step(3 * SEC);
    CHECK(r.c.status().run_source == Source::NONE);
    CHECK(!(r.c.status().alerts & al::BURN));
  }
  {
    Rig r;
    r.h.st.answering = true;
    CHECK(r.c.set_mode(Mode::PRIME, Source::PHONE).accepted());
    r.h.st.state = HeaterState::PRIMING;
    r.step(5 * SEC);
    r.h.st.mode = Mode::OFF;  // done: the backend ends it
    r.h.st.state = HeaterState::OFF;
    r.step(3 * SEC);
    CHECK(r.c.status().run_source == Source::NONE);
    CHECK(!(r.c.status().alerts & al::BURN));
  }
  {
    Rig r;  // ...but a fault while priming still is one
    r.h.st.answering = true;
    CHECK(r.c.set_mode(Mode::PRIME, Source::PHONE).accepted());
    r.step(5 * SEC);
    r.h.st.fault = fk::Fault::PUMP;
    r.h.st.mode = Mode::OFF;
    r.h.st.state = HeaterState::OFF;
    r.step(3 * SEC);
    CHECK(r.c.status().alerts & al::BURN);
  }
}

static void test_frost() {
  Rig r;
  r.probe = 12.0f;
  r.c.set_heat_mode(fk::HeatMode::POWER, Source::PHONE);
  r.c.set_target(21.0f, Source::PHONE);
  r.c.set_power(60, Source::PHONE);
  r.set(fk::FR_ENABLED, 1);
  r.set(fk::FR_TEMP, 50);        // 5 C
  r.set(fk::FR_STOP_ABOVE, 30);  // stop at 8 C
  r.step(5 * SEC);
  CHECK(r.c.status().mode == Mode::OFF);
  CHECK(r.c.status().automatic & fk::automatic::FROST_ON);
  // Cold: frost protection starts a temperature run, its target 8 C.
  r.probe = 4.8f;
  r.step(3 * SEC);
  CHECK(r.c.status().mode == Mode::HEAT);
  CHECK(r.c.status().run_source == Source::FROST);
  CHECK(r.c.status().heat_mode == fk::HeatMode::TEMPERATURE);
  r.heater_follows();
  r.step(60 * SEC);
  // Not warm enough yet: still on. Warm: off, and the user's settings are back.
  r.probe = 7.9f;
  r.step(5 * SEC);
  CHECK(r.c.status().mode == Mode::HEAT);
  r.probe = 8.0f;
  r.step(3 * SEC);
  CHECK(r.c.status().mode == Mode::OFF);
  r.heater_follows();
  r.step(3 * SEC);
  CHECK(r.c.status().heat_mode == fk::HeatMode::POWER);
  CHECK(r.c.status().target_c == 21.0f);
  CHECK_EQ(r.c.status().power, 60);
  CHECK(!(r.c.status().alerts & al::BURN));
  CHECK(!r.a.disarmed());
  // Cold again: a fresh start. A person's off turns frost protection off.
  r.probe = 3.0f;
  r.step(3 * SEC);
  CHECK(r.c.status().run_source == Source::FROST);
  r.heater_follows();
  r.step(3 * SEC);
  CHECK(r.c.set_mode(Mode::OFF, Source::PHONE).accepted());
  r.heater_follows();
  r.step(3 * SEC);
  CHECK_EQ(r.c.tuning().extra(fk::FR_ENABLED), 0);
  CHECK(r.c.status().mode == Mode::OFF);
  r.step(60 * SEC);
  CHECK(r.c.status().mode == Mode::OFF);
  // No room temperature: no start, an alert.
  r.probe = NAN;
  r.step(6 * MIN);  // the last probe reading goes stale
  r.set(fk::FR_ENABLED, 1);
  r.step(3 * SEC);
  CHECK(r.c.status().mode == Mode::OFF);
  CHECK(r.c.status().alerts & al::FROST_NO_TEMP);
}

// A board firmware update refuses every start: frost protection waits for it, armed.
static void test_frost_waits_for_board_update() {
  Rig r;
  r.probe = 2.0f;
  r.c.set_board_updating(true);
  r.set(fk::FR_ENABLED, 1);
  r.step(30 * SEC);
  CHECK(!r.a.disarmed());
  CHECK(r.c.status().mode == Mode::OFF);
  r.c.set_board_updating(false);
  r.step(3 * SEC);
  CHECK(r.c.status().mode == Mode::HEAT);
  CHECK(r.c.status().run_source == Source::FROST);
}

static void test_disarm_on_doubt() {
  Rig r;
  r.probe = 2.0f;
  r.h.st.answering = false;  // the heater does not answer: the automatic start is refused
  r.set(fk::FR_ENABLED, 1);
  r.step(3 * SEC);
  CHECK(r.a.disarmed());
  CHECK(r.c.status().alerts & al::AUTO_OFF);
  CHECK(r.c.status().automatic & fk::automatic::DISARMED);
  CHECK(r.alerted(3));
  for (const auto &a : r.alerts)  // the reason whole, brackets closed
    if (a.first == 3)
      CHECK(a.second.size() <= 124 && a.second.back() == ')' &&
            a.second.rfind("automatic starts are off (", 0) == 0);
  CHECK(r.last_saved.disarmed == 1);  // kept over a restart
  r.h.st.answering = true;
  r.step(30 * SEC);
  CHECK(r.c.status().mode == Mode::OFF);  // never retried by itself
  CHECK(r.a.rearm(Source::PHONE).accepted());
  CHECK(!(r.c.status().alerts & al::AUTO_OFF));
  r.step(3 * SEC);
  CHECK(r.c.status().run_source == Source::FROST);
  // The heater stops by itself during the frost run (a flame-out): disarmed, a burn alert.
  r.heater_follows();
  r.step(3 * SEC);
  r.h.st.fault = fk::Fault::FLAME_OUT;
  r.h.st.mode = Mode::OFF;  // the backend's own: the run is over
  r.h.st.state = HeaterState::OFF;
  r.step(3 * SEC);
  CHECK(r.a.disarmed());
  CHECK(r.c.status().alerts & al::BURN);
  CHECK(r.c.status().mode == Mode::OFF);
  r.h.st.fault = fk::Fault::NONE;
  r.step(30 * SEC);
  CHECK(r.c.status().mode == Mode::OFF);
  // A cooldown is not a failure: the start waits.
  CHECK(r.a.rearm(Source::PHONE).accepted());
  CHECK(r.a.clear_alerts(al::BURN | al::AUTO_OFF).accepted());
  CHECK(!(r.c.status().alerts & al::BURN));
  r.h.st.state = HeaterState::COOLING;
  r.step(10 * SEC);
  CHECK(!r.a.disarmed());
  CHECK(r.c.status().mode == Mode::OFF);
  r.h.st.state = HeaterState::OFF;
  r.step(3 * SEC);
  CHECK(r.c.status().run_source == Source::FROST);
}

static void test_restored_after_restart() {
  Rig r;
  r.set(fk::FR_ENABLED, 1);
  r.h.st.answering = false;
  r.probe = 2.0f;
  r.step(3 * SEC);
  CHECK(r.a.disarmed());
  fk::Automation::Saved saved = r.last_saved;
  Rig q;
  q.a.load(saved);
  CHECK(q.a.disarmed());
  q.step(2 * SEC);
  CHECK(q.c.status().alerts & al::AUTO_OFF);
}

static void test_clock() {
  Rig r;
  CHECK_EQ(r.a.utc(r.now), 0);
  CHECK(r.a.set_clock(1700000000u, BERLIN, fk::ClockSource::APP, r.now).result == Result::OUT_OF_RANGE);
  CHECK(r.a.set_clock(MON_0600_CET, "1nvalid", fk::ClockSource::APP, r.now).result == Result::OUT_OF_RANGE);
  CHECK(r.a.set_clock(MON_0600_CET, "CET-1;rm", fk::ClockSource::APP, r.now).result == Result::OUT_OF_RANGE);
  CHECK(r.a.set_clock(MON_0600_CET, BERLIN, fk::ClockSource::APP, r.now).accepted());
  r.step(10 * SEC);
  CHECK_EQ(r.a.utc(r.now), MON_0600_CET + 10);
  CHECK(r.a.time_zone() == BERLIN);
  CHECK(r.c.status().automatic & fk::automatic::CLOCK_KNOWN);
  CHECK(std::string(r.last_saved.tz) == BERLIN);
}

// Apps set the clock on every connection, Home Assistant every 10 minutes: the log and the saved
// settings change only when the clock was unknown or wrong, or the zone changed.
static void test_clock_log_quiet() {
  Rig r;
  auto clock_logs = [&]() {
    fk::LogRecord recs[fk::EventLog::SIZE];
    size_t n = r.a.log().read(0, recs, fk::EventLog::SIZE);
    int k = 0;
    for (size_t i = 0; i < n; i++)
      k += recs[i].kind == uint8_t(fk::LogKind::CLOCK_SET);
    return k;
  };
  CHECK(r.a.set_clock(MON_0600_CET, BERLIN, fk::ClockSource::APP, r.now).accepted());
  CHECK_EQ(clock_logs(), 1);  // unknown before
  int saves = r.saves;
  r.step(10 * SEC);
  // Reconnects: the same time give or take a minute, the same zone.
  CHECK(r.a.set_clock(MON_0600_CET + 10, BERLIN, fk::ClockSource::APP, r.now).accepted());
  CHECK(r.a.set_clock(MON_0600_CET + 10 + 60, BERLIN, fk::ClockSource::APP, r.now).accepted());
  CHECK(r.a.set_clock(MON_0600_CET + 10, "", fk::ClockSource::APP, r.now).accepted());  // 60 s back
  CHECK_EQ(clock_logs(), 1);
  CHECK_EQ(r.saves, saves);
  CHECK(r.a.set_clock(MON_0600_CET + 40, "", fk::ClockSource::APP, r.now).accepted());
  CHECK_EQ(r.a.utc(r.now), MON_0600_CET + 40);  // still taken
  // Home Assistant sets it: CLOCK_GET says so, saved, but nothing logged.
  CHECK(r.a.set_clock(MON_0600_CET + 5, "", fk::ClockSource::HOME_ASSISTANT, r.now).accepted());
  CHECK(r.a.clock_source() == fk::ClockSource::HOME_ASSISTANT);
  CHECK_EQ(r.saves, saves + 1);
  CHECK_EQ(clock_logs(), 1);
  // Off by more than a minute: logged.
  CHECK(r.a.set_clock(MON_0600_CET + 5 + 61, "", fk::ClockSource::HOME_ASSISTANT, r.now).accepted());
  CHECK_EQ(clock_logs(), 2);
  // A new zone: logged and saved.
  saves = r.saves;
  CHECK(r.a.set_clock(MON_0600_CET + 66, "CST6CDT,M3.2.0,M11.1.0", fk::ClockSource::HOME_ASSISTANT, r.now).accepted());
  CHECK_EQ(clock_logs(), 3);
  CHECK_EQ(r.saves, saves + 1);
  CHECK(std::string(r.last_saved.tz) == "CST6CDT,M3.2.0,M11.1.0");

  // After a restart the system clock is put back quietly; the next app sets nothing new.
  Rig q;
  q.a.restore_clock(MON_0600_CET, q.now);
  CHECK_EQ(q.a.utc(q.now), MON_0600_CET);
  CHECK_EQ(q.saves, 0);
  fk::LogRecord recs[4];
  CHECK_EQ(q.a.log().read(0, recs, 4), 0);
  CHECK(q.a.set_clock(MON_0600_CET + 30, "", fk::ClockSource::APP, q.now).accepted());
  CHECK_EQ(q.a.log().read(0, recs, 4), 0);
}

// Zones whose abbreviations are not letters come as quoted POSIX names (<+0530>), as the apps send
// them. The board's newlib reads them too (checked in QEMU on the ESP32's own C library).
static void test_quoted_time_zones() {
  // A weekday timer at 06:30 local starts at 01:00 UTC in Kolkata and at 21:30 UTC the day before in
  // Tokyo.
  fk::TimerSpec t;
  t.flags = fk::TimerSpec::ENABLED;
  t.days = 0x7F;
  t.start_min = 6 * 60 + 30;
  t.duration_min = 30;
  t.mode = 2;
  t.power = 30;
  const uint32_t day = 1768176000u;  // 2026-01-12 00:00 UTC
  struct Z {
    const char *tz;
    uint32_t start_utc;
  };
  const Z zones[] = {{"<+0530>-5:30", day + 3600}, {"<+09>-9", day - 2 * 3600 - 30 * 60}};
  for (const Z &z : zones) {
    Rig r;
    CHECK(r.a.timer_set(0, t).accepted());
    CHECK(r.a.set_clock(z.start_utc - 60, z.tz, fk::ClockSource::APP, r.now).accepted());
    CHECK(r.a.time_zone() == z.tz);
    r.step(30 * SEC);
    CHECK(r.c.status().mode == Mode::OFF);
    r.step(60 * SEC);
    CHECK(r.c.status().run_source == Source::TIMER);
  }
}

static void test_timers() {
  Rig r;
  fk::TimerSpec t;
  t.flags = fk::TimerSpec::ENABLED;
  t.days = 0x01;          // Mondays
  t.start_min = 6 * 60 + 30;  // 06:30 local
  t.duration_min = 60;
  t.mode = 1;
  t.heat_mode = 1;
  t.target_x10 = 215;
  CHECK(r.a.timer_set(0, t).accepted());
  // Out of range fields.
  fk::TimerSpec bad = t;
  bad.duration_min = 0;
  CHECK(r.a.timer_set(1, bad).result == Result::OUT_OF_RANGE);
  bad = t;
  bad.days = 0;
  CHECK(r.a.timer_set(1, bad).result == Result::OUT_OF_RANGE);
  bad = t;
  bad.target_x10 = 400;
  CHECK(r.a.timer_set(1, bad).result == Result::OUT_OF_RANGE);
  CHECK(r.a.timer_set(8, t).result == Result::OUT_OF_RANGE);
  // Timers but no clock: an alert.
  r.step(2 * SEC);
  CHECK(r.c.status().alerts & al::NO_CLOCK);
  CHECK(r.c.status().automatic & fk::automatic::TIMER_ENABLED);
  // 06:00 Berlin: the clock is set; at 06:30 the timer starts, at 07:30 it stops.
  CHECK(r.a.set_clock(MON_0600_CET, BERLIN, fk::ClockSource::APP, r.now).accepted());
  r.step(2 * SEC);
  CHECK(!(r.c.status().alerts & al::NO_CLOCK));
  r.step(29 * MIN);
  CHECK(r.c.status().mode == Mode::OFF);
  r.step(1 * MIN);
  CHECK(r.c.status().mode == Mode::HEAT);
  CHECK(r.c.status().run_source == Source::TIMER);
  CHECK(r.c.status().target_c == 21.5f);
  CHECK_EQ(r.c.status().timer_slot, 0);  // the chip "Timer until 07:30"
  CHECK_EQ(r.c.status().timer_end_utc, MON_0600_CET + 90 * 60);
  r.heater_follows();
  r.step(58 * MIN);
  CHECK(r.c.status().mode == Mode::HEAT);
  r.step(2 * MIN);
  CHECK(r.c.status().mode == Mode::OFF);
  r.heater_follows();
  r.step(5 * SEC);
  CHECK(!(r.c.status().alerts & al::BURN));
  CHECK(r.c.status().target_c == 20.0f);  // the user's own again
  // Not again the same day.
  r.step(30 * MIN);
  CHECK(r.c.status().mode == Mode::OFF);

  // A restart in the middle of the window: not resumed.
  Rig q;
  CHECK(q.a.timer_set(0, t).accepted());
  q.now = 50 * MIN;  // the board has been up 50 minutes when the app sets the clock at 06:45
  CHECK(q.a.set_clock(MON_0600_CET + 45 * 60, BERLIN, fk::ClockSource::APP, q.now).accepted());
  q.step(5 * SEC);
  CHECK(q.c.status().mode == Mode::HEAT);  // the board was up at 06:30: it just did not know the time
  Rig w;
  CHECK(w.a.timer_set(0, t).accepted());
  w.now = 5 * MIN;  // up only 5 minutes at 06:45: it restarted inside the window
  CHECK(w.a.set_clock(MON_0600_CET + 45 * 60, BERLIN, fk::ClockSource::APP, w.now).accepted());
  w.step(5 * SEC);
  CHECK(w.c.status().mode == Mode::OFF);

  // Someone already running: the timer does nothing and stops nothing.
  Rig p;
  CHECK(p.a.timer_set(0, t).accepted());
  CHECK(p.a.set_clock(MON_0600_CET + 29 * 60, BERLIN, fk::ClockSource::APP, p.now).accepted());
  CHECK(p.c.set_mode(Mode::FAN, Source::PHONE).accepted());
  p.heater_follows();
  p.step(2 * MIN);
  CHECK(p.c.status().run_source == Source::PHONE);
  p.step(61 * MIN);
  CHECK(p.c.status().mode == Mode::FAN);

  // A person's off ends the occurrence, not the timer.
  Rig o;
  CHECK(o.a.timer_set(0, t).accepted());
  CHECK(o.a.set_clock(MON_0600_CET + 29 * 60, BERLIN, fk::ClockSource::APP, o.now).accepted());
  o.step(2 * MIN);
  CHECK(o.c.status().run_source == Source::TIMER);
  o.heater_follows();
  CHECK(o.c.set_mode(Mode::OFF, Source::PHONE).accepted());
  o.heater_follows();
  o.step(5 * MIN);
  CHECK(o.c.status().mode == Mode::OFF);
  CHECK(!o.a.disarmed());
  fk::TimerSpec still;
  CHECK(o.a.timer_get(0, still) && still.enabled());

  // A one-off: runs once, then disabled. Fan only at 40 %.
  Rig n;
  fk::TimerSpec once;
  once.flags = fk::TimerSpec::ENABLED | fk::TimerSpec::ONE_OFF;
  once.once_utc = MON_0600_CET + 10 * 60;
  once.duration_min = 5;
  once.mode = 2;
  once.power = 40;
  CHECK(n.a.timer_set(3, once).accepted());
  CHECK(n.a.set_clock(MON_0600_CET, BERLIN, fk::ClockSource::APP, n.now).accepted());
  n.step(11 * MIN);
  CHECK(n.c.status().mode == Mode::FAN);
  CHECK_EQ(n.h.st.power, 40);
  n.heater_follows();
  n.step(5 * MIN);
  CHECK(n.c.status().mode == Mode::OFF);
  n.heater_follows();
  n.step(5 * SEC);
  fk::TimerSpec after;
  CHECK(n.a.timer_get(3, after) && !after.enabled());
  // A one-off whose time passed while nobody could run it: disabled too.
  CHECK(n.a.timer_delete(3).accepted());
  CHECK(n.a.timer_get(3, after) && after.empty());
}

static void test_burn_alerts() {
  Rig r;
  CHECK(r.c.set_mode(Mode::HEAT, Source::PHONE).accepted());
  r.heater_follows();
  r.h.st.heat_exchanger_c = 150.0f;
  r.step(6 * MIN);
  CHECK(!(r.c.status().alerts & al::BURN));
  // Cooling fast while it burns.
  for (int i = 0; i < 12; i++) {
    r.h.st.heat_exchanger_c -= 4.0f;
    r.step(10 * SEC);
  }
  CHECK(r.c.status().alerts & al::BURN);
  CHECK(r.alerted(2));
  CHECK(r.a.clear_alerts(al::BURN).accepted());
  CHECK(!(r.c.status().alerts & al::BURN));
  // A fault the heater reports.
  size_t before = r.alerts.size();
  r.h.st.fault = fk::Fault::OVERHEAT;
  r.step(2 * SEC);
  CHECK(r.alerts.size() > before);
  CHECK(r.alerts.back().second.find("Overheat") != std::string::npos ||
        r.alerts.back().second.find("overheat") != std::string::npos);
}

static void test_bluetooth_room() {
  Rig r;
  fk::RoomSensors &rs = r.a.sensors();
  const uint8_t addr[6] = {0xA4, 0xC1, 0x38, 0x01, 0x02, 0x03};
  const uint8_t other[6] = {0xA4, 0xC1, 0x38, 0x09, 0x09, 0x09};
  fk::ThermoReading reading;
  reading.kind = fk::ThermoKind::XIAOMI_CUSTOM;
  reading.temperature_c = 18.5f;
  reading.humidity_pct = 52.0f;
  reading.battery_pct = 88;
  // Not scanning, none chosen: ignored.
  rs.advert(addr, -60, "ATC_010203", reading, r.now);
  CHECK_EQ(rs.found_count(), 0);
  rs.scan(10, r.now);
  rs.advert(addr, -60, "ATC_010203", reading, r.now);
  rs.advert(addr, -58, nullptr, reading, r.now);
  rs.advert(other, -80, "", reading, r.now);
  CHECK_EQ(rs.found_count(), 2);
  CHECK_EQ(rs.found(0).rssi, -58);
  CHECK(std::string(rs.found(0).name) == "ATC_010203");  // kept from the advert that had it
  r.step(11 * SEC);
  CHECK(!rs.scanning(r.now));
  // Chosen and used as the room source, with its offset.
  r.a.sensor_use(addr, fk::ThermoKind::XIAOMI_CUSTOM);
  CHECK(r.c.set_room_source(fk::RoomSource::BLUETOOTH, Source::PHONE).accepted());
  r.set(fk::O_BLUETOOTH, -5);  // -0.5 C
  rs.advert(addr, -61, nullptr, reading, r.now);
  r.step(2 * SEC);
  CHECK(std::fabs(r.c.status().room_c - 18.0f) < 0.01f);
  CHECK(!r.c.status().room_fallback);
  // Not heard for 10 minutes: dropped, the probe takes over, an alert.
  r.probe = 15.0f;
  r.step(11 * MIN);
  CHECK(r.c.status().alerts & al::SENSOR_LOST);
  CHECK(std::fabs(r.c.status().room_c - 15.0f) < 0.01f);
  CHECK(r.c.status().room_fallback);
  rs.advert(addr, -61, nullptr, reading, r.now);
  r.step(2 * SEC);
  CHECK(!(r.c.status().alerts & al::SENSOR_LOST));
  // Probe offset too.
  r.set(fk::O_PROBE, 10);
  r.c.set_room_source(fk::RoomSource::PROBE, Source::PHONE);
  r.step(2 * SEC);
  CHECK(std::fabs(r.c.status().room_c - 16.0f) < 0.01f);
  // Kept over a restart.
  CHECK(r.last_saved.sensor_address[3] == 0x01);
  CHECK(r.last_saved.sensor_kind == uint8_t(fk::ThermoKind::XIAOMI_CUSTOM));
}

// The 1.3 commands, byte for byte (PROTOCOL.md).
struct NoLink : fk::Link {
  fk::Transport transport() const override { return fk::Transport::BLUETOOTH; }
  fk::Outcome forget_me() override { return fk::Outcome::ok(); }
};
struct NoBoard : fk::BoardSettings {
  fk::Outcome set_pin(uint32_t) override { return fk::Outcome::ok(); }
  fk::Outcome set_web_password(const uint8_t *, size_t, const uint8_t *, size_t) override { return fk::Outcome::ok(); }
  void wifi_status(fk::WifiStatus &) const override {}
  fk::Outcome wifi_set(const uint8_t *, size_t, const uint8_t *, size_t) override { return fk::Outcome::ok(); }
  fk::Outcome wifi_off() override { return fk::Outcome::ok(); }
  bool ha_switchable() const override { return false; }
  fk::HaState ha_state() const override { return fk::HaState::OFF; }
  bool ha_key_set() const override { return false; }
  fk::Outcome ha_set(bool) override { return fk::Outcome::ok(); }
  fk::Outcome ha_key(uint8_t *) override { return fk::Outcome::ok(); }
  fk::Outcome ha_key_new(uint8_t *) override { return fk::Outcome::ok(); }
};
static uint16_t u16(const uint8_t *p) { return uint16_t(p[0] | (p[1] << 8)); }
static uint32_t u32(const uint8_t *p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

static void test_protocol() {
  Rig r;
  NoLink link;
  NoBoard board;
  uint8_t out[ble::RESULT_MAX];
  auto cmd = [&](std::vector<uint8_t> in) { return ble::handle_command(in.data(), in.size(), r.c, link, board, out, sizeof(out)); };

  // CLOCK_SET / CLOCK_GET
  std::vector<uint8_t> set = {1, ble::op::CLOCK_SET};
  for (int i = 0; i < 4; i++)
    set.push_back(uint8_t(MON_0600_CET >> (8 * i)));
  set.push_back(uint8_t(std::strlen(BERLIN)));
  for (const char *p = BERLIN; *p; p++)
    set.push_back(uint8_t(*p));
  CHECK_EQ(cmd(set), 3);
  CHECK_EQ(out[2], 0);
  set[6]++;  // length one too many
  CHECK(cmd(set) && out[2] == uint8_t(Result::BAD_LENGTH));
  size_t n = cmd({2, ble::op::CLOCK_GET});
  CHECK_EQ(n, 3 + 5 + std::strlen(BERLIN));
  CHECK(u32(out + 3) >= MON_0600_CET);
  CHECK_EQ(out[7], 1);
  CHECK(std::string((const char *) out + 8, n - 8) == BERLIN);

  // METERS: 13 u32
  n = cmd({3, ble::op::METERS});
  CHECK_EQ(n, 3 + 56);
  CHECK_EQ(u32(out + 3 + 12), 0);  // 24 h known now (clock set): 0 mL

  // TIMER_SET / GET / DELETE
  std::vector<uint8_t> ts = {4, ble::op::TIMER_SET, 2, 0x01, 0x1F, 0xBE, 0x01, 0, 0, 0, 0, 90, 0, 1, 1, 0, 0xD7, 0x00};
  CHECK_EQ(cmd(ts), 3);
  CHECK_EQ(out[2], 0);
  n = cmd({5, ble::op::TIMER_GET, 2});
  CHECK_EQ(n, 3 + 2 + 15);
  CHECK_EQ(out[3], 2);
  CHECK_EQ(out[4], 8);
  CHECK(std::memcmp(out + 5, ts.data() + 3, 15) == 0);
  CHECK(cmd({6, ble::op::TIMER_GET, 8}) && out[2] == uint8_t(Result::OUT_OF_RANGE));
  ts[3 + 10] = 3;  // mode 3
  CHECK(cmd(ts) && out[2] == uint8_t(Result::OUT_OF_RANGE));
  CHECK(cmd({7, ble::op::TIMER_DELETE, 2}) && out[2] == 0);
  n = cmd({5, ble::op::TIMER_GET, 2});
  for (int i = 0; i < 15; i++)
    CHECK_EQ(out[5 + i], 0);

  // TANK_SET without a tank; ALERTS_CLEAR; AUTO_REARM
  CHECK(cmd({8, ble::op::TANK_SET, 100}) && out[2] == uint8_t(Result::REFUSED));
  CHECK(cmd({9, ble::op::ALERTS_CLEAR, 0x04, 0x00}) && out[2] == 0);
  CHECK(cmd({10, ble::op::AUTO_REARM}) && out[2] == 0);

  // Sensors
  CHECK(cmd({11, ble::op::SENSOR_SCAN, 4}) && out[2] == uint8_t(Result::OUT_OF_RANGE));
  CHECK(cmd({11, ble::op::SENSOR_SCAN, 30}) && out[2] == 0);
  n = cmd({12, ble::op::SENSOR_FOUND, 0});
  CHECK_EQ(n, 6);
  CHECK_EQ(out[4], 0);     // count
  CHECK_EQ(out[5], 0x01);  // scanning
  const uint8_t addr[6] = {0xA4, 0xC1, 0x38, 0x01, 0x02, 0x03};
  fk::ThermoReading rd;
  rd.kind = fk::ThermoKind::GOVEE;
  rd.temperature_c = -3.25f;
  rd.humidity_pct = 61.0f;
  r.a.sensors().advert(addr, -72, "GVH5075_0203", rd, r.c.now());
  n = cmd({13, ble::op::SENSOR_FOUND, 0});
  CHECK_EQ(n, 3 + 3 + 12 + 12);
  CHECK_EQ(out[4], 1);
  CHECK(std::memcmp(out + 6, addr, 6) == 0);
  CHECK_EQ(out[12], 3);
  CHECK_EQ(int8_t(out[13]), -72);
  CHECK_EQ(int16_t(u16(out + 14)), -33);  // -3.25 rounds away from zero
  CHECK_EQ(out[16], 61);
  CHECK_EQ(out[17], 0xFF);
  CHECK(std::string((const char *) out + 18, 12) == "GVH5075_0203");
  CHECK(cmd({14, ble::op::SENSOR_FOUND, 1}) && out[2] == uint8_t(Result::OUT_OF_RANGE));
  std::vector<uint8_t> use = {15, ble::op::SENSOR_USE, 0xA4, 0xC1, 0x38, 0x01, 0x02, 0x03, 3};
  CHECK(cmd(use) && out[2] == 0);
  n = cmd({16, ble::op::SENSOR_STATUS});
  CHECK_EQ(n, 3 + 12 + 2);
  CHECK(std::memcmp(out + 3, addr, 6) == 0);
  CHECK(u16(out + 15) < 5);
  n = cmd({13, ble::op::SENSOR_FOUND, 0});
  CHECK_EQ(out[5] & 0x02, 0x02);  // the chosen one
  use[8] = 9;
  CHECK(cmd(use) && out[2] == uint8_t(Result::OUT_OF_RANGE));

  // State bytes 18-21 and event 08
  r.set(fk::F_TANK, 100);
  CHECK(cmd({17, ble::op::TANK_SET, 40}) && out[2] == 0);
  r.step(2 * SEC);
  uint8_t st[ble::STATE_SIZE];
  CHECK_EQ(ble::encode_state(r.c.status(), st), 33);
  CHECK_EQ(st[21], 40);
  CHECK_EQ(st[20] & fk::automatic::CLOCK_KNOWN, fk::automatic::CLOCK_KNOWN);
  n = ble::encode_event(fk::Event::ALERT, Result::OK, 2, "heater fault: Overheat", out, sizeof(out), 0x01020304u);
  CHECK_EQ(n, 3 + 1 + 4 + 22);
  CHECK_EQ(out[1], 8);
  CHECK_EQ(out[3], 2);
  CHECK(out[4] == 4 && out[5] == 3 && out[6] == 2 && out[7] == 1 && out[8] == 'h');  // the log sequence number
}


// ---------------------------------------------------------------- 1.4

static void test_low_battery() {
  Rig r;
  r.set(fk::B_CUTOFF, 115);  // 11.5 V
  r.set(fk::B_DELAY, 30);
  r.h.st.supply_v = 11.6f;   // below cut-off + 0.2: starts refused
  CHECK(r.c.set_mode(Mode::HEAT, Source::PHONE).result == Result::REFUSED);
  {
    // A refused frost start: the disarm alert quotes the whole reason.
    Rig f;
    f.set(fk::B_CUTOFF, 115);
    f.h.st.supply_v = 11.6f;
    f.probe = 2.0f;
    f.set(fk::FR_ENABLED, 1);
    f.step(3 * SEC);
    CHECK(f.a.disarmed());
    std::string text;
    for (const auto &a : f.alerts)
      if (a.first == 3)
        text = a.second;
    std::printf("  disarm alert (%zu bytes): %s\n", text.size(), text.c_str());
    CHECK(text.size() <= 124 && text.size() >= 16 &&
          text.compare(text.size() - 16, 16, "charge it first)") == 0);
  }
  r.h.st.supply_v = 12.6f;
  CHECK(r.c.set_mode(Mode::HEAT, Source::PHONE).accepted());
  // Starting: the glow plug pulls it to 10.8 V; only below 10.5 for 30 s stops it.
  r.h.st.state = HeaterState::STARTING;
  r.h.st.supply_v = 10.8f;
  r.step(60 * SEC);
  CHECK(r.c.status().mode == Mode::HEAT);
  r.h.st.supply_v = 10.4f;
  r.step(20 * SEC);
  CHECK(r.c.status().mode == Mode::HEAT);
  r.h.st.supply_v = 12.2f;  // recovered before the delay
  r.step(5 * SEC);
  // Heating at 11.3 V for 30 s: stopped (a normal stop), alert 7, logged.
  r.h.st.state = HeaterState::HEATING;
  r.h.st.supply_v = 11.3f;
  r.step(25 * SEC);
  CHECK(r.c.status().mode == Mode::HEAT);
  r.step(10 * SEC);
  CHECK(r.c.status().mode == Mode::OFF);
  CHECK(r.c.status().alerts & fk::alert::BATTERY_LOW);
  CHECK(r.alerted(7));
  r.h.st.state = HeaterState::COOLING;  // the cooldown goes on
  r.step(5 * SEC);
  r.h.st.state = HeaterState::OFF;
  r.step(2 * SEC);
  CHECK(!(r.c.status().alerts & al::BURN));  // asked: not "stopped by itself"
  // Clears 0.5 V above the cut-off.
  r.h.st.supply_v = 11.9f;
  r.step(2 * SEC);
  CHECK(r.c.status().alerts & fk::alert::BATTERY_LOW);
  r.h.st.supply_v = 12.1f;
  r.step(2 * SEC);
  CHECK(!(r.c.status().alerts & fk::alert::BATTERY_LOW));
  bool logged = false;
  fk::LogRecord recs[fk::EventLog::SIZE];
  size_t n = r.a.log().read(0, recs, fk::EventLog::SIZE);
  for (size_t i = 0; i < n; i++)
    logged = logged || (recs[i].kind == uint8_t(fk::LogKind::LOW_BATTERY_STOP) && recs[i].b == 11300);
  CHECK(logged);
  // Off: no protection.
  r.set(fk::B_CUTOFF, 0);
  r.h.st.supply_v = 9.0f;
  CHECK(r.c.set_mode(Mode::HEAT, Source::PHONE).accepted());
}

// One start with the glow plug at a given hot resistance.
static void glow_start(Rig &r, float ohm) {
  CHECK(r.c.set_mode(Mode::HEAT, Source::PHONE).accepted());
  r.h.st.state = HeaterState::STARTING;
  r.h.st.glow_a = 8.0f;
  r.h.st.glow_v = 8.0f * ohm;
  r.step(40 * SEC);
  r.h.st.glow_a = 0.0f;
  r.h.st.glow_v = 0.0f;
  r.h.st.state = HeaterState::HEATING;
  r.step(5 * SEC);
  CHECK(r.c.set_mode(Mode::OFF, Source::PHONE).accepted());
  r.h.st.state = HeaterState::OFF;
  r.step(5 * SEC);
}

static void test_glow_plug_type_warning() {
  Rig r;
  // The heater started with a plug-type warning: alert 12, with its text.
  r.h.st.plug_warning = true;
  r.step(2 * SEC);
  CHECK(r.c.status().alerts & fk::alert::GLOW_PLUG_TYPE);
  CHECK(r.alerted(12));
  // A new plug fitted (after checking the setting): the alert goes.
  r.h.st.plug_warning = false;
  CHECK(r.a.glow_new_plug().accepted());
  r.step(2 * SEC);
  CHECK(!(r.c.status().alerts & fk::alert::GLOW_PLUG_TYPE));
  CHECK_EQ(r.h.plugs_fitted, 1);
}

static void test_glow_health() {
  Rig r;
  r.set(fk::M_GLOW_DRIFT, 30);
  glow_start(r, 1.0f);
  glow_start(r, 1.05f);
  glow_start(r, 0.95f);
  const fk::Automation::Maintenance &m = r.a.maintenance();
  CHECK_EQ(m.glow_starts, 3);
  CHECK(m.glow_baseline_mohm >= 990 && m.glow_baseline_mohm <= 1010);
  // Two drifted starts: not yet; the third in a row: alert 8.
  glow_start(r, 1.4f);
  glow_start(r, 1.4f);
  CHECK(!(r.c.status().alerts & fk::alert::GLOW_DRIFT));
  glow_start(r, 1.45f);
  CHECK(r.c.status().alerts & fk::alert::GLOW_DRIFT);
  CHECK(r.alerted(8));
  CHECK_EQ(m.glow_n, 6);
  CHECK(m.glow_readings[5] >= 1440 && m.glow_readings[5] <= 1460);
  // A new plug: a new baseline, the alert gone, and the heater told (it forgets the plug's type).
  CHECK(r.a.glow_new_plug().accepted());
  CHECK(!(r.c.status().alerts & fk::alert::GLOW_DRIFT));
  CHECK_EQ(r.h.plugs_fitted, 1);
  CHECK_EQ(r.a.maintenance().glow_n, 0);
  CHECK_EQ(r.a.maintenance().glow_baseline_mohm, 0xFFFF);
  glow_start(r, 1.2f);
  CHECK(r.a.maintenance().glow_baseline_mohm >= 1190 && r.a.maintenance().glow_baseline_mohm <= 1210);
}

static void test_burnoff() {
  Rig r;
  r.set(fk::M_REMINDER, 5);  // 5 h
  r.set(fk::M_LOW_POWER, 40);
  r.c.set_heat_mode(fk::HeatMode::TEMPERATURE, Source::PHONE);
  r.c.set_power(35, Source::PHONE);
  CHECK(r.c.set_mode(Mode::HEAT, Source::PHONE).accepted());
  r.heater_follows();
  r.h.st.power_pct = 30.0f;
  r.step(5 * 3600 * SEC + 5 * SEC);
  CHECK(r.c.status().alerts & fk::alert::BURNOFF_DUE);
  CHECK(r.alerted(9));
  // Only from off.
  CHECK(r.a.burnoff_start(20, Source::PHONE).result == Result::NOT_OFF);
  CHECK(r.c.set_mode(Mode::OFF, Source::PHONE).accepted());
  r.heater_follows();
  r.step(2 * SEC);
  CHECK(r.a.burnoff_start(5, Source::PHONE).result == Result::OUT_OF_RANGE);
  CHECK(r.a.burnoff_start(20, Source::PHONE).accepted());
  CHECK(r.c.status().heat_mode == fk::HeatMode::POWER);
  CHECK_EQ(r.c.status().power, 100);
  CHECK_EQ(r.c.status().burnoff_left_min, 20);
  CHECK(r.c.status().run_source == Source::PHONE);
  r.heater_follows();
  r.h.st.power_pct = 100.0f;
  r.step(10 * MIN);
  CHECK(r.c.status().burnoff_left_min >= 9 && r.c.status().burnoff_left_min <= 10);
  r.step(10 * MIN + 2 * SEC);
  CHECK(r.c.status().mode == Mode::OFF);
  r.heater_follows();
  r.step(3 * SEC);
  CHECK(!(r.c.status().alerts & fk::alert::BURNOFF_DUE));
  CHECK_EQ(r.a.maintenance().low_power_s, 0);
  CHECK(r.c.status().heat_mode == fk::HeatMode::TEMPERATURE);  // the user's own again
  CHECK_EQ(r.c.status().power, 35);
  CHECK_EQ(r.c.status().burnoff_left_min, 0);
  // Stopped early by a person: the counter stays.
  r.step(3600 * SEC);
  CHECK(r.c.set_mode(Mode::HEAT, Source::PHONE).accepted());
  r.heater_follows();
  r.h.st.power_pct = 30.0f;
  r.step(3600 * SEC);
  CHECK(r.c.set_mode(Mode::OFF, Source::PHONE).accepted());
  r.heater_follows();
  r.step(2 * SEC);
  const uint32_t before = r.a.maintenance().low_power_s;
  CHECK(before >= 3590);
  CHECK(r.a.burnoff_start(15, Source::PHONE).accepted());
  r.heater_follows();
  r.h.st.power_pct = 100.0f;
  r.step(2 * MIN);
  CHECK(r.c.set_mode(Mode::OFF, Source::PHONE).accepted());
  r.heater_follows();
  r.step(2 * SEC);
  CHECK_EQ(r.a.maintenance().low_power_s, before);
  {  // METERS ends with the low-power time since the last burn-off
    NoLink link;
    NoBoard board;
    uint8_t out[ble::RESULT_MAX];
    const uint8_t in[2] = {9, ble::op::METERS};
    CHECK_EQ(ble::handle_command(in, 2, r.c, link, board, out, sizeof(out)), 3 + 56);
    CHECK_EQ(u32(out + 3 + 52), before);
  }
  // 20 minutes at 90 % or more in normal use counts as a burn-off.
  CHECK(r.c.set_mode(Mode::HEAT, Source::PHONE).accepted());
  r.heater_follows();
  r.h.st.power_pct = 95.0f;
  r.step(21 * MIN);
  CHECK_EQ(r.a.maintenance().low_power_s, 0);
}

static void test_co() {
  Rig r;
  // The safety MCU reports an alarm and stops the heater; if the heater still ran, the ESP32 asks.
  CHECK(r.c.set_mode(Mode::HEAT, Source::PHONE).accepted());
  r.heater_follows();
  r.step(2 * SEC);
  r.h.st.co_state = uint8_t(fk::CoState::ALARM);
  r.h.st.co_ppm = 420.0f;
  r.step(2 * SEC);
  CHECK(r.c.status().alerts & fk::alert::CO_ALARM);
  CHECK(r.alerted(10));
  CHECK(r.c.status().mode == Mode::OFF);
  r.heater_follows();
  r.h.st.fault = fk::Fault::CO;
  r.step(2 * SEC);
  CHECK(r.c.set_mode(Mode::HEAT, Source::PHONE).result == Result::REFUSED);
  CHECK(r.c.set_mode(Mode::FAN, Source::HOME_ASSISTANT).result == Result::REFUSED);
  // Cleared by the MCU (a fault reset once the air is clean): starts again.
  r.h.st.fault = fk::Fault::NONE;
  r.h.st.co_state = uint8_t(fk::CoState::OK);
  r.step(2 * SEC);
  CHECK(!(r.c.status().alerts & fk::alert::CO_ALARM));
  CHECK(r.c.set_mode(Mode::HEAT, Source::PHONE).accepted());
  r.heater_follows();
  // A sensor fault: alert 11, starts refused.
  CHECK(r.c.set_mode(Mode::OFF, Source::PHONE).accepted());
  r.heater_follows();
  r.h.st.co_state = uint8_t(fk::CoState::SENSOR_FAULT);
  r.step(2 * SEC);
  CHECK(r.c.status().alerts & fk::alert::CO_SENSOR);
  CHECK(r.c.set_mode(Mode::HEAT, Source::PHONE).result == Result::REFUSED);
  // No sensor fitted: nothing.
  r.h.st.co_state = uint8_t(fk::CoState::NOT_FITTED);
  r.step(2 * SEC);
  CHECK_EQ(r.c.status().alerts & (fk::alert::CO_ALARM | fk::alert::CO_SENSOR), 0);
}

static void test_event_log() {
  fk::EventLog log;
  CHECK_EQ(log.newest(), 0);
  fk::LogRecord recs[8];
  CHECK_EQ(log.read(0, recs, 8), 0);
  for (int i = 0; i < 200; i++)
    log.add(fk::LogKind::ALERT, 0, uint16_t(i), 0, 0);
  CHECK_EQ(log.newest(), 200);
  CHECK_EQ(log.oldest(), 73);  // the last 128
  CHECK_EQ(log.read(1, recs, 8), 8);
  CHECK_EQ(recs[0].seq, 73);
  CHECK_EQ(recs[0].a, 72);
  CHECK_EQ(log.read(198, recs, 8), 3);
  CHECK_EQ(recs[2].seq, 200);
  CHECK_EQ(log.read(201, recs, 8), 0);
  fk::EventLog copy;
  CHECK(copy.load(log.saved()));
  CHECK_EQ(copy.newest(), 200);
  copy.add(fk::LogKind::STOP_ASKED, 2, 0, 0, 1768194000u);
  CHECK_EQ(copy.newest(), 201);

  // What a run leaves in the board's log.
  Rig r;
  r.a.log_board_started(3);
  CHECK(r.a.set_clock(MON_0600_CET, BERLIN, fk::ClockSource::APP, r.now).accepted());
  CHECK(r.c.set_mode(Mode::HEAT, Source::PHONE).accepted());
  r.heater_follows();
  r.step(3 * SEC);
  CHECK(r.c.set_mode(Mode::OFF, Source::WIFI_APP).accepted());
  r.heater_follows();
  r.step(3 * SEC);
  size_t n = r.a.log().read(0, recs, 8);
  CHECK(n >= 4);
  CHECK_EQ(recs[0].kind, uint8_t(fk::LogKind::BOARD_STARTED));
  CHECK_EQ(recs[0].a, 3);
  CHECK_EQ(recs[1].kind, uint8_t(fk::LogKind::CLOCK_SET));
  CHECK_EQ(recs[2].kind, uint8_t(fk::LogKind::RUN_STARTED));
  CHECK_EQ(recs[2].source, uint8_t(Source::PHONE));
  CHECK_EQ(recs[2].a, uint8_t(Mode::HEAT));
  CHECK(recs[2].utc >= MON_0600_CET);
  CHECK_EQ(recs[3].kind, uint8_t(fk::LogKind::STOP_ASKED));
  CHECK_EQ(recs[3].source, uint8_t(Source::WIFI_APP));
}

static void test_automation_commands() {
  Rig r;
  NoLink link;
  NoBoard board;
  uint8_t out[ble::RESULT_MAX];
  auto cmd = [&](std::vector<uint8_t> in) { return ble::handle_command(in.data(), in.size(), r.c, link, board, out, sizeof(out)); };
  // LOG_READ: newest, n, records of 16 bytes, at most 7.
  for (int i = 0; i < 10; i++)
    r.a.log().add(fk::LogKind::TANK_SET, 0, uint16_t(i), 0, 0);
  size_t n = cmd({1, ble::op::LOG_READ, 0, 0, 0, 0});
  CHECK_EQ(n, 3 + 5 + 7 * 16);
  CHECK_EQ(u32(out + 3), r.a.log().newest());
  CHECK_EQ(out[7], 7);
  CHECK_EQ(u32(out + 8), r.a.log().oldest());
  const uint32_t last = r.a.log().newest();
  n = cmd({2, ble::op::LOG_READ, uint8_t(last), uint8_t(last >> 8), 0, 0});
  CHECK_EQ(out[7], 1);
  CHECK_EQ(out[8 + 8], uint8_t(fk::LogKind::TANK_SET));
  CHECK_EQ(u16(out + 8 + 10), 9);
  CHECK(cmd({3, ble::op::LOG_READ, 0}) && out[2] == uint8_t(Result::BAD_LENGTH));
  // GLOW_HEALTH with nothing yet; GLOW_NEW_PLUG.
  n = cmd({4, ble::op::GLOW_HEALTH});
  CHECK_EQ(n, 3 + 7);
  CHECK_EQ(u16(out + 3), 0xFFFF);
  CHECK_EQ(u16(out + 5), 0xFFFF);
  CHECK(cmd({5, ble::op::GLOW_NEW_PLUG}) && out[2] == 0);
  glow_start(r, 1.0f);
  n = cmd({6, ble::op::GLOW_HEALTH});
  CHECK_EQ(n, 3 + 7 + 2);
  CHECK_EQ(u16(out + 7), 1);
  CHECK_EQ(out[9], 1);
  // BURNOFF_START: range, then a run; State byte 22.
  CHECK(cmd({7, ble::op::BURNOFF_START, 31}) && out[2] == uint8_t(Result::OUT_OF_RANGE));
  CHECK(cmd({8, ble::op::BURNOFF_START, 12}) && out[2] == 0);
  uint8_t st[ble::STATE_SIZE];
  ble::encode_state(r.c.status(), st);
  CHECK_EQ(st[22], 12);
  CHECK_EQ(st[12], uint8_t(Source::PHONE));
  // Telemetry byte 30: the CO state.
  r.h.st.co_state = uint8_t(fk::CoState::LEARNING);
  r.step(2 * SEC);
  uint8_t tel[ble::TELEMETRY_SIZE];
  CHECK_EQ(ble::encode_telemetry(r.c.status(), tel), 41);
  CHECK_EQ(tel[30], 2);
}

int main() {
  test_fuel_meter();
  test_tank_and_low_fuel();
  test_priming_no_burn_alert();
  test_frost();
  test_frost_waits_for_board_update();
  test_disarm_on_doubt();
  test_restored_after_restart();
  test_clock();
  test_clock_log_quiet();
  test_quoted_time_zones();
  test_timers();
  test_burn_alerts();
  test_bluetooth_room();
  test_protocol();
  test_low_battery();
  test_glow_plug_type_warning();
  test_glow_health();
  test_burnoff();
  test_co();
  test_event_log();
  test_automation_commands();
  return report("automation");
}
