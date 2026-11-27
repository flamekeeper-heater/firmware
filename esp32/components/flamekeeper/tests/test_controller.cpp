// Controller: common checks, run source, the Home Assistant failsafe (only for runs started from
// Home Assistant), room temperature with fallback.
#include <cstring>

#include "fake_heater.h"

using fk::Mode;
using fk::Result;
using fk::Source;
using fk::HeaterState;

static const uint32_t MIN = 60 * 1000;

static void test_checks() {
  FakeHeater h;
  h.caps = fk::cap::PRIME;  // no fan-only, no temperature mode, no parameters
  fk::Controller c(&h);
  CHECK(c.set_mode(Mode::FAN, Source::PHONE).result == Result::NOT_SUPPORTED);
  // The controller runs the thermostat itself: temperature mode on any heater.
  CHECK(c.set_heat_mode(fk::HeatMode::TEMPERATURE, Source::PHONE).result == Result::OK);
  CHECK(c.set_heat_mode(fk::HeatMode(2), Source::PHONE).result == Result::OUT_OF_RANGE);
  CHECK(c.set_heat_mode(fk::HeatMode::POWER, Source::PHONE).result == Result::OK);
  CHECK(c.set_power(-1, Source::PHONE).result == Result::OUT_OF_RANGE);
  CHECK(c.set_power(101, Source::HOME_ASSISTANT).result == Result::OUT_OF_RANGE);
  CHECK(c.set_target(40.0f, Source::PHONE).result == Result::OUT_OF_RANGE);
  CHECK(c.set_target(NAN, Source::PHONE).result == Result::OUT_OF_RANGE);
  CHECK(c.set_target(21.0f, Source::PHONE).result == Result::OK);
  CHECK(c.param_set(1, 2, Source::PHONE).result == Result::NOT_SUPPORTED);
  CHECK(c.stop_prime(Source::PHONE).result == Result::NOT_SUPPORTED);
  CHECK(c.set_mode(Mode(7), Source::PHONE).result == Result::OUT_OF_RANGE);
  CHECK(c.set_room_source(fk::RoomSource(5), Source::PHONE).result == Result::OUT_OF_RANGE);
  // Nothing refused by the controller reached the backend.
  CHECK(h.calls.empty());
  // The backend's own refusal comes back unchanged.
  h.st.answering = false;
  fk::Outcome o = c.set_mode(Mode::HEAT, Source::PHONE);
  CHECK(o.result == Result::NO_REPLY);
  CHECK(std::strcmp(o.reason, "the heater does not answer") == 0);
  CHECK_EQ(int(c.status().run_source), int(Source::NONE));
  // ROOM_SOURCE is always offered (the controller owns the room temperature).
  CHECK(c.status().capabilities & fk::cap::ROOM_SOURCE);
}

// Every source goes through the same path: same outcome for the same request.
static void test_same_checks_every_source() {
  for (Source src : {Source::HOME_ASSISTANT, Source::PHONE, Source::BOARD}) {
    FakeHeater h;
    fk::Controller c(&h);
    int logged = 0;
    Source seen = Source::NONE;
    c.on_request([&](Source s, const char *, const fk::Outcome &) {
      logged++;
      seen = s;
    });
    h.st.answering = false;
    CHECK(c.set_mode(Mode::HEAT, src).result == Result::NO_REPLY);
    CHECK(c.set_power(30, src).result == Result::OK);
    h.st.answering = true;
    CHECK(c.set_mode(Mode::HEAT, src).result == Result::OK);
    CHECK(c.set_mode(Mode::PRIME, src).result == Result::NOT_OFF);
    CHECK_EQ(logged, 4);
    CHECK_EQ(int(seen), int(src));
    CHECK_EQ(int(c.status().run_source), int(src));
  }
}

