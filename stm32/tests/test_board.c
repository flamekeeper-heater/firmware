/* The board layer against a fake chip: the conversions, the schedule (1 / 5 / 10 ms), the watchdog
 * and heartbeat only from a completed loop pass, the power-up self-test through the real interlock
 * lines, the link over the UART, flash writes only with the heater off, and the CO alarm: its
 * heater pin, its reading a second, its faults into the sequencer and the link, and R0 in flash. */
#include <stdio.h>
#include <string.h>

#include "../board/analog.h"
#include "../board/board.h"
#include "../board/pump_oc.h"
#include "../hal/hal_stm32g0.h"
#include "co_sensor.h"
#include "fake_chip.h"
#include "../../common/test/check.h"

/* ---------------------------------------------------------------- the fake chip's plant */

/* What this test adds to the fake chip (fake_chip.h). */
static struct {
  uint8_t tx[256]; /* what the board sent */
  size_t tx_len;
  uint32_t stroke_ms, stroke_ms_unguarded; /* PUMP_CMD on, and on without the watchdog armed */
  /* The glow timer (TIM3): a period every 5 ms with its readings, while it runs. */
  uint8_t tim3_stopped;
  double glow_pulse_a, glow_gap_a;
  /* The CO sensor (co_sensor.h): the air as Rs / R0 (x 1000); co_adc >= 0 holds the pin there
   * (open, or no sensor). */
  uint32_t co_ratio;
  int co_adc;
} sim;

void fake_chip_adc(uint16_t raw[HAL_ADC_COUNT]) {
  raw[HAL_ADC_PUMP_I] = counts(pump_powered() && chip.pump_duty ? 1.0 * 0.45 : 0); /* a 1 A stroke */
  raw[HAL_ADC_CO] = sim.co_adc >= 0 ? (uint16_t) sim.co_adc : co_sensor_adc(sim.co_ratio, chip.co_heater);
}
uint8_t hal_glow_period(hal_glow_sample *g) {
  if (sim.tim3_stopped || (int32_t) (chip.now - chip.glow_next_ms) < 0)
    return 0;
  chip.glow_next_ms += 5;
  g->pulse_i = counts(sim.glow_pulse_a * 0.009);
  g->gap_i = counts(sim.glow_gap_a * 0.009);
  g->gap_vin = counts(chip.vin * 6.8 / 106.8);
  return 1;
}
void hal_uart_write(const uint8_t *d, size_t n) {
  if (sim.tx_len + n <= sizeof(sim.tx)) {
    memcpy(sim.tx + sim.tx_len, d, n);
    sim.tx_len += n;
  }
}

static void chip_reset(void) {
  fake_chip_reset(12.6);
  memset(&sim, 0, sizeof(sim));
  sim.co_ratio = 1000;
  sim.co_adc = -1;
}

static void run(board *b, uint32_t ms) {
  for (uint32_t i = 0; i < ms; i++) {
    chip.now++;
    board_ms(b, chip.now);
    if (chip.pump_duty > 0) {
      sim.stroke_ms++;
      sim.stroke_ms_unguarded += !chip.oc_armed;
    }
  }
}

/* ---------------------------------------------------------------- tests */

static void test_conversions(void) {
  uint16_t cal = hal_vrefint_cal();
  uint16_t v = analog_vdda_mv(counts(VREFINT), cal);
  CHECK(v >= 3290 && v <= 3310);
  /* PWR-5's reading is not clamped: 3.8 V reads as 3.8 V, and no conversion as the highest. */
  uint16_t over = analog_vdda_check_mv(counts(VREFINT * 3.3 / 3.8), cal);
  CHECK(over >= 3790 && over <= 3810 && analog_vdda_mv(counts(VREFINT * 3.3 / 3.8), cal) == 3600);
  CHECK(analog_vdda_check_mv(0, cal) == 0xFFFFu);
  uint16_t vin = analog_vin_cv(counts(12.6 * 6.8 / 106.8), v);
  CHECK(vin >= 1250 && vin <= 1270);
  CHECK(analog_vin_cv(counts(24.0 * 6.8 / 106.8), v) >= 2385);
  uint16_t gv = analog_div19_cv(counts(12.6 / 19.0), v);
  CHECK(gv >= 1250 && gv <= 1270);
  uint16_t ga = analog_glow_ca(counts(10.0 * 0.009), v); /* 10 A */
  CHECK(ga >= 990 && ga <= 1010);
  uint16_t pa = analog_pump_ma(counts(2.0 * 0.45), v); /* 2 A */
  CHECK(pa >= 1990 && pa <= 2010);
  uint16_t fa = analog_fan_ma(counts(1.5 * 0.015), v); /* 1.5 A x duty */
  CHECK(fa >= 1450 && fa <= 1550);
  /* The heat-exchanger thermistor. */
  int16_t t25 = analog_hx_c10(4057), t200 = analog_hx_c10(1820);
  CHECK(t25 >= 200 && t25 <= 300);
  CHECK(t200 >= 1950 && t200 <= 2050);
  CHECK(analog_hx_c10(4094) == ANALOG_UNKNOWN_TEMP); /* open */
  CHECK(analog_hx_c10(50) == 3500);                  /* shorted or very hot: reads hot */
  /* The board thermistor. */
  int16_t b25 = analog_board_c10(2048);
  CHECK(b25 >= 245 && b25 <= 255);
  CHECK(analog_board_c10(4090) == ANALOG_UNKNOWN_TEMP); /* below -40 C: a sensor fault */
  CHECK(analog_board_c10(100) == ANALOG_UNKNOWN_TEMP);  /* above 125 C */
}

/* PMP-7's threshold in ADC counts: 3.3 A at 0.45 V/A, against the measured VDDA. */
static void test_pump_oc_threshold(void) {
  uint16_t t = analog_pump_oc_raw(3300, 3300);
  CHECK(t == 1842); /* 1.485 V of 3.3 V */
  uint16_t back = analog_pump_ma(t, 3300);
  CHECK(back >= 3290 && back <= 3300);
  CHECK(analog_pump_oc_raw(3300, 3170) == 1918); /* a low rail: more counts for the same current */
  CHECK(analog_pump_oc_raw(3300, 3460) == 1757);
  CHECK(analog_pump_oc_raw(3300, 0) == 1842);    /* no VDDA yet: the nominal rail */
  CHECK(analog_pump_oc_raw(9000, 3300) == 4094); /* never above full scale: saturation trips */
}

