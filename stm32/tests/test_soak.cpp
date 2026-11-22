// Randomised soak of the whole safety MCU: the core (stm32/core/) on the heater model, driven by the
// ESP32's real link client over a byte wire, for hours of simulated time per seed. A seed draws a
// schedule of events: the user (start heat, fan or priming, stop, power, parameter writes and
// applies, CO fitted or not, fault resets, a new glow plug), the link (silence, lost bytes, noise in
// both directions), MCU and ESP32 resets mid-run, and faults (a fan that stalls and frees, a fan
// switch stuck on, an open sensor, supply sags and spikes, an overheat latch trip, glow high-side
// trips and a stuck low side, an open pump coil, a pump low side stuck on, a pump high-side fault, a
// shorted pump coil for the overcurrent cut, a hot board, no ignition, a flame-out, CO in the air,
// an open CO sensor, a dead CO sensor heater).
//
// After every call into the core the bench checks (DESIGN.md, "Burn sequence"):
//   airflow     no glow pulse, and no glow asked, without measured airflow
//   glow_caps   glow at most 120 s per attempt and 240 s in any 10 minutes since the MCU's reset
//   glow_budget the glow stage's heat budget never above its cap (one probe's heat aside)
//   fuel        a pump stroke only in a fuel step with the heartbeat running; fuel for a burn only
//               with the fan asked to turn, and not for long without measured airflow
//   fan_hot     the fan never stopped while the heat exchanger is hot after fuel went in, except a
//               fan stall retry or a fan lockout or fault
//   no_self_start  off to anything else only on a POLL with a new start number (never in a pass of
//               the safety loop, never on the first poll after a reset, never while latched)
//   stall_retries  at most 2 fan stall retries before the lockout (the count forgets as the burn's
//               does, or more leniently)
//   latch       a latched fault clears only with a fault reset the MCU accepted (or an MCU reset)
//   deadman     2 s without a valid frame: nothing still starting or running
//   co          a CO alarm or sensor fault ends a run (a burn-off and cooldown once fuel went in) and
//               latches; nothing starts while either is on; the alarm clears only after a minute
//               of clean air, the sensor fault only with a plausible reading; not fitted, neither
//   pump_cut    no stroke while the overcurrent cut is reported or a pump fault is latched, and
//               every stroke guarded by the watchdog
//   fan_false   no fan speed fault (overspeed, turning uncommanded) unless the fan switch is stuck on
//   live        and the other way round: the fan turns when asked (and nothing stops it), the glow
//               stage pulses when asked (with airflow, a cool board and room in its budget), the pump
//               strokes when asked (with a healthy stage)
//   config      the stages keep their parameters (none is a setting), and the sequencer runs on the
//               parameter table as applied
//   wire        the MCU answers exactly the frames a parser on the wire sees, and every reply
//               parses on the ESP32's side
//
//   test_soak                         the quick, fixed seeds (part of run.sh)
//   test_soak --seeds A-B --hours H   seeds A to B, H simulated hours each
//   test_soak --shrink SEED --hours H minimise a failing seed's schedule and print it as a test
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "../../esp32/components/flamekeeper/link_client.h"

extern "C" {
#include "../core/safety.h"
#include "co_sensor.h"
#include "heater_model.h"
}

using esphome::flamekeeper::LinkClient;
using esphome::flamekeeper::LinkTransport;
using esphome::flamekeeper::Mode;

namespace {

const uint32_t TICK = 10;
const uint32_t SEC = 1000;
const uint32_t MIN = 60 * SEC;
const uint32_t HOUR = 60 * MIN;

struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x2545F4914F6CDD1Dull) {}
  uint32_t next() {
    s ^= s << 13, s ^= s >> 7, s ^= s << 17;
    return uint32_t(s >> 16);
  }
  uint32_t below(uint32_t n) { return n ? next() % n : 0; }
  uint32_t range(uint32_t lo, uint32_t hi) { return lo + below(hi - lo + 1); }
};

enum Kind : uint8_t {
  U_HEAT, U_FAN, U_PRIME, U_OFF, U_POWER, U_PARAM, U_APPLY, U_DISCARD, U_DEFAULTS, U_FAULT_RESET,
  U_NEW_PLUG,
  L_SILENT,   // a: ms the ESP32 says nothing (hung)
  L_GARBAGE,  // a: bytes of noise towards the MCU, b: its seed
  L_GARBAGE_ESP,
  L_DROP,     // a: ms, b: permille of bytes lost both ways
  M_RESET,    // a: LINK_RESET_*
  F_FAN_STALL, F_FAN_STUCK_ON, F_SENSOR_OPEN, F_SUPPLY, F_OVERHEAT_LATCH, F_GLOW_TRIPS, F_GLOW_LS,
  F_PUMP_OPEN, F_PUMP_LS, F_PUMP_HS, F_PUMP_OC, F_BOARD_HOT, F_NO_IGNITION, F_FLAME_OUT, F_CO,
  E_RESET,    // the ESP32 restarts: a fresh link client
  F_CO_OPEN,  // the CO sensor reads open
  F_CO_HEATER, // the CO sensor's heater is dead: its check fails
  U_CO_FITTED, // a: the CO-fitted parameter, applied and saved
  KINDS
};
const char *const KIND_NAMES[KINDS] = {
    "U_HEAT", "U_FAN", "U_PRIME", "U_OFF", "U_POWER", "U_PARAM", "U_APPLY", "U_DISCARD", "U_DEFAULTS",
    "U_FAULT_RESET", "U_NEW_PLUG", "L_SILENT", "L_GARBAGE", "L_GARBAGE_ESP", "L_DROP", "M_RESET",
    "F_FAN_STALL", "F_FAN_STUCK_ON", "F_SENSOR_OPEN", "F_SUPPLY", "F_OVERHEAT_LATCH", "F_GLOW_TRIPS",
    "F_GLOW_LS", "F_PUMP_OPEN", "F_PUMP_LS", "F_PUMP_HS", "F_PUMP_OC", "F_BOARD_HOT", "F_NO_IGNITION",
    "F_FLAME_OUT", "F_CO", "E_RESET", "F_CO_OPEN", "F_CO_HEATER", "U_CO_FITTED"};

// One scheduled event. Faults: a = 1 on, 0 off (F_SUPPLY: the supply, 0.01 V; F_BOARD_HOT: 0.1 C;
// F_CO: the sensor's Rs / R0 in 1/1000, 1000 clean air).
struct Ev {
  uint32_t t;
  uint8_t kind;
  uint32_t a, b;
};

