/* The STM32G030's registers this firmware uses (reference manual RM0454). Written by hand to avoid
 * a vendor HAL: only what hal_stm32g0.c touches. Checked against ST's SVD by
 * tools/check_registers.py (run it after any change here or in hal/); UNVERIFIED on silicon. */
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
#define RCC_CSR REG(RCC + 0x60)
#define RCC_BDCR REG(RCC + 0x5C)

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

/* PWR and the RTC: the second clock for GLW-1's tick check */
#define PWR 0x40007000u
#define PWR_CR1 REG(PWR + 0x00)
#define RTC 0x40002800u
#define RTC_SSR REG(RTC + 0x08)
#define RTC_ICSR REG(RTC + 0x0C)
#define RTC_PRER REG(RTC + 0x10)
#define RTC_CR REG(RTC + 0x18)
#define RTC_WPR REG(RTC + 0x24)

/* Cortex-M0+ */
#define SYST_CSR REG(0xE000E010u)
#define SYST_RVR REG(0xE000E014u)
#define SYST_CVR REG(0xE000E018u)
#define NVIC_ISER REG(0xE000E100u)
#define SCB_AIRCR REG(0xE000ED0Cu) /* the bring-up image's software reset */
#define SCB_AIRCR_SYSRESETREQ (1u << 2)
#define SCB_AIRCR_VECTKEY_Pos 16u
#define SCB_AIRCR_VECTKEY_Msk (0xFFFFu << SCB_AIRCR_VECTKEY_Pos)
#define SCB_AIRCR_VECTKEY_KEY (0x05FAu << SCB_AIRCR_VECTKEY_Pos) /* a write without it is ignored */
#define IRQ_TIM3 16u
#define IRQ_USART2 28u

/* The ADC schedule (hal_stm32g0.c): the scan by DMA, the analog watchdogs, the glow timer's gap
 * compare, interrupt priorities. Offsets from RM0454 sections 9.6, 10.6, 14.12. */
#define ADC_IER REG(ADC + 0x04)
#define ADC_AWD1TR REG(ADC + 0x20)
#define ADC_AWD2TR REG(ADC + 0x24)
#define ADC_AWD2CR REG(ADC + 0xA0)
#define DMA_ISR REG(DMA1 + 0x00)
#define DMA_IFCR REG(DMA1 + 0x04)
#define DMA_CCR2 REG(DMA1 + 0x1C)
#define DMA_CNDTR2 REG(DMA1 + 0x20)
#define DMA_CPAR2 REG(DMA1 + 0x24)
#define DMA_CMAR2 REG(DMA1 + 0x28)
#define DMAMUX_C1CR REG(0x40020804u)
#define DMAMUX_REQ_ADC 5u
#define TIM_CCR3(t) REG((t) + 0x3C)
#define NVIC_IPR(n) REG(0xE000E400u + 4u * (n)) /* word access only on the M0+ */
#define IRQ_ADC 12u

/* Bits, named REGISTER_FIELD (checked by tools/check_registers.py against the SVD; the driver writes
 * no bit inline). REGISTER_FIELD_VALUE is a value of a field, in place; REGISTER_FIELD(v) places v
 * in the field. A field per pin (GPIO) or per interrupt (NVIC_IPR) takes its index n:
 * REGISTER_FIELD_Msk(n) is field n's mask, REGISTER_FIELD(n) its bit, REGISTER_FIELD(n, v) places v
 * in it. */

/* RCC, FLASH_ACR: the clock tree */
#define RCC_CR_PLLON (1u << 24)
#define RCC_CR_PLLRDY (1u << 25)
#define RCC_CFGR_SW (7u << 0)
#define RCC_CFGR_SW_PLL (2u << 0) /* PLLRCLK */
#define RCC_CFGR_SWS (7u << 3)
#define RCC_CFGR_SWS_PLL (2u << 3)
#define RCC_PLLCFGR_PLLSRC_HSI16 (2u << 0)
#define RCC_PLLCFGR_PLLM(v) ((uint32_t) (v) << 4)  /* divides by v + 1 */
#define RCC_PLLCFGR_PLLN(n) ((uint32_t) (n) << 8)  /* multiplies by n */
#define RCC_PLLCFGR_PLLREN (1u << 28)
#define RCC_PLLCFGR_PLLR(v) ((uint32_t) (v) << 29) /* divides by v + 1 */
#define FLASH_ACR_LATENCY (7u << 0)
#define FLASH_ACR_LATENCY_2WS (2u << 0)
#define FLASH_ACR_PRFTEN (1u << 8)
#define FLASH_ACR_ICEN (1u << 9)
#define SYST_CSR_ENABLE (1u << 0)
#define SYST_CSR_CLKSOURCE (1u << 2) /* the processor clock */
#define SYST_CSR_COUNTFLAG (1u << 16)