/* The overcurrent filter against a train of PUMP_ISENSE conversions, one every scan_us, with an
 * excess of dur_us at a phase: returns the time from the excess's start to the cut, -1 if none. */
static int oc_train(uint16_t dur_us, uint16_t phase_us, uint16_t scan_us, uint8_t need) {
  pump_oc f;
  pump_oc_arm(&f, need);
  int cut_at = -1;
  const uint32_t start = 500u + phase_us;
  for (uint32_t t = 0; t < 3000; t += scan_us) {
    int above = t >= start && t < start + dur_us;
    int high = above, low = !above && pump_oc_listening(&f);
    if ((high || low) && pump_oc_event(&f, high, low)) {
      CHECK(cut_at < 0); /* once */
      cut_at = (int) (t - start);
    }
  }
  return cut_at;
}

static void test_pump_oc_filter(void) {
  CHECK(pump_oc_need(50, 20) == 2);
  CHECK(pump_oc_need(60, 20) == 3);
  CHECK(pump_oc_need(50, 10) == 5);
  CHECK(pump_oc_need(10, 20) == 1);
  CHECK(pump_oc_need(0, 20) == 1);
  CHECK(pump_oc_need(50, 0) == 1);
  /* Every phase of the scan against the excess: longer than 50 us is always cut, within two scans
   * of its start; shorter than one scan never is. */
  const uint16_t scan = 20;
  uint8_t need = pump_oc_need(50, scan);
  for (uint16_t ph = 0; ph < scan; ph++) {
    int c51 = oc_train(51, ph, scan, need), c1000 = oc_train(1000, ph, scan, need);
    CHECK(c51 >= 0 && c51 <= 2 * scan);
    CHECK(c1000 >= 0 && c1000 <= 2 * scan);
    CHECK(oc_train(19, ph, scan, need) < 0);
    CHECK(oc_train(0, ph, scan, need) < 0);
  }
  /* Single conversions above it, never two in a row: glitches, no cut. */
  pump_oc f;
  pump_oc_arm(&f, need);
  int cuts = 0;
  for (int k = 0; k < 100; k++) {
    int high = k % 2 == 0, low = !high && pump_oc_listening(&f);
    if (high || low)
      cuts += pump_oc_event(&f, high, low);
  }
  CHECK(cuts == 0);
  /* A late interrupt that sees both flags: the high one counts. */
  pump_oc_arm(&f, need);
  CHECK(pump_oc_event(&f, 1, 0) == 0);
  CHECK(pump_oc_listening(&f));
  CHECK(pump_oc_event(&f, 1, 1) == 1);
  CHECK(f.cut && !pump_oc_listening(&f));
  CHECK(pump_oc_event(&f, 1, 0) == 0); /* latched: reported once */
  pump_oc_arm(&f, need);               /* the next stroke starts clean */
  CHECK(!f.cut && f.above == 0);
  /* A low reading while not counting changes nothing. */
  CHECK(pump_oc_event(&f, 0, 1) == 0 && f.above == 0);
}

static void test_schedule_and_watchdog(void) {
  chip_reset();
  board b;
  board_init(&b, chip.now, LINK_RESET_POWER_ON, 0);
  CHECK(b.s.burn.fault == BURN_FAULT_NONE); /* the tripped power-up latch is not an overheat */
  run(&b, 1000);
  CHECK(b.loops == 100);
  CHECK(chip.iwdg_refreshes == 100); /* WDG-16: from the loop pass only */
  CHECK(b.s.chk.power_up_done);
  CHECK(b.s.power_up_failed == 0);   /* through the real interlock lines of the fake chip */
  CHECK(chip.latch_armed && b.s.latch_was_armed);
  CHECK(b.s.burn.fault == BURN_FAULT_NONE);
  CHECK(chip.hb_mode == HAL_HB_STOPPED); /* off: no heartbeat */
  /* OVH-1: a fall of the re-armed latch is a hardware overheat trip, latched. */
  chip.latch_armed = 0;
  run(&b, 20);
  CHECK(b.s.in.overheat_latch && b.s.burn.fault == BURN_FAULT_OVERHEAT && b.s.burn.fault_latched);
  CHECK(b.s.burn.why == BURN_WHY_OVERHEAT_LATCH);
}

static void test_link(void) {
  chip_reset();
  board b;
  board_init(&b, chip.now, LINK_RESET_POWER_ON, 0);
  const uint8_t hello[2] = {LINK_MAJOR, LINK_MINOR};
  chip.rx_len = link_build_request(5, LINK_OP_HELLO, hello, 2, chip.rx);
  run(&b, 2);
  CHECK(sim.tx_len > 7);
  CHECK(sim.tx[0] == LINK_REPLY && link_frame_seq(sim.tx) == 5 && link_frame_op(sim.tx) == LINK_OP_HELLO);
  link_info info;
  CHECK(link_decode_info(link_reply_payload(sim.tx), link_reply_payload_len(sim.tx), &info));
  CHECK(info.kind == LINK_KIND_STM32 && info.reset_reason == LINK_RESET_POWER_ON);
}

/* An ESP32 on the UART: HELLO once, then a POLL every 50 ms (the dead-man needs it). */
static void esp32_polls(board *bd, uint32_t ms) {
  static uint8_t seq = 1;
  for (uint32_t t = 0; t < ms; t += 50) {
    chip.rx_len = chip.rx_pos = 0;
    if (bd->s.client_major == 0) {
      const uint8_t hello[2] = {LINK_MAJOR, LINK_MINOR};
      chip.rx_len = link_build_request(seq++, LINK_OP_HELLO, hello, 2, chip.rx);
    }
    link_desired w = {1, LINK_MODE_HEAT, 50, 0, 0, 0}; /* one start number, never a new one */
    uint8_t p[LINK_DESIRED_SIZE];
    link_encode_desired(&w, p);
    chip.rx_len += link_build_request(seq++, LINK_OP_POLL, p, LINK_DESIRED_SIZE, chip.rx + chip.rx_len);
    run(bd, 50);
  }
}

