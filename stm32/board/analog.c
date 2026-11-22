/* ADC counts to units: see analog.h. */
#include "analog.h"

typedef struct {
  uint16_t raw;
  int16_t c10;
} ntc_point;

#include "ntc_tables.h"

uint16_t analog_vdda_mv(uint16_t vref_raw, uint16_t vrefint_cal) {
  if (vref_raw == 0)
    return 3300; /* no reading yet: the nominal supply */
  uint32_t mv = 3000u * vrefint_cal / vref_raw;
  return (uint16_t) (mv > 3600u ? 3600u : mv);
}

uint32_t analog_mv(uint16_t raw, uint16_t vdda_mv) { return (uint32_t) raw * vdda_mv / 4095u; }

static uint16_t clamp16(uint32_t v) { return (uint16_t) (v > 0xFFFFu ? 0xFFFFu : v); }

uint16_t analog_vin_cv(uint16_t raw, uint16_t vdda_mv) {
  return clamp16(analog_mv(raw, vdda_mv) * 1068u / 680u); /* mV x 106.8 / 6.8 / 10 */
}

uint16_t analog_div19_cv(uint16_t raw, uint16_t vdda_mv) { return clamp16(analog_mv(raw, vdda_mv) * 19u / 10u); }

uint16_t analog_glow_ca(uint16_t raw, uint16_t vdda_mv) { return clamp16(analog_mv(raw, vdda_mv) * 100u / 9u); }

uint16_t analog_pump_ma(uint16_t raw, uint16_t vdda_mv) { return clamp16(analog_mv(raw, vdda_mv) * 1000u / 450u); }

uint16_t analog_fan_ma(uint16_t raw, uint16_t vdda_mv) { return clamp16(analog_mv(raw, vdda_mv) * 1000u / 15u); }

/* Counts fall as the temperature rises: interpolate between the table's points. */
static int16_t lookup(const ntc_point *t, unsigned n, uint16_t raw) {
  if (raw > t[0].raw || raw < t[n - 1].raw)
    return ANALOG_UNKNOWN_TEMP;
  for (unsigned i = 1; i < n; i++) {
    if (raw >= t[i].raw) {
      int32_t r0 = t[i - 1].raw, r1 = t[i].raw, c0 = t[i - 1].c10, c1 = t[i].c10;
      if (r0 == r1)
        return (int16_t) c1;
      return (int16_t) (c0 + (c1 - c0) * (r0 - (int32_t) raw) / (r0 - r1));
    }
  }
  return ANALOG_UNKNOWN_TEMP;
}

int16_t analog_hx_c10(uint16_t raw) {
  const unsigned n = sizeof(HX_NTC) / sizeof(HX_NTC[0]);
  if (raw >= 4093u)
    return ANALOG_UNKNOWN_TEMP; /* open (OVH-6) */
  if (raw < HX_NTC[n - 1].raw)
    return HX_NTC[n - 1].c10; /* hotter than the table, or shorted: reads as hot, never unknown */
  return lookup(HX_NTC, n, raw);
}

int16_t analog_board_c10(uint16_t raw) {
  int16_t c = lookup(BOARD_NTC, sizeof(BOARD_NTC) / sizeof(BOARD_NTC[0]), raw);
  if (c == ANALOG_UNKNOWN_TEMP || c < -400 || c > 1250)
    return ANALOG_UNKNOWN_TEMP; /* TS-1 */
  return c;
}
