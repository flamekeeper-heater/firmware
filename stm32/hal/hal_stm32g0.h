/* The STM32G030 driver's own start-up calls, beside hal.h. */
#ifndef FLAMEKEEPER_HAL_STM32G0_H
#define FLAMEKEEPER_HAL_STM32G0_H

#include <stdint.h>

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

/* The NMI's decision (host-tested): for FLASH_ECCR (ECCD bit 31, SYSF_ECC bit 20, ADDR_ECC [13:0]
 * the failing double word's index in the main flash), the saved page holding a double ECC error,
 * 0 or 1 (HAL_PAGE_*), or -1 for anything else. UNVERIFIED: ADDR_ECC read as a double-word index. */
static inline int hal_ecc_saved_page(uint32_t eccr) {
  uint32_t page;
  if (!(eccr & (1u << 31)) || (eccr & (1u << 20)))
    return -1;
  page = (eccr & 0x3FFFu) * 8u / 2048u;
  return page == 30u || page == 31u ? (int) (page - 30u) : -1;
}

#endif /* FLAMEKEEPER_HAL_STM32G0_H */