static void test_heartbeat_needs_the_loop(void) {
  chip_reset();
  board b;
  board_init(&b, chip.now, LINK_RESET_POWER_ON, 0);
  esp32_polls(&b, 1000);
  /* Ask the core for the heartbeat (a burn step that needs it), then hang the loop. */
  b.s.burn.step = BURN_STEP_SELF_TEST; /* the fan check before preheat: heartbeat on, no strokes */
  esp32_polls(&b, 100);
  CHECK(hb_ok());
  uint32_t bursts = b.bursts;
  esp32_polls(&b, 50);
  CHECK(b.bursts >= bursts + 5); /* re-armed by each completed pass */
  CHECK(b.s.burn.step == BURN_STEP_SELF_TEST);
  /* The program hangs: nothing calls board_ms any more. The burst runs out by itself. */
  chip.now += 30;
  CHECK(!hb_ok());
}

/* PMP-7 through the board: the watchdog armed for every stroke with the threshold in counts; its
 * cut reported by the chip latches the pump fault and stops the heartbeat. */
static void test_pump_overcurrent(void) {
  chip_reset();
  board b;
  board_init(&b, chip.now, LINK_RESET_POWER_ON, 0);
  esp32_polls(&b, 1000);
  CHECK(!chip.oc_armed && sim.stroke_ms == 0);
  b.s.burn.step = BURN_STEP_PRIMING; /* strokes at once, no start sequence */
  b.s.burn.step_ms = chip.now;
  esp32_polls(&b, 500);
  CHECK(sim.stroke_ms >= 38);
  CHECK(sim.stroke_ms_unguarded == 0);
  CHECK(chip.oc_threshold >= 1835 && chip.oc_threshold <= 1850); /* 3.3 A at 0.45 V/A, 3.3 V */
  CHECK(chip.oc_hold_us == 50);
  CHECK(b.s.pump.fault == PUMP_FAULT_NONE);
  /* Wait for a stroke, then the watchdog's interrupt cuts it. */
  for (int k = 0; k < 200 && chip.pump_duty == 0; k++)
    run(&b, 1);
  CHECK(chip.pump_duty > 0 && chip.oc_armed);
  chip.oc_report = 1;
  run(&b, 1);
  CHECK(b.s.pump.fault == PUMP_FAULT_OVERCURRENT);
  CHECK(chip.pump_duty == 0 && !chip.oc_armed);
  CHECK(chip.hb_mode == HAL_HB_STOPPED);
  esp32_polls(&b, 100);
  CHECK(chip.hb_mode == HAL_HB_STOPPED && chip.pump_duty == 0); /* latched */
  CHECK(b.s.pump.fault == PUMP_FAULT_OVERCURRENT);

  /* A cut reported while a self-test runs (no stroke steps then): latched at once all the same. */
  chip_reset();
  board_init(&b, chip.now, LINK_RESET_POWER_ON, 0);
  run(&b, 5);
  CHECK(checks_busy(&b.s.chk) && b.s.pump.fault == PUMP_FAULT_NONE);
  chip.oc_report = 1;
  run(&b, 1);
  CHECK(checks_busy(&b.s.chk) && b.s.pump.fault == PUMP_FAULT_OVERCURRENT);
}

/* GLW-6 through the board: the planner steps once per glow period, on the timer's readings; if
 * the timer's periods stop it still steps every 10 ms, so GLOW_EN keeps following the burn. */
static void test_glow_periods(void) {
  chip_reset();
  board b;
  board_init(&b, chip.now, LINK_RESET_POWER_ON, 0);
  run(&b, 1000);
  uint32_t n = chip.glow_cmds;
  run(&b, 1000);
  CHECK(chip.glow_cmds - n == 200);
  sim.tim3_stopped = 1;
  n = chip.glow_cmds;
  run(&b, 1000);
  CHECK(chip.glow_cmds - n >= 99 && chip.glow_cmds - n <= 101);
  sim.tim3_stopped = 0;
  chip.glow_next_ms = chip.now + 1;
  run(&b, 10);
  /* The in-pulse reading reaches the planner for the pulse it planned. */
  sim.glow_pulse_a = 40.0;
  b.s.glow.last_width_us = 150; /* as if a probe went out this period */
  run(&b, 5);
  CHECK(b.s.glow.i_ca >= 3950 && b.s.glow.i_ca <= 4050);
  /* Current in the gap with the high side on: the low side is stuck on (GLW-6). */
  sim.glow_gap_a = 10.0;
  b.s.glow.on = 1;
  run(&b, 5);
  CHECK(b.s.glow.fault == GLOW_FAULT_STUCK_LOW_SIDE);
}

static void test_flash_only_when_off(void) {
  chip_reset();
  board b;
  board_init(&b, chip.now, LINK_RESET_POWER_ON, 0);
  run(&b, 1000);
  b.s.burn.step = BURN_STEP_RUNNING; /* the heater runs */
  b.s.params.save_pending = 1;
  run(&b, 100);
  CHECK(chip.flash_writes == 0);
  b.s.burn.step = BURN_STEP_OFF;
  run(&b, 20);
  CHECK(chip.flash_writes == 1 && !b.s.params.save_pending);
  /* The next reset loads it. */
  board b2;
  board_init(&b2, chip.now, LINK_RESET_POWER_ON, 0);
  CHECK(memcmp(&b2.s.burn.p, &b.s.params.live, sizeof(burn_params)) == 0);
}

/* ---------------------------------------------------------------- the CO alarm */

/* The DETAIL record over the UART, as the ESP32 would read it. */
static link_detail read_detail(board *b) {
  link_detail d;
  memset(&d, 0, sizeof(d));
  chip.rx_len = link_build_request(9, LINK_OP_DETAIL, NULL, 0, chip.rx);
  chip.rx_pos = 0;
  sim.tx_len = 0;
  run(b, 2);
  CHECK(link_decode_detail(link_reply_payload(sim.tx), link_reply_payload_len(sim.tx), &d));
  return d;
}

/* The status over the link (a POLL that asks for nothing new: the heater off). */
static link_status read_status(board *b) {
  link_status st;
  memset(&st, 0, sizeof(st));
  link_desired w = {1, LINK_MODE_OFF, 0, 0, 0, 0};
  uint8_t p[LINK_DESIRED_SIZE];
  link_encode_desired(&w, p);
  chip.rx_len = link_build_request(10, LINK_OP_POLL, p, LINK_DESIRED_SIZE, chip.rx);
  chip.rx_pos = 0;
  sim.tx_len = 0;
  run(b, 2);
  CHECK(link_decode_status(link_reply_payload(sim.tx), link_reply_payload_len(sim.tx), &st));
  return st;
}

/* FAULT_RESET over the link: its result. */
static uint8_t fault_reset(board *b) {
  chip.rx_len = link_build_request(11, LINK_OP_FAULT_RESET, NULL, 0, chip.rx);
  chip.rx_pos = 0;
  sim.tx_len = 0;
  run(b, 2);
  CHECK(sim.tx_len >= 7);
  return link_reply_result(sim.tx);
}