/* RCC: the clock enables and the reset flags */
#define RCC_IOPENR_GPIOAEN (1u << 0)
#define RCC_IOPENR_GPIOBEN (1u << 1)
#define RCC_IOPENR_GPIOCEN (1u << 2)
#define RCC_AHBENR_DMA1EN (1u << 0)
#define RCC_APBENR1_TIM3EN (1u << 1)
#define RCC_APBENR1_USART2EN (1u << 17)
#define RCC_APBENR2_SYSCFGEN (1u << 0)
#define RCC_APBENR2_TIM1EN (1u << 11)
#define RCC_APBENR2_TIM16EN (1u << 17)
#define RCC_APBENR2_TIM17EN (1u << 18)
#define RCC_APBENR2_ADCEN (1u << 20)
#define RCC_APBENR1_RTCAPBEN (1u << 10)
#define RCC_APBENR1_PWREN (1u << 28)
#define RCC_CSR_LSION (1u << 0)
#define RCC_CSR_LSIRDY (1u << 1)
#define RCC_CSR_RMVF (1u << 23)
#define RCC_CSR_PINRSTF (1u << 26)
#define RCC_CSR_PWRRSTF (1u << 27)
#define RCC_CSR_SFTRSTF (1u << 28)
#define RCC_CSR_IWDGRSTF (1u << 29)

/* The backup domain and the RTC (GLW-1): the RTC on the LSI, its sub-second counter read straight */
#define RCC_BDCR_RTCSEL (3u << 8)
#define RCC_BDCR_RTCSEL_LSI (2u << 8)
#define RCC_BDCR_RTCEN (1u << 15)
#define RCC_BDCR_BDRST (1u << 16)
#define PWR_CR1_DBP (1u << 8) /* the backup domain (RCC_BDCR, the RTC) writable */
#define RTC_SSR_SS 0xFFFFu
#define RTC_ICSR_INITF (1u << 6)
#define RTC_ICSR_INIT (1u << 7)
#define RTC_PRER_PREDIV_S(v) ((uint32_t) (v) << 0)
#define RTC_PRER_PREDIV_A(v) ((uint32_t) (v) << 16)
#define RTC_CR_BYPSHAD (1u << 5) /* the calendar and SSR read from the counters, not the shadows */
#define RTC_WPR_KEY_UNLOCK1 0xCAu /* the write protection's two keys, in this order */
#define RTC_WPR_KEY_UNLOCK2 0x53u
#define RTC_WPR_KEY_LOCK 0xFFu    /* any other value locks it again */

/* IWDG */
#define IWDG_KR_KEY_START 0xCCCCu
#define IWDG_KR_KEY_UNLOCK 0x5555u /* PR and RLR writable */
#define IWDG_KR_KEY_RELOAD 0xAAAAu
#define IWDG_SR_PVU (1u << 0)
#define IWDG_SR_RVU (1u << 1)
#define IWDG_SR_WVU (1u << 2)

/* GPIO, a field per pin n */
#define GPIO_MODER_MODER_Msk(n) (3u << (2u * (n)))
#define GPIO_MODER_MODER(n, v) ((uint32_t) (v) << (2u * (n)))
#define GPIO_MODER_MODER_INPUT 0u
#define GPIO_MODER_MODER_OUTPUT 1u
#define GPIO_MODER_MODER_AF 2u
#define GPIO_PUPDR_PUPDR_Msk(n) (3u << (2u * (n)))
#define GPIO_PUPDR_PUPDR(n, v) ((uint32_t) (v) << (2u * (n)))
#define GPIO_PUPDR_PUPDR_UP 1u
#define GPIO_AFRL_AFSEL_Msk(n) (15u << (4u * (n)))         /* pins 0-7 */
#define GPIO_AFRL_AFSEL(n, v) ((uint32_t) (v) << (4u * (n)))
#define GPIO_AFRH_AFSEL_Msk(n) (15u << (4u * ((n) - 8u))) /* pins 8-15 */
#define GPIO_AFRH_AFSEL(n, v) ((uint32_t) (v) << (4u * ((n) - 8u)))
#define GPIO_BSRR_BS(n) (1u << (n))
#define GPIO_BSRR_BR(n) (1u << ((n) + 16u))
#define GPIO_IDR_IDR(n) (1u << (n))
#define GPIO_LCKR_LCK(n) (1u << (n))
#define GPIO_LCKR_LCKK (1u << 16)

