/* ADC counts to the units the core works in, from the board's dividers and sense scales
 * (DESIGN.md). Integer maths only. Host-tested in ../tests/test_board.c. */
#ifndef FLAMEKEEPER_ANALOG_H
#define FLAMEKEEPER_ANALOG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANALOG_UNKNOWN_TEMP ((int16_t) 0x8000)

/* VDDA from VREFINT (PWR-6: its calibration was taken at 3.0 V), mV. */
uint16_t analog_vdda_mv(uint16_t vref_raw, uint16_t vrefint_cal);
/* A 12-bit count as mV at this VDDA. */
uint32_t analog_mv(uint16_t raw, uint16_t vdda_mv);

uint16_t analog_vin_cv(uint16_t raw, uint16_t vdda_mv);     /* PWR-8: x 106.8 / 6.8 */
uint16_t analog_div19_cv(uint16_t raw, uint16_t vdda_mv);   /* GLOW_VSENSE, PUMP_VSENSE: x 19 */
uint16_t analog_glow_ca(uint16_t raw, uint16_t vdda_mv);    /* GLW-18: 9 mV per A */
uint16_t analog_pump_ma(uint16_t raw, uint16_t vdda_mv);    /* PMP-17: 0.45 V per A */
uint16_t analog_fan_ma(uint16_t raw, uint16_t vdda_mv);     /* FAN-8: 15 mV per A */

/* The thermistors (ratiometric: VDDA cancels). Open or out of range: ANALOG_UNKNOWN_TEMP. */
int16_t analog_hx_c10(uint16_t raw);     /* OVH-6: within 2 counts of full scale = open */
int16_t analog_board_c10(uint16_t raw);  /* TS-1: outside -40..125 C = a sensor fault */

#ifdef __cplusplus
}
#endif

#endif /* FLAMEKEEPER_ANALOG_H */
