/* Bench actions of the bring-up build: single, explicitly asked for and time-limited uses of one
 * power stage, for measurements on a new board. They act only through the safety core's bench
 * requests (safety.h, SAFETY_BENCH), which the core honours only while the burn is off and no
 * self-test runs, through the same stage drivers as a run:
 *
 *   fan    a fixed duty (fan_step_open) or a speed (the speed loop): the 10 V cap, the ramp, the
 *          start current limit, the stall and stuck-on checks
 *   hb     the heartbeat alone (re-armed by completed loop passes only, as in a run)
 *   pump   n strokes through the pump driver: the pre-start self-test first, then the heartbeat,
 *          the driver's arming wait, stroke length, rate cap and current checks, the overcurrent
 *          cut (PMP-7); PUMP_CMD low, the decay wait, then the heartbeat stops (PMP-10)
 *   pump_oc  one such stroke with the overcurrent cut's threshold lowered for it (never raised),
 *          to see the cut act on a dummy load; the threshold goes back when the action ends
 *   glow   one probe pulse through the glow planner: needs a fan action turning at the glow's
 *          minimum speed; the pre-start self-test, the heartbeat for 50 ms, then the core lets the
 *          planner issue exactly one probe
 *   precheck  the pre-start self-test alone
 *
 * Two slots: the fan, and one of hb / pump / glow / precheck. Plain C99 over the safety core.
 * Host-tested in ../tests/test_bringup.c. */
#ifndef FLAMEKEEPER_BENCH_H
#define FLAMEKEEPER_BENCH_H

#include <stdint.h>

#include "../core/safety.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SAFETY_BENCH
#error "the bring-up build needs SAFETY_BENCH"
#endif

/* Limits. */
#define BENCH_FAN_MAX_S 600u
#define BENCH_FAN_RPM_MIN 200u
#define BENCH_FAN_RPM_MAX 6000u
#define BENCH_HB_MAX_S 60u
#define BENCH_PUMP_MAX_STROKES 20u
#define BENCH_PUMP_MIN_CHZ 10u
#define BENCH_PUMP_DEFAULT_CHZ 100u
#define BENCH_PUMP_OC_MIN_MA 100u
#define BENCH_PRECHECK_MAX_MS 3000u /* the pre-start self-test (its own timeout is 2 s) */
#define BENCH_GLOW_ARM_MS 50u       /* heartbeat before the probe (GLW-7: at least 20 ms) */
#define BENCH_GLOW_PROBE_MAX_MS 300u /* no probe within this: given up */
#define BENCH_GLOW_READ_MS 20u      /* after the probe: the planner has its reading */

enum {
  BENCH_OK = 0,
  BENCH_E_BUSY = 1,       /* that slot runs an action already, or a self-test runs */
  BENCH_E_NOT_OFF = 2,    /* the burn is not off */
  BENCH_E_RANGE = 3,
  BENCH_E_INTERLOCKS = 4, /* power-up self-test not passed, option bytes wrong, or no starts */
  BENCH_E_FAULT = 5,      /* a fault needs a fault reset first */
  BENCH_E_NO_AIR = 6,     /* glow: no fan action, or the fan below the glow's minimum speed */
};

enum {
  BENCH_ACT_NONE = 0,
  BENCH_ACT_FAN_DUTY = 1,
  BENCH_ACT_FAN_RPM = 2,
  BENCH_ACT_HB = 3,
  BENCH_ACT_PUMP = 4,
  BENCH_ACT_GLOW = 5,
  BENCH_ACT_PRECHECK = 6,
  BENCH_ACT_PUMP_OC = 7,
};

/* How an action ended. */
enum {
  BENCH_END_DONE = 1,
  BENCH_END_STOPPED = 2,       /* bench_stop */
  BENCH_END_TIMEOUT = 3,
  BENCH_END_PRECHECK_FAILED = 4, /* fail: CHECK_FAIL_* */
  BENCH_END_FAULT = 5,         /* fault: the stage's FAN_FAULT_* / PUMP_FAULT_* / GLOW_FAULT_* */
  BENCH_END_BURN = 6,          /* the burn left off (a start over the link): bench actions end */
  BENCH_END_NO_PROBE = 7,      /* glow: the planner gave no probe (airflow, board hot, budget) */
  BENCH_END_NO_AIR = 8,        /* glow: the fan action ended under it */
};

typedef struct {
  uint8_t action; /* BENCH_ACT_* */
  uint8_t end;    /* BENCH_END_* */
  uint8_t fault;
  uint16_t fail;
  /* Results. pump: strokes, lowest and highest stroke peak (mA). glow: probe current (0.01 A),
   * supply at the probe (0.01 V), resistance (mOhm, 0xFFFF none), cold-range type (12, 24, 0). */
  uint16_t a, b, c, d;
} bench_event;

#define BENCH_EVENTS 4

typedef struct {
  uint8_t fan;          /* BENCH_ACT_FAN_*, or 0 */
  uint16_t fan_value;   /* duty permille or rpm */
  uint32_t fan_end_ms;
  uint8_t out;          /* BENCH_ACT_HB / PUMP / GLOW / PRECHECK, or 0 */
  uint8_t phase;
  uint32_t phase_ms, out_end_ms;
  uint16_t pump_n, pump_cHz, pump_done, peak_min, peak_max;
  uint32_t strokes0;
  uint8_t was_stroking;
  uint8_t stopping;     /* the pump's tail after a stop */
  uint16_t oc_saved;    /* pump_oc: the cut's own threshold, back at the end (0: not lowered) */
  bench_event ev[BENCH_EVENTS];
  uint8_t ev_n;
} bench;

void bench_init(bench *bn);

/* Requests: BENCH_OK or BENCH_E_*. */
int bench_fan(bench *bn, safety *s, uint32_t now, uint8_t action, uint16_t value, uint16_t seconds);
int bench_hb(bench *bn, safety *s, uint32_t now, uint16_t seconds);
int bench_pump(bench *bn, safety *s, uint32_t now, uint16_t strokes, uint16_t cHz);
/* One stroke with the cut at ma (BENCH_PUMP_OC_MIN_MA up to below the setting). */
int bench_pump_oc(bench *bn, safety *s, uint32_t now, uint16_t ma);
int bench_glow(bench *bn, safety *s, uint32_t now);
int bench_precheck(bench *bn, safety *s, uint32_t now);
/* Every action ends now (the pump with its decay wait, PMP-10). */
void bench_stop(bench *bn, safety *s, uint32_t now);
static inline int bench_busy(const bench *bn) { return bn->fan || bn->out; }

/* Once a millisecond, after board_ms: ends actions on time, on a fault or when the burn starts,
 * and sets the core's bench requests for the next millisecond. */
void bench_ms(bench *bn, safety *s, uint32_t now);

/* The oldest unread event: 1 and *ev, or 0. */
int bench_next_event(bench *bn, bench_event *ev);

/* The interlocks a pump stroke or glow probe needs (as for a start): the power-up self-test
 * passed, the option bytes right, no update agreed. */
int bench_interlocks_ok(const safety *s);

#ifdef __cplusplus
}
#endif

#endif /* FLAMEKEEPER_BENCH_H */
