/* hal.h on the STM32G030K8 of the rev A2 board: pins from the board's pin map, timers per CFG-13,
 * the heartbeat burst per PMP-1, the watchdog per WDG-2/3/16. Bare registers (stm32g030.h), no
 * vendor HAL.
 *
 * UNVERIFIED: written from RM0444 and the datasheet's alternate-function table, built but never run.
 * Check on the first board or a NUCLEO-G031K8 before trusting any of it: the clock tree, every
 * alternate-function number, the DMAMUX request id, the ADC channel order, the flash page
 * programming, and the timer outputs on a scope.
 *
 * Not done yet: the pump overcurrent watchdog (PMP-7: the ADC's analog watchdog on PUMP_ISENSE
 * during strokes; the pump high side's own over-current latch is the backstop meanwhile), and the
 * CO sensor. */
#include "../board/hal.h"

#include <string.h>

#include "../../common/link/link.h"
#include "hal_stm32g0.h"
#include "stm32g030.h"

#define SYSCLK_HZ 64000000u

/* ---------------------------------------------------------------- clocks, ticks, reset, options */

void hal_clock_init(void) {
  FLASH_ACR = (FLASH_ACR & ~7u) | 2u | (1u << 8) | (1u << 9); /* 2 wait states, prefetch, cache */
  while ((FLASH_ACR & 7u) != 2u) {
  }
  /* PLL: HSI16 / M 1 x N 8 / R 2 = 64 MHz. */
  RCC_PLLCFGR = 2u | (0u << 4) | (8u << 8) | (1u << 28) | (1u << 29);
  RCC_CR |= 1u << 24;
  while (!(RCC_CR & (1u << 25))) {
  }
  RCC_CFGR = (RCC_CFGR & ~7u) | 2u;
  while (((RCC_CFGR >> 3) & 7u) != 2u) {
  }
  SYST_RVR = SYSCLK_HZ / 1000u - 1u;
  SYST_CVR = 0;
  SYST_CSR = 5u; /* processor clock, counter on, no interrupt: polled */
}

int hal_ms_elapsed(void) { return (SYST_CSR & (1u << 16)) != 0; }

uint8_t hal_reset_reason(void) {
  uint32_t csr = RCC_CSR;
  RCC_CSR |= 1u << 23; /* clear the flags for next time */
  if (csr & (1u << 29))
    return LINK_RESET_WATCHDOG;
  if (csr & (1u << 28))
    return LINK_RESET_SOFTWARE;
  if (csr & (1u << 27))
    return LINK_RESET_POWER_ON;
  if (csr & (1u << 26))
    return LINK_RESET_PIN;
  return LINK_RESET_UNKNOWN;
}

/* WDG-1, WDG-4: IWDG in hardware, stopped in neither stop nor standby, boot from the main flash. */
uint8_t hal_option_faults(void) {
  uint32_t optr = FLASH_OPTR;
  int iwdg_sw = (optr >> 16) & 1u, nboot_sel = (optr >> 24) & 1u, optverr = (FLASH_SR >> 15) & 1u;
  return (uint8_t) ((iwdg_sw || nboot_sel || optverr) ? LINK_CHECK_OPTION_BYTES : 0u);
}

/* WDG-2, WDG-3: started on every start, about 80 ms (LSI 32 kHz / 4, 640). */
void hal_iwdg_start(void) {
  IWDG_KR = 0xCCCCu;
  IWDG_KR = 0x5555u;
  IWDG_PR = 0u;
  IWDG_RLR = 640u;
  while (IWDG_SR & 7u) {
  }
  IWDG_KR = 0xAAAAu;
}

void hal_iwdg_refresh(void) { IWDG_KR = 0xAAAAu; }

/* ---------------------------------------------------------------- pins */

