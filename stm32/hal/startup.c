/* Cortex-M0+ start-up for the STM32G030: the vector table, then the C runtime (.data copied,
 * .bss zeroed) and main. Every unused interrupt goes to a handler that waits for the watchdog. */
#include <stdint.h>

#include "hal_stm32g0.h"

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
int main(void);

void Reset_Handler(void);
void Default_Handler(void);

void Default_Handler(void) {
  /* Something unexpected: stop here and let the watchdog reset the chip (every output then falls
   * back to its default-off pull). */
  for (;;) {
  }
}

void Reset_Handler(void) {
  uint32_t *src = &_sidata, *dst = &_sdata;
  while (dst < &_edata)
    *dst++ = *src++;
  for (dst = &_sbss; dst < &_ebss;)
    *dst++ = 0;
  main();
  for (;;) {
  }
}

#define V(f) ((uint32_t) (uintptr_t) (f))

__attribute__((section(".isr_vector"), used)) const uint32_t vectors[16 + 32] = {
    [0] = (uint32_t) (uintptr_t) &_estack,
    [1] = V(Reset_Handler),
    [2] = V(Default_Handler),  /* NMI */
    [3] = V(Default_Handler),  /* HardFault */
    [11] = V(Default_Handler), /* SVCall */
    [14] = V(Default_Handler), /* PendSV */
    [15] = V(Default_Handler), /* SysTick (polled, never enabled) */
    [16 + 12] = V(ADC_IRQHandler),
    [16 + 16] = V(TIM3_IRQHandler),
    [16 + 28] = V(USART2_IRQHandler),
};