static void test_failsafe_ha_run() {
  FakeHeater h;
  fk::Controller c(&h);
  int events = 0;
  c.on_event([&](fk::Event e, Result, uint16_t, const char *) { events += e == fk::Event::FAILSAFE_STOP; });
  uint32_t t = 1000;
  c.tick(t, true);
  CHECK(c.set_mode(Mode::HEAT, Source::HOME_ASSISTANT).result == Result::OK);
  CHECK_EQ(int(c.status().run_source), int(Source::HOME_ASSISTANT));
  CHECK_EQ(c.status().failsafe_left_s, UINT32_MAX);  // HA connected: not counting

  // HA goes away: 15 minutes, then a stop.
  c.tick(t += 1000, false);
  CHECK_EQ(c.status().failsafe_left_s, 900);
  c.tick(t += 14 * MIN, false);
  CHECK(h.st.mode == Mode::HEAT);
  CHECK_EQ(c.status().failsafe_left_s, 60);
  // Back for a moment: the count starts again.
  c.tick(t += 30000, true);
  CHECK_EQ(c.status().failsafe_left_s, UINT32_MAX);
  c.tick(t += 1000, false);
  c.tick(t += 15 * MIN - 1, false);
  CHECK(h.st.mode == Mode::HEAT);
  c.tick(t += 1, false);
  CHECK(h.st.mode == Mode::OFF);
  CHECK_EQ(events, 1);
  CHECK(c.status().failsafe_stopped);
  CHECK_EQ(int(c.status().run_source), int(Source::NONE));
  // A new start clears the flag.
  c.tick(t += 1000, true);
  c.set_mode(Mode::HEAT, Source::PHONE);
  CHECK(!c.status().failsafe_stopped);
}

static void test_failsafe_not_for_other_runs() {
  for (Source src : {Source::PHONE, Source::BOARD}) {
    FakeHeater h;
    fk::Controller c(&h);
    uint32_t t = 5;
    c.set_mode(Mode::HEAT, src);
    for (int i = 0; i < 24 * 60; i++)  // a day without Home Assistant
      c.tick(t += MIN, false);
    CHECK(h.st.mode == Mode::HEAT);
    CHECK_EQ(c.status().failsafe_left_s, UINT32_MAX);
  }
}

static void test_run_source_kept_until_off() {
  FakeHeater h;
  fk::Controller c(&h);
  uint32_t t = 0;
  c.set_mode(Mode::HEAT, Source::PHONE);
  // Home Assistant changes things mid-run: still the phone's run, no failsafe.
  c.set_power(80, Source::HOME_ASSISTANT);
  c.set_mode(Mode::FAN, Source::HOME_ASSISTANT);
  CHECK_EQ(int(c.status().run_source), int(Source::PHONE));
  c.tick(t += 20 * MIN, false);
  CHECK(h.st.mode == Mode::FAN);
  // Off (by anyone), then a start from Home Assistant: now covered.
  c.set_mode(Mode::OFF, Source::PHONE);
  c.tick(t += 1000, true);
  CHECK_EQ(int(c.status().run_source), int(Source::NONE));
  c.set_mode(Mode::HEAT, Source::HOME_ASSISTANT);
  CHECK_EQ(int(c.status().run_source), int(Source::HOME_ASSISTANT));
  // The backend ends the run itself (priming done, control off...): the source goes with it.
  h.st.mode = Mode::OFF;
  c.tick(t += 1000, false);
  CHECK_EQ(int(c.status().run_source), int(Source::NONE));
  c.tick(t += 20 * MIN, false);
  CHECK_EQ(c.status().failsafe_left_s, UINT32_MAX);
}