static void pin_mode(uint32_t port, unsigned pin, unsigned mode) {
  GPIO_MODER(port) = (GPIO_MODER(port) & ~(3u << (2 * pin))) | (mode << (2 * pin));
}
static void pin_af(uint32_t port, unsigned pin, unsigned af) {
  if (pin < 8)
    GPIO_AFRL(port) = (GPIO_AFRL(port) & ~(15u << (4 * pin))) | (af << (4 * pin));
  else
    GPIO_AFRH(port) = (GPIO_AFRH(port) & ~(15u << (4 * (pin - 8)))) | (af << (4 * (pin - 8)));
  pin_mode(port, pin, 2u);
}
static void pin_pull_up(uint32_t port, unsigned pin) {
  GPIO_PUPDR(port) = (GPIO_PUPDR(port) & ~(3u << (2 * pin))) | (1u << (2 * pin));
}
static void pin_set(uint32_t port, unsigned pin, int on) { GPIO_BSRR(port) = on ? (1u << pin) : (1u << (pin + 16)); }
static int pin_get(uint32_t port, unsigned pin) { return (GPIO_IDR(port) >> pin) & 1u; }

/* ---------------------------------------------------------------- timers */

static volatile uint16_t burst[26];

static void timers_init(void) {
  /* TIM1, 25 kHz: CH1 the heartbeat (bursts by DMA), CH2 the fan. Repetition counter 24: an update
   * (and a DMA request) every 25 periods = 1 ms. */
  TIM_PSC(TIM1) = 0;
  TIM_ARR(TIM1) = SYSCLK_HZ / 25000u - 1u;
  TIM_RCR(TIM1) = 24;
  TIM_CCMR1(TIM1) = (6u << 4) | (1u << 3) | (6u << 12) | (1u << 11); /* PWM1, preload, both */
  TIM_CCR1(TIM1) = 0;
  TIM_CCR2(TIM1) = 0;
  TIM_CCER(TIM1) = (1u << 0) | (1u << 4);
  TIM_BDTR(TIM1) = 1u << 15; /* main output enable */
  TIM_DIER(TIM1) = 1u << 8;  /* update DMA request */
  TIM_EGR(TIM1) = 1;
  TIM_CR1(TIM1) = 1u | (1u << 7);
  DMAMUX_C0CR = DMAMUX_REQ_TIM1_UP;
  DMA_CPAR1 = TIM1 + 0x34u; /* CCR1 */
  /* TIM3, 200 Hz at 1 MHz: CH1 the glow pulse; CH2 at 120 us samples the pulse current (GLW-6). */
  TIM_PSC(TIM3) = SYSCLK_HZ / 1000000u - 1u;
  TIM_ARR(TIM3) = 4999;
  TIM_CCMR1(TIM3) = (6u << 4) | (1u << 3);
  TIM_CCR1(TIM3) = 0;
  TIM_CCR2(TIM3) = 120;
  TIM_CCER(TIM3) = 1u << 0;
  TIM_DIER(TIM3) = 1u << 2; /* CC2 interrupt */
  TIM_EGR(TIM3) = 1;
  TIM_CR1(TIM3) = 1u | (1u << 7);
  NVIC_ISER = 1u << IRQ_TIM3;
  /* TIM16, 100 kHz: input capture of the Hall on CH1. */
  TIM_PSC(TIM16) = SYSCLK_HZ / 100000u - 1u;
  TIM_ARR(TIM16) = 0xFFFF;
  TIM_CCMR1(TIM16) = 1u; /* CC1 input on TI1 */
  TIM_CCER(TIM16) = 1u;
  TIM_CR1(TIM16) = 1u;
  /* TIM17, 20 kHz: the pump's PWM. */
  TIM_PSC(TIM17) = 0;
  TIM_ARR(TIM17) = SYSCLK_HZ / 20000u - 1u;
  TIM_CCMR1(TIM17) = (6u << 4) | (1u << 3);
  TIM_CCR1(TIM17) = 0;
  TIM_CCER(TIM17) = 1u;
  TIM_BDTR(TIM17) = 1u << 15;
  TIM_EGR(TIM17) = 1;
  TIM_CR1(TIM17) = 1u | (1u << 7);
}