struct Stats {
  uint64_t sim_ms = 0;
  uint32_t starts = 0, running = 0, stalls = 0, lockouts = 0, deadman = 0, resets = 0, latched = 0,
           fault_resets = 0, glow_s = 0, frames = 0, bad_frames = 0, events = 0, co_alarms = 0, co_faults = 0,
           pump_cuts = 0;
  void add(const Stats &o) {
    sim_ms += o.sim_ms, starts += o.starts, running += o.running, stalls += o.stalls;
    lockouts += o.lockouts, deadman += o.deadman, resets += o.resets, latched += o.latched;
    fault_resets += o.fault_resets, glow_s += o.glow_s, frames += o.frames, bad_frames += o.bad_frames;
    events += o.events, co_alarms += o.co_alarms, co_faults += o.co_faults, pump_cuts += o.pump_cuts;
  }
};

struct Bench : LinkTransport {
  safety s;
  heater_model m;
  uint32_t now = 0;
  LinkClient client{this};
  std::deque<uint8_t> to_esp;
  Stats st;

  // what "flash" keeps across MCU resets
  std::vector<uint8_t> params_flash, plug_flash;

  // the board
  int16_t board_c10 = 350;
  double plug_heat = 0, last_glow_i = 0;
  uint16_t fan_duty = 0;
  int hb_running_ms = 0;
  int latch_armed = 0;
  uint16_t test_pulse = 0;
  int glow_en = 0;
  double pump_i = 0;
  uint8_t last_result = 0;
  bool pulsed = false, stroked = false;  // this pass
  bool fan_stop_excused = false;
  bool fan_stuck_seen = false;  // the fan switch was stuck on since the last reset of its fault
  uint32_t fan_slow_ms = 0, glow_wait_ms = 0, pump_wait_ms = 0;

  // faults
  bool fan_stuck_on = false, glow_trips = false, glow_ls = false, pump_open = false, pump_ls = false,
       pump_hs = false, pump_oc = false, overheat_trip = false;
  uint32_t co_ratio = 1000;    // the CO sensor: Rs / R0 in 1/1000
  bool co_open = false, co_heater_dead = false;
  uint32_t co_r0_flash = 0;    // the clean-air R0 kept in flash
  uint8_t pump_cut = 0;        // the overcurrent cut, reported until the core latches it (board.c)
  bool co_shadow = false;      // the CO module had an alarm or fault last pass
  bool co_alarm_shadow = false, co_fault_shadow = false;
  uint32_t co_clean_ms = 0;  // the air clean since

  // the link
  bool esp_alive = true;
  uint32_t silent_until = 0, drop_until = 0;
  uint32_t drop_permille = 0;
  Rng wire_rng{1};

  // oracles
  std::string violation;
  uint32_t violation_ms = 0;
  link_parser mirror;            // what the MCU's parser should see
  uint8_t oracle_last = 0;       // the last start number the MCU took from a POLL it answered
  bool oracle_seen = false;      // a POLL answered since the MCU's reset
  bool heard = false;
  uint32_t last_frame_ms = 0;
  bool latched_shadow = false;
  bool fuel_went_in = false;
  uint32_t no_air_fuel_ms = 0;
  uint32_t attempt_glow_ms = 0;
  uint8_t prev_step = BURN_STEP_OFF;
  std::vector<uint8_t> glow_ring;  // 10 ms ticks with glow, the last 10 minutes
  size_t glow_ring_i = 0;
  uint32_t glow_window_ticks = 0;
  uint32_t stall_retries = 0, turning_ms = 0;
  uint64_t budget_cap_a2us = 0;

  Bench() : glow_ring(10 * MIN / TICK, 0) {
    heater_model_init(&m, 10);
    link_parser_init(&mirror, LINK_REQUEST);
    init_mcu(LINK_RESET_POWER_ON);
  }