// State's run identity: one id per run (boot id, run number), and the seconds it has lasted.
static void test_run_identity() {
  FakeHeater h;
  fk::Controller c(&h);
  c.set_boot_id(0xBEEF);
  uint32_t t = 5000;
  c.tick(t, false);
  CHECK_EQ(c.status().run_id, 0u);
  CHECK_EQ(c.status().run_s, 0u);
  c.set_mode(Mode::HEAT, Source::PHONE);
  const uint32_t first = c.status().run_id;
  CHECK_EQ(first, 0xBEEF0001u);
  c.tick(t += 90 * 1000, false);
  CHECK_EQ(c.status().run_s, 90u);
  // Changes during the run keep its id and its clock.
  c.set_mode(Mode::FAN, Source::HOME_ASSISTANT);
  c.set_power(30, Source::HOME_ASSISTANT);
  c.tick(t += 30 * 1000, false);
  CHECK_EQ(c.status().run_id, first);
  CHECK_EQ(c.status().run_s, 120u);
  // Off: no run. The next run, started the same way, is a new one.
  c.set_mode(Mode::OFF, Source::PHONE);
  c.tick(t += 1000, false);
  CHECK_EQ(c.status().run_id, 0u);
  CHECK_EQ(c.status().run_s, 0u);
  c.set_mode(Mode::HEAT, Source::PHONE);
  c.tick(t += 1000, false);
  CHECK_EQ(c.status().run_id, 0xBEEF0002u);
  CHECK_EQ(c.status().run_s, 1u);
  // Stopping and cooling down are still this run: same id and source, the time stops at the stop.
  h.st.state = fk::HeaterState::HEATING;
  c.tick(t += 60 * 1000, false);
  const uint32_t second = c.status().run_id;
  c.set_mode(Mode::OFF, Source::HOME_ASSISTANT);
  h.st.state = fk::HeaterState::COOLING;
  c.tick(t += 1000, false);
  CHECK(c.status().run_source == Source::NONE);  // the controller's own rules see no run
  CHECK_EQ(c.status().run_id, second);
  CHECK(c.status().ending_source == Source::PHONE);
  const uint32_t ran = c.status().run_s;
  CHECK(ran >= 61 && ran <= 62);
  c.tick(t += 5 * 60 * 1000, false);
  CHECK_EQ(c.status().run_s, ran);
  // A start during the cooldown is a new run.
  CHECK(c.set_mode(Mode::HEAT, Source::PHONE).accepted());
  CHECK_EQ(c.status().run_id, second + 1);
  CHECK(c.status().ending_source == Source::NONE);
  c.set_mode(Mode::OFF, Source::PHONE);
  c.tick(t += 1000, false);
  CHECK_EQ(c.status().run_id, second + 1);
  // Off for real: no run.
  h.st.state = fk::HeaterState::OFF;
  c.tick(t += 1000, false);
  CHECK_EQ(c.status().run_id, 0u);
  CHECK_EQ(c.status().run_s, 0u);
  CHECK(c.status().ending_source == Source::NONE);
  // A start the heater refuses is no run.
  c.set_mode(Mode::OFF, Source::PHONE);
  c.tick(t += 1000, false);
  h.st.answering = false;
  CHECK(!c.set_mode(Mode::HEAT, Source::PHONE).accepted());
  c.tick(t += 1000, false);
  CHECK(c.status().run_source == Source::NONE);
  CHECK_EQ(c.status().run_id, 0u);
  h.st.answering = true;
  CHECK(c.set_mode(Mode::HEAT, Source::PHONE).accepted());
  CHECK_EQ(c.status().run_id, 0xBEEF0004u);  // the refused start used no number
}

static void test_failsafe_wraps_and_disabled() {
  FakeHeater h;
  fk::Controller c(&h);
  uint32_t t = UINT32_MAX - 5 * MIN;  // millis() wraps during the count
  c.tick(t, true);
  c.set_mode(Mode::HEAT, Source::HOME_ASSISTANT);
  c.tick(t += 1000, false);
  c.tick(t += 14 * MIN, false);
  CHECK(h.st.mode == Mode::HEAT);
  c.tick(t += MIN, false);
  CHECK(h.st.mode == Mode::OFF);

  FakeHeater h2;
  fk::Controller c2(&h2);
  c2.set_failsafe_timeout(0);  // 0 = no failsafe
  c2.set_mode(Mode::HEAT, Source::HOME_ASSISTANT);
  for (uint32_t s = 1; s < 100; s++)
    c2.tick(s * MIN, false);
  CHECK(h2.st.mode == Mode::HEAT);
}