void hal_heartbeat(uint8_t mode) {
  uint32_t ccmr = TIM_CCMR1(TIM1) & ~(7u << 4);
  if (mode == HAL_HB_BURST) {
    /* PMP-1: 25 updates of half a period, then 0: the burst ends in hardware, interrupts or not. */
    for (int i = 0; i < 25; i++)
      burst[i] = (uint16_t) ((TIM_ARR(TIM1) + 1u) / 2u);
    burst[25] = 0;
    DMA_CCR1 = 0;
    DMA_CMAR1 = (uint32_t) burst;
    DMA_CNDTR1 = 26;
    /* memory to peripheral, memory increment, 16 bit both sides */
    DMA_CCR1 = (1u << 4) | (1u << 7) | (1u << 8) | (1u << 10) | 1u;
    TIM_CCMR1(TIM1) = ccmr | (6u << 4);
    return;
  }
  DMA_CCR1 = 0;
  TIM_CCR1(TIM1) = 0;
  /* Static levels for the self-tests (PMP-11): forced active / inactive. */
  TIM_CCMR1(TIM1) = ccmr | ((mode == HAL_HB_STATIC_HIGH ? 5u : 4u) << 4);
}

void hal_fan_duty(uint16_t permille) { TIM_CCR2(TIM1) = (TIM_ARR(TIM1) + 1u) * permille / 1000u; }
void hal_pump_duty(uint16_t permille) { TIM_CCR1(TIM17) = (TIM_ARR(TIM17) + 1u) * permille / 1000u; }
void hal_glow_next_pulse_us(uint16_t us) { TIM_CCR1(TIM3) = us; }

static uint32_t hall_last, hall_period, hall_seen_ms;
static uint32_t ms_count;

uint16_t hal_hall_rpm(void) {
  if (TIM_SR(TIM16) & (1u << 1)) {
    uint32_t cap = TIM_CCR1(TIM16);
    hall_period = (cap - hall_last) & 0xFFFFu;
    hall_last = cap;
    hall_seen_ms = ms_count;
  }
  if (ms_count - hall_seen_ms > 300u || hall_period == 0)
    return 0;
  return (uint16_t) (60u * 100000u / hall_period); /* one Hall pulse per turn: PROVISIONAL */
}

/* ---------------------------------------------------------------- ADC */

static volatile uint8_t adc_busy;
static volatile uint16_t glow_pulse_raw;

static void adc_init(void) {
  ADC_CR = 1u << 28; /* the regulator */
  for (volatile int i = 0; i < 2000; i++) {
  }
  ADC_CR |= 1u << 31; /* calibration */
  while (ADC_CR & (1u << 31)) {
  }
  ADC_CFGR2 = 2u << 30;  /* PCLK / 4 */
  ADC_SMPR = 4u;         /* 19.5 cycles */
  ADC_CFGR1 = 0;         /* CFG-1: bit-mask selection, 12 bit, single */
  ADC_CCR |= 1u << 22;   /* VREFINT */
  ADC_ISR = 1u;
  ADC_CR |= 1u;
  while (!(ADC_ISR & 1u)) {
  }
}

static uint16_t adc_one(uint32_t mask) {
  ADC_CHSELR = mask;
  ADC_ISR = (1u << 2) | (1u << 3);
  ADC_CR |= 1u << 2;
  while (!(ADC_ISR & (1u << 2))) {
  }
  return (uint16_t) ADC_DR;
}

void hal_adc_scan(uint16_t raw[HAL_ADC_COUNT]) {
  /* Ascending channel numbers: IN0, 1, 4, 5, 6, 9, 10, 11, 13 (VREFINT), 15. */
  static const uint8_t ch[HAL_ADC_COUNT] = {0, 1, 4, 5, 6, 9, 10, 11, 13, 15};
  adc_busy = 1;
  for (int i = 0; i < HAL_ADC_COUNT; i++)
    raw[i] = adc_one(1u << ch[i]);
  adc_busy = 0;
}

uint16_t hal_vrefint_cal(void) { return VREFINT_CAL; }

/* 120 us into each glow period (GLW-6): the pulse current, unless a scan holds the ADC (then this
 * period has no reading and the planner probes again). */