/* PWR-5 through the board: VDDA from VREFINT every millisecond. A rail still rising after the
 * reset holds everything off and does not latch; out of 3.0-3.6 V for 10 ms afterwards drops every
 * enable and the heartbeat at once, latches a fault the link reports, and needs a fault reset once
 * VDDA is back. A short dip does nothing. */
static void test_vdda(void) {
  chip_reset();
  chip.vdda = 2.4; /* the 3.3 V converter still starting */
  board b;
  board_init(&b, chip.now, LINK_RESET_POWER_ON, 0);
  run(&b, 200);
  CHECK(!b.s.power_up_started && chip.hb_mode == HAL_HB_STOPPED && !chip.latch_level);
  CHECK(b.s.mon.fault == MONITOR_OK && b.s.burn.fault == BURN_FAULT_NONE);
  chip.vdda = 3.3;
  run(&b, 1000);
  CHECK(b.s.chk.power_up_done && b.s.power_up_failed == 0 && b.s.burn.fault == BURN_FAULT_NONE);

  /* Priming: strokes and the heartbeat. A 6 ms excursion either way changes nothing. */
  esp32_polls(&b, 200);
  b.s.burn.step = BURN_STEP_PRIMING;
  b.s.burn.step_ms = chip.now;
  esp32_polls(&b, 200);
  CHECK(hb_ok());
  chip.vdda = 3.7;
  run(&b, 6);
  chip.vdda = 3.3;
  run(&b, 1);
  chip.vdda = 2.9;
  run(&b, 6);
  chip.vdda = 3.3;
  esp32_polls(&b, 100);
  CHECK(b.s.mon.fault == MONITOR_OK && b.s.burn.step == BURN_STEP_PRIMING && hb_ok());
  /* 3.7 V for 11 ms: every enable off at once, then the latched fault. */
  for (int k = 0; k < 200 && chip.pump_duty == 0; k++)
    run(&b, 1); /* in a stroke */
  CHECK(chip.pump_duty > 0);
  chip.vdda = 3.7;
  run(&b, 11);
  CHECK(b.s.mon.fault == MONITOR_VDDA_HIGH);
  CHECK(chip.hb_mode == HAL_HB_STOPPED && chip.pump_duty == 0 && !chip.glow_en && !chip.latch_level);
  run(&b, 10);
  CHECK(b.s.burn.fault == BURN_FAULT_MCU && b.s.burn.why == BURN_WHY_VDDA_HIGH && b.s.burn.fault_latched);
  CHECK(b.s.burn.step == BURN_STEP_OFF);
  link_status st = read_status(&b);
  CHECK(st.fault == BURN_FAULT_MCU && st.fault_detail == BURN_WHY_VDDA_HIGH);
  CHECK(st.flags & LINK_STATUS_FAULT_LATCHED);
  link_detail d = read_detail(&b);
  CHECK(d.vdda_mv >= 3590); /* clamped for the conversions, as before */
  /* Still over: a fault reset is refused; back in the window, it clears. */
  CHECK(fault_reset(&b) == LINK_REFUSED);
  chip.vdda = 3.3;
  run(&b, 20);
  CHECK(b.s.mon.fault == MONITOR_VDDA_HIGH && chip.hb_mode == HAL_HB_STOPPED); /* latched */
  CHECK(fault_reset(&b) == LINK_OK);
  run(&b, 20);
  CHECK(b.s.mon.fault == MONITOR_OK && b.s.burn.fault == BURN_FAULT_NONE && !b.s.burn.fault_latched);

  /* GLOW_EN drops in the millisecond VDDA latches, not at the next glow period. */
  sim.tim3_stopped = 1; /* the planner steps every 10 ms only */
  chip.vdda = 3.65;
  for (int k = 0; k < 20 && b.s.mon.fault == MONITOR_OK; k++) {
    if (b.s.mon.vdda_bad.on && chip.now - b.s.mon.vdda_bad.since_ms == 9) {
      b.glow_en_planner = 1; /* the planner's last step had the plug on, ... */
      b.last_glow_ms = chip.now; /* ... and its next is 10 ms away */
    }
    run(&b, 1);
  }
  CHECK(b.s.mon.fault == MONITOR_VDDA_HIGH && !chip.glow_en && chip.glow_us == 0);
  sim.tim3_stopped = 0;
  chip.vdda = 3.3;
  run(&b, 10);
  CHECK(fault_reset(&b) == LINK_OK);

  /* Below 3.0 V while off: latched as well. */
  chip.vdda = 2.95;
  run(&b, 30);
  CHECK(b.s.burn.fault == BURN_FAULT_MCU && b.s.burn.why == BURN_WHY_VDDA_LOW && b.s.burn.fault_latched);

  /* A rail that never comes up: a fault half a second after the reset. */
  chip_reset();
  chip.vdda = 2.6;
  board_init(&b, chip.now, LINK_RESET_POWER_ON, 0);
  run(&b, 600);
  CHECK(b.s.burn.fault == BURN_FAULT_MCU && b.s.burn.why == BURN_WHY_VDDA_LOW && !b.s.power_up_started);
  CHECK(chip.hb_mode == HAL_HB_STOPPED);
}

/* GLW-1 through the board: the tick against the driver's second clock once a second. A clock 30 %
 * apart latches a fault that ends the run and that the link reports; 10 % is inside the LSI's
 * spread and does nothing. */
static void test_tick_check(void) {
  chip_reset();
  board b;
  board_init(&b, chip.now, LINK_RESET_POWER_ON, 0);
  chip.clock_permille = 900;
  esp32_polls(&b, 5000);
  CHECK(b.s.mon.tick_checks >= 4 && b.s.mon.fault == MONITOR_OK && b.s.burn.fault == BURN_FAULT_NONE);
  CHECK(b.s.mon.tick_permille >= 890 && b.s.mon.tick_permille <= 910);
  chip.clock_permille = 700;
  esp32_polls(&b, 2100);
  CHECK(b.s.burn.fault == BURN_FAULT_MCU && b.s.burn.why == BURN_WHY_TICK && b.s.burn.fault_latched);
  link_status st = read_status(&b);
  CHECK(st.fault == BURN_FAULT_MCU && st.fault_detail == BURN_WHY_TICK);
  CHECK(fault_reset(&b) == LINK_REFUSED);
  chip.clock_permille = 1000;
  run(&b, 2100);
  CHECK(fault_reset(&b) == LINK_OK);
  CHECK(!b.s.burn.fault_latched && b.s.mon.fault == MONITOR_OK);
}