static void test_room() {
  FakeHeater h;
  fk::Controller c(&h);
  uint32_t t = 1000;
  c.tick(t, true);
  CHECK(std::isnan(h.room));
  CHECK(std::isnan(c.status().room_c));
  // Default source: the board's probe; only Home Assistant has a value -> fallback.
  CHECK(c.status().room_source == fk::RoomSource::PROBE);
  c.room().home_assistant(21.0f);
  c.tick(t += 1000, true);
  CHECK(h.room == 21.0f);
  CHECK(c.status().room_fallback);
  // Every source's reading is reported, the one in use or not.
  CHECK(c.status().room_ha_c == 21.0f && std::isnan(c.status().room_probe_c) && std::isnan(c.status().room_bt_c));
  c.room().probe(19.5f, t);
  c.tick(t += 1000, true);
  CHECK(h.room == 19.5f);
  CHECK(!c.status().room_fallback);
  CHECK(c.status().room_probe_c == 19.5f && c.status().room_ha_c == 21.0f);
  CHECK(c.set_room_source(fk::RoomSource::HOME_ASSISTANT, Source::PHONE).result == Result::OK);
  c.tick(t += 1000, true);
  CHECK(h.room == 21.0f);
  CHECK(!c.status().room_fallback);
  // Switch to the probe: immediate.
  CHECK(c.set_room_source(fk::RoomSource::PROBE, Source::PHONE).result == Result::OK);
  CHECK(h.room == 19.5f);
  c.tick(t += 1000, true);
  CHECK(!c.status().room_fallback);
  CHECK(c.status().room_source == fk::RoomSource::PROBE);
  // The probe goes stale after 5 minutes: back to Home Assistant's value.
  c.tick(t += 5 * MIN, true);
  CHECK(h.room == 21.0f);
  CHECK(c.status().room_fallback);
  // Nothing at all.
  c.room().home_assistant(NAN);
  c.tick(t += 1000, true);
  CHECK(std::isnan(h.room));
  CHECK(!c.status().room_fallback);
}

static void test_altitude_and_events() {
  FakeHeater h;
  fk::Controller c(&h);
  c.set_altitude(812.0f);
  CHECK(h.altitude == 812.0f);
  c.tick(1, true);
  CHECK(c.status().altitude_m == 812.0f);  // the heater reports none: the board's
  h.st.altitude_m = 400.0f;
  c.tick(2, true);
  CHECK(c.status().altitude_m == 400.0f);  // the heater's own wins

  h.params.push_back({{1, 0, fk::Unit::SECONDS, 0, 0, 100, 1, "Preheat time"}, {true, false, 60, 0}});
  int applied = 0;
  c.on_event([&](fk::Event e, Result, uint16_t, const char *) { applied += e == fk::Event::PARAMS_APPLIED; });
  CHECK(c.param_set(1, 70, Source::HOME_ASSISTANT).result == Result::OK);
  CHECK(c.param_set(1, 170, Source::HOME_ASSISTANT).result == Result::OUT_OF_RANGE);
  CHECK(c.param_apply(Source::PHONE).result == Result::OK);
  CHECK_EQ(applied, 1);
  fk::ParamValue v;
  CHECK(h.param_get(1, v) && v.value == 70 && !v.staged);
}

