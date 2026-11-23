/* The safety MCU's checks of itself (DESIGN.md): its own supply and its own clock. Plain C99, no
 * hardware access. Host-tested in ../tests/test_monitor.c.
 *
 * - VDDA (PWR-5): every millisecond's reading (from one VREFINT conversion) inside 3.0-3.6 V. Out
 *   of the window on every reading for vdda_bad_ms is a fault: one stray conversion is not, and
 *   the fault still acts within a pass of the safety loop. After a reset the rail may still be
 *   rising (the G030 has no brown-out reset, only its power-on reset well below 3.0 V): until a
 *   reading reaches the window with some margin, a low reading holds every output off instead of
 *   latching; a rail still low vdda_rise_ms after the reset is a fault. A high reading is never a
 *   power-up effect: it counts at once.
 * - The tick (GLW-1): once a second, the safety loop's millisecond count against a second clock
 *   that does not share its oscillator (the LSI, which also clocks the watchdog). Apart by more than
 *   tick_tolerance_percent over the second is a fault: every timing, the glow caps among them, is
 *   counted in that tick.
 *
 * A fault latches until monitor_clear finds its cause gone (the user's fault reset). While a VDDA
 * fault is latched, or the rail is still rising, monitor_holding says every output stays off. */
#ifndef FLAMEKEEPER_MONITOR_H
#define FLAMEKEEPER_MONITOR_H

#include <stdint.h>

#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint16_t vdda_min_mv, vdda_max_mv; /* PWR-5: the window, 3.0-3.6 V */
  uint16_t vdda_up_margin_mv;        /* after a reset: in the window by this much counts as up */
  uint16_t vdda_bad_ms;              /* out of the window this long, every reading: a fault */
  uint16_t vdda_rise_ms;             /* after a reset the rail gets this long to come up */
  uint16_t tick_check_ms;            /* GLW-1: the check's interval, by the tick */
  uint8_t tick_tolerance_percent;    /* GLW-1: 20 % */
  uint16_t clock_hz;                 /* the second clock's nominal rate */
  uint32_t clock_wrap;               /* its count wraps here */
} monitor_params;

/* The second clock's rate and wrap come from the hardware layer (hal.h). */
void monitor_params_default(monitor_params *p, uint16_t clock_hz, uint32_t clock_wrap);

enum {
  MONITOR_OK = 0,
  MONITOR_VDDA_HIGH = 1, /* above 3.6 V: V3V3 over-voltage */
  MONITOR_VDDA_LOW = 2,  /* below 3.0 V */
  MONITOR_TICK = 3,      /* the tick and the second clock disagree */
};

typedef struct {
  monitor_params p;
  uint32_t reset_ms;
  uint8_t vdda_seen;       /* a reading has come since the reset */
  uint8_t vdda_up;         /* ... and one reached the window */
  uint16_t vdda_mv;        /* the last reading */
  held vdda_bad;           /* out of the window without a break */
  uint8_t fault;           /* latched: MONITOR_* */
  uint8_t clock_seen;
  uint32_t clock_ms;       /* the tick at the last check */
  uint32_t clock_then;     /* the second clock then */
  uint8_t tick_bad;        /* the last check failed */
  uint16_t tick_permille;  /* the last check: the second clock's count per the tick's, 1000 = agree */
  uint32_t tick_checks;    /* checks done since the reset */
} monitor;

/* After a reset. */
void monitor_init(monitor *m, const monitor_params *p, uint32_t now);

/* Every millisecond: VDDA, mV, from that millisecond's VREFINT conversion (not clamped). */
void monitor_vdda(monitor *m, uint32_t now, uint16_t vdda_mv);

/* Every pass of the safety loop: the second clock's count now. */
void monitor_clock(monitor *m, uint32_t now, uint32_t count);

/* 1 while every output must stay off: a VDDA fault, or the rail not up yet after the reset. */
static inline int monitor_holding(const monitor *m) {
  return m->fault == MONITOR_VDDA_HIGH || m->fault == MONITOR_VDDA_LOW || (m->vdda_seen && !m->vdda_up);
}

/* A fault reset: the fault goes if its cause has (VDDA back in the window, the last tick check
 * good). Returns 1 when no fault is left. */
int monitor_clear(monitor *m);

#ifdef __cplusplus
}
#endif

#endif /* FLAMEKEEPER_MONITOR_H */
