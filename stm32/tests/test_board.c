/* The board layer against a fake chip: the conversions, the schedule (1 / 5 / 10 ms), the watchdog
 * and heartbeat only from a completed loop pass, the power-up self-test through the real interlock
 * lines, the link over the UART, and flash writes only with the heater off. */
#include <stdio.h>
#include <string.h>

#include "../board/analog.h"
#include "../board/board.h"

static int n_checks = 0, failures = 0;
#define CHECK(c)                                                  \
  do {                                                            \
    n_checks++;                                                     \
    if (!(c)) {                                                   \
      failures++;                                                 \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);         \
    }                                                             \
  } while (0)

/* ---------------------------------------------------------------- the fake chip */

static const double VDDA = 3.3, VREFINT = 1.212;
static uint16_t counts(double volts) { return (uint16_t) (volts / VDDA * 4095.0 + 0.5); }

static struct {
  uint32_t now;
  double vin;
  uint16_t ntc_raw, board_raw;
  /* Outputs, as the pins last were. */
  uint8_t hb_mode, glow_en, latch_level;
  uint32_t burst_ms;  /* when the last burst was armed: it runs 25 ms by itself */
  uint16_t fan_duty, pump_duty, glow_us, test_pulse_us;
  /* Hardware state. */
  uint8_t latch_armed;
  uint32_t iwdg_refreshes;
  uint8_t rx[256];
  size_t rx_len, rx_pos;
  uint8_t tx[256];
  size_t tx_len;
  uint8_t flash[2][256];
  size_t flash_len[2];
  uint32_t flash_writes;
} chip;

static int hb_ok(void) {
  return chip.hb_mode == HAL_HB_BURST && (uint32_t) (chip.now - chip.burst_ms) < 25u;
}

void hal_adc_scan(uint16_t raw[HAL_ADC_COUNT]) {
  int pump_on = hb_ok() && chip.latch_armed;
  raw[HAL_ADC_GLOW_V] = counts(chip.glow_en && hb_ok() ? chip.vin / 19.0 : 0);
  raw[HAL_ADC_GLOW_I] = 0;
  raw[HAL_ADC_PUMP_V] = counts(pump_on ? chip.vin / 19.0 : 0.02);
  raw[HAL_ADC_FAN_I] = 0;
  raw[HAL_ADC_PUMP_I] = 0;
  raw[HAL_ADC_NTC] = chip.ntc_raw;
  raw[HAL_ADC_BOARD] = chip.board_raw;
  raw[HAL_ADC_VIN] = counts(chip.vin * 6.8 / 106.8);
  raw[HAL_ADC_VREF] = counts(VREFINT);
  raw[HAL_ADC_CO] = counts(1.0);
}
uint16_t hal_vrefint_cal(void) { return (uint16_t) (VREFINT / 3.0 * 4095.0 + 0.5); }
uint16_t hal_glow_pulse_raw(void) { return 0; }
uint16_t hal_pump_peak_raw(void) {
  uint16_t r = chip.test_pulse_us && hb_ok() && chip.latch_armed ? counts(0.4 * 0.45) : 0;
  chip.test_pulse_us = 0;
  return r;
}
uint8_t hal_pump_overcurrent(void) { return 0; }
uint16_t hal_hall_rpm(void) { return 0; }
uint8_t hal_overheat_n(void) { return chip.latch_armed; }
uint8_t hal_pump_hs_flt_n(void) { return 1; }
uint8_t hal_pwr_flt_n(void) { return 1; }
void hal_heartbeat(uint8_t mode) {
  chip.hb_mode = mode;
  if (mode == HAL_HB_BURST)
    chip.burst_ms = chip.now;
}
void hal_glow_en(uint8_t on) { chip.glow_en = on; }
void hal_glow_next_pulse_us(uint16_t us) { chip.glow_us = us; }
void hal_fan_duty(uint16_t p) { chip.fan_duty = p; }
void hal_pump_duty(uint16_t p) { chip.pump_duty = p; }
void hal_pump_test_pulse_us(uint16_t us) { chip.test_pulse_us = us; }
void hal_latch_reset(uint8_t level) {
  if (level && !chip.latch_level && chip.ntc_raw > 1000)
    chip.latch_armed = 1; /* a rising edge re-arms it while the heat exchanger is cool */
  chip.latch_level = level;
}
void hal_co_heater(uint8_t on) { (void) on; }
int hal_uart_getc(void) { return chip.rx_pos < chip.rx_len ? chip.rx[chip.rx_pos++] : -1; }
void hal_uart_write(const uint8_t *d, size_t n) {
  if (chip.tx_len + n <= sizeof(chip.tx)) {
    memcpy(chip.tx + chip.tx_len, d, n);
    chip.tx_len += n;
  }
}
void hal_iwdg_refresh(void) { chip.iwdg_refreshes++; }
size_t hal_flash_read(uint8_t page, uint8_t *out, size_t cap) {
  size_t n = chip.flash_len[page] < cap ? chip.flash_len[page] : cap;
  memcpy(out, chip.flash[page], n);
  return n;
}
int hal_flash_write(uint8_t page, const uint8_t *d, size_t n) {
  memcpy(chip.flash[page], d, n);
  chip.flash_len[page] = n;
  chip.flash_writes++;
  return 0;
}

static void chip_reset(void) {
  memset(&chip, 0, sizeof(chip));
  chip.vin = 12.6;
  chip.ntc_raw = 4057;   /* 25 C */
  chip.board_raw = 2048; /* 25 C */
}

static void run(board *b, uint32_t ms) {
  for (uint32_t i = 0; i < ms; i++) {
    chip.now++;
    board_ms(b, chip.now);
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
  CHECK(chip.latch_armed && b.latch_was_armed);
  CHECK(b.s.burn.fault == BURN_FAULT_NONE);
  CHECK(chip.hb_mode == HAL_HB_STOPPED); /* off: no heartbeat */
}

static void test_link(void) {
  chip_reset();
  board b;
  board_init(&b, chip.now, LINK_RESET_POWER_ON, 0);
  const uint8_t hello[2] = {LINK_MAJOR, LINK_MINOR};
  chip.rx_len = link_build_request(5, LINK_OP_HELLO, hello, 2, chip.rx);
  run(&b, 2);
  CHECK(chip.tx_len > 7);
  CHECK(chip.tx[0] == LINK_REPLY && link_frame_seq(chip.tx) == 5 && link_frame_op(chip.tx) == LINK_OP_HELLO);
  link_info info;
  CHECK(link_decode_info(link_reply_payload(chip.tx), link_reply_payload_len(chip.tx), &info));
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

int main(void) {
  test_conversions();
  test_schedule_and_watchdog();
  test_link();
  test_heartbeat_needs_the_loop();
  test_flash_only_when_off();
  printf("board: %d checks, %d failures\n", n_checks, failures);
  return failures ? 1 : 0;
}