static void test_co_heater_and_alarm(void) {
  chip_reset();
  board b;
  board_init(&b, chip.now, LINK_RESET_POWER_ON, 0);
  CHECK(chip.co_heater == 1); /* CO-5: heated from power-up */
  /* Warm-up (CO-5): a high reading in the first 3 minutes is ignored. */
  sim.co_ratio = 8;
  run(&b, 170000);
  CHECK(b.s.co.state == CO_WARMING_UP && !b.s.co.alarm);
  CHECK(b.s.burn.fault == BURN_FAULT_NONE);
  sim.co_ratio = 1000;
  /* The heater check (CO-4): CO_HEAT_EN off for about 5 s once the warm-up is over, then on. */
  uint32_t off_ms = 0, first_off = 0;
  for (int i = 0; i < 30000; i++) {
    run(&b, 1);
    if (!chip.co_heater) {
      off_ms++;
      if (first_off == 0)
        first_off = chip.now;
    }
  }
  CHECK(first_off >= 180000 && first_off <= 181100);
  CHECK(off_ms >= 4900 && off_ms <= 6100);
  CHECK(chip.co_heater == 1 && b.s.co.checked && !b.s.co.fault);
  link_detail d = read_detail(&b);
  CHECK(d.co_state == CO_LEARNING);
  run(&b, 10 * 60000);
  d = read_detail(&b);
  CHECK(d.co_state == CO_OK && d.co_ppm == 0);
  /* About 1200 ppm for 11 s: the alarm, into the sequencer's latched CO fault and the link. */
  sim.co_ratio = 8;
  run(&b, 11000);
  CHECK(b.s.burn.fault == BURN_FAULT_CO && b.s.burn.fault_latched);
  d = read_detail(&b);
  CHECK(d.co_state == CO_ALARM && d.co_ppm >= 1000 && d.co_ppm != LINK_UNKNOWN_U16);
}

static void test_co_sensor_fault(void) {
  chip_reset();
  board b;
  board_init(&b, chip.now, LINK_RESET_POWER_ON, 0);
  run(&b, 190000);
  CHECK(b.s.co.checked && b.s.burn.fault == BURN_FAULT_NONE);
  sim.co_adc = 10; /* open (CO-3) */
  run(&b, 7000);
  CHECK(b.s.burn.fault == BURN_FAULT_CO_SENSOR && b.s.burn.fault_latched);
  link_detail d = read_detail(&b);
  CHECK(d.co_state == CO_SENSOR_FAULT && d.co_ppm == LINK_UNKNOWN_U16);
}

static void test_co_not_fitted(void) {
  /* CO-1: a saved table with "CO sensor fitted" 0. No sensor on the board: no fault, no heater. */
  chip_reset();
  params ps;
  params_init(&ps);
  params_set(&ps, 0x52, 0);
  CHECK(params_apply(&ps, 1, 1) == LINK_OK);
  uint8_t img[PARAMS_IMAGE_MAX];
  size_t n = params_pack(&ps, img, sizeof(img));
  CHECK(hal_flash_write(HAL_PAGE_PARAMS, img, n) == 0);
  sim.co_adc = 0;
  board b;
  board_init(&b, chip.now, LINK_RESET_POWER_ON, 0);
  CHECK(chip.co_heater == 0);
  run(&b, 200000);
  CHECK(chip.co_heater == 0 && b.s.co.state == CO_NOT_FITTED);
  CHECK(b.s.burn.fault == BURN_FAULT_NONE);
  CHECK(read_detail(&b).co_state == CO_NOT_FITTED);

  /* The same board with the default table: fitted, so it is a sensor fault, never "absent". */
  chip_reset();
  sim.co_adc = 0;
  board_init(&b, chip.now, LINK_RESET_POWER_ON, 0);
  CHECK(chip.co_heater == 1);
  run(&b, 200000);
  CHECK(b.s.burn.fault == BURN_FAULT_CO_SENSOR);
}

/* The R0 log's slots in use. */
static unsigned log_slots_used(void) {
  unsigned used = 0;
  for (unsigned i = 0; i < BOARD_CO_LOG_SLOTS; i++)
    if (chip.flash[HAL_PAGE_PLUG][BOARD_CO_LOG_START + i * BOARD_CO_LOG_SLOT] != 0xFF)
      used++;
  return used;
}

