/* The fake STM32: see fake_chip.h. */
#include "fake_chip.h"

#include <string.h>

const double VDDA = 3.3, VREFINT = 1.212;
fake_chip chip;

uint16_t counts(double volts) {
  double c = volts / VDDA * 4095.0 + 0.5;
  return (uint16_t) (c > 4095 ? 4095 : c < 0 ? 0 : c);
}

void fake_chip_reset(double vin) {
  memset(&chip, 0, sizeof(chip));
  chip.vin = vin;
  chip.ntc_raw = 4057;   /* 25 C */
  chip.board_raw = 2048; /* 25 C */
  memset(chip.flash, 0xFF, sizeof(chip.flash));
}

int hb_ok(void) { return chip.hb_mode == HAL_HB_BURST && (uint32_t) (chip.now - chip.burst_ms) < 25u; }
int pump_powered(void) { return hb_ok() && chip.latch_armed; }

/* ---------------------------------------------------------------- the ADC */

void hal_adc_scan(uint16_t raw[HAL_ADC_COUNT]) {
  raw[HAL_ADC_GLOW_V] = counts(chip.glow_en && hb_ok() ? chip.vin / 19.0 : 0);
  raw[HAL_ADC_GLOW_I] = 0;
  raw[HAL_ADC_PUMP_V] = counts(pump_powered() ? chip.vin / 19.0 : 0.02);
  raw[HAL_ADC_FAN_I] = 0;
  raw[HAL_ADC_PUMP_I] = 0;
  raw[HAL_ADC_NTC] = chip.ntc_raw;
  raw[HAL_ADC_BOARD] = chip.board_raw;
  raw[HAL_ADC_VIN] = counts(chip.vin * 6.8 / 106.8);
  raw[HAL_ADC_VREF] = counts(chip.vdda > 0 ? VREFINT * VDDA / chip.vdda : VREFINT);
  raw[HAL_ADC_CO] = 0;
  fake_chip_adc(raw);
}
uint16_t hal_vrefint_cal(void) { return (uint16_t) (VREFINT / 3.0 * 4095.0 + 0.5); }
uint16_t hal_pump_peak_raw(void) {
  uint16_t r = chip.test_pulse_us && pump_powered() && !chip.pump_open ? counts(0.4 * 0.45) : 0;
  chip.test_pulse_us = 0;
  return r;
}

/* ---------------------------------------------------------------- the pins */

void hal_pump_oc_arm(uint8_t on, uint16_t threshold_raw, uint16_t hold_us) {
  if (on && !chip.oc_armed && !chip.oc_report)
    chip.pump_forced = 0; /* a new stroke: PUMP_CMD back to its PWM */
  chip.oc_armed = on;
  chip.oc_threshold = threshold_raw;
  chip.oc_hold_us = hold_us;
}
uint8_t hal_pump_overcurrent(void) {
  uint8_t r = chip.oc_report;
  chip.oc_report = 0;
  return r;
}
uint16_t hal_hall_rpm(void) { return chip.rpm < 50 ? 0 : (uint16_t) chip.rpm; }
uint8_t hal_overheat_n(void) { return chip.latch_armed; }
uint8_t hal_pump_hs_flt_n(void) { return 1; }
uint8_t hal_pwr_flt_n(void) { return 1; }
void hal_heartbeat(uint8_t mode) {
  if (mode == HAL_HB_BURST) {
    if (!hb_ok())
      chip.hb_start_ms = chip.now;
    chip.burst_ms = chip.now;
  }
  chip.hb_mode = mode;
}
void hal_glow_en(uint8_t on) { chip.glow_en = on; }
void hal_glow_next_pulse_us(uint16_t us) {
  chip.glow_us = us;
  chip.glow_cmds++;
}
void hal_fan_duty(uint16_t p) { chip.fan_duty = p; }
void hal_pump_duty(uint16_t p) { chip.pump_duty = p; }
void hal_pump_test_pulse_us(uint16_t us) { chip.test_pulse_us = us; }
void hal_latch_reset(uint8_t level) {
  if (level && !chip.latch_level && chip.ntc_raw > 1000)
    chip.latch_armed = 1; /* a rising edge re-arms it while the heat exchanger is cool */
  chip.latch_level = level;
}
void hal_co_heater(uint8_t on) { chip.co_heater = on; }
int hal_uart_getc(void) { return chip.rx_pos < chip.rx_len ? chip.rx[chip.rx_pos++] : -1; }
void hal_iwdg_refresh(void) { chip.iwdg_refreshes++; }
uint16_t hal_second_clock(void) {
  uint64_t ratio = chip.clock_permille ? chip.clock_permille : 1000u;
  return (uint16_t) ((uint64_t) chip.now * HAL_SECOND_CLOCK_HZ * ratio / 1000000u % HAL_SECOND_CLOCK_WRAP);
}

/* ---------------------------------------------------------------- the flash */

size_t hal_flash_read(uint8_t page, uint8_t *out, size_t cap) {
  size_t n = cap < 256u ? cap : 256u;
  if (chip.ecc_bad[page])
    chip.unreadable |= (uint8_t) (1u << page); /* the NMI, then the load completes with garbage */
  memcpy(out, chip.flash[page], n);
  return n;
}
int hal_flash_write(uint8_t page, const uint8_t *d, size_t n) {
  if (n > 256u)
    return -1;
  memset(chip.flash[page], 0xFF, HAL_FLASH_PAGE_SIZE);
  memcpy(chip.flash[page], d, n);
  chip.flash_writes++;
  chip.ecc_bad[page] = 0;
  chip.unreadable &= (uint8_t) ~(1u << page);
  return 0;
}
const uint8_t *hal_flash_page(uint8_t page) {
  if (chip.ecc_bad[page])
    chip.unreadable |= (uint8_t) (1u << page);
  return chip.flash[page];
}
uint8_t hal_flash_unreadable(uint8_t page) { return (uint8_t) ((chip.unreadable >> page) & 1u); }
int hal_flash_program(uint8_t page, uint16_t offset, const uint8_t *d, size_t n) {
  if ((offset & 7u) || (n & 7u) || offset + n > HAL_FLASH_PAGE_SIZE)
    return -1;
  for (size_t i = 0; i < n; i++)
    if (chip.flash[page][offset + i] != 0xFF) {
      chip.flash_bad_programs++;
      return -1;
    }
  memcpy(chip.flash[page] + offset, d, n);
  chip.flash_programs++;
  return 0;
}