/* Timers */
#define TIM_CR1_CEN (1u << 0)
#define TIM_CR1_ARPE (1u << 7)
#define TIM_DIER_UDE (1u << 8)
#define TIM_SR_CC1IF (1u << 1)
#define TIM_EGR_UG (1u << 0)
#define TIM_CCMR1_CC1S_TI1 (1u << 0) /* CC1 an input, on TI1 */
#define TIM_CCMR1_OC1PE (1u << 3)
#define TIM_CCMR1_OC1M (7u << 4)
#define TIM_CCMR1_OC1M_FORCE_LOW (4u << 4) /* OC1REF forced inactive */
#define TIM_CCMR1_OC1M_FORCE_HIGH (5u << 4)
#define TIM_CCMR1_OC1M_PWM1 (6u << 4)
#define TIM_CCMR1_OC2PE (1u << 11)
#define TIM_CCMR1_OC2M_PWM1 (6u << 12)
#define TIM_CCER_CC1E (1u << 0)
#define TIM_CCER_CC2E (1u << 4)
#define TIM_BDTR_MOE (1u << 15)

/* USART2 */
#define USART_CR1_UE (1u << 0)
#define USART_CR1_RE (1u << 2)
#define USART_CR1_TE (1u << 3)
#define USART_CR1_RXNEIE (1u << 5)
#define USART_CR1_TXEIE (1u << 7)
#define USART_ISR_ORE (1u << 3)
#define USART_ISR_RXNE (1u << 5)
#define USART_ISR_TXE (1u << 7)
#define USART_ICR_ORECF (1u << 3)

/* NVIC: the interrupt enables, a priority byte per interrupt */
#define NVIC_ISER_SETENA(irq) (1u << (irq))
#define NVIC_IPR_PRI_Msk(irq) (0xFFu << (8u * ((irq) % 4u)))
#define NVIC_IPR_PRI(irq, v) ((uint32_t) (v) << (8u * ((irq) % 4u)))

/* The ADC set-up, the DMA's memory-to-peripheral direction */
#define ADC_CR_ADEN (1u << 0)
#define ADC_CR_ADCAL (1u << 31)
#define ADC_CFGR2_CKMODE_PCLK_DIV4 (2u << 30)
#define ADC_SMPR_SMP1_19_5 (4u << 0) /* 19.5 ADC clocks */
#define ADC_CCR_VREFEN (1u << 22)
#define DMA_CCR_DIR (1u << 4) /* read from memory */

