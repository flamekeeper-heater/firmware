/* The pump's overcurrent cut (PMP-7) as the ADC watchdog's interrupt runs it. The ADC converts
 * PUMP_ISENSE once per scan; one watchdog flags a conversion above the threshold, a second one, only
 * listened to while a run of those is counting, a conversion at or below it. PUMP_ISENSE above the
 * threshold on `need` conversions in a row is the cut. The threshold itself is analog_pump_oc_raw().
 * Plain C99, no hardware access: the interrupt in hal_stm32g0.c only reads the flags and acts on the
 * answer. Host-tested in ../tests/test_board.c. */
#ifndef FLAMEKEEPER_PUMP_OC_H
#define FLAMEKEEPER_PUMP_OC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint8_t need;  /* conversions above the threshold in a row that cut the stroke */
  uint8_t above; /* in a row so far */
  uint8_t cut;   /* latched until the next arm */
} pump_oc;

/* Conversions in a row for a hold time, one conversion of the channel every scan_us: an excess
 * that lasts longer than hold_us always gives this many in a row, so it is always cut, within
 * need x scan_us of its start; one shorter than scan_us gives at most one. At least 1. */
uint8_t pump_oc_need(uint16_t hold_us, uint16_t scan_us);

/* A new stroke: nothing counted, no cut. */
void pump_oc_arm(pump_oc *f, uint8_t need);

/* One watchdog interrupt. high: a conversion above the threshold since the last call; low: one at
 * or below it (pass it only while pump_oc_listening). Both at once (a late interrupt, order
 * unknown): the high one counts and the run goes on, the safe reading. Returns 1 once, when the
 * cut is due. */
int pump_oc_event(pump_oc *f, int high, int low);

/* A run is counting: the interrupt listens for the next conversion at or below the threshold. */
static inline int pump_oc_listening(const pump_oc *f) { return f->above > 0 && !f->cut; }

#ifdef __cplusplus
}
#endif

#endif /* FLAMEKEEPER_PUMP_OC_H */
