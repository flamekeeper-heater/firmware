// The safety core (stm32/core/) on the host, called as the board layer calls it on the chip, for
// test_e2e, test_soak and fuzz_link:
//
//   Loopback     the core and a byte wire to it: every byte sent goes to safety_receive() at once,
//                its reply back the other way (a LinkTransport for the ESP32's link client)
//   CoreRig      a Loopback with board_ms's schedule, pass(): the board around it is the derived
//                bench's (its readings, and what it does with the core's outputs)
//   HeaterBench  a CoreRig on the heater model: a board whose interlocks, pump coil, glow plug, fan
//                motor and CO sensor follow the core's outputs, and the ESP32's link client on the
//                wire. Its faults are all off unless a test switches them on.
//
// test_board and test_bringup run board.c itself over a fake chip (fake_chip.h); these benches stay
// on the core's calls, so a bench can check what each call did.
#pragma once

#include <cstdint>
#include <cstring>
#include <deque>

#include "../../esp32/components/flamekeeper/link_client.h"

extern "C" {
#include "../core/safety.h"
#include "co_sensor.h"
#include "heater_model.h"
}

static const uint32_t TICK = 10;  // the safety loop's pass, ms
static const uint32_t SEC = 1000;
static const uint32_t MIN = 60 * SEC;

struct Loopback : esphome::flamekeeper::LinkTransport {
  safety s;
  uint32_t now = 0;
  std::deque<uint8_t> to_esp;  // the MCU's replies on their way
  uint8_t reply[LINK_FRAME_MAX];
  size_t reply_len = 0;  // the reply to the last byte (0: none)

  // A byte lost on the wire (asked once for each byte, either way).
  virtual bool lost() { return false; }
  // One byte into the MCU; it answers a complete request at once (well inside its 20 ms).
  virtual void to_mcu(uint8_t byte) {
    reply_len = safety_receive(&s, byte, now, reply);
    to_esp.insert(to_esp.end(), reply, reply + reply_len);
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
};

// Board is the derived bench (CoreRig<Bench>): pass() calls its hooks directly, so they inline as
// board_ms's code does. A bench defines the hooks without a default and may hide the others:
//   inputs()                 the sequencer's measurements for a loop pass
//   ms_meas(o, pm)           a millisecond's readings for safety_ms
//   ms_out(io, step)         what the board does with its outputs (step: the burn step before it)
//   glow_meas_now(gm)        a glow period's readings for safety_glow_period
//   glow_out(c, step)        the pulse it planned
//   plant()                  the models' 10 ms, after the glow periods        (default: nothing)
//   co_reading(adc)          the CO sensor's reading, once a second; false: none  (default: false)
//   co_stepped()             after safety_co_step                               (default: nothing)
//   step_out(in, step)       after safety_step                                  (default: nothing)
//   fan_meas_now(in)         the fan stage's readings
//   fan_out(duty)            its duty                                           (default: nothing)
//   pass_out(in)             the pass is over                                   (default: nothing)
template <class Board> struct CoreRig : Loopback {
  // One 10 ms pass, in board_ms's order: ten milliseconds of the self-tests and pump strokes
  // (safety_ms), the pass's two glow periods (safety_glow_period), the plant's 10 ms, then the
  // safety loop's pass: the CO sensor's reading once a second (safety_co_step), the sequencer
  // (safety_step) and the fan stage (safety_fan_step).
  void pass() {
    Board &b = static_cast<Board &>(*this);
    for (uint32_t i = 0; i < TICK; i++) {
      checks_obs o;
      pump_meas pm;
      b.ms_meas(&o, &pm);
      uint8_t step = s.burn.step;
      b.ms_out(safety_ms(&s, now + i, &o, &pm), step);
    }
    for (uint32_t t = now; t < now + TICK; t += 5) {
      glow_meas gm;
      b.glow_meas_now(&gm);
      uint8_t step = s.burn.step;
      b.glow_out(safety_glow_period(&s, t, &gm), step);
    }
    b.plant();
    now += TICK;
    burn_inputs in = b.inputs();
    uint16_t co_adc;
    if (now % SEC == 0 && b.co_reading(&co_adc)) {
      safety_co_step(&s, now, co_adc, in.board_c10);
      b.co_stepped();
    }
    uint8_t step = s.burn.step;
    safety_step(&s, now, &in);
    b.step_out(in, step);
    fan_meas fm = b.fan_meas_now(in);
    b.fan_out(safety_fan_step(&s, now, &fm));
    b.pass_out(in);
  }
  void run(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += TICK)
      pass();
  }

