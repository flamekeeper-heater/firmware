/* Small helpers the safety MCU's modules share. Plain C99, header only. */
#ifndef FLAMEKEEPER_UTIL_H
#define FLAMEKEEPER_UTIL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A temperature not known (a sensor open or out of range, or not read yet), 0.1 C. */
#define UNKNOWN_TEMP ((int16_t) 0x8000)
static inline int temp_known(int16_t c10) { return c10 != UNKNOWN_TEMP; }

/* Time is a wrapping uint32_t millisecond count. */
static inline uint32_t since(uint32_t now, uint32_t then) { return (uint32_t) (now - then); }
/* now is at or past when (within half the wrap). */
static inline int reached(uint32_t now, uint32_t when) { return (int32_t) (now - when) >= 0; }
#define MS(seconds) ((uint32_t) (seconds) * 1000u)

/* A condition that must hold a while without a break. */
typedef struct {
  uint8_t on;        /* it held at the last call */
  uint32_t since_ms; /* ... from then */
} held;

/* 1 once cond has held for ms, counted from the first call that saw it (that call itself only starts
 * the count, so it never answers 1). */
static inline int held_for(held *h, int cond, uint32_t now, uint32_t ms) {
  if (!cond) {
    h->on = 0;
    return 0;
  }
  if (!h->on) {
    h->on = 1;
    h->since_ms = now;
    return 0;
  }
  return since(now, h->since_ms) >= ms;
}

/* The PWM duty that gives an average of v_cv from a supply of vin_cv, permille: at most 1000, and 0
 * with no supply reading. */
static inline uint16_t duty_permille_for(uint16_t v_cv, uint16_t vin_cv) {
  if (vin_cv == 0)
    return 0;
  uint32_t d = (uint32_t) v_cv * 1000u / vin_cv;
  return (uint16_t) (d > 1000u ? 1000u : d);
}

#ifdef __cplusplus
}
#endif

#endif /* FLAMEKEEPER_UTIL_H */
