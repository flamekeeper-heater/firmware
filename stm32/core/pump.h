/* The fuel pump's stroke driver (DESIGN.md): the dosing pump takes one stroke per pulse, so the
 * fuel rate is the stroke rate. Stepped once a millisecond, it turns the rate the sequencer asks for
 * into strokes on PUMP_CMD (a ~20 kHz PWM at a 12 V equivalent duty) and watches the coil current
 * between and during them. Plain C99, no hardware access. Host-tested in ../tests/test_pump.c
 * against a pump coil model.
 *
 * - Strokes of stroke_ms, at most max_cHz (priming: prime_max_cHz).
 * - The first stroke only once the heartbeat has run hs_arm_ms and the high side reports no fault.
 * - The overcurrent watchdog (PMP-7) is armed from a stroke's first pulse until decay_ms after its
 *   last; the board's ADC watchdog does the cut itself and reports it here.
 * - Faults: the hardware overcurrent cut during a stroke; current not gone decay_ms after a stroke,
 *   or back between strokes; no current rise in a stroke (open coil). A high-side fault gets one
 *   heartbeat restart per run, then locks out.
 *
 * Figures are PROVISIONAL until measured on the first boards. */
#ifndef FLAMEKEEPER_PUMP_H
#define FLAMEKEEPER_PUMP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint16_t stroke_ms;      /* PMP-3: about 38 ms */
  uint16_t stroke_v_cv;    /* PMP-3: the coil's average voltage in a stroke, 0.01 V (12 V) */
  uint16_t max_cHz;        /* PMP-4: 6.0 Hz */
  uint16_t prime_max_cHz;  /* PMP-4: 10 Hz when priming */
  uint16_t hs_arm_ms;      /* PMP-2: heartbeat running this long before the first stroke */
  uint16_t decay_ms;       /* PMP-8: the coil current must be gone this long after a stroke */
  uint16_t gap_ma;         /* PMP-8: above this between strokes: the low side conducts */
  uint16_t rise_ma;        /* PMP-15: a stroke's current must reach this, else the coil is open */
  uint16_t rearm_ms;       /* PMP-9: heartbeat stopped this long to clear the high side's latch */
  uint16_t oc_ma;          /* PMP-7: the overcurrent cut's threshold (about 3.3 A) */
  uint16_t oc_hold_us;     /* PMP-7: ... held this long */
} pump_params;

void pump_params_default(pump_params *p);

typedef struct {
  uint16_t vin_cv;
  uint16_t i_ma;       /* PUMP_ISENSE this millisecond */
  uint8_t overcurrent; /* the ADC watchdog cut a stroke (PMP-7: above about 3.3 A) */
  uint8_t hs_ok;       /* PUMP_HS_FLT_N high */
  uint16_t hb_running_ms; /* how long the heartbeat has run without a break (0: not running) */
} pump_meas;

enum {
  PUMP_FAULT_NONE = 0,
  PUMP_FAULT_OVERCURRENT = 1, /* PMP-7 */
  PUMP_FAULT_LOW_SIDE = 2,    /* PMP-8: current between strokes */
  PUMP_FAULT_OPEN_COIL = 3,   /* PMP-15 */
  PUMP_FAULT_HIGH_SIDE = 4,   /* PMP-9: a second high-side fault in one run */
};

typedef struct {
  uint16_t duty;         /* PUMP_CMD duty, permille (0 = low) */
  uint8_t hb_hold_off;   /* 1: the heartbeat must stop (a fault, or a high-side re-arm) */
  uint8_t oc_armed;      /* 1: the overcurrent watchdog guards this millisecond (PMP-7) */
} pump_out;

typedef struct {
  pump_params p;
  uint8_t stroking;
  uint32_t stroke_ms;     /* when the current stroke began */
  uint32_t next_ms;       /* when the next stroke is due */
  uint8_t scheduled;
  uint32_t ended_ms;      /* when the last stroke ended */
  uint8_t after_stroke;   /* the decay window after a stroke is open */
  uint16_t stroke_peak_ma;
  uint8_t gap_high;       /* consecutive samples above gap_ma between strokes */
  uint8_t rearms;         /* high-side re-arms used in this run */
  uint8_t rearming;
  uint32_t rearm_ms;
  uint8_t fault;
  uint32_t strokes;       /* since init (fuel used = strokes x stroke volume) */
} pump;

void pump_init(pump *pu, const pump_params *p);
/* A new run (a start): the high side gets its one re-arm again. */
void pump_new_run(pump *pu);
/* One millisecond. rate_cHz: strokes per 100 s the sequencer asks for (0 = none). */
pump_out pump_ms(pump *pu, uint32_t now_ms, uint16_t rate_cHz, int priming, const pump_meas *m);

#ifdef __cplusplus
}
#endif

#endif /* FLAMEKEEPER_PUMP_H */