static void test_thermostat_in_controller() {
  FakeHeater h;
  fk::Controller c(&h);
  uint32_t t = 1000;
  c.tick(t, true);
  c.set_power(30, Source::PHONE);
  CHECK_EQ(h.st.power, 30);
  // Cycling on, stop 1.5 above / start 1.0 below the target (the defaults).
  CHECK(c.param_set(fk::T_CYCLE, 1, Source::PHONE).accepted());
  CHECK(c.set_heat_mode(fk::HeatMode::TEMPERATURE, Source::PHONE).accepted());
  CHECK(c.set_target(21.0f, Source::PHONE).accepted());
  c.room().home_assistant(18.0f);
  c.tick(t += 1000, true);
  CHECK(c.set_mode(Mode::HEAT, Source::PHONE).accepted());
  CHECK(h.st.mode == Mode::HEAT);
  CHECK_EQ(int(c.status().run_source), int(Source::PHONE));
  // The user's power stays the setting; the thermostat sets the heater's demand.
  CHECK(c.set_power(10, Source::PHONE).accepted());
  CHECK_EQ(c.status().power, 10);
  h.st.state = HeaterState::HEATING;
  for (int i = 0; i < 5; i++)
    c.tick(t += 1000, true);
  CHECK(h.st.power > 10);  // 3 degrees cold: the demand rises
  CHECK(c.status().heat_mode == fk::HeatMode::TEMPERATURE);
  // Warm: above the stop point the heater is switched off once it has run its minimum time (10 min),
  // and the run goes on.
  c.room().home_assistant(23.0f);
  for (int i = 0; i < 3; i++)
    c.tick(t += 1000, true);
  CHECK(h.st.mode == Mode::HEAT);
  for (int i = 0; i < 10 * 60; i++)
    c.tick(t += 1000, true);
  CHECK(h.st.mode == Mode::OFF);
  CHECK(c.status().mode == Mode::HEAT);
  CHECK_EQ(int(c.status().run_source), int(Source::PHONE));
  CHECK(c.status().thermo_paused);  // through the pause's cooldown too
  h.st.state = HeaterState::OFF;
  // Cool again, but the minimum off time (5 min) holds it.
  c.room().home_assistant(19.0f);
  c.tick(t += 1000, true);
  CHECK(h.st.mode == Mode::OFF);
  c.tick(t += 5 * MIN, true);
  c.tick(t += 1000, true);
  CHECK(h.st.mode == Mode::HEAT);  // a new start
  CHECK(!c.status().thermo_paused);
  // Off by the user ends it all.
  CHECK(c.set_mode(Mode::OFF, Source::PHONE).accepted());
  CHECK(c.status().mode == Mode::OFF);
  CHECK(!c.status().thermo_paused);

  // Warm enough at the start: armed, not started.
  FakeHeater h2;
  fk::Controller c2(&h2);
  c2.tick(1000, true);
  c2.param_set(fk::T_CYCLE, 1, Source::BOARD);
  c2.set_heat_mode(fk::HeatMode::TEMPERATURE, Source::BOARD);
  c2.set_target(20.0f, Source::BOARD);
  c2.room().home_assistant(25.0f);
  c2.tick(2000, true);
  CHECK(c2.set_mode(Mode::HEAT, Source::HOME_ASSISTANT).accepted());
  CHECK(h2.st.mode == Mode::OFF);
  CHECK(c2.status().mode == Mode::HEAT);
  CHECK_EQ(int(c2.status().run_source), int(Source::HOME_ASSISTANT));
  CHECK(c2.status().thermo_paused);  // a run that begins paused
  // Switching to the power setting while held off: heat means heat.
  c2.set_heat_mode(fk::HeatMode::POWER, Source::BOARD);
  c2.tick(3000, true);
  CHECK(h2.st.mode == Mode::HEAT);
}

// A temperature-mode heat run the thermostat holds off, armed while the room is warm.
static void held_run(FakeHeater &h, fk::Controller &c, uint32_t &t) {
  c.tick(t, true);
  c.param_set(fk::T_CYCLE, 1, Source::BOARD);
  c.set_heat_mode(fk::HeatMode::TEMPERATURE, Source::BOARD);
  c.set_target(20.0f, Source::BOARD);
  c.room().home_assistant(25.0f);
  c.tick(t += 1000, true);
  CHECK(c.set_mode(Mode::HEAT, Source::PHONE).accepted());
  CHECK(h.st.mode == Mode::OFF);
  CHECK(c.status().mode == Mode::HEAT);
}

static void test_thermostat_safety() {
  // The Home Assistant room temperature is dropped while the API is disconnected.
  {
    FakeHeater h;
    fk::Controller c(&h);
    c.room().home_assistant(17.0f);
    c.tick(1000, true);
    CHECK(c.status().room_c == 17.0f);
    c.tick(2000, false);
    CHECK(std::isnan(c.status().room_c));
    CHECK(std::isnan(h.room));
    c.room().probe(19.0f, 3000);  // the probe, the default source, takes over
    c.tick(3000, false);
    CHECK(c.status().room_c == 19.0f);
    CHECK(!c.status().room_fallback);
  }
  // A held run is accepted only when the heater could start now.
  {
    FakeHeater h;
    fk::Controller c(&h);
    c.tick(1000, true);
    c.param_set(fk::T_CYCLE, 1, Source::BOARD);
    c.set_heat_mode(fk::HeatMode::TEMPERATURE, Source::BOARD);
    c.set_target(20.0f, Source::BOARD);
    c.room().home_assistant(25.0f);
    c.tick(2000, true);
    h.st.answering = false;
    CHECK(c.set_mode(Mode::HEAT, Source::PHONE).result == Result::NO_REPLY);
    h.st.answering = true;
    h.st.state = HeaterState::COOLING;
    CHECK(c.set_mode(Mode::HEAT, Source::PHONE).result == Result::NOT_OFF);
    h.st.state = HeaterState::OFF;
    h.st.update_running = true;
    CHECK(c.set_mode(Mode::HEAT, Source::PHONE).result == Result::BUSY);
    h.st.update_running = false;
    CHECK(c.set_mode(Mode::HEAT, Source::PHONE).accepted());
    CHECK(c.status().mode == Mode::HEAT);
  }
  // A fault while the thermostat holds the heater off ends the run.
  {
    FakeHeater h;
    fk::Controller c(&h);
    uint32_t t = 1000;
    held_run(h, c, t);
    h.st.fault = fk::Fault::OVERHEAT;
    c.room().home_assistant(10.0f);
    for (int i = 0; i < 20 * 60; i++)
      c.tick(t += 1000, true);
    CHECK(h.st.mode == Mode::OFF);
    CHECK(c.status().mode == Mode::OFF);
  }
}

