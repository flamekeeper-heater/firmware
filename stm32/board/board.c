/* The board layer: see board.h. */
#include "board.h"

#include <string.h>

#include "analog.h"

/* The second clock the driver gives is the one the core checks the tick against (GLW-1). */
typedef char second_clock_agrees[HAL_SECOND_CLOCK_HZ == SAFETY_CLOCK_HZ && HAL_SECOND_CLOCK_WRAP == SAFETY_CLOCK_WRAP ? 1 : -1];

/* The sequencer's measurements, from the last scan (vin_cv: VIN_SENSE from it). */
static burn_inputs read_inputs(board *b, uint16_t vin_cv) {
  burn_inputs in;
  memset(&in, 0, sizeof(in));
  in.hx_c10 = analog_hx_c10(b->raw[HAL_ADC_NTC]);
  in.fan_rpm = hal_hall_rpm();
  in.supply_cv = vin_cv;
  /* overheat_latch: the core's, from the latch readings (OVH-1); co_alarm, co_fault: the core's
   * own CO alarm (safety_co_step) */
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

/* ---------------------------------------------------------------- the fault log (parameter page) */

/* The parameter image must end before the log. */
typedef char params_before_fault_log[PARAMS_IMAGE_MAX <= BOARD_FAULT_LOG_START ? 1 : -1];

static int fault_slot_erased(const uint8_t *e) {
  for (unsigned i = 0; i < FAULTLOG_SLOT; i++)
    if (e[i] != 0xFFu)
      return 0;
  return 1;
}

/* At boot: the log's slots, oldest first, into the core's log; notes the next free slot. */
static void fault_log_scan(board *b) {
  const uint8_t *page = hal_flash_page(HAL_PAGE_PARAMS);
  b->log_next = 0;
  for (unsigned i = 0; i < BOARD_FAULT_LOG_SLOTS; i++) {
    const uint8_t *e = page + BOARD_FAULT_LOG_START + i * FAULTLOG_SLOT;
    if (fault_slot_erased(e))
      continue;
    b->log_next = (uint16_t) (i + 1u); /* written, or torn: skipped either way */
    (void) faultlog_load_slot(&b->s.log, e);
  }
}

/* A failed slot counts as written too: the next goes after it. */
static void fault_log_program(board *b, const uint8_t *slot) {
  uint16_t at = (uint16_t) (BOARD_FAULT_LOG_START + b->log_next * FAULTLOG_SLOT);
  b->log_next++;
  (void) hal_flash_program(HAL_PAGE_PARAMS, at, slot, FAULTLOG_SLOT);
}

/* Erases the parameter page and writes img there, then the whole fault log after it. */
static int params_page_write(board *b, const uint8_t *img, size_t n) {
  if (hal_flash_write(HAL_PAGE_PARAMS, img, n) != 0)
    return -1;
  b->log_next = 0;
  uint8_t slot[FAULTLOG_SLOT];
  for (unsigned i = 0; i < b->s.log.count; i++) {
    faultlog_slot(&b->s.log, i, slot);
    fault_log_program(b, slot);
  }
  faultlog_saved(&b->s.log);
  return 0;
}

/* The entries noted since the last write (and a clear), with the heater off. A full log rewrites
 * the page with the parameter image flash holds (not the applied values: those may be unsaved). */
static __attribute__((noinline)) void fault_log_flush(board *b) {
  faultlog *l = &b->s.log;
  unsigned n = faultlog_unsaved_slots(l);
  if (n == 0)
    return;
  if (b->log_next + n > BOARD_FAULT_LOG_SLOTS) {
    if (l->uptime_s - b->log_erased_s < BOARD_FAULT_LOG_ERASE_S)
      return; /* they wait in RAM */
    b->log_erased_s = l->uptime_s;
    uint8_t img[PARAMS_IMAGE_MAX];
    size_t len = hal_flash_read(HAL_PAGE_PARAMS, img, sizeof(img));
    while (len > 0 && img[len - 1u] == 0xFFu)
      len--;
    (void) params_page_write(b, img, len);
    return;
  }
  uint8_t slot[FAULTLOG_SLOT];
  for (unsigned i = 0; i < n; i++) {
    faultlog_unsaved_slot(l, i, slot);
    fault_log_program(b, slot);
  }
  faultlog_saved(l);
}

/* noinline here and in save(): the page buffer stays out of board_ms's frame, under the link's. */
static __attribute__((noinline)) void load(board *b) {
  uint8_t buf[PARAMS_IMAGE_MAX];
  /* A page read with a double ECC error (hal_flash_unreadable): nothing from it is trusted, the
   * defaults stand, and the next save erases and rewrites it. */
  size_t n = hal_flash_read(HAL_PAGE_PARAMS, buf, sizeof(buf));
  if (hal_flash_unreadable(HAL_PAGE_PARAMS))
    b->s.params.save_pending = 1;
  else if (n > 0)
    safety_load_params(&b->s, buf, n);
  /* The fault log: slots that fail their CRC are skipped. An ECC error in it (a torn slot) has the
   * page rewritten too, with the parameters as loaded and the log's good entries. */
  fault_log_scan(b);
  if (hal_flash_unreadable(HAL_PAGE_PARAMS))
    b->s.params.save_pending = 1;
  n = hal_flash_read(HAL_PAGE_PLUG, buf, sizeof(buf));
  uint32_t r0 = co_log_scan(b);
  if (hal_flash_unreadable(HAL_PAGE_PLUG)) {
    b->s.plug.save_pending = 1;
    r0 = 0;
  } else if (n > 0) {
    safety_load_plug(&b->s, buf, n);
  }
  safety_load_co_r0(&b->s, r0);
}

void board_init(board *b, uint32_t now, uint8_t reset_reason, uint8_t option_faults) {
  memset(b, 0, sizeof(*b));
  b->now = now;
  b->vrefint_cal = hal_vrefint_cal();
  hal_adc_scan(b->raw);
  b->vdda_mv = analog_vdda_mv(b->raw[HAL_ADC_VREF], b->vrefint_cal);
  burn_inputs in = read_inputs(b, analog_vin_cv(b->raw[HAL_ADC_VIN], b->vdda_mv));
  safety_init(&b->s, now, &in, reset_reason, option_faults);
  safety_vdda(&b->s, now, analog_vdda_check_mv(b->raw[HAL_ADC_VREF], b->vrefint_cal));
  load(b);
  safety_log_reset(&b->s); /* after the log's slots: the reset's entry is the newest */
  b->last_glow_ms = b->last_loop_ms = b->last_co_ms = now;
  hal_co_heater(safety_co_heater(&b->s)); /* CO-5: heated whenever the board is awake, if fitted */
}

/* The slow readings for the link's Detail record, with the pass's own conversions. */
static void fill_detail(board *b, const burn_inputs *in, uint16_t fan_ma, uint16_t pump_ma) {
  link_detail *d = &b->s.detail;
  uint16_t v = b->vdda_mv;
  d->board = in->board_c10;
  d->glow_v = analog_div19_cv(b->raw[HAL_ADC_GLOW_V], v);
  d->glow_a = analog_glow_ca(b->raw[HAL_ADC_GLOW_I], v);
  d->fan_a = (uint16_t) (fan_ma / 10u);
  d->pump_ma = pump_ma;
  d->vdda_mv = v;
}

/* Saved parameters, the plug record, R0 and the fault log go to flash only with the heater off: erasing stalls
 * the CPU for longer than a heartbeat burst lasts. */
static __attribute__((noinline)) void save(board *b) {
  if (!burn_is_off(&b->s.burn))
    return;
  if (safety_params_save_pending(&b->s)) {
    uint8_t buf[PARAMS_IMAGE_MAX];
    size_t n = params_pack(&b->s.params, buf, sizeof(buf));
    if (n > 0 && params_page_write(b, buf, n) == 0) /* the fault log goes back after the image */
      safety_params_saved(&b->s);
  }
  /* The erase takes the R0 log too: the saved R0 goes back first. */
  if (safety_plug_save_pending(&b->s) && plug_page_rewrite(b) == 0) {
    safety_plug_saved(&b->s);
    if (b->s.co_r0_saved != 0)
      (void) co_log_append(b, b->s.co_r0_saved);
  }
  /* A failed write counts as saved too: the next try waits for the next change, so a bad page is
   * not erased over and over. */
  uint32_t r0 = safety_co_r0_to_save(&b->s);
  if (r0 != 0) {
    (void) co_log_append(b, r0);
    safety_co_r0_saved(&b->s, r0);
  }
  fault_log_flush(b);
}

void board_ms(board *b, uint32_t now) {
  b->now = now;
  hal_adc_scan(b->raw);
  b->vdda_mv = analog_vdda_mv(b->raw[HAL_ADC_VREF], b->vrefint_cal);
  uint16_t v = b->vdda_mv;
  uint16_t vin = analog_vin_cv(b->raw[HAL_ADC_VIN], v);
  /* PWR-5: this millisecond's VDDA, before anything is driven from it. */
  safety_vdda(&b->s, now, analog_vdda_check_mv(b->raw[HAL_ADC_VREF], b->vrefint_cal));

  /* The link: every byte received, every reply sent at once. */
  int c;
  uint8_t reply[LINK_FRAME_MAX];
  while ((c = hal_uart_getc()) >= 0) {
    size_t n = safety_receive(&b->s, (uint8_t) c, now, reply);
    if (n > 0)
      hal_uart_write(reply, n);
  }

  /* Every millisecond: the self-tests and the pump strokes. The tests' readings only while one runs
   * or may start; the latch's every time (OVH-1). */
  checks_obs o;
  memset(&o, 0, sizeof(o));
  o.latch_armed = hal_overheat_n();
  o.pump_hs_ok = hal_pump_hs_flt_n();
  if (safety_checks_may_run(&b->s)) {
    o.vin_cv = vin;
    o.pump_v_cv = analog_div19_cv(b->raw[HAL_ADC_PUMP_V], v);
    o.glow_v_cv = analog_div19_cv(b->raw[HAL_ADC_GLOW_V], v);
    o.pump_peak_ma = analog_pump_ma(hal_pump_peak_raw(), v);
    o.glow_ca = analog_glow_ca(b->raw[HAL_ADC_GLOW_I], v);
    o.hx_c10 = analog_hx_c10(b->raw[HAL_ADC_NTC]);
  }
  pump_meas pm;
  pm.vin_cv = vin;
  pm.i_ma = analog_pump_ma(b->raw[HAL_ADC_PUMP_I], v);
  pm.overcurrent = hal_pump_overcurrent(); /* PMP-7: the watchdog's interrupt has already cut */
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
  if (io.pump_oc_armed) /* the threshold follows VDDA while armed; unused while not */
    b->oc_raw = analog_pump_oc_raw(b->s.pump.p.oc_ma, v);
  hal_pump_oc_arm(io.pump_oc_armed, b->oc_raw, b->s.pump.p.oc_hold_us);
  hal_pump_duty(io.pump_duty);
  if (io.pump_pulse_us)
    hal_pump_test_pulse_us(io.pump_pulse_us);
  b->glow_en_test = io.glow_en;

  /* Every glow period (5 ms): the planner's next pulse, from the readings that period's timer took
   * in its pulse and in the gap before it (GLW-6). Should the periods stop coming (the timer's
   * interrupt lost), it still steps every 10 ms, with no readings, so GLOW_EN keeps following the
   * burn and only probes go out. */
  hal_glow_sample gs;
  int period = hal_glow_period(&gs);
  if (period || since(now, b->last_glow_ms) >= 10u) {
    b->last_glow_ms = now;
    glow_meas gm;
    gm.vin_cv = period ? analog_vin_cv(gs.gap_vin, v) : vin;
    gm.pulse_ca = period ? analog_glow_ca(gs.pulse_i, v) : 0;
    gm.gap_ca = period ? analog_glow_ca(gs.gap_i, v) : 0;
    gm.airflow_ok = (uint8_t) (hal_hall_rpm() >= b->s.burn.p.glow_min_fan_rpm);
    gm.board_c10 = analog_board_c10(b->raw[HAL_ADC_BOARD]);
    gm.high_side_trip = (uint8_t) !hal_pwr_flt_n();
    glow_cmd gc = safety_glow_period(&b->s, now, &gm);
    b->glow_en_planner = gc.enable;
    hal_glow_next_pulse_us(gc.width_us);
  }
  if (safety_held(&b->s) && b->glow_en_planner) { /* PWR-5: at once, not at the next glow period */
    b->glow_en_planner = 0;
    hal_glow_next_pulse_us(0);
  }
  hal_glow_en((uint8_t) (b->glow_en_test || b->glow_en_planner));

  /* Every 10 ms: the safety loop pass. */
  if (since(now, b->last_loop_ms) >= 10u) {
    b->last_loop_ms = now;
    burn_inputs in = read_inputs(b, vin);
    /* Once a second: the CO alarm's reading (ratiometric: VDDA cancels), taken with the heater as it
     * said; an alarm or a sensor fault reaches the sequencer in this same pass. */
    if (since(now, b->last_co_ms) >= 1000u) {
      b->last_co_ms = now;
      safety_co_step(&b->s, now, b->raw[HAL_ADC_CO], in.board_c10);
    }
    hal_co_heater(safety_co_heater(&b->s));
    safety_clock_check(&b->s, now, hal_second_clock()); /* GLW-1: a fault reaches this pass */
    safety_step(&b->s, now, &in);
    fan_meas fm;
    fm.rpm = in.fan_rpm;
    fm.vin_cv = vin;
    fm.isense_ma = analog_fan_ma(b->raw[HAL_ADC_FAN_I], v);
    hal_fan_duty(safety_fan_step(&b->s, now, &fm));
    fill_detail(b, &in, fm.isense_ma, pm.i_ma);
    save(b);
    /* The pass completed: the next heartbeat burst (PMP-1) and the watchdog (WDG-16), here only. */
    if (b->want_burst) {
      hal_heartbeat(HAL_HB_BURST);
      b->bursts++;
    }
    hal_iwdg_refresh();
    b->loops++;
  }
}