void TIM3_IRQHandler(void) {
  TIM_SR(TIM3) = ~(1u << 2);
  if (!adc_busy && TIM_CCR1(TIM3) >= 150u)
    glow_pulse_raw = adc_one(1u << 1);
}

uint16_t hal_glow_pulse_raw(void) {
  uint16_t r = glow_pulse_raw;
  glow_pulse_raw = 0;
  return r;
}

static uint16_t pump_peak;

void hal_pump_test_pulse_us(uint16_t us) {
  /* One pulse on PUMP_CMD by hand (PB9 as a plain output), sampling PUMP_ISENSE throughout. */
  uint16_t peak = 0;
  uint32_t ticks = us * (SYSCLK_HZ / 1000000u);
  pin_mode(GPIOB, 9, 1u);
  pin_set(GPIOB, 9, 1);
  uint32_t start = SYST_CVR;
  while (((start - SYST_CVR) & 0xFFFFFFu) < ticks) {
    uint16_t r = adc_one(1u << 6);
    if (r > peak)
      peak = r;
  }
  pin_set(GPIOB, 9, 0);
  pin_af(GPIOB, 9, 2u);
  pump_peak = peak;
}

uint16_t hal_pump_peak_raw(void) {
  uint16_t r = pump_peak;
  pump_peak = 0;
  return r;
}

uint8_t hal_pump_overcurrent(void) { return 0; } /* not done yet: see the top of this file */

/* ---------------------------------------------------------------- digital lines */

uint8_t hal_overheat_n(void) { return (uint8_t) pin_get(GPIOC, 14); }
uint8_t hal_pump_hs_flt_n(void) { return (uint8_t) pin_get(GPIOC, 15); }
uint8_t hal_pwr_flt_n(void) { return (uint8_t) pin_get(GPIOA, 7); }
void hal_glow_en(uint8_t on) { pin_set(GPIOB, 0, on); }
void hal_latch_reset(uint8_t level) { pin_set(GPIOB, 6, level); }
void hal_co_heater(uint8_t on) { pin_set(GPIOC, 6, on); }

/* ---------------------------------------------------------------- the link (USART2) */

static volatile uint8_t rx_buf[128], tx_buf[128];
static volatile uint8_t rx_head, rx_tail, tx_head, tx_tail;

void USART2_IRQHandler(void) {
  uint32_t isr = USART_ISR;
  if (isr & (1u << 3))
    USART_ICR = 1u << 3; /* overrun: drop, the parser resynchronises */
  if (isr & (1u << 5)) {
    uint8_t next = (uint8_t) ((rx_head + 1u) & 127u);
    uint8_t c = (uint8_t) USART_RDR;
    if (next != rx_tail) {
      rx_buf[rx_head] = c;
      rx_head = next;
    }
  }
  if ((isr & (1u << 7)) && (USART_CR1 & (1u << 7))) {
    if (tx_tail != tx_head) {
      USART_TDR = tx_buf[tx_tail];
      tx_tail = (uint8_t) ((tx_tail + 1u) & 127u);
    } else {
      USART_CR1 &= ~(1u << 7);
    }
  }
}

int hal_uart_getc(void) {
  if (rx_tail == rx_head)
    return -1;
  uint8_t c = rx_buf[rx_tail];
  rx_tail = (uint8_t) ((rx_tail + 1u) & 127u);
  return c;
}

void hal_uart_write(const uint8_t *d, size_t n) {
  for (size_t i = 0; i < n; i++) {
    uint8_t next = (uint8_t) ((tx_head + 1u) & 127u);
    if (next == tx_tail)
      break; /* full: the ESP32 times out and asks again */
    tx_buf[tx_head] = d[i];
    tx_head = next;
  }
  USART_CR1 |= 1u << 7;
}

/* ---------------------------------------------------------------- flash (the last two pages) */

static uint32_t page_addr(uint8_t page) { return 0x08000000u + (30u + page) * 2048u; }