static void test_co_r0_in_flash(void) {
  chip_reset();
  board b;
  board_init(&b, chip.now, LINK_RESET_POWER_ON, 0);
  /* Learnt in clean air (CO-7), written at once: one double word, no erase. */
  run(&b, 13 * 60000);
  CHECK(b.s.co.state == CO_LEARNING && chip.flash_programs == 0);
  run(&b, 60000);
  CHECK(b.s.co.state == CO_OK);
  CHECK(chip.flash_programs == 1 && chip.flash_writes == 0 && log_slots_used() == 1);
  uint32_t r0 = b.s.co_r0_saved;
  CHECK(r0 > 290000 && r0 < 310000 && r0 == b.s.co.r0);
  /* A changed R0 an hour on waits while the heater runs. */
  b.s.co_r0_saved = r0 / 2;
  b.s.co_r0_saved_ms = chip.now - 2u * 3600000u;
  link_deadman_feed(&b.s.deadman, chip.now); /* the ESP32 is there: no dead-man stop to log */
  b.s.burn.step = BURN_STEP_RUNNING;
  run(&b, 100);
  CHECK(chip.flash_programs == 1);
  b.s.burn.step = BURN_STEP_OFF;
  run(&b, 20);
  CHECK(chip.flash_programs == 2 && log_slots_used() == 2 && b.s.co_r0_saved == r0);

  /* A reset: R0 comes back, so the alarm is OK straight after warm-up and check, no learning. */
  board b2;
  board_init(&b2, chip.now, LINK_RESET_POWER_ON, 0);
  CHECK(b2.s.co_r0_saved == r0 && b2.s.co.r0 == r0);
  run(&b2, 3 * 60000 + 8000);
  CHECK(b2.s.co.state == CO_OK);

  /* The plug record is rewritten: the page is erased, R0 goes back in. */
  b2.s.plug.rec.verified_v = 12;
  b2.s.plug.save_pending = 1;
  run(&b2, 20);
  CHECK(chip.flash_writes == 1 && log_slots_used() == 1 && chip.flash_bad_programs == 0);
  board b3;
  board_init(&b3, chip.now, LINK_RESET_POWER_ON, 0);
  CHECK(b3.s.co_r0_saved == r0 && b3.s.plug.rec.verified_v == 12);

  /* A full log: the next R0 erases the page, keeping the plug record, and starts the log again. */
  uint8_t e[BOARD_CO_LOG_SLOT];
  memcpy(e, chip.flash[HAL_PAGE_PLUG] + BOARD_CO_LOG_START, sizeof(e));
  for (unsigned i = 1; i < BOARD_CO_LOG_SLOTS; i++)
    CHECK(hal_flash_program(HAL_PAGE_PLUG, (uint16_t) (BOARD_CO_LOG_START + i * BOARD_CO_LOG_SLOT), e,
                            sizeof(e)) == 0);
  board b4;
  board_init(&b4, chip.now, LINK_RESET_POWER_ON, 0);
  CHECK(b4.co_log_next == BOARD_CO_LOG_SLOTS && b4.s.co_r0_saved == r0);
  uint32_t writes = chip.flash_writes;
  safety_co_r0_saved(&b4.s, 0); /* as if nothing were saved: the next learnt R0 is written */
  run(&b4, 3 * 60000 + 8000);
  CHECK(b4.s.co.state == CO_OK);
  run(&b4, 20);
  CHECK(chip.flash_writes == writes + 1 && log_slots_used() == 1 && chip.flash_bad_programs == 0);
  board b5;
  board_init(&b5, chip.now, LINK_RESET_POWER_ON, 0);
  CHECK(b5.s.co_r0_saved == b4.s.co.r0 && b5.s.plug.rec.verified_v == 12 && b5.co_log_next == 1);

  /* A torn slot (power lost while writing) is skipped. */
  chip.flash[HAL_PAGE_PLUG][BOARD_CO_LOG_START + BOARD_CO_LOG_SLOT] = 0x00;
  board b6;
  board_init(&b6, chip.now, LINK_RESET_POWER_ON, 0);
  CHECK(b6.s.co_r0_saved == b4.s.co.r0 && b6.co_log_next == 2);
}

/* A double ECC error in a saved page (a write torn by a power loss): the NMI handler records the
 * page instead of resetting in a loop; the board takes the defaults and rewrites the page. */
static void test_flash_ecc_error(void) {
  /* The NMI's decision from FLASH_ECCR: ECCD set, main flash, double word in page 30 or 31. */
  const uint32_t ECCD = 1u << 31, SYSF = 1u << 20;
  CHECK(hal_ecc_saved_page(ECCD | (30u * 2048u / 8u)) == HAL_PAGE_PARAMS);
  CHECK(hal_ecc_saved_page(ECCD | (31u * 2048u / 8u + 255u)) == HAL_PAGE_PLUG);
  CHECK(hal_ecc_saved_page(ECCD | (30u * 2048u / 8u - 1u)) == -1); /* the program: watchdog */
  CHECK(hal_ecc_saved_page(ECCD | SYSF | (30u * 2048u / 8u)) == -1); /* system memory */
  CHECK(hal_ecc_saved_page(30u * 2048u / 8u) == -1);                /* not a double error */

  /* Saved pages: a table (fan max 5000, CO not fitted), a plug record and an R0. */
  chip_reset();
  params ps;
  params_init(&ps);
  params_set(&ps, 0x11, 5000);
  params_set(&ps, 0x52, 0);
  CHECK(params_apply(&ps, 1, 1) == LINK_OK);
  uint8_t img[PARAMS_IMAGE_MAX];
  size_t n = params_pack(&ps, img, sizeof(img));
  CHECK(hal_flash_write(HAL_PAGE_PARAMS, img, n) == 0);
  plug pl;
  plug_params pp;
  plug_params_default(&pp);
  plug_init(&pl, &pp, 0);
  pl.rec.verified_v = 12;
  n = plug_pack(&pl, img, sizeof(img));
  CHECK(hal_flash_write(HAL_PAGE_PLUG, img, n) == 0);
  const uint8_t slot[8] = {'R', '0', 0xE0, 0x93, 0x04, 0x00, 0, 0}; /* 300000, CRC filled below */
  uint8_t e[8];
  memcpy(e, slot, 8);
  uint16_t crc = link_crc16(e, 6);
  e[6] = (uint8_t) crc, e[7] = (uint8_t) (crc >> 8);
  CHECK(hal_flash_program(HAL_PAGE_PLUG, BOARD_CO_LOG_START, e, 8) == 0);
  board b;
  board_init(&b, chip.now, LINK_RESET_POWER_ON, 0);
  CHECK(b.s.burn.p.fan_max_rpm == 5000 && b.s.plug.rec.verified_v == 12 && b.s.co_r0_saved == 300000);

  /* Both pages now read with an ECC error: defaults, nothing remembered, no reset loop. */
  chip.ecc_bad[HAL_PAGE_PARAMS] = chip.ecc_bad[HAL_PAGE_PLUG] = 1;
  uint32_t writes = chip.flash_writes;
  board_init(&b, chip.now, LINK_RESET_POWER_ON, 0);
  CHECK(b.s.burn.p.fan_max_rpm == 4500 && b.s.burn.p.co_fitted == 1);
  CHECK(b.s.plug.rec.verified_v == 0 && b.s.co_r0_saved == 0 && b.s.co.r0 == 0);
  /* The next pass with the heater off rewrites both, erasing first. */
  run(&b, 20);
  CHECK(chip.flash_writes == writes + 2 && chip.unreadable == 0);
  CHECK(!chip.ecc_bad[0] && !chip.ecc_bad[1] && log_slots_used() == 0);
  board b2;
  board_init(&b2, chip.now, LINK_RESET_POWER_ON, 0);
  CHECK(b2.s.burn.p.fan_max_rpm == 4500 && b2.s.plug.rec.verified_v == 0 && chip.unreadable == 0);
}

/* ---------------------------------------------------------------- the fault log in flash */

/* The fault log's slots in use in page 30. */
static unsigned fault_slots_used(void) {
  unsigned used = 0;
  for (unsigned i = 0; i < BOARD_FAULT_LOG_SLOTS; i++) {
    const uint8_t *e = chip.flash[HAL_PAGE_PARAMS] + BOARD_FAULT_LOG_START + i * FAULTLOG_SLOT;
    for (unsigned k = 0; k < FAULTLOG_SLOT; k++)
      if (e[k] != 0xFF) {
        used++;
        break;
      }
  }
  return used;
}

