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
void USART2_IRQHandler(void);

#endif /* FLAMEKEEPER_HAL_STM32G0_H */
