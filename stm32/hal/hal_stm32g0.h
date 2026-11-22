/* The STM32G030 driver's own start-up calls, beside hal.h. */
#ifndef FLAMEKEEPER_HAL_STM32G0_H
#define FLAMEKEEPER_HAL_STM32G0_H

#include <stdint.h>

#include "../board/hal.h"
#include "stm32g030.h"

/* The saved pages (HAL_PAGE_*): the last two of the flash, as the linker script's SAVED region.
 * Images written whole (hal_flash_write) and read back (hal_flash_read) are at most
 * HAL_FLASH_IMAGE_MAX bytes from a page's start. */
#define HAL_SAVED_PAGE_FIRST 30u
#define HAL_SAVED_PAGES 2u
#define HAL_FLASH_IMAGE_MAX 256u

void hal_iwdg_start(void);      /* first thing after reset (WDG-2, WDG-3) */
void hal_clock_init(void);      /* 64 MHz from HSI16 through the PLL; SysTick at 1 ms, polled */
void hal_init(void);            /* pins default-off, timers, ADC, USART2 */
uint8_t hal_reset_reason(void); /* LINK_RESET_* */
uint8_t hal_option_faults(void); /* LINK_CHECK_OPTION_BYTES when the option bytes are wrong */
int hal_ms_elapsed(void);       /* 1 once per millisecond (SysTick's count flag) */
void hal_ms_tick(void);         /* the driver's own millisecond count */

void TIM3_IRQHandler(void);
void ADC_IRQHandler(void);
void USART2_IRQHandler(void);
void NMI_Handler(void);

/* The NMI's decision (host-tested): for FLASH_ECCR, the saved page holding a double ECC error, 0 or
 * 1 (HAL_PAGE_*), or -1 for anything else (no ECCD, or the error is in system flash: SYSF_ECC).
 * ADDR_ECC is the failing double word's offset from the start of the flash: its 14 bits span
 * exactly the G0's largest single bank (128 KB) in double words, where a byte offset would reach
 * only 16 KB. UNVERIFIED on silicon. */
static inline int hal_ecc_saved_page(uint32_t eccr) {
  uint32_t page;
  if (!(eccr & FLASH_ECCR_ECCD) || (eccr & FLASH_ECCR_SYSF_ECC))
    return -1;
  page = (eccr & FLASH_ECCR_ADDR_ECC) * 8u / HAL_FLASH_PAGE_SIZE;
  return page - HAL_SAVED_PAGE_FIRST < HAL_SAVED_PAGES ? (int) (page - HAL_SAVED_PAGE_FIRST) : -1;
}

/* WDG-1, WDG-4: the option bytes as the ESP32's updater writes them (its optr_required): the IWDG
 * started by hardware (IWDG_SW 0) and running in stop and standby (IWDG_STOP, IWDG_STDBY 1); boot
 * from the BOOT0 pin (nBOOT_SEL 0), main flash while the pin is low and the ROM bootloader while
 * the ESP32 holds it high (nBOOT1 1), nBOOT0 1. Every other bit is free. They are wrong too if they
 * failed to load (FLASH_SR's OPTVERR). Host-tested against the updater's own rule. */
#define HAL_OPTR_CHECKED                                                                     \
  (FLASH_OPTR_IWDG_SW | FLASH_OPTR_IWDG_STOP | FLASH_OPTR_IWDG_STDBY | FLASH_OPTR_NBOOT_SEL | \
   FLASH_OPTR_NBOOT1 | FLASH_OPTR_NBOOT0)
#define HAL_OPTR_REQUIRED (FLASH_OPTR_IWDG_STOP | FLASH_OPTR_IWDG_STDBY | FLASH_OPTR_NBOOT1 | FLASH_OPTR_NBOOT0)
static inline int hal_optr_wdg1_ok(uint32_t optr, uint32_t flash_sr) {
  return (optr & HAL_OPTR_CHECKED) == HAL_OPTR_REQUIRED && !(flash_sr & FLASH_SR_OPTVERR);
}

#endif /* FLAMEKEEPER_HAL_STM32G0_H */
