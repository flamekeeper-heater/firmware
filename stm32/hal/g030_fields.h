/* The STM32G030's registers come from ST's device header (st/stm32g030xx.h, with ARM's Cortex-M0+
 * core header): its register structs (RCC->CR, TIM1->CCR1) and its bit names. This header adds
 * only what that one leaves out, each from the reference manual (RM0454) or the datasheet, and
 * built from ST's own names where it can be: the keys a few registers take, the DMAMUX request
 * numbers, the factory VREFINT calibration, and a field per pin or interrupt.
 *
 * Every register write names its bits: an ST name or one of these, never a bare number, except a
 * whole number in a one-number register (ARR, PSC, RLR, BRR, CNDTR, CCRx). tools/check_named_bits.sh
 * checks it. UNVERIFIED on silicon. */
#ifndef FLAMEKEEPER_G030_FIELDS_H
#define FLAMEKEEPER_G030_FIELDS_H

#include <stdint.h>

#include "st/stm32g030xx.h"

/* Keys (RM0454: 27.4.4 IWDG_KR, 25.6.10 RTC_WPR, 3.3.6 the flash unlock sequence; ARMv6-M AIRCR) */
#define IWDG_KR_KEY_START 0xCCCCu
#define IWDG_KR_KEY_UNLOCK 0x5555u /* PR and RLR writable */
#define IWDG_KR_KEY_RELOAD 0xAAAAu
#define RTC_WPR_KEY_UNLOCK1 0xCAu /* the write protection's two keys, in this order */
#define RTC_WPR_KEY_UNLOCK2 0x53u
#define RTC_WPR_KEY_LOCK 0xFFu /* any other value locks it again */
#define FLASH_KEY1 0x45670123u
#define FLASH_KEY2 0xCDEF89ABu
#define SCB_AIRCR_VECTKEY_KEY (0x05FAu << SCB_AIRCR_VECTKEY_Pos) /* ARMv6-M: AIRCR ignores a write without it */

/* DMAMUX request inputs (RM0454 table 43) and the factory VREFINT reading (datasheet 3.13.2) */
#define DMAMUX_REQ_ADC 5u
#define DMAMUX_REQ_TIM1_UP 25u
#define VREFINT_CAL (*(const volatile uint16_t *) 0x1FFF75AAu)

/* TIM_CCMR1's OC1M without its fourth bit (OC1M_3, bit 16), which this firmware never sets */
#define TIM_CCMR1_OC1M_LOW (TIM_CCMR1_OC1M_0 | TIM_CCMR1_OC1M_1 | TIM_CCMR1_OC1M_2)

/* Every flash error flag (all rc_w1), and those with EOP: the clear. */
#define FLASH_SR_ERRORS (FLASH_SR_OPERR | FLASH_SR_PROGERR | FLASH_SR_WRPERR | FLASH_SR_PGAERR | \
                         FLASH_SR_SIZERR | FLASH_SR_PGSERR | FLASH_SR_MISERR | FLASH_SR_FASTERR | \
                         FLASH_SR_OPTVERR)
#define FLASH_SR_CLEAR (FLASH_SR_EOP | FLASH_SR_ERRORS)

/* A field per GPIO pin n, from ST's pin 0 field: its mask, its bit, or v placed in it. */
#define GPIO_MODER_MODE_Msk(n) (GPIO_MODER_MODE0_Msk << (2u * (n)))
#define GPIO_MODER_MODE(n, v) ((uint32_t) (v) << (GPIO_MODER_MODE0_Pos + 2u * (n)))
#define GPIO_MODE_INPUT 0u
#define GPIO_MODE_OUTPUT 1u
#define GPIO_MODE_AF 2u
#define GPIO_PUPDR_PUPD_Msk(n) (GPIO_PUPDR_PUPD0_Msk << (2u * (n)))
#define GPIO_PUPDR_PUPD(n, v) ((uint32_t) (v) << (GPIO_PUPDR_PUPD0_Pos + 2u * (n)))
#define GPIO_PULL_UP 1u
#define GPIO_AFRL_AFSEL_Msk(n) (GPIO_AFRL_AFSEL0_Msk << (4u * (n))) /* pins 0-7, in AFR[0] */
#define GPIO_AFRL_AFSEL(n, v) ((uint32_t) (v) << (GPIO_AFRL_AFSEL0_Pos + 4u * (n)))
#define GPIO_AFRH_AFSEL_Msk(n) (GPIO_AFRH_AFSEL8_Msk << (4u * ((n) - 8u))) /* pins 8-15, in AFR[1] */
#define GPIO_AFRH_AFSEL(n, v) ((uint32_t) (v) << (GPIO_AFRH_AFSEL8_Pos + 4u * ((n) - 8u)))
#define GPIO_BSRR_BS_PIN(n) (GPIO_BSRR_BS0 << (n))
#define GPIO_BSRR_BR_PIN(n) (GPIO_BSRR_BR0 << (n))
#define GPIO_IDR_ID_PIN(n) (GPIO_IDR_ID0 << (n))
#define GPIO_LCKR_LCK_PIN(n) (GPIO_LCKR_LCK0 << (n))

/* NVIC: an interrupt's enable bit, and its priority byte in NVIC->IP[irq / 4] (word access only) */
#define NVIC_ISER_SETENA(irq) (1u << (irq))
#define NVIC_IP_PRI_Msk(irq) (0xFFu << (8u * ((irq) % 4u)))
#define NVIC_IP_PRI(irq, v) ((uint32_t) (v) << (8u * ((irq) % 4u)))

#endif /* FLAMEKEEPER_G030_FIELDS_H */