/* An entry noted in the core as a pass would note it (a flame-out in the running step). */
static void note_fault(board *b, uint8_t fault) {
  burn_inputs in = b->s.in;
  faultlog_note(&b->s.log, b->now, 0, 0, BURN_STEP_OFF, &in);
  faultlog_note(&b->s.log, b->now, fault, BURN_WHY_FLAME_LOST, BURN_STEP_RUNNING, &in);
}

static void test_fault_log_flash(void) {
  chip_reset();
  /* A cold power-up: nothing to log, nothing written (DESIGN.md). */
  board b;
  board_init(&b, chip.now, LINK_RESET_POWER_ON, 0);
  run(&b, 50);
  CHECK(b.s.log.count == 0 && chip.flash_programs == 0 && chip.flash_writes == 0);
  /* A watchdog reset: its entry, written in the first pass with the heater off, one slot, no erase. */
  board_init(&b, chip.now, LINK_RESET_WATCHDOG, 0);
  CHECK(b.s.log.count == 1 && b.s.log.unsaved == 1);
  run(&b, 20);
  CHECK(chip.flash_programs == 1 && chip.flash_writes == 0 && fault_slots_used() == 1 && b.s.log.unsaved == 0);
  CHECK_EQ(b.log_next, 1);
  /* The same reset again: no second slot (a reset loop costs one). */
  board_init(&b, chip.now, LINK_RESET_WATCHDOG, 0);
  run(&b, 20);
  CHECK(chip.flash_programs == 1 && b.s.log.count == 1 && faultlog_newest_seq(&b.s.log) == 1);

  /* A fault while the heater runs: in RAM (the link reads it) but not in flash until it is off. */
  link_deadman_feed(&b.s.deadman, chip.now); /* the ESP32 is there: no dead-man stop */
  b.s.burn.step = BURN_STEP_RUNNING;
  note_fault(&b, BURN_FAULT_FLAME_OUT);
  run(&b, 100);
  CHECK(chip.flash_programs == 1 && b.s.log.count == 2 && b.s.log.unsaved == 1);
  b.s.burn.step = BURN_STEP_OFF;
  run(&b, 20);
  CHECK(chip.flash_programs == 2 && fault_slots_used() == 2 && b.s.log.unsaved == 0);
  /* A reset loads both, and the numbers go on. */
  board b2;
  board_init(&b2, chip.now, LINK_RESET_POWER_ON, 0);
  CHECK(b2.s.log.count == 2 && faultlog_newest_seq(&b2.s.log) == 2 && b2.log_next == 2);
  link_fault_entry e[2];
  CHECK_EQ(faultlog_read(&b2.s.log, 0, e, 2), 2);
  CHECK(e[0].kind == LINK_LOG_FAULT && e[0].code == BURN_FAULT_FLAME_OUT && e[0].step == BURN_STEP_RUNNING);
  CHECK(e[1].kind == LINK_LOG_RESET && e[1].code == LINK_RESET_WATCHDOG);

  /* An entry still unsaved when the MCU resets is lost (the heater was running). */
  link_deadman_feed(&b2.s.deadman, chip.now);
  b2.s.burn.step = BURN_STEP_RUNNING;
  note_fault(&b2, BURN_FAULT_SENSOR);
  run(&b2, 20);
  board b3;
  board_init(&b3, chip.now, LINK_RESET_POWER_ON, 0);
  CHECK(b3.s.log.count == 2 && faultlog_newest_seq(&b3.s.log) == 2);

  /* A torn slot (the power lost between its two double words): skipped, and the next one goes
   * after it. */
  note_fault(&b3, BURN_FAULT_SENSOR);
  run(&b3, 20);
  CHECK(fault_slots_used() == 3);
  memset(chip.flash[HAL_PAGE_PARAMS] + BOARD_FAULT_LOG_START + 2 * FAULTLOG_SLOT + 8, 0xFF, 8);
  board b4;
  board_init(&b4, chip.now, LINK_RESET_POWER_ON, 0);
  CHECK(b4.s.log.count == 2 && b4.log_next == 3);
  note_fault(&b4, BURN_FAULT_START);
  run(&b4, 20);
  CHECK(b4.log_next == 4 && chip.flash_bad_programs == 0 && b4.s.log.count == 3);

  /* Cleared: one slot (the marker), no erase; empty after a reset, and the numbers go on. */
  uint32_t writes = chip.flash_writes;
  faultlog_clear(&b4.s.log);
  run(&b4, 20);
  CHECK(chip.flash_writes == writes && b4.log_next == 5 && b4.s.log.count == 0);
  board b5;
  board_init(&b5, chip.now, LINK_RESET_POWER_ON, 0);
  CHECK(b5.s.log.count == 0 && b5.log_next == 5);
  note_fault(&b5, BURN_FAULT_GLOW);
  CHECK_EQ(faultlog_newest_seq(&b5.s.log), 4);
}

/* Fills page 30's log with valid slots from the n-th on (as earlier boots would have). */
static void fill_fault_log(unsigned from) {
  faultlog l;
  faultlog_init(&l, 0);
  burn_inputs in;
  memset(&in, 0, sizeof(in));
  for (unsigned i = from; i < BOARD_FAULT_LOG_SLOTS; i++) {
    faultlog_note(&l, 0, 0, 0, BURN_STEP_OFF, &in);
    faultlog_note(&l, 0, BURN_FAULT_FLAME_OUT, BURN_WHY_FLAME_LOST, BURN_STEP_RUNNING, &in);
    uint8_t slot[FAULTLOG_SLOT];
    faultlog_unsaved_slot(&l, 0, slot);
    faultlog_saved(&l);
    CHECK(hal_flash_program(HAL_PAGE_PARAMS, (uint16_t) (BOARD_FAULT_LOG_START + i * FAULTLOG_SLOT), slot,
                            FAULTLOG_SLOT) == 0);
  }
}