  void fail(const char *invariant, const char *detail) {
    if (!violation.empty())
      return;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s at %.2f s: %s (step %u, fault %u/%u, hx %d, fan %u rpm)", invariant,
                  now / 1000.0, detail, s.burn.step, s.burn.fault, s.burn.why, heater_model_hx(&m),
                  unsigned(m.fan_rpm));
    violation = buf;
    violation_ms = now;
  }

  burn_inputs inputs() const {
    burn_inputs in;
    std::memset(&in, 0, sizeof(in));
    in.hx_c10 = heater_model_hx(&m);
    in.fan_rpm = uint16_t(m.fan_rpm + 0.5);
    in.supply_cv = m.supply_cv;
    in.overheat_latch = uint8_t(!latch_armed && overheat_trip);
    in.co_alarm = 0;  // the core's own CO alarm (safety_co_step) is the path
    in.board_c10 = board_c10;
    return in;
  }

  void init_mcu(uint8_t reason) {
    burn_inputs in = inputs();
    safety_init(&s, now, &in, reason, 0);
    if (!params_flash.empty())
      safety_load_params(&s, params_flash.data(), params_flash.size());
    if (!plug_flash.empty())
      safety_load_plug(&s, plug_flash.data(), plug_flash.size());
    if (co_r0_flash)
      safety_load_co_r0(&s, co_r0_flash);
    pump_cut = 0;
    co_shadow = co_alarm_shadow = co_fault_shadow = false;
    latch_armed = 0;  // a reset leaves the latch tripped until the power-up test re-arms it
    hb_running_ms = 0;
    link_parser_init(&mirror, LINK_REQUEST);
    oracle_seen = false;
    heard = false;
    latched_shadow = s.burn.fault_latched;
    fan_stuck_seen = fan_stuck_on;
    stall_retries = 0, turning_ms = 0;
    attempt_glow_ms = 0;
    std::fill(glow_ring.begin(), glow_ring.end(), 0);  // the window is the MCU's since its reset
    glow_window_ticks = 0;
    prev_step = s.burn.step;
    budget_cap_a2us = uint64_t(s.glow.p.bucket_cap_a2s) * 1000000u;
  }

  // ---------------------------------------------------------------- the wire

  bool lost() { return now < drop_until && wire_rng.below(1000) < drop_permille; }

  void to_mcu(uint8_t byte) {
    uint8_t before_step = s.burn.step;
    bool latched_before = s.burn.fault_latched;
    int framed = link_parser_feed(&mirror, byte);
    uint8_t reply[LINK_FRAME_MAX];
    size_t n = safety_receive(&s, byte, now, reply);
    if ((n > 0) != (framed == 1))
      return fail("wire", "the MCU and a parser on the wire disagree about a frame");
    bool token = false, reset_ok = false;
    if (n > 0) {
      st.frames++;
      heard = true;
      last_frame_ms = now;
      link_parser rp;
      link_parser_init(&rp, LINK_REPLY);
      int whole = 0;
      for (size_t i = 0; i < n; i++)
        whole += link_parser_feed(&rp, reply[i]);
      if (whole != 1 || link_frame_len(rp.buf) != n || link_frame_seq(reply) != link_frame_seq(mirror.buf))
        return fail("wire", "a reply that does not parse as one frame");
      uint8_t op = link_frame_op(mirror.buf), result = link_reply_result(reply);
      if (op == LINK_OP_POLL && result == LINK_OK) {
        link_desired want;
        link_decode_desired(link_request_payload(mirror.buf), link_request_payload_len(mirror.buf), &want);
        token = oracle_seen && want.start != oracle_last && want.mode != LINK_MODE_OFF;
        oracle_last = want.start;
        oracle_seen = true;
        link_status ss;
        if (!link_decode_status(link_reply_payload(reply), link_reply_payload_len(reply), &ss))
          return fail("wire", "a status that does not decode");
      }
      to_esp.insert(to_esp.end(), reply, reply + n);
      if (op == LINK_OP_FAULT_RESET && result == LINK_OK)
        reset_ok = true, st.fault_resets++, stall_retries = 0, fan_stop_excused = false;  // the lockout goes too
      if (reset_ok)
        fan_stuck_seen = fan_stuck_on;
    }
    if (before_step == BURN_STEP_OFF && s.burn.step == BURN_STEP_COOLDOWN && reset_ok) {
      // A fault reset on a hot heater cools it down, as after an MCU reset: no fuel, no glow.
    } else if (before_step == BURN_STEP_OFF && s.burn.step != BURN_STEP_OFF) {
      st.starts++;
      if (!token)
        return fail("no_self_start", "a start without a new start number");
      if (latched_before)
        return fail("no_self_start", "a start with a latched fault");
      if (s.co.alarm || s.co.fault)
        return fail("co", "a start with a CO alarm or sensor fault");
    }
    if (latched_shadow && !s.burn.fault_latched && !reset_ok)
      return fail("latch", "a latched fault cleared without a fault reset");
    if (latched_shadow && !s.burn.fault_latched && (s.co.alarm || s.co.fault))
      return fail("co", "a fault reset cleared the latch while the CO module still holds one");
    latched_shadow = s.burn.fault_latched;
  }

  void send(const uint8_t *data, size_t len) override {
    for (size_t i = 0; i < len; i++)
      if (!lost())
        to_mcu(data[i]);
  }
  int read() override {
    while (!to_esp.empty()) {
      int b = to_esp.front();
      to_esp.pop_front();
      if (!lost())
        return b;
    }
    return -1;
  }

  // ---------------------------------------------------------------- the board, a millisecond and a pass

  void ms(uint32_t t) {
    bool hb_ok = hb_running_ms >= 1;
    bool pump_plus = hb_ok && latch_armed && !pump_hs;
    double vin = m.supply_cv / 100.0;
    checks_obs co_;
    co_.vin_cv = m.supply_cv;
    co_.pump_v_cv = pump_plus ? m.supply_cv : 30;
    co_.glow_v_cv = (glow_en && hb_ok) ? m.supply_cv : 0;
    co_.glow_ca = glow_ls && hb_ok ? 5000 : 0;
    co_.pump_peak_ma = test_pulse ? (pump_plus && !pump_open ? 400 : 3) : 0;
    co_.latch_armed = uint8_t(latch_armed);
    co_.pump_hs_ok = !pump_hs;
    co_.hx_c10 = heater_model_hx(&m);
    pump_meas pm;
    pm.vin_cv = m.supply_cv;
    pm.i_ma = uint16_t(pump_i * 1000);
    pm.overcurrent = pump_cut;  // PMP-7: reported until the core latches it (board.c)
    pm.hs_ok = !pump_hs;
    pm.hb_running_ms = 0;
    uint8_t step_before = s.burn.step;
    safety_io out = safety_ms(&s, t, &co_, &pm);
    if (step_before == BURN_STEP_OFF && s.burn.step != BURN_STEP_OFF)
      fail("no_self_start", "the millisecond step left off");
    if (out.pump_duty && pump_cut)
      fail("pump_cut", "a stroke while the overcurrent cut is reported");
    if (out.pump_duty && s.pump.fault != PUMP_FAULT_NONE)
      fail("pump_cut", "a stroke with a pump fault latched");
    if (out.pump_duty && !out.pump_oc_armed)
      fail("pump_cut", "a stroke the overcurrent watchdog does not guard");
    if (s.pump.fault != PUMP_FAULT_NONE)
      pump_cut = 0;
    // A shorted coil: the watchdog cuts an armed stroke at once, and the coil gets no drive.
    bool cut_now = pump_oc && pump_plus && out.pump_duty && out.pump_oc_armed;
    if (cut_now && !pump_cut)
      pump_cut = 1, st.pump_cuts++;
    if (out.pump_duty) {
      stroked = true;
      uint8_t st_ = s.burn.step;
      bool fuel_step = st_ == BURN_STEP_IGNITION || st_ == BURN_STEP_WARM_UP || st_ == BURN_STEP_RUNNING ||
                       st_ == BURN_STEP_PRIMING;
      if (out.heartbeat != CHECKS_HB_RUNNING || s.burn.out.pump_cHz == 0 || !fuel_step)
        fail("fuel", "a pump stroke outside a fuel step or without the heartbeat");
      if (st_ != BURN_STEP_PRIMING)
        fuel_went_in = true;
    }
    double v = (pump_plus && out.pump_duty && !pump_open && !pump_cut) ? out.pump_duty / 1000.0 * vin : 0;
    if (pump_ls && pump_plus && !pump_open)
      v = vin;  // the low side conducts: the coil is on whenever its + side is
    pump_i += (v / 6.0 - pump_i) * 0.393;
    hb_running_ms = out.heartbeat == CHECKS_HB_RUNNING ? hb_running_ms + 1 : 0;
    if (out.latch_reset && !(overheat_trip && heater_model_hx(&m) > 2700))
      latch_armed = 1, overheat_trip = false;
    test_pulse = out.pump_pulse_us;
    glow_en = out.glow_en;
  }

  void glow_period(uint32_t t) {
    double vin = m.supply_cv / 100.0;
    double r = 0.2 + 0.8 * plug_heat;
    double i = vin / (r + 0.02);
    glow_meas gm;
    gm.vin_cv = m.supply_cv;
    gm.pulse_ca = uint16_t(last_glow_i * 100 + 0.5);
    gm.gap_ca = glow_ls && s.glow.on ? uint16_t(i * 100) : 0;
    gm.airflow_ok = m.fan_rpm >= s.burn.p.glow_min_fan_rpm;
    gm.board_c10 = board_c10;
    gm.high_side_trip = glow_trips && s.glow.on;
    uint8_t step_before = s.burn.step;
    glow_cmd c = safety_glow_period(&s, t, &gm);
    if (step_before == BURN_STEP_OFF && s.burn.step != BURN_STEP_OFF)
      fail("no_self_start", "the glow period left off");
    if (c.width_us > 0) {
      pulsed = true;
      if (s.in.fan_rpm < s.burn.p.glow_min_fan_rpm || m.fan_rpm < s.burn.p.glow_min_fan_rpm - 100)
        fail("airflow", "a glow pulse without measured airflow");
    }
    // The budget: no pulse overfills it; a probe (150 us, allowed while any room is left) may.
    uint64_t probe = uint64_t(20000) * 20000 * s.glow.p.min_pulse_us / 10000u;
    if (s.glow.bucket_a2us > budget_cap_a2us + probe)
      fail("glow_budget", "the glow heat budget over its cap");
    last_glow_i = c.width_us ? i : 0;
    double w = i * i * r * c.width_us / 5000.0;
    plug_heat += (w / 90.0 - plug_heat) * (5.0 / 8000.0);
  }

  void tick() {
    const burn_outputs *o = safety_outputs(&s);
    for (uint32_t i = 0; i < TICK; i++)
      ms(now + i);
    glow_period(now);
    glow_period(now + 5);

    // The fan: the motor follows its average voltage (10 V: 5000 rpm), with a stuck-on switch at
    // the full supply; a frozen fan does not turn at all.
    double vin = m.supply_cv / 100.0;
    double vmotor = fan_stuck_on ? vin : fan_duty / 1000.0 * vin;
    uint16_t fan_target = uint16_t(vmotor / 10.0 * 5000.0);
    heater_model_step(&m, TICK, fan_target, o->pump_cHz, uint16_t(plug_heat > 0.5 ? 90 : 0));
    now += TICK;
    st.sim_ms += TICK;

    burn_inputs in = inputs();
    if (now % SEC == 0) {
      safety_co_step(&s, now, co_reading(), board_c10);
      uint32_t r0 = safety_co_r0_to_save(&s);
      if (r0)
        co_r0_flash = r0, safety_co_r0_saved(&s, r0);
    }
    uint8_t step_before = s.burn.step;
    bool fuel_before = s.burn.step == BURN_STEP_IGNITION || s.burn.step == BURN_STEP_PURGE ||
                       s.burn.step == BURN_STEP_WARM_UP || s.burn.step == BURN_STEP_RUNNING;
    safety_step(&s, now, &in);
    check_co(fuel_before);
    if (step_before == BURN_STEP_OFF && s.burn.step != BURN_STEP_OFF)
      fail("no_self_start", "a pass of the safety loop left off");
    if (latched_shadow && !s.burn.fault_latched)
      fail("latch", "a latched fault cleared in a pass of the safety loop");
    if (!latched_shadow && s.burn.fault_latched)
      st.latched++;
    latched_shadow = s.burn.fault_latched;

    double emf = m.fan_rpm / 5000.0 * 9.0;
    double i_motor = vmotor > emf ? (vmotor - emf) / 2.0 : 0;
    fan_meas fm;
    fm.rpm = in.fan_rpm;
    fm.vin_cv = m.supply_cv;
    fm.isense_ma = uint16_t(1000.0 * i_motor * (fan_stuck_on ? 1.0 : fan_duty / 1000.0));
    fan_duty = safety_fan_step(&s, now, &fm);

    check_pass(in);

    if (safety_params_save_pending(&s)) {
      params_flash.assign(PARAMS_IMAGE_MAX, 0);
      size_t n = params_pack(&s.params, params_flash.data(), params_flash.size());
      params_flash.resize(n);
      safety_params_saved(&s);
    }
    if (safety_plug_save_pending(&s)) {
      plug_flash.assign(PLUG_IMAGE_SIZE, 0);
      plug_pack(&s.plug, plug_flash.data(), plug_flash.size());
      safety_plug_saved(&s);
    }
    esp_alive = now >= silent_until;
    if (esp_alive)
      client.loop(now);
  }

  // The CO sensor (co_sensor.h), open, or with a dead heater: no change, so its check fails.
  uint16_t co_reading() const {
    if (co_open)
      return 10;
    return co_sensor_adc(co_ratio, safety_co_heater(&s) || co_heater_dead);
  }

  // CO (CO-1, CO-2): an alarm or a sensor fault ends a run as any fault does, with a burn-off and
  // a cooldown once fuel went in, and latches; nothing starts while either is on; not fitted,
  // neither ever comes.
  void check_co(bool fuel_before) {
    bool co_now = s.co.alarm || s.co.fault;
    // The alarm clears only after a minute of clean air, the sensor fault only with a plausible
    // reading (the bench's own view of the air and the sensor, not the CO module's).
    if (co_alarm_shadow && !s.co.alarm && (co_ratio < s.co.cfg.clear_ratio || now - co_clean_ms < s.co.cfg.clear_ms))
      fail("co", "the CO alarm cleared without a minute of clean air");
    if (co_fault_shadow && !s.co.fault && co_open && s.co.cfg.fitted)  // "not fitted" clears it (CO-1)
      fail("co", "the CO sensor fault cleared with the sensor still open");
    st.co_alarms += s.co.alarm && !co_alarm_shadow;
    st.co_faults += s.co.fault && !co_fault_shadow;
    co_alarm_shadow = s.co.alarm, co_fault_shadow = s.co.fault;
    if (!s.co.cfg.fitted && co_now)
      fail("co", "a CO alarm or sensor fault with no sensor fitted");
    if (s.co.cfg.fitted != (s.burn.p.co_fitted ? 1 : 0))
      fail("co", "the CO alarm does not follow the CO-fitted parameter");
    if (co_now) {
      if (!s.burn.fault_latched)
        fail("co", "a CO alarm or sensor fault without a latched fault");
      if (burn_active(&s.burn))
        fail("co", "still running with a CO alarm or sensor fault");
      if (!co_shadow && fuel_before && s.burn.step != BURN_STEP_BURN_OFF && s.burn.step != BURN_STEP_COOLDOWN)
        fail("co", "a CO stop after fuel without a burn-off and cooldown");
    }
    co_shadow = co_now;
  }

  // Invariants after each pass.
  void check_pass(const burn_inputs &in) {
    const burn_params &p = s.burn.p;
    const burn_outputs &o = s.burn.out;
    uint8_t step = s.burn.step;

    if (o.glow_w > 0 && in.fan_rpm < p.glow_min_fan_rpm)
      fail("airflow", "glow asked without measured airflow");

    // Glow caps: per attempt (a new one from each entry into preheat), and in any 10 minutes.
    if (step == BURN_STEP_PREHEAT && prev_step != BURN_STEP_PREHEAT)
      attempt_glow_ms = 0;
    bool glowing = o.glow_w > 0;
    if (glowing)
      attempt_glow_ms += TICK;
    glow_window_ticks -= glow_ring[glow_ring_i];
    glow_ring[glow_ring_i] = glowing;
    glow_window_ticks += glowing;
    glow_ring_i = (glow_ring_i + 1) % glow_ring.size();
    if (attempt_glow_ms > uint32_t(p.glow_attempt_max_s) * SEC + TICK)
      fail("glow_caps", "glow over its cap for one attempt");
    if (glow_window_ticks * TICK > uint32_t(p.glow_window_max_s) * SEC + TICK)
      fail("glow_caps", "glow over its cap in 10 minutes");

    // Fuel follows air.
    bool burn_fuel = o.pump_cHz > 0 && step != BURN_STEP_PRIMING;
    if (burn_fuel && o.fan_rpm == 0)
      fail("fuel", "fuel asked with the fan stopped");
    if (burn_fuel && in.fan_rpm < p.fan_stall_rpm) {
      no_air_fuel_ms += TICK;
      if (no_air_fuel_ms > 1500)
        fail("fuel", "fuel for more than 1.5 s without measured airflow");
    } else {
      no_air_fuel_ms = 0;
    }

    // The fan keeps a hot heater cooling once fuel went in.
    bool hot = in.hx_c10 != BURN_UNKNOWN_TEMP && in.hx_c10 >= p.hot_start_c10;
    if (step == BURN_STEP_OFF && !hot)
      fuel_went_in = false;
    // A stall or a lockout stops the fan at once; it stays stopped until it is next asked to turn or
    // the lockout is reset (a reset that finds the heater hot cools it down).
    if (step == BURN_STEP_FAN_RETRY || (s.burn.fault_latched && s.burn.fault == BURN_FAULT_FAN) ||
        s.fan.fault != FAN_FAULT_NONE)
      fan_stop_excused = true;
    else if (o.fan_rpm > 0)
      fan_stop_excused = false;
    if (fuel_went_in && hot && o.fan_rpm == 0 && !fan_stop_excused)
      fail("fan_hot", "the fan stopped with a hot heat exchanger after fuel went in");

    // Fan stall retries.
    if (step == BURN_STEP_FAN_RETRY && prev_step != BURN_STEP_FAN_RETRY) {
      st.stalls++;
      if (++stall_retries > p.fan_retries)
        fail("stall_retries", "more fan stall retries than allowed before a lockout");
      turning_ms = 0;
    }
    if (s.burn.fault_latched && s.burn.fault == BURN_FAULT_FAN && s.burn.why == BURN_WHY_FAN_STALL &&
        prev_step != step && step == BURN_STEP_OFF)
      st.lockouts++;
    if (o.fan_rpm > 0 && in.fan_rpm >= p.fan_stall_rpm && step != BURN_STEP_FAN_RETRY) {
      turning_ms += TICK;
      if (turning_ms >= uint32_t(p.fan_retry_forget_s) * SEC)
        stall_retries = 0;
    }

    // The fan turns only from its duty here, unless its switch is stuck on: a speed fault (FAN-3,
    // FAN-6) without that is a false alarm.
    fan_stuck_seen = fan_stuck_seen || fan_stuck_on;
    if ((s.fan.fault == FAN_FAULT_OVERSPEED || s.fan.fault == FAN_FAULT_UNCOMMANDED) && !fan_stuck_seen)
      fail("fan_false", "a fan speed fault with the fan driven only by its duty");

    // The stages' parameters are not settings: they never change.
    glow_params gp;
    glow_params_default(&gp);
    fan_params fp;
    fan_params_default(&fp);
    pump_params pp;
    pump_params_default(&pp);
    checks_params cp;
    checks_params_default(&cp);
    if (std::memcmp(&gp, &s.glow.p, sizeof(gp)) || std::memcmp(&fp, &s.fan.p, sizeof(fp)) ||
        std::memcmp(&pp, &s.pump.p, sizeof(pp)) || std::memcmp(&cp, &s.chk.p, sizeof(cp)))
      fail("config", "a stage's parameters changed");
    if (std::memcmp(&s.burn.p, &s.params.live, sizeof(burn_params)))
      fail("config", "the sequencer does not run on the applied parameters");

    // Liveness: what the sequencer asks of a healthy stage happens.
    bool fan_can = o.fan_rpm >= 1000 && !m.fan_stalled && !fan_stuck_on && s.fan.fault == FAN_FAULT_NONE &&
                   m.supply_cv >= 1000;
    fan_slow_ms = fan_can && in.fan_rpm < o.fan_rpm / 2 ? fan_slow_ms + TICK : 0;
    if (fan_slow_ms > 6 * SEC)
      fail("live", "the fan stage does not turn a free fan");
    bool glow_can = o.glow_w > 0 && m.fan_rpm >= p.glow_min_fan_rpm && board_c10 <= s.glow.p.board_max_c10 &&
                    !glow_trips && !glow_ls && s.glow.fault == GLOW_FAULT_NONE &&
                    s.glow.bucket_a2us < budget_cap_a2us / 2;
    glow_wait_ms = glow_can && !pulsed ? glow_wait_ms + TICK : 0;
    if (glow_wait_ms > 2 * SEC)
      fail("live", "the glow stage does not pulse");
    bool pump_can = o.pump_cHz > 0 && s.pump.fault == PUMP_FAULT_NONE && latch_armed && !pump_hs && !pump_open &&
                    !pump_ls && !pump_oc;
    pump_wait_ms = pump_can && !stroked ? pump_wait_ms + TICK : 0;
    if (pump_wait_ms > 3 * SEC)
      fail("live", "the pump does not stroke");
    pulsed = stroked = false;

    // The dead-man.
    uint8_t state = burn_state(&s.burn);
    bool running = state == BURN_STATE_STARTING || state == BURN_STATE_HEATING || state == BURN_STATE_FAN_ONLY ||
                   state == BURN_STATE_PRIMING;
    if (running && (!heard || now - last_frame_ms >= LINK_DEADMAN_MS + TICK))
      fail("deadman", "still running 2 s after the last valid frame");
    if (s.burn.why == BURN_WHY_DEADMAN && prev_step != step && (step == BURN_STEP_BURN_OFF || step == BURN_STEP_OFF) &&
        (prev_step != BURN_STEP_BURN_OFF && prev_step != BURN_STEP_COOLDOWN))
      st.deadman++;
    if (step == BURN_STEP_RUNNING && prev_step != BURN_STEP_RUNNING)
      st.running++;
    if (trace && s.start_result != last_result)
      std::printf("%10.2f   start result %u (fault %u why %u)\n", now / 1000.0, s.start_result, s.burn.fault, s.burn.why);
    last_result = s.start_result;
    if (trace && step != prev_step)
      std::printf("%10.2f   step %u -> %u (fault %u why %u%s, start result %u)\n", now / 1000.0, prev_step, step,
                  s.burn.fault, s.burn.why, s.burn.fault_latched ? " latched" : "", s.start_result);
    if (glowing && (now % SEC) == 0)
      st.glow_s++;
    prev_step = step;
  }

  // ---------------------------------------------------------------- events

  bool trace = std::getenv("SOAK_TRACE") != nullptr;

  void user_start(Mode mode) {
    esphome::flamekeeper::Outcome o = client.set_mode(mode);
    if (o.result == esphome::flamekeeper::Result::NOT_OFF)
      client.set_mode(Mode::OFF);  // running in another mode: the user stops it first
    if (trace)
      std::printf("%10.2f   start %u: %s\n", now / 1000.0, unsigned(mode), o.accepted() ? "sent" : o.reason);
  }

  void apply(const Ev &e) {
    st.events++;
    if (trace)
      std::printf("%10.2f %s %u %u\n", now / 1000.0, KIND_NAMES[e.kind], e.a, e.b);
    switch (e.kind) {
      case U_HEAT: user_start(Mode::HEAT); break;
      case U_FAN: user_start(Mode::FAN); break;
      case U_PRIME: user_start(Mode::PRIME); break;
      case U_OFF: client.set_mode(Mode::OFF); break;
      case U_POWER: client.set_power_demand(uint8_t(e.a)); break;
      case U_PARAM: {
        const param_def &d = PARAM_TABLE[e.a % PARAM_COUNT];
        int32_t v = e.b & 0x80000000u ? int32_t(e.b) : d.min + int32_t(e.b % uint32_t((d.max - d.min) / (d.step ? d.step : 1) + 1)) * d.step;
        client.param_set(d.id, v);
        break;
      }
      case U_APPLY: client.param_apply(e.a != 0); break;
      case U_DISCARD: client.param_discard(); break;
      case U_DEFAULTS: client.reset_parameters(); break;
      case U_FAULT_RESET: client.reset_fault(); break;
      case U_NEW_PLUG: client.glow_plug_fitted(); break;
      case L_SILENT: silent_until = now + e.a; break;
      case L_GARBAGE:
      case L_GARBAGE_ESP: {
        Rng r(e.b);
        for (uint32_t i = 0; i < e.a; i++) {
          uint8_t b = uint8_t(r.below(4) == 0 ? (e.kind == L_GARBAGE ? LINK_REQUEST : LINK_REPLY) : r.next());
          if (e.kind == L_GARBAGE)
            to_mcu(b);
          else
            to_esp.push_back(b);
        }
        break;
      }
      case L_DROP:
        drop_until = now + e.a, drop_permille = e.b % 1000, wire_rng = Rng(e.b);
        break;
      case M_RESET:
        st.resets++;
        init_mcu(uint8_t(e.a));
        break;
      case E_RESET:
        client = LinkClient(this);
        to_esp.clear();
        break;
      case F_FAN_STALL: m.fan_stalled = int(e.a); break;
      case F_FAN_STUCK_ON: fan_stuck_on = e.a != 0; break;
      case F_SENSOR_OPEN: m.sensor_open = int(e.a); break;
      case F_SUPPLY: m.supply_cv = uint16_t(e.a); break;
      case F_OVERHEAT_LATCH:
        if (e.a)
          overheat_trip = true, latch_armed = 0;
        break;
      case F_GLOW_TRIPS: glow_trips = e.a != 0; break;
      case F_GLOW_LS: glow_ls = e.a != 0; break;
      case F_PUMP_OPEN: pump_open = e.a != 0; break;
      case F_PUMP_LS: pump_ls = e.a != 0; break;
      case F_PUMP_HS: pump_hs = e.a != 0; break;
      case F_PUMP_OC: pump_oc = e.a != 0; break;
      case F_BOARD_HOT: board_c10 = int16_t(e.a); break;
      case F_NO_IGNITION: m.no_ignition = int(e.a); break;
      case F_FLAME_OUT: m.kill_flame = int(e.a); break;
      case F_CO:
        if (e.a >= 300 && co_ratio < 300)
          co_clean_ms = now;
        co_ratio = e.a;
        break;
      case F_CO_OPEN: co_open = e.a != 0; break;
      case F_CO_HEATER: co_heater_dead = e.a != 0; break;
      case U_CO_FITTED:
        client.param_set(0x52, int32_t(e.a));
        client.param_apply(true);
        break;
      default: break;
    }
  }
};

