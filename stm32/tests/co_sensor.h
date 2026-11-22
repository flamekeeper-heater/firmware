/* The CO sensor for the host tests (test_board, test_e2e, test_soak): a MiCS-5524 whose sensing layer
 * is 300 k in clean air (R0) and five times that with its heater off, on the board's 47 k load to
 * ground, read by the 12-bit ADC ratiometrically. The plant's own figures, not the CO alarm's. */
#ifndef FLAMEKEEPER_CO_SENSOR_H
#define FLAMEKEEPER_CO_SENSOR_H

#include <stdint.h>

/* The ADC count for air at Rs / R0 = ratio_permille / 1000, the heater on or off. */
static inline uint16_t co_sensor_adc(uint32_t ratio_permille, int heated) {
  uint64_t rs = 300000ull * ratio_permille / 1000u * (heated ? 1u : 5u);
  return (uint16_t) (4095ull * 47000u / (47000u + rs));
}

#endif