static void test_fault_log_full(void) {
  /* Saved parameters (fan max 5000) and a full log. */
  chip_reset();
  params ps;
  params_init(&ps);
  params_set(&ps, 0x11, 5000);
  CHECK(params_apply(&ps, 1, 1) == LINK_OK);
  uint8_t img[PARAMS_IMAGE_MAX];
  size_t n = params_pack(&ps, img, sizeof(img));
  CHECK(hal_flash_write(HAL_PAGE_PARAMS, img, n) == 0);
  fill_fault_log(0);
  uint8_t before[BOARD_FAULT_LOG_START];
  memcpy(before, chip.flash[HAL_PAGE_PARAMS], sizeof(before));
  board b;
  board_init(&b, chip.now, LINK_RESET_POWER_ON, 0);
  CHECK(b.log_next == BOARD_FAULT_LOG_SLOTS && b.s.log.count == FAULTLOG_SIZE);
  CHECK_EQ(faultlog_newest_seq(&b.s.log), BOARD_FAULT_LOG_SLOTS);
  CHECK_EQ(b.s.burn.p.fan_max_rpm, 5000);
  /* A new entry with the log full and the MCU up less than an hour: no erase; it waits in RAM. */
  uint32_t writes = chip.flash_writes;
  note_fault(&b, BURN_FAULT_FAN);
  run(&b, 20);
  CHECK(chip.flash_writes == writes && b.s.log.unsaved == 1);
  /* An hour up: the page is erased once, the parameter image goes back as it was, then the newest
   * 32 entries, the new one with them. */
  b.s.log.uptime_s = BOARD_FAULT_LOG_ERASE_S;
  run(&b, 20);
  CHECK(chip.flash_writes == writes + 1 && b.s.log.unsaved == 0 && chip.flash_bad_programs == 0);
  CHECK(memcmp(before, chip.flash[HAL_PAGE_PARAMS], n) == 0);
  CHECK_EQ(b.log_next, FAULTLOG_SIZE);
  CHECK_EQ(fault_slots_used(), FAULTLOG_SIZE);
  board b2;
  board_init(&b2, chip.now, LINK_RESET_POWER_ON, 0);
  CHECK(b2.s.burn.p.fan_max_rpm == 5000 && b2.s.log.count == FAULTLOG_SIZE && b2.log_next == FAULTLOG_SIZE);
  link_fault_entry e;
  CHECK_EQ(faultlog_read(&b2.s.log, 0, &e, 1), 1);
  CHECK(e.code == BURN_FAULT_FAN && e.seq == BOARD_FAULT_LOG_SLOTS + 1);
  CHECK_EQ(faultlog_read(&b2.s.log, FAULTLOG_SIZE - 1, &e, 1), 1);
  CHECK_EQ(e.seq, BOARD_FAULT_LOG_SLOTS + 1 - FAULTLOG_SIZE + 1);

  /* Full again soon after: the next erase waits for another hour of uptime. */
  fill_fault_log(FAULTLOG_SIZE);
  board_init(&b2, chip.now, LINK_RESET_POWER_ON, 0);
  b2.s.log.uptime_s = 2u * BOARD_FAULT_LOG_ERASE_S;
  run(&b2, 20);
  note_fault(&b2, BURN_FAULT_PUMP);
  run(&b2, 20);
  CHECK(chip.flash_writes == writes + 2 && b2.s.log.unsaved == 0);
  fill_fault_log(FAULTLOG_SIZE);
  note_fault(&b2, BURN_FAULT_GLOW);
  b2.log_next = BOARD_FAULT_LOG_SLOTS; /* as if the slots had been written here */
  b2.s.log.uptime_s += BOARD_FAULT_LOG_ERASE_S - 1u;
  run(&b2, 20);
  CHECK(chip.flash_writes == writes + 2 && b2.s.log.unsaved == 1);
  b2.s.log.uptime_s += 1u;
  run(&b2, 20);
  CHECK(chip.flash_writes == writes + 3 && b2.s.log.unsaved == 0);
}

static void test_fault_log_page_rewrite(void) {
  /* Saving the parameters erases page 30: the log goes back after the new image. */
  chip_reset();
  board b;
  board_init(&b, chip.now, LINK_RESET_SOFTWARE, 0);
  note_fault(&b, BURN_FAULT_OVERHEAT);
  run(&b, 20);
  CHECK(fault_slots_used() == 2 && chip.flash_writes == 0);
  b.s.params.live.fan_max_rpm = 4800;
  b.s.params.save_pending = 1;
  run(&b, 20);
  CHECK(chip.flash_writes == 1 && fault_slots_used() == 2 && chip.flash_bad_programs == 0 && b.log_next == 2);
  board b2;
  board_init(&b2, chip.now, LINK_RESET_POWER_ON, 0);
  CHECK(b2.s.burn.p.fan_max_rpm == 4800 && b2.s.log.count == 2 && faultlog_newest_seq(&b2.s.log) == 2);
  /* Unsaved entries go in with the rewrite (the heater is off for both). */
  note_fault(&b2, BURN_FAULT_SENSOR);
  b2.s.params.save_pending = 1;
  run(&b2, 20);
  CHECK(chip.flash_writes == 2 && fault_slots_used() == 3 && b2.s.log.unsaved == 0);

  /* The page reads with an ECC error: the parameters' defaults, the log's entries whose CRCs hold,
   * and the next pass rewrites the page with both. */
  chip.ecc_bad[HAL_PAGE_PARAMS] = 1;
  board b3;
  board_init(&b3, chip.now, LINK_RESET_POWER_ON, 0);
  CHECK(b3.s.burn.p.fan_max_rpm == 4500 && b3.s.params.save_pending && b3.s.log.count == 3);
  run(&b3, 20);
  CHECK(chip.flash_writes == 3 && !chip.ecc_bad[HAL_PAGE_PARAMS] && chip.unreadable == 0);
  CHECK(fault_slots_used() == 3 && b3.log_next == 3);
  board b4;
  board_init(&b4, chip.now, LINK_RESET_POWER_ON, 0);
  CHECK(b4.s.log.count == 3 && faultlog_newest_seq(&b4.s.log) == 3 && chip.unreadable == 0);
}

int main(void) {
  test_conversions();
  test_pump_oc_threshold();
  test_pump_oc_filter();
  test_schedule_and_watchdog();
  test_link();
  test_heartbeat_needs_the_loop();
  test_pump_overcurrent();
  test_glow_periods();
  test_flash_only_when_off();
  test_vdda();
  test_tick_check();
  test_co_heater_and_alarm();
  test_co_sensor_fault();
  test_co_not_fitted();
  test_co_r0_in_flash();
  test_flash_ecc_error();
  test_fault_log_flash();
  test_fault_log_full();
  test_fault_log_page_rewrite();
  return check_report("board");
}