bool is_fault(uint32_t kind) { return kind >= F_FAN_STALL && kind != E_RESET && kind != U_CO_FITTED; }

// A seed's schedule: events about every two minutes on average, faults switched off again after a
// while (some only by the next MCU reset).
std::vector<Ev> schedule(uint32_t seed, uint32_t hours) {
  Rng r(seed);
  std::vector<Ev> ev;
  const uint32_t end = hours * HOUR;
  struct W {
    uint8_t kind;
    uint32_t weight;
  };
  const W weights[] = {{U_HEAT, 40}, {U_FAN, 6},  {U_PRIME, 3}, {U_OFF, 16}, {U_POWER, 12}, {U_PARAM, 6},
                       {U_APPLY, 4}, {U_DISCARD, 1}, {U_DEFAULTS, 1}, {U_FAULT_RESET, 10}, {U_NEW_PLUG, 2},
                       {L_SILENT, 5}, {L_GARBAGE, 5}, {L_GARBAGE_ESP, 3}, {L_DROP, 3}, {M_RESET, 3},
                       {F_FAN_STALL, 3}, {F_FAN_STUCK_ON, 1}, {F_SENSOR_OPEN, 2}, {F_SUPPLY, 3},
                       {F_OVERHEAT_LATCH, 1}, {F_GLOW_TRIPS, 2}, {F_GLOW_LS, 1}, {F_PUMP_OPEN, 1},
                       {F_PUMP_LS, 1}, {F_PUMP_HS, 1}, {F_PUMP_OC, 1}, {F_BOARD_HOT, 2}, {F_NO_IGNITION, 3},
                       {F_FLAME_OUT, 2}, {F_CO, 2}, {E_RESET, 2}, {F_CO_OPEN, 1}, {F_CO_HEATER, 1},
                       {U_CO_FITTED, 1}};
  uint32_t total = 0;
  for (const W &w : weights)
    total += w.weight;
  uint32_t t = r.range(1 * SEC, 30 * SEC);
  while (t < end) {
    uint32_t pick = r.below(total), kind = 0;
    for (const W &w : weights) {
      if (pick < w.weight) {
        kind = w.kind;
        break;
      }
      pick -= w.weight;
    }
    Ev e{t, uint8_t(kind), 0, 0};
    uint32_t off_after = 0;
    switch (kind) {
      case U_POWER: e.a = r.below(101); break;
      case U_PARAM: e.a = r.below(64), e.b = r.below(8) == 0 ? r.next() | 0x80000000u : r.next(); break;
      case U_APPLY: e.a = r.below(2); break;
      case L_SILENT: e.a = r.below(3) == 0 ? r.range(100, 2500) : r.range(1 * SEC, 20 * SEC); break;
      case L_GARBAGE:
      case L_GARBAGE_ESP: e.a = r.range(1, 200), e.b = r.next(); break;
      case L_DROP: e.a = r.range(1 * SEC, 30 * SEC), e.b = r.range(1, 500) + 1000 * r.below(1000); break;
      case M_RESET: e.a = r.range(LINK_RESET_POWER_ON, LINK_RESET_SOFTWARE); break;
      case F_SUPPLY: {
        uint32_t c = r.below(4);
        e.a = c == 0 ? r.range(800, 1100) : c == 1 ? r.range(2800, 3300) : c == 2 ? r.range(1800, 2700) : r.range(1100, 1300);
        off_after = r.range(100, 20 * SEC);
        break;
      }
      case F_BOARD_HOT: e.a = r.range(800, 1150), off_after = r.range(5 * SEC, 10 * MIN); break;
      case F_OVERHEAT_LATCH: e.a = 1; break;
      case F_CO: {
        uint32_t c = r.below(3);
        e.a = c == 0 ? r.range(1, 10) : c == 1 ? r.range(11, 30) : r.range(31, 600);
        off_after = r.range(5 * SEC, 10 * MIN);
        break;
      }
      case U_CO_FITTED: e.a = r.below(3) ? 1 : 0; break;
      default:
        if (is_fault(kind)) {
          e.a = 1;
          off_after = r.below(2) ? r.range(100, 3 * SEC) : r.range(3 * SEC, 15 * MIN);
        }
        break;
    }
    ev.push_back(e);
    if (off_after) {
      Ev off = e;
      off.t = t + off_after;
      off.a = kind == F_SUPPLY ? 1260 : kind == F_BOARD_HOT ? 350 : kind == F_CO ? 1000 : 0;
      ev.push_back(off);
    }
    // The user notices: a fault reset once the fault has gone, and a power cycle for a tripped latch.
    if (is_fault(kind) && r.below(2))
      ev.push_back(Ev{t + off_after + r.range(1 * SEC, 5 * MIN), U_FAULT_RESET, 0, 0});
    if (kind == F_OVERHEAT_LATCH)
      ev.push_back(Ev{t + r.range(10 * SEC, 30 * MIN), M_RESET, LINK_RESET_POWER_ON, 0});
    // Mostly minutes apart, sometimes in a burst.
    t += r.below(4) == 0 ? r.range(50, 5 * SEC) : r.range(5 * SEC, 5 * MIN);
  }
  std::stable_sort(ev.begin(), ev.end(), [](const Ev &x, const Ev &y) { return x.t < y.t; });
  return ev;
}

