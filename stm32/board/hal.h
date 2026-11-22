/* What the board layer needs from the chip. hal_stm32g0.c implements it on the STM32G030's
 * registers; the host tests implement it with a fake chip. Every function is called from the main
 * loop only (the safety loop), never from an interrupt. */
#ifndef FLAMEKEEPER_HAL_H
#define FLAMEKEEPER_HAL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ADC channels, in the scan's order (CFG-1: bit-mask selection, ascending channel numbers). */
enum {
  HAL_ADC_GLOW_V = 0,  /* PA0  IN0: GLOW_VSENSE, the plug's + side / 19 */
  HAL_ADC_GLOW_I,      /* PA1  IN1: GLOW_ISENSE between pulses, 9 mV/A */
  HAL_ADC_PUMP_V,      /* PA4  IN4: PUMP_VSENSE, the pump's + wire / 19 */
  HAL_ADC_FAN_I,       /* PA5  IN5: FAN_ISENSE, 15 mV/A of average switch current */
  HAL_ADC_PUMP_I,      /* PA6  IN6: PUMP_ISENSE, 0.45 V/A */
  HAL_ADC_NTC,         /* PB1  IN9: the heat-exchanger thermistor, ratiometric */
  HAL_ADC_BOARD,       /* PB2  IN10: BOARD_NTC, ratiometric */
  HAL_ADC_VIN,         /* PB7  IN11: VIN_SENSE = VIN x 6.8 / 106.8 */
  HAL_ADC_VREF,        /* IN13: VREFINT */
  HAL_ADC_CO,          /* PA11 IN15: CO_ADC, ratiometric */
  HAL_ADC_COUNT
};

void hal_adc_scan(uint16_t raw[HAL_ADC_COUNT]); /* one fresh conversion of every channel */
uint16_t hal_vrefint_cal(void);                 /* the factory VREFINT reading at VDDA = 3.0 V */
uint16_t hal_glow_pulse_raw(void);  /* GLOW_ISENSE sampled 100-150 us into the last pulse */
uint16_t hal_pump_peak_raw(void);   /* the highest PUMP_ISENSE during the last test pulse */
uint8_t hal_pump_overcurrent(void); /* the ADC watchdog cut a stroke since the last call */
uint16_t hal_hall_rpm(void);        /* 0 when no edge for a while */

uint8_t hal_overheat_n(void);    /* OVERHEAT_N_MCU (PC14): 1 = the latch is armed */
uint8_t hal_pump_hs_flt_n(void); /* PUMP_HS_FLT_N (PC15): 0 = fault */
uint8_t hal_pwr_flt_n(void);     /* PWR_FLT_N (PA7): 0 = the glow high side latched */

enum { HAL_HB_STOPPED = 0, HAL_HB_STATIC_HIGH = 1, HAL_HB_STATIC_LOW = 2, HAL_HB_BURST = 3 };
/* HEARTBEAT (PA8, TIM1_CH1): BURST arms one burst of 25 periods that ends by itself (PMP-1). */
void hal_heartbeat(uint8_t mode);
void hal_glow_en(uint8_t on);                /* GLOW_EN (PB0) */
void hal_glow_next_pulse_us(uint16_t us);    /* GLOW_PWM (PB4, TIM3_CH1): the next period's pulse */
void hal_fan_duty(uint16_t permille);        /* FAN_PWM (PB3, TIM1_CH2), 25 kHz */
void hal_pump_duty(uint16_t permille);       /* PUMP_CMD (PB9, TIM17_CH1), ~20 kHz */
void hal_pump_test_pulse_us(uint16_t us);    /* one PUMP_CMD pulse of this length */
void hal_latch_reset(uint8_t level);         /* LATCH_RESET (PB6) */
void hal_co_heater(uint8_t on);              /* CO_HEAT_EN (PC6) */

int hal_uart_getc(void);                     /* LINK_RX (USART2): a byte, or -1 */
void hal_uart_write(const uint8_t *data, size_t len);

void hal_iwdg_refresh(void);                 /* WDG-16: only ever from the safety loop pass */

/* Flash pages for the saved parameters and the plug record. Erasing stalls the CPU: the board
 * layer only writes while the heater is off. */
enum { HAL_PAGE_PARAMS = 0, HAL_PAGE_PLUG = 1 };
size_t hal_flash_read(uint8_t page, uint8_t *out, size_t cap);
int hal_flash_write(uint8_t page, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* FLAMEKEEPER_HAL_H */
