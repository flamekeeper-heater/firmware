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
  test_co_heater_and_alarm();
  test_co_sensor_fault();
  test_co_not_fitted();
  test_co_r0_in_flash();
  test_flash_ecc_error();
  return check_report("board");
}
