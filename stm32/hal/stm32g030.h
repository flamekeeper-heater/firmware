/* The STM32G030's registers this firmware uses, from the reference manual (RM0444). Written by
 * hand to avoid a vendor HAL: only what hal_stm32g0.c touches. UNVERIFIED on silicon. */
#ifndef FLAMEKEEPER_STM32G030_H
#define FLAMEKEEPER_STM32G030_H

#include <stdint.h>

#define REG(addr) (*(volatile uint32_t *) (addr))

/* RCC */
#define RCC 0x40021000u
#define RCC_CR REG(RCC + 0x00)
#define RCC_CFGR REG(RCC + 0x08)
#define RCC_PLLCFGR REG(RCC + 0x0C)
#define RCC_IOPENR REG(RCC + 0x34)
#define RCC_AHBENR REG(RCC + 0x38)
#define RCC_APBENR1 REG(RCC + 0x3C)
#define RCC_APBENR2 REG(RCC + 0x40)
#define RCC_CSR REG(RCC + 0x94)

/* FLASH */
#define FLASH 0x40022000u
#define FLASH_ACR REG(FLASH + 0x00)
#define FLASH_KEYR REG(FLASH + 0x08)
#define FLASH_SR REG(FLASH + 0x10)
#define FLASH_CR REG(FLASH + 0x14)
#define FLASH_ECCR REG(FLASH + 0x18)
#define FLASH_OPTR REG(FLASH + 0x20)

/* GPIO */
#define GPIOA 0x50000000u
#define GPIOB 0x50000400u
#define GPIOC 0x50000800u
#define GPIO_MODER(p) REG((p) + 0x00)
#define GPIO_OTYPER(p) REG((p) + 0x04)
#define GPIO_OSPEEDR(p) REG((p) + 0x08)
#define GPIO_PUPDR(p) REG((p) + 0x0C)
#define GPIO_IDR(p) REG((p) + 0x10)
#define GPIO_BSRR(p) REG((p) + 0x18)
#define GPIO_LCKR(p) REG((p) + 0x1C)
#define GPIO_AFRL(p) REG((p) + 0x20)
#define GPIO_AFRH(p) REG((p) + 0x24)

/* Timers (TIM1, TIM3, TIM16, TIM17 share these offsets) */
#define TIM1 0x40012C00u
#define TIM3 0x40000400u
#define TIM16 0x40014400u
#define TIM17 0x40014800u
#define TIM_CR1(t) REG((t) + 0x00)
#define TIM_DIER(t) REG((t) + 0x0C)
#define TIM_SR(t) REG((t) + 0x10)
#define TIM_EGR(t) REG((t) + 0x14)
#define TIM_CCMR1(t) REG((t) + 0x18)
#define TIM_CCER(t) REG((t) + 0x20)
#define TIM_CNT(t) REG((t) + 0x24)
#define TIM_PSC(t) REG((t) + 0x28)
#define TIM_ARR(t) REG((t) + 0x2C)
#define TIM_RCR(t) REG((t) + 0x30)
#define TIM_CCR1(t) REG((t) + 0x34)
#define TIM_CCR2(t) REG((t) + 0x38)
#define TIM_BDTR(t) REG((t) + 0x44)

/* USART2 */
#define USART2 0x40004400u
#define USART_CR1 REG(USART2 + 0x00)
#define USART_BRR REG(USART2 + 0x0C)
#define USART_ISR REG(USART2 + 0x1C)
#define USART_ICR REG(USART2 + 0x20)
#define USART_RDR REG(USART2 + 0x24)
#define USART_TDR REG(USART2 + 0x28)

/* ADC */
#define ADC 0x40012400u
#define ADC_ISR REG(ADC + 0x00)
#define ADC_CR REG(ADC + 0x08)
#define ADC_CFGR1 REG(ADC + 0x0C)
#define ADC_CFGR2 REG(ADC + 0x10)
#define ADC_SMPR REG(ADC + 0x14)
#define ADC_CHSELR REG(ADC + 0x28)
#define ADC_DR REG(ADC + 0x40)
#define ADC_CCR REG(ADC + 0x308)
#define VREFINT_CAL (*(const volatile uint16_t *) 0x1FFF75AAu)

/* DMA1 channel 1 and its DMAMUX channel 0 */
#define DMA1 0x40020000u
#define DMA_CCR1 REG(DMA1 + 0x08)
#define DMA_CNDTR1 REG(DMA1 + 0x0C)
#define DMA_CPAR1 REG(DMA1 + 0x10)
#define DMA_CMAR1 REG(DMA1 + 0x14)
#define DMAMUX_C0CR REG(0x40020800u)
#define DMAMUX_REQ_TIM1_UP 25u

/* IWDG */
#define IWDG 0x40003000u
#define IWDG_KR REG(IWDG + 0x00)
#define IWDG_PR REG(IWDG + 0x04)
#define IWDG_RLR REG(IWDG + 0x08)
#define IWDG_SR REG(IWDG + 0x0C)

/* Cortex-M0+ */
#define SYST_CSR REG(0xE000E010u)
#define SYST_RVR REG(0xE000E014u)
#define SYST_CVR REG(0xE000E018u)
#define NVIC_ISER REG(0xE000E100u)
#define IRQ_TIM3 16u
#define IRQ_USART2 28u

#endif /* FLAMEKEEPER_STM32G030_H */