/* The ADC schedule's bits */
#define ADC_ISR_ADRDY (1u << 0)
#define ADC_ISR_EOS (1u << 3)
#define ADC_ISR_OVR (1u << 4)
#define ADC_ISR_AWD1 (1u << 7)
#define ADC_ISR_AWD2 (1u << 8)
#define ADC_ISR_CCRDY (1u << 13)
#define ADC_IER_AWD1IE (1u << 7)
#define ADC_IER_AWD2IE (1u << 8)
#define ADC_CR_ADSTART (1u << 2)
#define ADC_CR_ADSTP (1u << 4)
#define ADC_CR_ADVREGEN (1u << 28)
#define ADC_CFGR1_DMAEN (1u << 0)
#define ADC_CFGR1_DMACFG (1u << 1) /* DMA circular */
#define ADC_CFGR1_CONT (1u << 13)
#define ADC_CFGR1_AWD1SGL (1u << 22)
#define ADC_CFGR1_AWD1EN (1u << 23)
#define ADC_CFGR1_AWD1CH(ch) ((uint32_t) (ch) << 26)
#define ADC_AWD1TR_LT1(raw) ((uint32_t) (raw) << 0)
#define ADC_AWD1TR_HT1(raw) ((uint32_t) (raw) << 16)
#define ADC_AWD2TR_LT2(raw) ((uint32_t) (raw) << 0)
#define ADC_AWD2TR_HT2(raw) ((uint32_t) (raw) << 16)
#define ADC_AWD2CR_AWD2CH6 (1u << 6)
#define DMA_ISR_TEIF2 (1u << 7)
#define DMA_IFCR_CGIF2 (1u << 4)
#define DMA_IFCR_CTCIF2 (1u << 5)
#define DMA_IFCR_CHTIF2 (1u << 6)
#define DMA_IFCR_CTEIF2 (1u << 7)
#define DMA_CCR_EN (1u << 0)
#define DMA_CCR_CIRC (1u << 5)
#define DMA_CCR_MINC (1u << 7)
#define DMA_CCR_PSIZE_16 (1u << 8)
#define DMA_CCR_MSIZE_16 (1u << 10)
#define TIM_DIER_CC2IE (1u << 2)
#define TIM_DIER_CC3IE (1u << 3)
#define TIM_SR_CC2IF (1u << 2)
#define TIM_SR_CC3IF (1u << 3)
#define FLASH_ECCR_ADDR_ECC 0x3FFFu /* the failing double word's index in the flash */
#define FLASH_ECCR_SYSF_ECC (1u << 20)
#define FLASH_ECCR_ECCCIE (1u << 24)
#define FLASH_ECCR_ECCD (1u << 31)
#define FLASH_SR_EOP (1u << 0)
#define FLASH_SR_OPERR (1u << 1)
#define FLASH_SR_PROGERR (1u << 3)
#define FLASH_SR_WRPERR (1u << 4)
#define FLASH_SR_PGAERR (1u << 5)
#define FLASH_SR_SIZERR (1u << 6)
#define FLASH_SR_PGSERR (1u << 7)
#define FLASH_SR_MISERR (1u << 8)
#define FLASH_SR_FASTERR (1u << 9)
#define FLASH_SR_OPTVERR (1u << 15) /* the option bytes failed to load */
#define FLASH_SR_BSY1 (1u << 16)
#define FLASH_SR_CFGBSY (1u << 18) /* FLASH_CR in use by a program or erase */
/* Every error flag, OPERR..FASTERR and OPTVERR (all rc_w1), and those with EOP: the clear. */
#define FLASH_SR_ERRORS (FLASH_SR_OPERR | FLASH_SR_PROGERR | FLASH_SR_WRPERR | FLASH_SR_PGAERR | \
                         FLASH_SR_SIZERR | FLASH_SR_PGSERR | FLASH_SR_MISERR | FLASH_SR_FASTERR | \
                         FLASH_SR_OPTVERR)
#define FLASH_SR_CLEAR (FLASH_SR_EOP | FLASH_SR_ERRORS)
#define FLASH_CR_PG (1u << 0)
#define FLASH_CR_PER (1u << 1)
#define FLASH_CR_PNB(page) ((uint32_t) (page) << 3)
#define FLASH_CR_STRT (1u << 16)
#define FLASH_CR_LOCK (1u << 31)
/* FLASH_KEYR's unlock sequence, and where the main flash starts (checked against ST's headers). */
#define FLASH_KEY1 0x45670123u
#define FLASH_KEY2 0xCDEF89ABu
#define FLASH_MEM_BASE 0x08000000u
#define FLASH_OPTR_IWDG_SW (1u << 16)    /* 0: the IWDG started by hardware */
#define FLASH_OPTR_IWDG_STOP (1u << 17)  /* 0: the IWDG frozen in stop mode */
#define FLASH_OPTR_IWDG_STDBY (1u << 18) /* 0: the IWDG frozen in standby */
#define FLASH_OPTR_NBOOT_SEL (1u << 24)  /* 0: the BOOT0 pin chooses, with nBOOT1 */
#define FLASH_OPTR_NBOOT1 (1u << 25)     /* 1: BOOT0 high boots the system memory (the ROM bootloader) */
#define FLASH_OPTR_NBOOT0 (1u << 26)     /* the boot choice when nBOOT_SEL is 1 */

#endif /* FLAMEKEEPER_STM32G030_H */