  void plant() {}
  bool co_reading(uint16_t *adc) { return (void) adc, false; }
  void co_stepped() {}
  void step_out(const burn_inputs &in, uint8_t step_before) { (void) in, (void) step_before; }
  void fan_out(uint16_t duty) { (void) duty; }
  void pass_out(const burn_inputs &in) { (void) in; }
};

// Bench: the test's own bench, derived from HeaterBench<Bench>.
template <class Bench> struct HeaterBench : CoreRig<Bench> {
  using CoreRig<Bench>::s;
  using CoreRig<Bench>::now;

  heater_model m;
  esphome::flamekeeper::LinkClient client{this};
  bool esp_alive = true;  // false: the ESP32 is hung (its loop stops)

  // The board.
  int16_t board_c10 = 350;
  double plug_heat = 0, last_glow_i = 0;  // the glow plug: 0.2 Ohm cold to 1.0 Ohm hot
  uint16_t fan_duty = 0;
  int hb_running_ms = 0;  // the heartbeat detector
  int latch_armed = 0;    // the overheat latch
  uint16_t test_pulse = 0;
  int glow_en = 0;
  double pump_i = 0;      // the pump coil's current, A (6 Ohm, 2 ms)
  uint8_t pump_cut = 0;   // the overcurrent cut, reported until the core latches it (board.c)
  uint32_t pump_cuts = 0;

  // Faults: all off on a healthy board.
  bool fan_stuck_on = false;  // the fan switch conducts: the motor at the full supply
  bool glow_trips = false;    // the glow high side trips whenever it is on
  bool glow_ls = false;       // the glow low side conducts: current between pulses
  bool pump_open = false, pump_ls = false, pump_hs = false;  // open coil, low side on, high-side fault
  bool pump_oc = false;       // a shorted coil: the overcurrent watchdog cuts every armed stroke
  bool overheat_trip = false; // the overheat latch tripped (and stays so while the HX is hot)
  uint32_t co_ratio = 1000;   // the air at the CO sensor, Rs / R0 in 1/1000 (1000: clean)
  int co_adc = -1;            // >= 0: CO_ADC held at this count (open: 10; no sensor: 0)
  bool co_heater_dead = false;  // the CO sensor's heater: no change, so its check fails

  HeaterBench() { heater_model_init(&m, 10); }

  // The MCU starts (or restarts) with the board as it is; the latch reads tripped until the
  // power-up test re-arms it.
  void mcu_reset(uint8_t reason) {
    burn_inputs in = inputs();
    safety_init(&s, now, &in, reason, 0);
    pump_cut = 0;
    latch_armed = 0;
    hb_running_ms = 0;
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

  bool hb_ok() const { return hb_running_ms >= 1; }
  bool pump_plus() const { return hb_ok() && latch_armed && !pump_hs; }  // the pump's + side
  double vin() const { return m.supply_cv / 100.0; }
  double plug_ohm() const { return 0.2 + 0.8 * plug_heat; }
  double plug_amps() const { return vin() / (plug_ohm() + 0.02); }  // 0.02 Ohm of wiring
  double motor_v() const { return fan_stuck_on ? vin() : fan_duty / 1000.0 * vin(); }

  // One millisecond: the interlocks' readings for the self-tests, the pump coil's current.
  void ms_meas(checks_obs *o, pump_meas *pm) {
    o->vin_cv = m.supply_cv;
    o->pump_v_cv = pump_plus() ? m.supply_cv : 30;
    o->glow_v_cv = (glow_en && hb_ok()) ? m.supply_cv : 0;  // the plug's + side follows its enable
    o->glow_ca = glow_ls && hb_ok() ? 5000 : 0;
    o->pump_peak_ma = test_pulse ? (pump_plus() && !pump_open ? 400 : 3) : 0;
    o->latch_armed = uint8_t(latch_armed);
    o->pump_hs_ok = !pump_hs;
    o->hx_c10 = heater_model_hx(&m);
    pm->vin_cv = m.supply_cv;
    pm->i_ma = uint16_t(pump_i * 1000);
    pm->overcurrent = pump_cut;  // PMP-7: reported until the core latches it (board.c)
    pm->hs_ok = !pump_hs;
    pm->hb_running_ms = 0;
  }
  void ms_out(safety_io io, uint8_t step_before) {
    (void) step_before;
    if (s.pump.fault != PUMP_FAULT_NONE)
      pump_cut = 0;
    // A shorted coil: the watchdog cuts an armed stroke at once, and the coil gets no drive.
    if (pump_oc && pump_plus() && io.pump_duty && io.pump_oc_armed && !pump_cut)
      pump_cut = 1, pump_cuts++;
    double v = (pump_plus() && io.pump_duty && !pump_open && !pump_cut) ? io.pump_duty / 1000.0 * vin() : 0;
    if (pump_ls && pump_plus() && !pump_open)
      v = vin();  // the low side conducts: the coil is on whenever its + side is
    pump_i += (v / 6.0 - pump_i) * 0.393;
    hb_running_ms = io.heartbeat == CHECKS_HB_RUNNING ? hb_running_ms + 1 : 0;
    if (io.latch_reset && !(overheat_trip && heater_model_hx(&m) > 2700))
      latch_armed = 1, overheat_trip = false;
    test_pulse = io.pump_pulse_us;
    glow_en = io.glow_en;
  }

  // One 5 ms glow period: the planner's pulse into the plug.
  void glow_meas_now(glow_meas *gm) {
    gm->vin_cv = m.supply_cv;
    gm->pulse_ca = uint16_t(last_glow_i * 100 + 0.5);
    gm->gap_ca = glow_ls && s.glow.on ? uint16_t(plug_amps() * 100) : 0;
    gm->airflow_ok = m.fan_rpm >= s.burn.p.glow_min_fan_rpm;
    gm->board_c10 = board_c10;
    gm->high_side_trip = glow_trips && s.glow.on;
  }
  void glow_out(glow_cmd c, uint8_t step_before) {
    (void) step_before;
    double i = plug_amps();
    last_glow_i = c.width_us ? i : 0;
    double w = i * i * plug_ohm() * c.width_us / 5000.0;
    plug_heat += (w / 90.0 - plug_heat) * (5.0 / 8000.0);
  }

  // The heater: the fan's motor follows its average voltage (10 V: 5000 rpm).
  void plant() {
    uint16_t fan_target = uint16_t(motor_v() / 10.0 * 5000.0);
    heater_model_step(&m, TICK, fan_target, s.burn.out.pump_cHz, uint16_t(plug_heat > 0.5 ? 90 : 0));
  }

  bool co_reading(uint16_t *adc) {
    *adc = co_adc >= 0 ? uint16_t(co_adc) : co_sensor_adc(co_ratio, safety_co_heater(&s) || co_heater_dead);
    return true;
  }

  // The fan's shunt: the motor's current (2 Ohm, 9 V back-EMF at 5000 rpm) times the duty.
  fan_meas fan_meas_now(const burn_inputs &in) {
    double emf = m.fan_rpm / 5000.0 * 9.0, vm = motor_v();
    double i_motor = vm > emf ? (vm - emf) / 2.0 : 0;
    fan_meas fm;
    fm.rpm = in.fan_rpm;
    fm.vin_cv = m.supply_cv;
    fm.isense_ma = uint16_t(1000.0 * i_motor * (fan_stuck_on ? 1.0 : fan_duty / 1000.0));
    return fm;
  }
  void fan_out(uint16_t duty) { fan_duty = duty; }

  void pass_out(const burn_inputs &in) {
    (void) in;
    if (esp_alive)
      client.loop(now);
  }
};
