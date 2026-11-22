/* The board layer: see board.h. */
#include "board.h"

#include <string.h>

#include "analog.h"

static uint32_t since(uint32_t now, uint32_t then) { return (uint32_t) (now - then); }

/* The sequencer's measurements, from the last scan. */
static burn_inputs read_inputs(board *b) {
  burn_inputs in;
  memset(&in, 0, sizeof(in));
  in.hx_c10 = analog_hx_c10(b->raw[HAL_ADC_NTC]);
  in.fan_rpm = hal_hall_rpm();
  in.supply_cv = analog_vin_cv(b->raw[HAL_ADC_VIN], b->vdda_mv);
  /* OVH-1: the latch reads tripped after every power-up until the self-test re-arms it; only a
   * fall after that is a hardware overheat trip. */
  in.overheat_latch = (uint8_t) (b->latch_was_armed && !hal_overheat_n());
  /* co_alarm, co_fault: the core's own CO alarm (safety_co_step) */
  in.board_c10 = analog_board_c10(b->raw[HAL_ADC_BOARD]);
  return in;
}

/* ---------------------------------------------------------------- the CO sensor's R0 log */

/* A slot: 'R' '0', R0 in ohm (4 bytes, little-endian), CRC16 of those six. Erased: all 0xFF. */
static int slot_erased(const uint8_t *e) {
  for (unsigned i = 0; i < BOARD_CO_LOG_SLOT; i++)
    if (e[i] != 0xFFu)
      return 0;
  return 1;
}

/* The last valid R0 in the log (0: none); notes the next free slot. */
static uint32_t co_log_scan(board *b) {
  const uint8_t *page = hal_flash_page(HAL_PAGE_PLUG);
  uint32_t r0 = 0;
  b->co_log_next = 0;
  for (unsigned i = 0; i < BOARD_CO_LOG_SLOTS; i++) {
    const uint8_t *e = page + BOARD_CO_LOG_START + i * BOARD_CO_LOG_SLOT;
    if (slot_erased(e))
      continue;
    b->co_log_next = (uint16_t) (i + 1u); /* written, or torn: skipped either way */
    if (e[0] == 'R' && e[1] == '0' && link_crc16(e, 6) == (uint16_t) (e[6] | (e[7] << 8)))
      r0 = (uint32_t) e[2] | ((uint32_t) e[3] << 8) | ((uint32_t) e[4] << 16) | ((uint32_t) e[5] << 24);
  }
  return r0;
}

/* Erases the plug page, keeping the plug record. */
static int plug_page_rewrite(board *b) {
  uint8_t img[PLUG_IMAGE_SIZE];
  size_t n = plug_pack(&b->s.plug, img, sizeof(img));
  if (n == 0 || hal_flash_write(HAL_PAGE_PLUG, img, n) != 0)
    return -1;
  b->co_log_next = 0;
  return 0;
}

static int co_log_append(board *b, uint32_t r0) {
  if (b->co_log_next >= BOARD_CO_LOG_SLOTS && plug_page_rewrite(b) != 0)
    return -1;
  uint8_t e[BOARD_CO_LOG_SLOT] = {'R', '0', (uint8_t) r0, (uint8_t) (r0 >> 8), (uint8_t) (r0 >> 16),
                                  (uint8_t) (r0 >> 24), 0, 0};
  uint16_t crc = link_crc16(e, 6);
  e[6] = (uint8_t) crc, e[7] = (uint8_t) (crc >> 8);
  uint16_t at = (uint16_t) (BOARD_CO_LOG_START + b->co_log_next * BOARD_CO_LOG_SLOT);
  b->co_log_next++;
  return hal_flash_program(HAL_PAGE_PLUG, at, e, sizeof(e));
}

static void load(board *b) {
  uint8_t buf[PARAMS_IMAGE_MAX];
  size_t n = hal_flash_read(HAL_PAGE_PARAMS, buf, sizeof(buf));
  if (n > 0)
    safety_load_params(&b->s, buf, n);
  n = hal_flash_read(HAL_PAGE_PLUG, buf, sizeof(buf));
  if (n > 0)
    safety_load_plug(&b->s, buf, n);
  safety_load_co_r0(&b->s, co_log_scan(b));
}

