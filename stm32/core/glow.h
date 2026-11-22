/* The glow plug's pulse planner (DESIGN.md): the plug is switched straight from the supply, and
 * its power is set pulse by pulse. Called once per PWM period with that period's readings, it
 * returns whether the high side may be enabled and how long the next pulse is. Plain C99, integer
 * maths, no hardware access. Host-tested in ../tests/test_glow.c against a plug model.
 *
 * Every width is fixed before the pulse starts, from the current read in the last pulse scaled by
 * the supply's change since, plus a margin: nothing is cut short inside a pulse. Limits per pulse:
 * the current caps (short pulses at high current), 15 A rms, at most 99 % duty; per run: the power
 * asked, a gentle start, and a heat budget for the switching transistors. Readings need pulses of
 * at least 150 us, so a duty below that becomes 150 us pulses at a lower rate.
 *
 * Figures marked provisional in the defaults wait for measurements on the first boards. */
#ifndef FLAMEKEEPER_GLOW_H
#define FLAMEKEEPER_GLOW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint16_t period_us;        /* the PWM period (200 Hz) */
  uint16_t min_pulse_us;     /* shortest pulse with a current reading, and the probe */
  uint16_t stale_ms;         /* no pulse for longer: the next one is a probe again */
  uint16_t duty_max_permille;
  uint16_t i_rms_max_ca;     /* 0.01 A */
  uint16_t i_high_ca, w_high_us;           /* above i_high: at most w_high per pulse */
  uint16_t i_very_high_ca, w_very_high_us; /* above i_very_high: at most w_very_high */
  uint8_t margin_percent;    /* added to the predicted current */
  uint16_t p_max_w, p_low_supply_w;
  uint16_t low_supply_cv;    /* below it, p_low_supply_w */
  uint16_t start_v_cv;       /* gentle start: the plug's rms voltage at the beginning, 0.01 V */
  uint16_t ramp_ms;          /* ... ramped to the power asked over this long */
  uint32_t bucket_cap_a2s;   /* heat budget (provisional) */
  uint32_t bucket_drain_ma2s_per_s; /* drained only while the plug is off, milli-A2s per s */
  uint16_t gap_fault_ca;     /* current between pulses above this: the low side is stuck on */
  uint16_t trip_hold_ms;     /* after a high-side trip: enable low this long to clear it */
  uint8_t trips_max;         /* this many trips in a row: fault */
  uint16_t trips_forget_ms;  /* this long without a trip resets the count */
  int16_t board_max_c10;     /* board thermistor above it: no glow */
} glow_params;

void glow_params_default(glow_params *p);

/* One period's readings from the hardware layer. */
typedef struct {
  uint16_t vin_cv;      /* VIN_SENSE in the gap before this pulse, 0.01 V */
  uint16_t pulse_ca;    /* GLOW_ISENSE read inside the last pulse, 0.01 A (ignored if there was none) */
  uint16_t gap_ca;      /* GLOW_ISENSE in the gap before this pulse */
  uint8_t airflow_ok;   /* a fresh Hall speed at or above the glow's minimum (GLW-14) */
  int16_t board_c10;    /* board thermistor */
  uint8_t high_side_trip; /* PWR_FLT_N low: the high side latched an over-current */
} glow_meas;

typedef struct {
  uint8_t enable;    /* GLOW_EN */
  uint16_t width_us; /* the next pulse, 0 = none */
} glow_cmd;

enum { GLOW_FAULT_NONE = 0, GLOW_FAULT_STUCK_LOW_SIDE = 1, GLOW_FAULT_HIGH_SIDE_TRIPS = 2 };

typedef struct {
  glow_params p;
  uint32_t last_ms;
  uint8_t on;               /* power was asked in the last period */
  uint32_t on_since_ms;     /* the gentle start counts from here */
  uint16_t last_width_us;   /* the pulse planned in the last period */
  uint32_t last_pulse_ms;   /* when the last pulse with a reading went out */
  uint8_t have_reading;
  uint16_t i_ca;            /* current read in the last pulse */
  uint16_t vin_then_cv;     /* supply when that pulse went out */
  uint32_t credit_us;       /* on-time owed (low duties become fewer pulses) */
  uint64_t bucket_a2us;     /* heat budget used, A2 x us */
  uint8_t fault;
  uint8_t trips;
  uint32_t trip_ms;         /* when the last trip began its hold */
  uint8_t holding;
} glow;

/* After a reset the heat budget starts as used up (GLW-21). */
void glow_init(glow *g, const glow_params *p, uint32_t now_ms);

/* One PWM period. power_w: what the burn asks of the plug (0 = off). */
glow_cmd glow_period(glow *g, uint32_t now_ms, uint16_t power_w, const glow_meas *m);

/* Heat budget used, A2s (for logs and tests). */
uint32_t glow_bucket_a2s(const glow *g);

#ifdef __cplusplus
}
#endif

#endif /* FLAMEKEEPER_GLOW_H */
