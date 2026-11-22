/* The bring-up image's main: the production start-up (the watchdog first, then the clocks and
 * peripherals), the production board layer once a millisecond, and the console after it. Built by
 * `build.sh bringup`, which compiles the STM32 driver with hal_uart_getc and hal_iwdg_refresh
 * renamed to bringup_chip_*, so the console's versions sit between the board layer and the chip. */
#include <string.h>

#include "../board/board.h"
#include "../hal/hal_stm32g0.h"
#include "../hal/stm32g030.h"
#include "console.h"

static board b;
static console con;

void bringup_sys_reset(void) {
  SCB_AIRCR = SCB_AIRCR_VECTKEY_KEY | SCB_AIRCR_SYSRESETREQ;
  for (;;) {
  }
}

void bringup_sys_read(uint32_t addr, uint8_t *out, size_t n) {
  memcpy(out, (const void *) (uintptr_t) addr, n);
}

int main(void) {
  hal_iwdg_start(); /* WDG-2, WDG-3: before anything that could hang */
  hal_clock_init();
  hal_init();
  console_boot boot;
  boot.csr = RCC_CSR; /* before hal_reset_reason clears the flags */
  boot.optr = FLASH_OPTR;
  boot.flash_sr = FLASH_SR;
  uint8_t reason = hal_reset_reason();
  uint8_t faults = hal_option_faults();
  uint32_t now = 0;
  board_init(&b, now, reason, faults);
  console_init(&con, &b, &boot, now);
  __asm__ volatile("cpsie i");
  for (;;) {
    if (!hal_ms_elapsed())
      continue;
    now++;
    hal_ms_tick();
    board_ms(&b, now);
    console_ms(&con, now);
    if (con.hang) {
      /* WDG-14: a hung program. Every timer keeps running as last set (the fan's PWM, a glow
       * pulse width, the pump's duty); the heartbeat burst runs out by itself within 25 ms, and
       * the watchdog resets the chip. */
      for (;;) {
      }
    }
  }
}
