/* The fake STM32 the board-layer tests run on (test_board.c, test_bringup.c): the HAL (board/hal.h)
 * over a few pins, ADC channels and the flash, the parts both tests see alike. Each test adds its
 * own plant: it defines fake_chip_adc() for the channels its models drive, hal_glow_period() for the
 * glow timer and hal_uart_write() for what the board sends.
 *
 * test_bringup builds this with build.sh's bring-up renames (hal_uart_getc to bringup_chip_uart_getc
 * and so on), so these functions sit under the console's taps as the driver's do on the chip. */
#ifndef FLAMEKEEPER_FAKE_CHIP_H
#define FLAMEKEEPER_FAKE_CHIP_H

#include <stddef.h>
#include <stdint.h>

#include "../board/hal.h"

extern const double VDDA, VREFINT; /* the 3.3 V rail; VREFINT's voltage */

/* The ADC count for a voltage on a pin, clamped to the converter's range. */
uint16_t counts(double volts);

typedef struct {
  uint32_t now;
  double vin;
  uint16_t ntc_raw, board_raw;
  double rpm; /* the fan, as the Hall input sees it (below 50 rpm: no edges) */
  /* Outputs, as the pins last were. */
  uint8_t hb_mode, glow_en, latch_level, co_heater;
  uint32_t burst_ms;    /* when the last burst was armed: it runs 25 ms by itself */
  uint32_t hb_start_ms; /* when the heartbeat last started from stopped */
  uint16_t fan_duty, pump_duty, glow_us, test_pulse_us;
  uint32_t glow_cmds; /* pulses the planner set (hal_glow_next_pulse_us) */
  /* Hardware state. */
  uint8_t latch_armed;
  uint8_t pump_open; /* the pump's coil is open: no current in a test pulse */
  uint32_t glow_next_ms; /* the glow timer's next period */
  uint32_t iwdg_refreshes;
  uint8_t rx[512]; /* what the UART will receive */
  size_t rx_len, rx_pos;
  /* The pump's overcurrent watchdog (PMP-7): armed with a threshold; its cut holds PUMP_CMD low
   * (pump_forced) until the next stroke arms it afresh, and is reported once. */
  uint8_t oc_armed, oc_report, pump_forced;
  uint16_t oc_threshold, oc_hold_us;
  /* The two saved pages. NOR: erased 0xFF, programmed only where erased. */
  uint8_t flash[2][HAL_FLASH_PAGE_SIZE];
  uint32_t flash_writes, flash_programs, flash_bad_programs;
  uint8_t ecc_bad[2]; /* a double ECC error somewhere in the page: reading it raises the NMI */
  uint8_t unreadable; /* what the NMI handler recorded, a bit per page */
} fake_chip;

extern fake_chip chip;

/* Everything back to power-on: the supply at vin, both thermistors at 25 C, the flash erased. */
void fake_chip_reset(double vin);
/* A heartbeat burst runs now. */
int hb_ok(void);
/* The pump's + side is up: a running heartbeat and an armed overheat latch. */
int pump_powered(void);

/* The test's part of hal_adc_scan: it sets HAL_ADC_FAN_I, HAL_ADC_PUMP_I and HAL_ADC_CO (zero until
 * then); the other channels follow the chip's pins and the supply. */
void fake_chip_adc(uint16_t raw[HAL_ADC_COUNT]);

#endif
