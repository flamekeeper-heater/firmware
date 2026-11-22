/* The safety MCU's firmware: the watchdog first, then the clocks and peripherals, then the board
 * layer once a millisecond, for ever. */
#include "../board/board.h"
#include "hal_stm32g0.h"

static board b;

int main(void) {
  hal_iwdg_start(); /* WDG-2, WDG-3: before anything that could hang */
  hal_clock_init();
  hal_init();
  uint8_t reason = hal_reset_reason();
  uint8_t faults = hal_option_faults();
  uint32_t now = 0;
  board_init(&b, now, reason, faults);
  __asm__ volatile("cpsie i");
  for (;;) {
    if (!hal_ms_elapsed())
      continue;
    now++;
    hal_ms_tick();
    board_ms(&b, now);
  }
}