struct Outcome {
  std::string violation;
  uint32_t at_ms = 0;
  Stats st;
};

Outcome run(const std::vector<Ev> &ev, uint32_t end_ms) {
  Bench *b = new Bench;
  size_t next = 0;
  while (b->now < end_ms && b->violation.empty()) {
    while (next < ev.size() && ev[next].t <= b->now)
      b->apply(ev[next++]);
    b->tick();
  }
  Outcome o;
  o.violation = b->violation;
  o.at_ms = b->violation_ms;
  o.st = b->st;
  o.st.bad_frames = b->s.parser.bad;
  delete b;
  return o;
}

std::string invariant_of(const std::string &v) { return v.substr(0, v.find(' ')); }

// Drops events while the same invariant still fails, coarse to fine.
std::vector<Ev> shrink(std::vector<Ev> ev, uint32_t &end_ms, const std::string &inv) {
  for (size_t chunk = ev.size() / 2; chunk >= 1; chunk /= 2) {
    for (size_t i = 0; i < ev.size();) {
      std::vector<Ev> trial(ev.begin(), ev.begin() + long(i));
      trial.insert(trial.end(), ev.begin() + long(i + chunk < ev.size() ? i + chunk : ev.size()), ev.end());
      Outcome o = run(trial, end_ms);
      if (!o.violation.empty() && invariant_of(o.violation) == inv) {
        ev = trial;
        end_ms = o.at_ms + TICK;
      } else {
        i += chunk;
      }
    }
    if (chunk == 1)
      break;
  }
  return ev;
}