void board_init(board *b, uint32_t now, uint8_t reset_reason, uint8_t option_faults) {
  memset(b, 0, sizeof(*b));
  b->now = now;
  b->vrefint_cal = hal_vrefint_cal();
  hal_adc_scan(b->raw);
  b->vdda_mv = analog_vdda_mv(b->raw[HAL_ADC_VREF], b->vrefint_cal);
  burn_params bp;
  glow_params gp;
  fan_params fp;
  checks_params cp;
  pump_params pp;
  burn_params_default(&bp);
  glow_params_default(&gp);
  fan_params_default(&fp);
  checks_params_default(&cp);
  pump_params_default(&pp);
  burn_inputs in = read_inputs(b);
  safety_init(&b->s, &bp, &gp, &fp, &cp, &pp, now, &in, reset_reason, option_faults);
  load(b);
  b->last_glow_ms = b->last_loop_ms = b->last_co_ms = now;
  hal_co_heater(safety_co_heater(&b->s)); /* CO-5: heated whenever the board is awake, if fitted */
}

/* The slow readings for the link's Detail record. */
static void fill_detail(board *b) {
  link_detail *d = &b->s.detail;
  uint16_t v = b->vdda_mv;
  d->board = analog_board_c10(b->raw[HAL_ADC_BOARD]);
  d->glow_v = analog_div19_cv(b->raw[HAL_ADC_GLOW_V], v);
  d->glow_a = analog_glow_ca(b->raw[HAL_ADC_GLOW_I], v);
  d->fan_a = (uint16_t) (analog_fan_ma(b->raw[HAL_ADC_FAN_I], v) / 10u);
  d->pump_ma = analog_pump_ma(b->raw[HAL_ADC_PUMP_I], v);
  d->vdda_mv = v;
}

/* Saved parameters, the plug record and R0 go to flash only with the heater off: erasing stalls
 * the CPU for longer than a heartbeat burst lasts. */
static void save(board *b) {
  if (b->s.burn.step != BURN_STEP_OFF)
    return;
  uint8_t buf[PARAMS_IMAGE_MAX];
  if (safety_params_save_pending(&b->s)) {
    size_t n = params_pack(&b->s.params, buf, sizeof(buf));
    if (n > 0 && hal_flash_write(HAL_PAGE_PARAMS, buf, n) == 0)
      safety_params_saved(&b->s);
  }
  if (safety_plug_save_pending(&b->s)) {
    size_t n = plug_pack(&b->s.plug, buf, sizeof(buf));
    if (n > 0 && hal_flash_write(HAL_PAGE_PLUG, buf, n) == 0) {
      safety_plug_saved(&b->s);
      b->co_log_next = 0; /* the erase took the R0 log: the saved R0 goes back first */
      if (b->s.co_r0_saved != 0)
        (void) co_log_append(b, b->s.co_r0_saved);
    }
  }
  /* A failed write counts as saved too: the next try waits for the next change, so a bad page is
   * not erased over and over. */
  uint32_t r0 = safety_co_r0_to_save(&b->s);
  if (r0 != 0) {
    (void) co_log_append(b, r0);
    safety_co_r0_saved(&b->s, r0);
  }
}

