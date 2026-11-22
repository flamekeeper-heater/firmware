/* The board layer (DESIGN.md): it runs the safety core on the chip, through hal.h. Called once a
 * millisecond from the main loop, it schedules the core's passes:
 *
 *   every 1 ms   fresh ADC scan, the link's received bytes, the self-tests and the pump strokes
 *   every 5 ms   the glow planner's next pulse
 *   every 10 ms  the safety loop pass: the sequencer, the fan, the slow readings, the flash; then,
 *                only if that pass completed, the heartbeat's next burst and the watchdog (PMP-1,
 *                WDG-16)
 *   every 1 s    (in a loop pass) the CO alarm's reading; CO_HEAT_EN follows it every pass
 *
 * The CO sensor's clean-air R0 (CO-7) is a log of 8-byte slots in the plug page past the plug
 * record, so a new R0 costs one double-word write and the page is erased only when the log is full
 * or the plug record changes (the STM32G0's flash is rated for 1000 erases).
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
  uint32_t last_glow_ms, last_loop_ms, last_co_ms;
  uint16_t co_log_next;      /* the R0 log's next free slot */
  uint32_t loops, bursts, refreshes; /* for the tests and the logs */
} board;

/* After the start-up code (option bytes, clocks, peripherals): reads the analog inputs once, starts
 * the core, loads the saved parameters and plug record. option_faults: LINK_CHECK_* bits. */
void board_init(board *b, uint32_t now_ms, uint8_t reset_reason, uint8_t option_faults);
/* Once a millisecond. */
void board_ms(board *b, uint32_t now_ms);

/* The R0 log (exposed for the tests). */
#define BOARD_CO_LOG_START 256u
#define BOARD_CO_LOG_SLOT 8u
#define BOARD_CO_LOG_SLOTS ((HAL_FLASH_PAGE_SIZE - BOARD_CO_LOG_START) / BOARD_CO_LOG_SLOT)

#ifdef __cplusplus
}
#endif

#endif /* FLAMEKEEPER_BOARD_H */