void print_events(const std::vector<Ev> &ev, uint32_t end_ms) {
  std::printf("  // %zu events, fails by %u ms\n  const Ev events[] = {\n", ev.size(), end_ms);
  for (const Ev &e : ev)
    std::printf("      {%u, %s, %u, %u},\n", e.t, KIND_NAMES[e.kind], e.a, e.b);
  std::printf("  };\n");
}

// ---------------------------------------------------------------- regressions (shrunk seeds)

int regressions_failed = 0, regressions_run = 0;

void regression(const char *name, const std::vector<Ev> &ev, uint32_t end_ms) {
  regressions_run++;
  Outcome o = run(ev, end_ms);
  if (!o.violation.empty()) {
    regressions_failed++;
    std::printf("FAIL regression %s: %s\n", name, o.violation.c_str());
  }
}

void regressions() {
  // CO in the air during a run: the alarm stopped it and latched. A staged "CO sensor not fitted",
  // applied once the heater was off, started the CO alarm afresh with no alarm, and a fault reset
  // would then have cleared the latch without a minute of clean air (seed 1157, moved earlier).
  regression("co_unfit_during_alarm",
             {{606163, U_HEAT, 0, 0}, {1197273, U_CO_FITTED, 0, 0}, {1733928, F_CO, 6, 0}, {1971721, U_APPLY, 0, 0},
              {1980 * SEC, U_FAULT_RESET, 0, 0}},
             2000 * SEC);
  // A fan that froze for 3 s in ignition and freed before a stall was declared: the speed loop's
  // integral wound up while it slowed, the freed fan overshot past 1.2 x its command and the fan
  // stage latched an overspeed fault (a burn-off and a fault reset for nothing).
  // Two short freezes 0.7 s apart in a run: the integral still wound up while the slowing fan was
  // between half and all of its command (seed 73).
  regression("fan_double_freeze",
             {{1944266, U_HEAT, 0, 0},
              {2099796, F_FAN_STALL, 1, 0},
              {2102195, F_FAN_STALL, 0, 0},
              {2102892, F_FAN_STALL, 1, 0},
              {2105686, F_FAN_STALL, 0, 0}},
             2120 * SEC);
  regression("fan_freed_overspeed",
             {{300 * SEC, U_HEAT, 0, 0}, {340 * SEC, F_FAN_STALL, 1, 0}, {343 * SEC, F_FAN_STALL, 0, 0}}, 420 * SEC);
  // A fan that froze in a run locked out in the cooldown and stopped with the heater hot; the user
  // freed it and reset the fault, and the heater stayed off and hot, nothing cooling it (seed 22,
  // moved earlier, the fan freed before the reset).
  regression("fault_reset_hot",
             {{300 * SEC, U_HEAT, 0, 0},
              {1200 * SEC, F_FAN_STALL, 1, 0},
              {1225 * SEC, F_FAN_STALL, 0, 0},
              {1230 * SEC, U_FAULT_RESET, 0, 0}},
             1300 * SEC);
  // A stop from the link between two passes of the safety loop: the pump went on stroking (and the
  // glow pulsing) from the last pass's outputs for up to 10 ms, in burn-off (seed 12).
  regression("stop_between_passes", {{300053, U_HEAT, 0, 0}, {512996, U_POWER, 92, 0}, {829914, U_OFF, 0, 0}},
             829980 + SEC);
  // Glow counted in one-minute buckets dropped a minute's glow up to a minute early: two failed
  // attempts, then a new start reached 240 s of glow in a 10-minute window and went on (seed 5,
  // moved 30 minutes earlier: the buckets keep their alignment).
  regression("glow_window_buckets",
             {{153318, U_FAN, 0, 0},
              {351667, F_FLAME_OUT, 1, 0},
              {568443, U_HEAT, 0, 0},
              {569086, U_HEAT, 0, 0},
              {803162, U_OFF, 0, 0},
              {1085994, U_HEAT, 0, 0}},
             1153240 + 30 * SEC);
  // The stages were started afresh with their own parameters (fan_init(&s->fan, &s->fan.p, ...)),
  // which the init zeroed before copying: after a fan stall retry the fan never turned again, and
  // after a fault reset of a pump fault the pump never stroked (seed 15). A stall that frees in the
  // wait, during the first start's preheat:
  regression("fan_retry_params", {{300 * SEC, U_HEAT, 0, 0}, {310 * SEC, F_FAN_STALL, 1, 0}, {318 * SEC, F_FAN_STALL, 0, 0}},
             420 * SEC);
  regression("pump_reset_params",
             {{195453, U_HEAT, 0, 0},
              {197918, F_PUMP_LS, 1, 0},
              {380903, F_PUMP_LS, 0, 0},
              {426291, U_FAULT_RESET, 0, 0},
              {449200, U_HEAT, 0, 0}},
             483420 + 10 * SEC);
  // An MCU reset with a hot heat exchanger: the saved parameters were loaded into the table but not
  // handed to the sequencer (it was already cooling down, so not "off"), which then ran the
  // cooldown, and every later start, on the defaults (seed 213, moved earlier).
  regression("params_after_hot_reset",
             {{300 * SEC, U_PARAM, 48, 1055619670},
              {310 * SEC, U_POWER, 92, 0},
              {320 * SEC, U_APPLY, 1, 0},
              {330 * SEC, U_HEAT, 0, 0},
              {650 * SEC, M_RESET, 6, 0}},
             660 * SEC);
}

}  // namespace