static void test_tuning_params() {
  FakeHeater h;
  h.params.push_back({{1, 0, fk::Unit::SECONDS, 0, 0, 100, 1, "Preheat time"}, {true, false, 60, 0}});
  fk::Controller c(&h);
  int saved = 0;
  c.on_tuning_changed([&](const fk::Tuning &) { saved++; });
  // The heater's parameters first, then the board's own.
  CHECK_EQ(c.param_count(), 1 + fk::Tuning::count());
  fk::ParamInfo pi;
  CHECK(c.param_info(0, pi) && pi.id == 1);
  CHECK(c.param_info(1, pi) && pi.id == fk::T_FUEL_TRIM && (pi.flags & fk::pflag::BOARD));
  fk::ParamValue v;
  CHECK(c.param_get(fk::T_BAND, v) && v.value == 20);  // 2.0 C
  CHECK(c.param_set(fk::T_BAND, 15, Source::PHONE).accepted());
  CHECK(c.param_get(fk::T_BAND, v) && v.value == 15);
  CHECK_EQ(saved, 1);
  CHECK(c.param_set(fk::T_BAND, 7, Source::PHONE).result == Result::OUT_OF_RANGE);  // not on a 5 step
  // Live trims are not stored, and reach the heater with the altitude trim.
  CHECK(c.param_set(fk::T_FUEL_TRIM, -5, Source::PHONE).accepted());
  CHECK_EQ(saved, 1);
  CHECK_EQ(h.fuel_trim, -5);
  c.tick(4000, true);
  CHECK_EQ(c.status().fuel_trim_pct, -5);                 // Telemetry: the trim in effect
  CHECK_EQ(int(c.status().altitude_trim_pct), INT8_MIN);  // altitude trim off
  CHECK(c.param_set(fk::A_ENABLED, 1, Source::PHONE).accepted());
  CHECK(c.param_set(fk::A_OFFSET, 0, Source::PHONE).accepted());
  c.set_altitude(1000.0f);  // 10 % less fuel per 1000 m by default
  c.tick(5000, true);
  CHECK_EQ(h.fuel_trim, -15);
  CHECK_EQ(c.status().altitude_trim_pct, -10);
  CHECK_EQ(c.status().fuel_trim_pct, -15);
  CHECK(c.status().altitude_smoothed_m == 1000.0f);
  // The offset corrects the board's reading: shown, sent to the heater and trimmed on.
  CHECK(c.param_set(fk::A_OFFSET, -500, Source::PHONE).accepted());
  c.set_altitude(1000.0f);
  c.tick(6000, true);
  CHECK(c.status().altitude_m == 500.0f);
  CHECK(h.altitude == 500.0f);
  // Stored and loaded back.
  fk::Tuning::Store st;
  c.tuning().save(st);
  FakeHeater h2;
  fk::Controller c2(&h2);
  c2.load_tuning(st);
  CHECK(c2.param_get(fk::T_BAND, v) && v.value == 15);
  CHECK(c2.param_get(fk::T_FUEL_TRIM, v) && v.value == 0);  // live: not stored
  st.check = 0;
  FakeHeater h3;
  fk::Controller c3(&h3);
  c3.load_tuning(st);
  CHECK(c3.param_get(fk::T_BAND, v) && v.value == 20);  // damaged record: defaults
  // A record from an older build: an id this build lacks is skipped, a setting it lacks keeps its
  // default, and out-of-range values are held to their ranges.
  fk::Tuning::Store old{};
  old.check = fk::Tuning::STORE_CHECK;
  old.count = 3;
  old.items[0] = {fk::T_BAND, 30};
  old.items[1] = {0x1FF, 7};             // no such setting
  old.items[2] = {fk::B_CUTOFF, 9999};   // held to 28.0 V
  FakeHeater h5;
  fk::Controller c5(&h5);
  c5.load_tuning(old);
  CHECK(c5.param_get(fk::T_BAND, v) && v.value == 30);
  CHECK(c5.param_get(fk::B_CUTOFF, v) && v.value == 280);
  CHECK(c5.param_get(fk::F_PUMP_SIZE, v) && v.value == 220);  // not in the record: default
  old.count = fk::Tuning::STORE_MAX + 1;
  FakeHeater h6;
  fk::Controller c6(&h6);
  c6.load_tuning(old);
  CHECK(c6.param_get(fk::T_BAND, v) && v.value == 20);  // a broken count: defaults
  // Everything but the live trims is stored.
  c.tuning().save(st);
  CHECK_EQ(st.count, fk::Tuning::count() - 2);
  // A heater without parameters still has the board's.
  FakeHeater h4;
  h4.caps = 0;
  fk::Controller c4(&h4);
  CHECK_EQ(c4.param_count(), fk::Tuning::count());
  CHECK(c4.param_set(fk::T_CYCLE, 1, Source::PHONE).accepted());
  CHECK(c4.param_set(1, 1, Source::PHONE).result == Result::NOT_SUPPORTED);
  // Fault reset goes to the heater.
  h.st.fault = fk::Fault::GLOW_PLUG;
  CHECK(c.reset_fault(Source::PHONE).accepted());
  CHECK_EQ(h.fault_resets, 1);
}