void board_ms(board *b, uint32_t now) {
  b->now = now;
  hal_adc_scan(b->raw);
  b->vdda_mv = analog_vdda_mv(b->raw[HAL_ADC_VREF], b->vrefint_cal);
  uint16_t v = b->vdda_mv;
  uint16_t vin = analog_vin_cv(b->raw[HAL_ADC_VIN], v);

  /* The link: every byte received, every reply sent at once. */
  int c;
  uint8_t reply[LINK_FRAME_MAX];
  while ((c = hal_uart_getc()) >= 0) {
    size_t n = safety_receive(&b->s, (uint8_t) c, now, reply);
    if (n > 0)
      hal_uart_write(reply, n);
  }

  /* Every millisecond: the self-tests and the pump strokes. */
  checks_obs o;
  o.vin_cv = vin;
  o.pump_v_cv = analog_div19_cv(b->raw[HAL_ADC_PUMP_V], v);
  o.glow_v_cv = analog_div19_cv(b->raw[HAL_ADC_GLOW_V], v);
  o.pump_peak_ma = analog_pump_ma(hal_pump_peak_raw(), v);
  o.glow_ca = analog_glow_ca(b->raw[HAL_ADC_GLOW_I], v);
  o.latch_armed = hal_overheat_n();
  o.pump_hs_ok = hal_pump_hs_flt_n();
  o.hx_c10 = analog_hx_c10(b->raw[HAL_ADC_NTC]);
  pump_meas pm;
  pm.vin_cv = vin;
  pm.i_ma = analog_pump_ma(b->raw[HAL_ADC_PUMP_I], v);
  pm.overcurrent = hal_pump_overcurrent();
  pm.hs_ok = o.pump_hs_ok;
  pm.hb_running_ms = 0;
  safety_io io = safety_ms(&b->s, now, &o, &pm);

  /* A running heartbeat is re-armed by the loop pass below; a static level or a stop is at once.
   * Going from stopped to running, the first burst starts now (the last pass completed). */
  if (io.heartbeat == CHECKS_HB_RUNNING) {
    if (!b->want_burst) {
      hal_heartbeat(HAL_HB_BURST);
      b->bursts++;
    }
    b->want_burst = 1;
  } else {
    b->want_burst = 0;
    hal_heartbeat(io.heartbeat == CHECKS_HB_STATIC_HIGH  ? HAL_HB_STATIC_HIGH
                  : io.heartbeat == CHECKS_HB_STATIC_LOW ? HAL_HB_STATIC_LOW
                                                         : HAL_HB_STOPPED);
  }
  hal_latch_reset(io.latch_reset);
  hal_pump_duty(io.pump_duty);
  if (io.pump_pulse_us)
    hal_pump_test_pulse_us(io.pump_pulse_us);
  b->glow_en_test = io.glow_en;

  /* Every 5 ms: the glow planner's next pulse. */
  if (since(now, b->last_glow_ms) >= 5u) {
    b->last_glow_ms = now;
    glow_meas gm;
    gm.vin_cv = vin;
    gm.pulse_ca = analog_glow_ca(hal_glow_pulse_raw(), v);
    gm.gap_ca = analog_glow_ca(b->raw[HAL_ADC_GLOW_I], v);
    gm.airflow_ok = (uint8_t) (hal_hall_rpm() >= b->s.burn.p.glow_min_fan_rpm);
    gm.board_c10 = analog_board_c10(b->raw[HAL_ADC_BOARD]);
    gm.high_side_trip = (uint8_t) !hal_pwr_flt_n();
    glow_cmd gc = safety_glow_period(&b->s, now, &gm);
    b->glow_en_planner = gc.enable;
    hal_glow_next_pulse_us(gc.width_us);
  }
  hal_glow_en((uint8_t) (b->glow_en_test || b->glow_en_planner));

  /* Every 10 ms: the safety loop pass. */
  if (since(now, b->last_loop_ms) >= 10u) {
    b->last_loop_ms = now;
    burn_inputs in = read_inputs(b);
    /* Once a second: the CO alarm's reading (ratiometric: VDDA cancels), taken with the heater as it
     * said; an alarm or a sensor fault reaches the sequencer in this same pass. */
    if (since(now, b->last_co_ms) >= 1000u) {
      b->last_co_ms = now;
      safety_co_step(&b->s, now, b->raw[HAL_ADC_CO], in.board_c10);
    }
    hal_co_heater(safety_co_heater(&b->s));
    safety_step(&b->s, now, &in);
    fan_meas fm;
    fm.rpm = in.fan_rpm;
    fm.vin_cv = vin;
    fm.isense_ma = analog_fan_ma(b->raw[HAL_ADC_FAN_I], v);
    hal_fan_duty(safety_fan_step(&b->s, now, &fm));
    if (b->s.chk.power_up_done && b->s.power_up_failed == 0 && hal_overheat_n())
      b->latch_was_armed = 1;
    fill_detail(b);
    save(b);
    /* The pass completed: the next heartbeat burst (PMP-1) and the watchdog (WDG-16), here only. */
    if (b->want_burst) {
      hal_heartbeat(HAL_HB_BURST);
      b->bursts++;
    }
    hal_iwdg_refresh();
    b->refreshes++;
    b->loops++;
  }
}