int main(int argc, char **argv) {
  uint32_t first = 1, last = 6, hours = 1, shrink_seed = 0;
  bool quiet = false;
  for (int i = 1; i < argc; i++) {
    if (!std::strcmp(argv[i], "--seeds") && i + 1 < argc) {
      const char *a = argv[++i];
      first = uint32_t(std::strtoul(a, nullptr, 0));
      const char *dash = std::strchr(a, '-');
      last = dash ? uint32_t(std::strtoul(dash + 1, nullptr, 0)) : first;
    } else if (!std::strcmp(argv[i], "--hours") && i + 1 < argc) {
      hours = uint32_t(std::strtoul(argv[++i], nullptr, 0));
    } else if (!std::strcmp(argv[i], "--shrink") && i + 1 < argc) {
      shrink_seed = uint32_t(std::strtoul(argv[++i], nullptr, 0));
    } else if (!std::strcmp(argv[i], "--quiet")) {
      quiet = true;
    }
  }
  if (shrink_seed != 0) {
    std::vector<Ev> ev = schedule(shrink_seed, hours);
    uint32_t end = hours * HOUR;
    Outcome o = run(ev, end);
    if (o.violation.empty()) {
      std::printf("seed %u: no violation in %u h\n", shrink_seed, hours);
      return 0;
    }
    std::printf("seed %u: %s\n", shrink_seed, o.violation.c_str());
    end = o.at_ms + TICK;
    ev = shrink(ev, end, invariant_of(o.violation));
    std::printf("shrunk: %s\n", run(ev, end).violation.c_str());
    print_events(ev, end);
    return 1;
  }

  regressions();
  Stats total;
  int failed = 0;
  for (uint32_t seed = first; seed <= last; seed++) {
    Outcome o = run(schedule(seed, hours), hours * HOUR);
    total.add(o.st);
    if (!o.violation.empty()) {
      failed++;
      std::printf("FAIL seed %u: %s\n", seed, o.violation.c_str());
    } else if (!quiet && last - first < 50) {
      std::printf("seed %u: ok (%u starts, %u running, %u stalls, %u lockouts, %u dead-man, %u resets, %u CO alarms, "
                  "%u CO faults, %u pump cuts)\n",
                  seed, o.st.starts, o.st.running, o.st.stalls, o.st.lockouts, o.st.deadman, o.st.resets,
                  o.st.co_alarms, o.st.co_faults, o.st.pump_cuts);
    }
  }
  std::printf("soak: %u seeds x %u h = %.1f h simulated, %u events, %u starts, %u reached running, %u stall "
              "retries, %u lockouts, %u dead-man stops, %u MCU resets, %u faults latched, %u fault resets, "
              "%u glow s, %u frames (%u bad), %u CO alarms, %u CO sensor faults, %u pump cuts; %d regressions; "
              "%d failures\n",
              last - first + 1, hours, total.sim_ms / 3600000.0, total.events, total.starts, total.running,
              total.stalls, total.lockouts, total.deadman, total.resets, total.latched, total.fault_resets,
              total.glow_s, total.frames, total.bad_frames, total.co_alarms, total.co_faults, total.pump_cuts,
              regressions_run, failed + regressions_failed);
  return failed + regressions_failed ? 1 : 0;
}