size_t hal_flash_read(uint8_t page, uint8_t *out, size_t cap) {
  const uint8_t *p = (const uint8_t *) page_addr(page);
  size_t n = cap < 256u ? cap : 256u;
  memcpy(out, p, n);
  return n;
}

int hal_flash_write(uint8_t page, const uint8_t *d, size_t n) {
  if (n > 256u)
    return -1;
  while (FLASH_SR & (1u << 16)) {
  }
  FLASH_KEYR = 0x45670123u;
  FLASH_KEYR = 0xCDEF89ABu;
  FLASH_SR = 0xFFFFu; /* clear old errors */
  FLASH_CR = (1u << 1) | ((30u + page) << 3) | (1u << 16); /* erase the page */
  while (FLASH_SR & (1u << 16)) {
  }
  FLASH_CR = 1u; /* program, 64 bits at a time */
  uint32_t a = page_addr(page);
  for (size_t i = 0; i < n; i += 8) {
    uint32_t w[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};
    memcpy(w, d + i, n - i < 8 ? n - i : 8);
    REG(a + i) = w[0];
    REG(a + i + 4) = w[1];
    while (FLASH_SR & (1u << 16)) {
    }
  }
  FLASH_CR = 1u << 31; /* lock */
  return (FLASH_SR & 0xC3FAu) ? -1 : 0;
}

/* ---------------------------------------------------------------- start-up */

void hal_init(void) {
  RCC_IOPENR = 7u;                                      /* GPIOA, B, C */
  RCC_AHBENR |= 1u;                                     /* DMA1 */
  RCC_APBENR1 |= (1u << 1) | (1u << 17);                /* TIM3, USART2 */
  RCC_APBENR2 |= 1u | (1u << 11) | (1u << 17) | (1u << 18) | (1u << 20); /* SYSCFG, TIM1, 16, 17, ADC */

  /* Outputs default-off before they are driven (CFG-9). */
  pin_set(GPIOB, 0, 0);
  pin_mode(GPIOB, 0, 1u); /* GLOW_EN */
  pin_set(GPIOB, 6, 0);
  pin_mode(GPIOB, 6, 1u); /* LATCH_RESET */
  pin_set(GPIOC, 6, 0);
  pin_mode(GPIOC, 6, 1u); /* CO_HEAT_EN */
  /* Inputs: PC14, PC15 inputs only (CFG-5), pull-ups on PA7 and PC15 (CFG-11). */
  pin_mode(GPIOC, 14, 0u);
  pin_mode(GPIOC, 15, 0u);
  pin_pull_up(GPIOC, 15);
  pin_mode(GPIOA, 7, 0u);
  pin_pull_up(GPIOA, 7);
  GPIO_LCKR(GPIOC) = (1u << 16) | (1u << 14); /* OVH-5: lock PC14 as an input */
  GPIO_LCKR(GPIOC) = (1u << 14);
  GPIO_LCKR(GPIOC) = (1u << 16) | (1u << 14);
  (void) GPIO_LCKR(GPIOC);
  /* Analog inputs stay in their reset (analog) mode. */

  timers_init();
  pin_af(GPIOA, 8, 2u);  /* HEARTBEAT, TIM1_CH1 */
  pin_af(GPIOB, 3, 1u);  /* FAN_PWM, TIM1_CH2 */
  pin_af(GPIOB, 4, 1u);  /* GLOW_PWM, TIM3_CH1 */
  pin_af(GPIOB, 8, 2u);  /* HALL, TIM16_CH1 */
  pin_af(GPIOB, 9, 2u);  /* PUMP_CMD, TIM17_CH1 */

  adc_init();

  /* USART2 at 115200 8N1 (PA2 TX, PA3 RX). */
  USART_BRR = SYSCLK_HZ / 115200u;
  USART_CR1 = (1u << 5) | (1u << 3) | (1u << 2) | 1u;
  pin_af(GPIOA, 2, 1u);
  pin_af(GPIOA, 3, 1u);
  NVIC_ISER = 1u << IRQ_USART2;
}

void hal_ms_tick(void) { ms_count++; }
