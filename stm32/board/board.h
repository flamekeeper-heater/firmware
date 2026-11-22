/* The board layer (DESIGN.md): it runs the safety core on the chip, through hal.h. Called once a
 * millisecond from the main loop, it schedules the core's passes:
 *
 *   every 1 ms   fresh ADC scan, the link's received bytes, the self-tests and the pump strokes
 *                (and the pump's overcurrent watchdog, armed for each stroke)
 *   every 5 ms   the glow planner's next pulse, once per glow period on that period's readings
 *   every 10 ms  the safety loop pass: the sequencer, the fan, the slow readings, the flash; then,
 *                only if that pass completed, the heartbeat's next burst and the watchdog (PMP-1,
 *                WDG-16)
 *
 * Plain C99 over hal.h. Host-tested in ../tests/test_board.c with a fake chip. */
#ifndef FLAMEKEEPER_BOARD_H
#define FLAMEKEEPER_BOARD_H

#include <stdint.h>

#include "../core/safety.h"
#include "hal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  safety s;
  uint32_t now;
  uint16_t raw[HAL_ADC_COUNT];
  uint16_t vdda_mv, vrefint_cal;
  uint8_t latch_was_armed;   /* the overheat latch has been armed since the reset (OVH-1) */
  uint8_t want_burst;        /* the core wants the heartbeat running */
  uint8_t glow_en_test, glow_en_planner;
  uint8_t pump_cut;          /* the overcurrent watchdog cut a stroke; kept until the core latched it */
  uint32_t last_glow_ms, last_loop_ms;
  uint32_t loops, bursts, refreshes; /* for the tests and the logs */
} board;

/* After the start-up code (option bytes, clocks, peripherals): reads the analog inputs once, starts
 * the core, loads the saved parameters and plug record. option_faults: LINK_CHECK_* bits. */
void board_init(board *b, uint32_t now_ms, uint8_t reset_reason, uint8_t option_faults);
/* Once a millisecond. */
void board_ms(board *b, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* FLAMEKEEPER_BOARD_H */
