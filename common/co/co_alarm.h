/* The CO alarm of the Flamekeeper safety MCU (DESIGN.md): the MiCS-5524 heated
 * metal-oxide sensor read as a divider on CO_ADC, judged on Rs / R0 with R0 learnt in clean air.
 * Plain C99 with integer maths only (the STM32G0 has no FPU) and no hardware access: the MCU's
 * main loop feeds it one ADC reading a second, switches CO_HEAT_EN as it says, latches the faults
 * it reports and keeps R0 in flash. Host-tested in tests/.
 *
 * Not a certified CO alarm (rev A2 design notes): it alarms at really high CO only and accepts a
 * nuisance shutdown (the layer also reacts to solvents, alcohol and humidity).
 *
 * Time is a wrapping uint32_t millisecond count; elapsed time is always (uint32_t)(now - then). */
#ifndef FLAMEKEEPER_CO_ALARM_H
#define FLAMEKEEPER_CO_ALARM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The app protocol's CO sensor states (PROTOCOL.md). */
enum {
  CO_NOT_FITTED = 0,
  CO_WARMING_UP = 1,
  CO_LEARNING = 2,
  CO_OK = 3,
  CO_ALARM = 4,
  CO_SENSOR_FAULT = 5,
  CO_TOO_COLD = 6
};

#define CO_ADC_FULL 4095u          /* 12-bit, ratiometric to V3V3 */
#define CO_LOAD_OHM 47000u         /* R_CO_LOAD */
#define CO_OPEN_ADC 21u            /* CO_OPEN_FRAC_MAX 0.5 % of full scale */
#define CO_UNKNOWN_PPM 0xFFFFu
#define CO_UNKNOWN_TEMP ((int16_t) -32768)

typedef struct {
  uint8_t fitted;             /* CO_FITTED: the only authority on presence */
  uint32_t warmup_ms;         /* readings ignored after power-up (ASSUMED 3 min) */
  uint32_t learn_ms;          /* clean-air learning before "ok" (10 min; 24 h for a hand-fitted part) */
  uint16_t alarm_ratio;       /* Rs / R0 in 1/1000 at or below which... (30: about 300 ppm) */
  uint32_t alarm_ms;          /* ...for this long raises the alarm (ASSUMED 60 s) */
  uint16_t alarm_fast_ratio;  /* (10: about 1000 ppm) */
  uint32_t alarm_fast_ms;     /* (ASSUMED 10 s) */
  uint16_t clear_ratio;       /* a reset clears the alarm only at or above this (300: clean-ish air) */
  uint32_t clear_ms;          /* ...held this long (ASSUMED 60 s) */
  uint32_t open_ms;           /* an implausible (open) reading this long is a sensor fault (5 s) */
  uint32_t check_off_ms;      /* heater check: heater off this long once per warm-up (5 s) */
  uint16_t check_drop_pct;    /* ...and the ADC reading must fall at least this much (ASSUMED 30 %) */
  int16_t cold_c10;           /* below this board temperature the reading is not trusted (-30 C) */
} co_config;

/* The defaults above, CO fitted. */
void co_default_config(co_config *cfg);

typedef struct {
  co_config cfg;
  uint32_t start_ms;
  uint8_t started;
  uint8_t state;        /* CO_* */
  uint8_t heater_on;    /* what CO_HEAT_EN should be now */
  uint8_t alarm;        /* latched: needs co_reset() */
  uint8_t fault;        /* latched sensor fault: needs co_reset() */
  uint8_t checked;      /* the heater check passed in this warm-up */
  uint8_t checking;     /* the heater is off for the check */
  uint32_t check_start_ms;
  uint16_t check_adc_before;
  uint32_t r0;          /* ohm, the clean-air resistance (0: not learnt) */
  uint32_t r0_aged_ms;  /* the last slow decay of R0 */
  uint32_t learned_ms;  /* time spent learning since warm-up */
  uint32_t low_since_ms, fast_since_ms, open_since_ms, clean_since_ms;
  uint8_t low, fast, open, clean;
  uint32_t last_ms;
  uint16_t ratio;       /* Rs / R0 in 1/1000 (0xFFFF unknown) */
  uint16_t ppm;         /* a rough estimate from the datasheet curve (CO_UNKNOWN_PPM) */
} co_alarm;

void co_init(co_alarm *c, const co_config *cfg);
/* A stored R0 from a previous run (0: none); the alarm then needs no learning time. */
void co_load_r0(co_alarm *c, uint32_t r0_ohm);
/* One reading (about once a second): the ADC count (0..CO_ADC_FULL) taken with the heater as
 * co_heater_on() said, and the board temperature in 0.1 C (CO_UNKNOWN_TEMP if not known). */
void co_step(co_alarm *c, uint32_t now, uint16_t adc, int16_t board_c10);
/* What CO_HEAT_EN should be. */
uint8_t co_heater_on(const co_alarm *c);
/* The user's fault reset (FAULT_RESET): clears the alarm once the air has been clean for clear_ms,
 * the sensor fault once the reading is plausible. Returns 1 when nothing is latched afterwards. */
int co_reset(co_alarm *c, uint32_t now);

/* Rs in ohm from an ADC count (UINT32_MAX for an open reading). */
uint32_t co_rs_ohm(uint16_t adc);
/* The rough ppm estimate for Rs / R0 in 1/1000 (CO_UNKNOWN_PPM above the curve's clean end). */
uint16_t co_ppm_from_ratio(uint16_t ratio);

#ifdef __cplusplus
}
#endif

#endif