// The fault and reason table (controller.cpp): every reason the safety MCU sends has a text and an
// alert; the fan stall is the one it retries; Home Assistant's texts.
static void test_fault_table() {
  using fk::Fault;
  using fk::FaultAlert;
  using fk::Reason;
  for (unsigned r = 0; r <= uint8_t(Reason::TICK); r++) {
    const char *t = fk::reason_text(uint8_t(r));
    CHECK(t != nullptr);
    if (t != nullptr) {
      CHECK((r == 0) == (t[0] == '\0'));
      CHECK(std::strlen(t) <= 70);  // fits "heater fault: <fault> (<reason> again: ...)"
    }
  }
  CHECK(fk::reason_text(uint8_t(Reason::TICK) + 1) == nullptr);  // a code not in the table yet
  CHECK(fk::reason_text(0xFF) == nullptr);
  for (unsigned r = 0; r < 256; r++)
    CHECK(fk::reason_retried(uint8_t(r)) == (r == uint8_t(Reason::FAN_STALL) || r == uint8_t(Reason::FAN_SPIN_UP)));
  // Alerts: by the reason when known, else by the fault.
  CHECK(fk::fault_alert(Fault::NONE, uint8_t(Reason::CO)) == FaultAlert::NONE);
  CHECK(fk::fault_alert(Fault::FAN, uint8_t(Reason::FAN_STALL)) == FaultAlert::BURN);
  CHECK(fk::fault_alert(Fault::CO, uint8_t(Reason::CO)) == FaultAlert::CO_ALARM);
  CHECK(fk::fault_alert(Fault::CO, 0) == FaultAlert::CO_ALARM);
  CHECK(fk::fault_alert(Fault::CONTROLLER, uint8_t(Reason::TICK)) == FaultAlert::BURN);
  CHECK(fk::fault_alert(Fault::CONTROLLER, 0) == FaultAlert::BURN);
  CHECK(fk::fault_alert(Fault::CO_SENSOR, uint8_t(Reason::CO_SENSOR)) == FaultAlert::CO_SENSOR);
  CHECK(fk::fault_alert(Fault::CO_SENSOR, 0) == FaultAlert::CO_SENSOR);
  CHECK(fk::fault_alert(Fault::CO_SENSOR, 0xEE) == FaultAlert::CO_SENSOR);  // an unknown reason: by fault
  CHECK(fk::fault_alert(Fault::GLOW_PLUG, uint8_t(Reason::PLUG_TYPE)) == FaultAlert::BURN);
  CHECK(fk::fault_alert(Fault(99), 0) == FaultAlert::BURN);  // a fault code not in the table
  CHECK(std::strcmp(fk::fault_name(Fault::CO_SENSOR), "CO sensor") == 0);
  CHECK(std::strcmp(fk::fault_name(Fault(99)), "Other") == 0);

  // The fan stall retry: only while it starts or runs fan only.
  fk::HeaterStatus s;
  s.answering = true;
  s.fault = Fault::FAN;
  s.panel_fault = uint8_t(Reason::FAN_STALL);
  const HeaterState states[] = {HeaterState::OFF,      HeaterState::STARTING, HeaterState::HEATING,
                                HeaterState::FAN_ONLY, HeaterState::STOPPING, HeaterState::COOLING};
  for (HeaterState st : states) {
    s.state = st;
    CHECK(fk::fault_retrying(s) == (st == HeaterState::STARTING || st == HeaterState::FAN_ONLY));
  }
  char text[128];
  s.state = HeaterState::STARTING;
  fk::fault_reason_text(s, text, sizeof(text));
  CHECK(std::strcmp(text, "The fan stalled: trying again") == 0);
  s.panel_fault = uint8_t(Reason::FAN_SPIN_UP);
  s.state = HeaterState::FAN_ONLY;
  CHECK(fk::fault_retrying(s));
  s.state = HeaterState::OFF;  // the lockout, or a stop during the wait
  fk::fault_reason_text(s, text, sizeof(text));
  CHECK(std::strcmp(text, "The fan did not reach its speed") == 0);
  s.fault = Fault::GLOW_PLUG;
  s.panel_fault = uint8_t(Reason::GLOW_STAGE);
  s.state = HeaterState::STARTING;
  CHECK(!fk::fault_retrying(s));
  s.fault = Fault::CO_SENSOR;
  s.panel_fault = uint8_t(Reason::CO_SENSOR);
  fk::fault_reason_text(s, text, sizeof(text));
  CHECK(std::strcmp(text, "The CO sensor reads open or failed its check") == 0);
  s.panel_fault = 0;
  fk::fault_reason_text(s, text, sizeof(text));
  CHECK(std::strcmp(text, "None") == 0);
  s.panel_fault = 42;
  fk::fault_reason_text(s, text, sizeof(text));
  CHECK(std::strcmp(text, "Reason 42") == 0);
  s.answering = false;
  fk::fault_reason_text(s, text, sizeof(text));
  CHECK(std::strcmp(text, "None") == 0);
  s.fault = Fault::FAN;
  s.panel_fault = uint8_t(Reason::FAN_STALL);
  s.state = HeaterState::STARTING;
  CHECK(!fk::fault_retrying(s));  // not answering: nothing is known
  // A normal stop's reason is shown, never alerted (there is no fault).
  s.answering = true;
  s.fault = Fault::NONE;
  s.panel_fault = uint8_t(Reason::DEADMAN);
  s.state = HeaterState::OFF;
  fk::fault_reason_text(s, text, sizeof(text));
  CHECK(std::strcmp(text, "The link from the board went silent during a burn") == 0);
  CHECK(!fk::fault_retrying(s));

  // The CO sensor's state.
  const char *const co[] = {"Not fitted", "Warming up", "Learning clean air", "OK", "Alarm", "Sensor fault", "Too cold",
                            "Unknown"};
  for (unsigned i = 0; i < 8; i++) {
    s.co_state = uint8_t(i);
    CHECK(std::strcmp(fk::co_state_name(s), co[i]) == 0);
  }
  s.co_state = 0;
  s.answering = false;
  CHECK(std::strcmp(fk::co_state_name(s), "Unknown") == 0);  // not "not fitted"

  // The heater firmware texts.
  FakeHeater h;
  fk::Controller c(&h);
  CHECK(std::strcmp(fk::heater_firmware_text(h), "Unknown") == 0);
  CHECK(std::strcmp(fk::firmware_update_status(c.status(), h), "Not available") == 0);
}

int main() {
  test_checks();
  test_same_checks_every_source();
  test_failsafe_ha_run();
  test_failsafe_not_for_other_runs();
  test_run_source_kept_until_off();
  test_run_identity();
  test_failsafe_wraps_and_disabled();
  test_room();
  test_altitude_and_events();
  test_thermostat_in_controller();
  test_thermostat_safety();
  test_tuning_params();
  test_fault_table();
  return report("controller");
}
