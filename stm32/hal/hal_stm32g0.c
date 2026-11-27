/* hal.h on the STM32G030K8 of the rev A2 board: pins from the board's pin map, timers per CFG-13,
 * the heartbeat burst per PMP-1, the watchdog per WDG-2/3/16. Bare registers from ST's device header
 * (g030_fields.h), no vendor HAL.
 *
 * UNVERIFIED: written from RM0444 and the datasheet's alternate-function table, built but never run.
 * Check on the first board or a NUCLEO-G031K8 before trusting any of it: the clock tree, every
 * alternate-function number, the DMAMUX request id, the ADC channel order, the flash page
 * programming, and the timer outputs on a scope.
 *
 * The ADC schedule (one ADC for the scan, the glow pulse readings and the pump's overcurrent
 * watchdog) is described above its section below.
 *
 * The CO sensor (CO-9) is CO_ADC on PA11 / IN15, last in the scan, and CO_HEAT_EN on PC6 as a plain
 * output (CFG-10: no hardware PWM; the MiCS-5524's heater is steady on). C_CO_ADC (100 nF) holds
 * the 47 k divider up for the short sample time. UNVERIFIED: PA11 is not remapped to PA9
 * (SYSCFG_CFGR1.PA11_RMP stays 0), and PA9, sharing the pin, stays analog. */
#include "../board/hal.h"

#include <string.h>

#include "../../common/link/link.h"
#include "../board/pump_oc.h"
#include "hal_stm32g0.h"
#include "g030_fields.h"

#define SYSCLK_HZ 64000000u

/* ---------------------------------------------------------------- clocks, ticks, reset, options */

void hal_clock_init(void) {
  /* 2 wait states, prefetch, cache */
  FLASH->ACR = (FLASH->ACR & ~FLASH_ACR_LATENCY) | FLASH_ACR_LATENCY_1 | FLASH_ACR_PRFTEN | FLASH_ACR_ICEN;
  while ((FLASH->ACR & FLASH_ACR_LATENCY) != FLASH_ACR_LATENCY_1) {
  }
  /* PLL: HSI16 / M 1 x N 8 / R 2 = 64 MHz. */
  RCC->PLLCFGR = RCC_PLLCFGR_PLLSRC_1 | ((uint32_t) (0u) << RCC_PLLCFGR_PLLM_Pos) | ((uint32_t) (8u) << RCC_PLLCFGR_PLLN_Pos) | RCC_PLLCFGR_PLLREN |
                ((uint32_t) (1u) << RCC_PLLCFGR_PLLR_Pos);
  RCC->CR |= RCC_CR_PLLON;
  while (!(RCC->CR & RCC_CR_PLLRDY)) {
  }
  RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_1;
  while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_1) {
  }
  SysTick->LOAD = SYSCLK_HZ / 1000u - 1u;
  SysTick->VAL = 0;
  SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_ENABLE_Msk; /* no interrupt: polled */
}

int hal_ms_elapsed(void) { return (SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk) != 0; }

uint8_t hal_reset_reason(void) {
  uint32_t csr = RCC->CSR;
  RCC->CSR |= RCC_CSR_RMVF; /* clear the flags for next time */
  if (csr & RCC_CSR_IWDGRSTF)
    return LINK_RESET_WATCHDOG;
  if (csr & RCC_CSR_SFTRSTF)
    return LINK_RESET_SOFTWARE;
  if (csr & RCC_CSR_PWRRSTF)
    return LINK_RESET_POWER_ON;
  if (csr & RCC_CSR_PINRSTF)
    return LINK_RESET_PIN;
  return LINK_RESET_UNKNOWN;
}

/* WDG-1, WDG-4: the option bytes the ESP32's updater writes (hal_optr_wdg1_ok). */
uint8_t hal_option_faults(void) {
  return (uint8_t) (hal_optr_wdg1_ok(FLASH->OPTR, FLASH->SR) ? 0u : LINK_CHECK_OPTION_BYTES);
}

/* WDG-2, WDG-3: started on every start, about 80 ms (LSI 32 kHz / 4, 640). */
void hal_iwdg_start(void) {
  IWDG->KR = IWDG_KR_KEY_START;
  IWDG->KR = IWDG_KR_KEY_UNLOCK;
  IWDG->PR = 0u;
  IWDG->RLR = 640u;
  while (IWDG->SR & (IWDG_SR_PVU | IWDG_SR_RVU | IWDG_SR_WVU)) {
  }
  IWDG->KR = IWDG_KR_KEY_RELOAD;
}

void hal_iwdg_refresh(void) { IWDG->KR = IWDG_KR_KEY_RELOAD; }

/* GLW-1: the second clock. The RTC on the LSI (the watchdog's oscillator, independent of HSI16 and
 * the PLL that make the tick), its asynchronous prescaler at 128 (LSI 32 kHz / 128 = 250 Hz) and
 * its synchronous one at the full 15 bits, so the sub-second counter (RTC->SSR) counts down from
 * 32767 at 250 Hz and wraps every 131 s; read straight from the counter (BYPSHAD). The calendar
 * runs too, unused. Started in every reset (RTCSEL is written once per backup-domain reset), every
 * wait bounded: a dead LSI leaves the count still, which the tick check reports.
 * UNVERIFIED on the chip: the RTC kernel clock from the LSI, SSR's rate, and that two reads in a row
 * agree within a few tries. */
#define RTC_PREDIV_A 127u
#define RTC_PREDIV_S (HAL_SECOND_CLOCK_WRAP - 1u)
typedef char rtc_gives_the_second_clock[(32000u / (RTC_PREDIV_A + 1u) == HAL_SECOND_CLOCK_HZ) ? 1 : -1];

static void rtc_init(void) {
  RCC->APBENR1 |= RCC_APBENR1_PWREN | RCC_APBENR1_RTCAPBEN;
  PWR->CR1 |= PWR_CR1_DBP;
  RCC->CSR |= RCC_CSR_LSION; /* on already when the IWDG runs */
  for (volatile int i = 0; i < 20000 && !(RCC->CSR & RCC_CSR_LSIRDY); i++) {
  }
  if ((RCC->BDCR & RCC_BDCR_RTCSEL) != RCC_BDCR_RTCSEL_1) {
    RCC->BDCR = RCC_BDCR_BDRST;
    RCC->BDCR = 0;
  }
  RCC->BDCR = RCC_BDCR_RTCSEL_1 | RCC_BDCR_RTCEN;
  RTC->WPR = RTC_WPR_KEY_UNLOCK1;
  RTC->WPR = RTC_WPR_KEY_UNLOCK2;
  RTC->ICSR = RTC_ICSR_INIT;
  for (volatile int i = 0; i < 20000 && !(RTC->ICSR & RTC_ICSR_INITF); i++) {
  }
  RTC->PRER = ((uint32_t) (RTC_PREDIV_S) << RTC_PRER_PREDIV_S_Pos); /* two writes, synchronous first (RM0454) */
  RTC->PRER = ((uint32_t) (RTC_PREDIV_S) << RTC_PRER_PREDIV_S_Pos) | ((uint32_t) (RTC_PREDIV_A) << RTC_PRER_PREDIV_A_Pos);
  RTC->CR = RTC_CR_BYPSHAD;
  RTC->ICSR = 0; /* out of init: the counters run */
  RTC->WPR = RTC_WPR_KEY_LOCK;
}

uint16_t hal_second_clock(void) {
  /* Straight from the counter: two equal reads in a row. */
  uint32_t a, b = RTC->SSR & RTC_SSR_SS;
  for (int i = 0; i < 4; i++) {
    a = b;
    b = RTC->SSR & RTC_SSR_SS;
    if (a == b)
      break;
  }
  if (b > RTC_PREDIV_S)
    b = RTC_PREDIV_S;
  return (uint16_t) (RTC_PREDIV_S - b); /* counting up */
}

/* ---------------------------------------------------------------- pins */

static void pin_mode(GPIO_TypeDef *port, unsigned pin, unsigned mode) {
  port->MODER = (port->MODER & ~GPIO_MODER_MODE_Msk(pin)) | GPIO_MODER_MODE(pin, mode);
}
static void pin_af(GPIO_TypeDef *port, unsigned pin, unsigned af) {
  if (pin < 8)
    port->AFR[0] = (port->AFR[0] & ~GPIO_AFRL_AFSEL_Msk(pin)) | GPIO_AFRL_AFSEL(pin, af);
  else
    port->AFR[1] = (port->AFR[1] & ~GPIO_AFRH_AFSEL_Msk(pin)) | GPIO_AFRH_AFSEL(pin, af);
  pin_mode(port, pin, GPIO_MODE_AF);
}
static void pin_pull_up(GPIO_TypeDef *port, unsigned pin) {
  port->PUPDR = (port->PUPDR & ~GPIO_PUPDR_PUPD_Msk(pin)) | GPIO_PUPDR_PUPD(pin, GPIO_PULL_UP);
}
static void pin_set(GPIO_TypeDef *port, unsigned pin, int on) {
  port->BSRR = on ? GPIO_BSRR_BS_PIN(pin) : GPIO_BSRR_BR_PIN(pin);
}
static int pin_get(GPIO_TypeDef *port, unsigned pin) { return (int) ((port->IDR & GPIO_IDR_ID_PIN(pin)) >> pin); }

/* Interrupts off around what an interrupt also writes; restores the previous state, so it is safe
 * before main enables them. */
static uint32_t irq_off(void) {
  uint32_t m;
  __asm__ volatile("mrs %0, primask\n cpsid i" : "=r"(m) : : "memory");
  return m;
}
static void irq_restore(uint32_t m) { __asm__ volatile("msr primask, %0" : : "r"(m) : "memory"); }

/* ---------------------------------------------------------------- timers */

/* PMP-1: the heartbeat's burst, CCR1 values TIM1's DMA writes one per update (1 ms): HB_BURST
 * updates at half duty, then 0, so the burst ends in hardware, interrupts or not. Filled once by
 * timers_init. In RAM, not flash: a flash page erase stalls every flash read, the DMA's too. */
#define HB_BURST 25
static volatile uint16_t burst[HB_BURST + 1];
/* The pump's overcurrent cut (PMP-7) has stopped the heartbeat: no burst until the core has the
 * report (hal_pump_overcurrent), and then holds the heartbeat off itself. */
static volatile uint8_t hb_blocked;

/* The glow period and the two readings TIM3 picks in it (the ADC schedule below). */
#define GLOW_PERIOD_US 5000u
#define ADC_SCAN_US (HAL_ADC_COUNT * 2u) /* 32 ADC clocks (19.5 sampling + 12.5) at 16 MHz: 2 us */
#define GLOW_PULSE_PICK_US (105u + ADC_SCAN_US) /* the reading picked was taken 105-125 us in */
#define GLOW_LONGEST_PULSE_US 4950u             /* GLW-4: at most 99 % duty */
#define GLOW_GAP_PICK_US (GLOW_LONGEST_PULSE_US + 25u + ADC_SCAN_US) /* >= 25 us after it */
/* Both picks need the scan to fit: the gap pick before the period ends, the pulse pick before the
 * shortest pulse (150 us) ends. */
typedef char adc_scan_fits_the_glow_period
    [(GLOW_GAP_PICK_US < GLOW_PERIOD_US - 1u && GLOW_PULSE_PICK_US < 150u) ? 1 : -1];

static void timers_init(void) {
  /* TIM1, 25 kHz: CH1 the heartbeat (bursts by DMA), CH2 the fan. Repetition counter 24: an update
   * (and a DMA request) every 25 periods = 1 ms. */
  TIM1->PSC = 0;
  TIM1->ARR = SYSCLK_HZ / 25000u - 1u;
  TIM1->RCR = 24;
  TIM1->CCMR1 = (TIM_CCMR1_OC1M_2 | TIM_CCMR1_OC1M_1) | TIM_CCMR1_OC1PE | (TIM_CCMR1_OC2M_2 | TIM_CCMR1_OC2M_1) | TIM_CCMR1_OC2PE;
  TIM1->CCR1 = 0;
  TIM1->CCR2 = 0;
  TIM1->CCER = TIM_CCER_CC1E | TIM_CCER_CC2E;
  TIM1->BDTR = TIM_BDTR_MOE;
  TIM1->DIER = TIM_DIER_UDE; /* the update's DMA request */
  TIM1->EGR = TIM_EGR_UG;
  TIM1->CR1 = TIM_CR1_CEN | TIM_CR1_ARPE;
  for (int i = 0; i < HB_BURST; i++)
    burst[i] = (uint16_t) ((TIM1->ARR + 1u) / 2u);
  burst[HB_BURST] = 0;
  DMAMUX1_Channel0->CCR = DMAMUX_REQ_TIM1_UP;
  DMA1_Channel1->CPAR = (uint32_t) &TIM1->CCR1;
  /* TIM3, 200 Hz at 1 MHz: CH1 the glow pulse; CH2 and CH3 (no pins) pick the pulse and gap
   * readings from the ADC's scan (GLW-6, the ADC schedule below). */
  TIM3->PSC = SYSCLK_HZ / 1000000u - 1u;
  TIM3->ARR = GLOW_PERIOD_US - 1u;
  TIM3->CCMR1 = (TIM_CCMR1_OC1M_2 | TIM_CCMR1_OC1M_1) | TIM_CCMR1_OC1PE;
  TIM3->CCR1 = 0;
  TIM3->CCR2 = GLOW_PULSE_PICK_US;
  TIM3->CCR3 = GLOW_GAP_PICK_US;
  TIM3->CCER = TIM_CCER_CC1E;
  TIM3->DIER = TIM_DIER_CC2IE | TIM_DIER_CC3IE;
  TIM3->EGR = TIM_EGR_UG;
  TIM3->CR1 = TIM_CR1_CEN | TIM_CR1_ARPE;
  NVIC->ISER[0] = NVIC_ISER_SETENA(TIM3_IRQn);
  /* TIM16, 100 kHz: input capture of the Hall on CH1. */
  TIM16->PSC = SYSCLK_HZ / 100000u - 1u;
  TIM16->ARR = 0xFFFF;
  TIM16->CCMR1 = TIM_CCMR1_CC1S_0;
  TIM16->CCER = TIM_CCER_CC1E;
  TIM16->CR1 = TIM_CR1_CEN;
  /* TIM17, 20 kHz: the pump's PWM. */
  TIM17->PSC = 0;
  TIM17->ARR = SYSCLK_HZ / 20000u - 1u;
  TIM17->CCMR1 = (TIM_CCMR1_OC1M_2 | TIM_CCMR1_OC1M_1) | TIM_CCMR1_OC1PE;
  TIM17->CCR1 = 0;
  TIM17->CCER = TIM_CCER_CC1E;
  TIM17->BDTR = TIM_BDTR_MOE;
  TIM17->EGR = TIM_EGR_UG;
  TIM17->CR1 = TIM_CR1_CEN | TIM_CR1_ARPE;
}

void hal_heartbeat(uint8_t mode) {
  /* TIM1's CCMR1 and the DMA channel are also written by the pump's overcurrent cut. */
  uint32_t m = irq_off();
  if (hb_blocked && mode != HAL_HB_STATIC_LOW)
    mode = HAL_HB_STOPPED;
  uint32_t ccmr = TIM1->CCMR1 & ~TIM_CCMR1_OC1M_LOW;
  if (mode == HAL_HB_BURST) {
    DMA1_Channel1->CCR = 0;
    DMA1_Channel1->CMAR = (uint32_t) burst;
    DMA1_Channel1->CNDTR = HB_BURST + 1;
    /* memory to peripheral, memory increment, 16 bit both sides */
    DMA1_Channel1->CCR = DMA_CCR_DIR | DMA_CCR_MINC | DMA_CCR_PSIZE_0 | DMA_CCR_MSIZE_0 | DMA_CCR_EN;
    TIM1->CCMR1 = ccmr | (TIM_CCMR1_OC1M_2 | TIM_CCMR1_OC1M_1);
    irq_restore(m);
    return;
  }
  DMA1_Channel1->CCR = 0;
  TIM1->CCR1 = 0;
  /* Static levels for the self-tests (PMP-11): forced active / inactive. */
  TIM1->CCMR1 = ccmr | (mode == HAL_HB_STATIC_HIGH ? (TIM_CCMR1_OC1M_2 | TIM_CCMR1_OC1M_0) : TIM_CCMR1_OC1M_2);
  irq_restore(m);
}

void hal_fan_duty(uint16_t permille) { TIM1->CCR2 = (TIM1->ARR + 1u) * permille / 1000u; }
void hal_pump_duty(uint16_t permille) { TIM17->CCR1 = (TIM17->ARR + 1u) * permille / 1000u; }
void hal_glow_next_pulse_us(uint16_t us) { TIM3->CCR1 = us; }

static uint32_t hall_last, hall_period, hall_seen_ms;
static uint32_t ms_count;

uint16_t hal_hall_rpm(void) {
  if (TIM16->SR & TIM_SR_CC1IF) {
    uint32_t cap = TIM16->CCR1;
    hall_period = (cap - hall_last) & 0xFFFFu;
    hall_last = cap;
    hall_seen_ms = ms_count;
  }
  if (ms_count - hall_seen_ms > 300u || hall_period == 0)
    return 0;
  return (uint16_t) (60u * 100000u / hall_period); /* one Hall pulse per turn: PROVISIONAL */
}

/* ---------------------------------------------------------------- ADC */

/* The ADC schedule. One ADC serves the regular readings, the glow pulse readings and the pump's
 * overcurrent watchdog, so nothing ever starts a conversion of its own:
 *
 *   The ADC scans every channel back to back, for ever (continuous mode, CFG-1's bit-mask order),
 *   and DMA channel 2 copies each result into adc_buf[] in circular mode: adc_buf[i] is always
 *   channel i's latest conversion. A conversion is 2 us, a scan ADC_SCAN_US (20 us).
 *
 *   - The main loop copies adc_buf every millisecond (hal_adc_scan), each value at most one scan
 *     old (PMP-1's fresh readings). It restarts a stopped ADC (an overrun, a DMA error, no end of
 *     sequence for a millisecond) from the first channel, so adc_buf never shifts.
 *   - Glow (GLW-6): TIM3 makes GLOW_PWM, its period starting with the pulse, and picks two readings
 *     from adc_buf in its compare interrupts. CC2 at GLOW_PULSE_PICK_US (125 us): GLOW_ISENSE
 *     converted 105-125 us after the pulse began (its 10 us filter settled; the shortest pulse is
 *     150 us), kept only if GLOW_PWM is still high (so the pulse was on throughout). CC3 at
 *     GLOW_GAP_PICK_US (4995 us): GLOW_ISENSE and VIN_SENSE converted 4975-4995 us, at least 25 us
 *     after the longest pulse (99 %) ended, kept only if GLOW_PWM is low. The CC2 interrupt hands
 *     both to the main loop (hal_glow_period), which plans the next pulse within the same period.
 *   - Pump (PMP-7): the analog watchdogs watch PUMP_ISENSE's conversion in every scan, AWD1 above
 *     the threshold, AWD2 at or below it. While hal_pump_oc_arm arms them (each stroke), their
 *     interrupt, the highest priority, counts conversions above in a row with pump_oc: two (40 us)
 *     cut the stroke there and then (PUMP_CMD forced low, the heartbeat's DMA stopped and its
 *     output forced low), and the next hal_pump_overcurrent reports it to the core.
 *   - Self-test pump pulses read PUMP_ISENSE from adc_buf too.
 *
 * UNVERIFIED on the chip: the conversion time (32 ADC clocks at PCLK / 4) and so the scan time and
 * every pick window; that continuous mode adds no gap between scans; the DMA request (DMAMUX 5);
 * the interrupt latencies the windows allow for (a few us; ADC first, then TIM3, then USART2); the
 * watchdog's latency from a conversion to PUMP_CMD low; and that a 1.2 us sampling time suits
 * every source (the high-impedance ones, CO_ADC and VIN_SENSE, unless a capacitor at the pin
 * holds them). */

static volatile uint16_t adc_buf[HAL_ADC_COUNT];

static void adc_init(void) {
  ADC1->CR = ADC_CR_ADVREGEN; /* the regulator */
  for (volatile int i = 0; i < 2000; i++) {
  }
  ADC1->CR |= ADC_CR_ADCAL; /* calibration */
  while (ADC1->CR & ADC_CR_ADCAL) {
  }
  ADC1->CFGR2 = ADC_CFGR2_CKMODE_1;
  ADC1->SMPR = ADC_SMPR_SMP1_2;
  ADC1->CFGR1 = 0; /* CFG-1: bit-mask selection, 12 bit, single */
  ADC1_COMMON->CCR |= ADC_CCR_VREFEN;
  ADC1->ISR = ADC_ISR_ADRDY;
  ADC1->CR |= ADC_CR_ADEN;
  while (!(ADC1->ISR & ADC_ISR_ADRDY)) {
  }
}

/* Waits for the end of one scan, a few times its length at most. */
static void adc_wait_scan(void) {
  for (volatile int i = 0; i < 400 && !(ADC1->ISR & ADC_ISR_EOS); i++) {
  }
}

/* (Re)starts the scan from its first channel, adc_buf[0] onwards. */
static void adc_start(void) {
  if (ADC1->CR & ADC_CR_ADSTART) {
    ADC1->CR = ADC_CR_ADVREGEN | ADC_CR_ADSTP;
    while (ADC1->CR & ADC_CR_ADSTART) {
    }
  }
  DMA1_Channel2->CCR = 0;
  DMA1->IFCR = DMA_IFCR_CGIF2 | DMA_IFCR_CTCIF2 | DMA_IFCR_CHTIF2 | DMA_IFCR_CTEIF2;
  DMA1_Channel2->CPAR = (uint32_t) &ADC1->DR;
  DMA1_Channel2->CMAR = (uint32_t) adc_buf;
  DMA1_Channel2->CNDTR = HAL_ADC_COUNT;
  /* peripheral to memory, circular, memory increment, 16 bit both sides */
  DMA1_Channel2->CCR = DMA_CCR_CIRC | DMA_CCR_MINC | DMA_CCR_PSIZE_0 | DMA_CCR_MSIZE_0 | DMA_CCR_EN;
  ADC1->ISR = ADC_ISR_OVR | ADC_ISR_EOS;
  ADC1->CR = ADC_CR_ADVREGEN | ADC_CR_ADSTART;
}

static void irq_priority(unsigned irq, uint32_t level) {
  NVIC->IP[irq / 4u] = (NVIC->IP[irq / 4u] & ~NVIC_IP_PRI_Msk(irq)) | NVIC_IP_PRI(irq, level);
}

static void adc_schedule_init(void) {
  /* Ascending channel numbers: IN0, 1, 4, 5, 6, 9, 10, 11, 13 (VREFINT), 15: the HAL_ADC_ order. */
  static const uint8_t ch[HAL_ADC_COUNT] = {0, 1, 4, 5, 6, 9, 10, 11, 13, 15};
  uint32_t mask = 0;
  for (int i = 0; i < HAL_ADC_COUNT; i++)
    mask |= 1u << ch[i];
  ADC1->CHSELR = mask;
  while (!(ADC1->ISR & ADC_ISR_CCRDY)) {
  }
  ADC1->ISR = ADC_ISR_CCRDY;
  /* DMA in circular mode, continuous, the data kept on an overrun (the DMA then stops, so
   * adc_buf never shifts; hal_adc_scan restarts it), AWD1 on PUMP_ISENSE (IN6) alone. */
  ADC1->CFGR1 = ADC_CFGR1_DMAEN | ADC_CFGR1_DMACFG | ADC_CFGR1_CONT | ADC_CFGR1_AWD1SGL |
              ADC_CFGR1_AWD1EN | ((uint32_t) (6) << ADC_CFGR1_AWD1CH_Pos);
  ADC1->AWD1TR = ((uint32_t) (0xFFFu) << ADC_AWD1TR_HT1_Pos); /* nothing above full scale: quiet until armed */
  ADC1->AWD2TR = ((uint32_t) (0xFFFu) << ADC_AWD2TR_HT2_Pos); /* nothing below 0 */
  ADC1->AWD2CR = ADC_AWD2CR_AWD2CH_6;
  ADC1->IER = 0;
  DMAMUX1_Channel1->CCR = DMAMUX_REQ_ADC;
  /* Priorities: the overcurrent cut first (0, the reset value), then the glow picks, the link. */
  irq_priority(TIM3_IRQn, 0x40u);
  irq_priority(USART2_IRQn, 0x80u);
  NVIC->ISER[0] = NVIC_ISER_SETENA(ADC1_IRQn);
  adc_start();
  adc_wait_scan();
}

void hal_adc_scan(uint16_t raw[HAL_ADC_COUNT]) {
  uint32_t isr = ADC1->ISR;
  /* Stopped: an overrun, a DMA error, or no end of a scan since the last call. */
  if ((isr & ADC_ISR_OVR) || (DMA1->ISR & DMA_ISR_TEIF2) || !(isr & ADC_ISR_EOS) || !(ADC1->CR & ADC_CR_ADSTART)) {
    adc_start();
    adc_wait_scan();
  }
  ADC1->ISR = ADC_ISR_EOS;
  for (int i = 0; i < HAL_ADC_COUNT; i++)
    raw[i] = adc_buf[i];
}

uint16_t hal_vrefint_cal(void) { return VREFINT_CAL; }

/* ---------------------------------------------------------------- glow readings (GLW-6) */

static volatile hal_glow_sample glow_ready;
static volatile uint8_t glow_fresh;
static uint16_t gap_i, gap_vin; /* the last gap pick, for the next period's hand-over */

void TIM3_IRQHandler(void) {
  uint32_t sr = TIM3->SR & (TIM_SR_CC2IF | TIM_SR_CC3IF);
  TIM3->SR = ~sr;
  int pulse_on = pin_get(GPIOB, 4); /* GLOW_PWM */
  if ((sr & TIM_SR_CC3IF) && !pulse_on) { /* the end of the gap */
    gap_i = adc_buf[HAL_ADC_GLOW_I];
    gap_vin = adc_buf[HAL_ADC_VIN];
  }
  if (sr & TIM_SR_CC2IF) { /* inside the pulse */
    glow_ready.pulse_i = pulse_on ? adc_buf[HAL_ADC_GLOW_I] : 0u;
    glow_ready.gap_i = gap_i;
    glow_ready.gap_vin = gap_vin;
    glow_fresh = 1;
  }
}

uint8_t hal_glow_period(hal_glow_sample *g) {
  if (!glow_fresh)
    return 0;
  uint32_t m = irq_off();
  g->pulse_i = glow_ready.pulse_i;
  g->gap_i = glow_ready.gap_i;
  g->gap_vin = glow_ready.gap_vin;
  glow_fresh = 0;
  irq_restore(m);
  return 1;
}

/* ---------------------------------------------------------------- pump (PMP-7, self-tests) */

static uint16_t pump_peak;

void hal_pump_test_pulse_us(uint16_t us) {
  /* One pulse on PUMP_CMD by hand (PB9 as a plain output), PUMP_ISENSE's peak from the scan. */
  uint16_t peak = 0;
  uint32_t ticks = us * (SYSCLK_HZ / 1000000u), reload = SysTick->LOAD + 1u;
  pin_mode(GPIOB, 9, GPIO_MODE_OUTPUT);
  pin_set(GPIOB, 9, 1);
  uint32_t start = SysTick->VAL, now;
  do {
    uint16_t r = adc_buf[HAL_ADC_PUMP_I];
    if (r > peak)
      peak = r;
    now = SysTick->VAL; /* counts down, wraps at the reload */
  } while ((start >= now ? start - now : start + reload - now) < ticks);
  pin_set(GPIOB, 9, 0);
  pin_af(GPIOB, 9, 2u);
  pump_peak = peak;
}

uint16_t hal_pump_peak_raw(void) {
  uint16_t r = pump_peak;
  pump_peak = 0;
  return r;
}

static pump_oc oc;
static volatile uint8_t oc_on;     /* armed by the core */
static volatile uint8_t oc_report; /* a cut the core has not been told of */
static uint8_t pump_forced;        /* PUMP_CMD forced low since a cut */
static uint16_t oc_threshold;

static void oc_thresholds(uint16_t t) {
  ADC1->AWD1TR = ((uint32_t) (t) << ADC_AWD1TR_HT1_Pos);                              /* AWD1: above t */
  ADC1->AWD2TR = ((uint32_t) (0xFFFu) << ADC_AWD2TR_HT2_Pos) | ((uint32_t) (t + 1u) << ADC_AWD2TR_LT2_Pos); /* AWD2: below t + 1, at or below t */
  oc_threshold = t;
}

/* The cut, in the interrupt: PUMP_CMD low and the heartbeat stopped at once (PMP-7), each as
 * hal_pump_duty(0) and hal_heartbeat(HAL_HB_STOPPED) would, without waiting for the main loop. */
static void pump_cut(void) {
  TIM17->CCMR1 = (TIM17->CCMR1 & ~TIM_CCMR1_OC1M_LOW) | TIM_CCMR1_OC1M_2;
  TIM17->CCR1 = 0;
  DMA1_Channel1->CCR = 0;
  TIM1->CCR1 = 0;
  TIM1->CCMR1 = (TIM1->CCMR1 & ~TIM_CCMR1_OC1M_LOW) | TIM_CCMR1_OC1M_2;
  ADC1->IER = 0;
  pump_forced = 1;
  hb_blocked = 1;
  oc_report = 1;
}

void ADC_IRQHandler(void) {
  uint32_t isr = ADC1->ISR & (ADC_ISR_AWD1 | ADC_ISR_AWD2), ier = ADC1->IER;
  ADC1->ISR = isr;
  if (!oc_on) {
    ADC1->IER = 0;
    return;
  }
  int high = (isr & ADC_ISR_AWD1) && (ier & ADC_IER_AWD1IE);
  int low = (isr & ADC_ISR_AWD2) && (ier & ADC_IER_AWD2IE);
  if (pump_oc_event(&oc, high, low)) {
    pump_cut();
    return;
  }
  ADC1->IER = ADC_IER_AWD1IE | (pump_oc_listening(&oc) ? ADC_IER_AWD2IE : 0u);
}

void hal_pump_oc_arm(uint8_t on, uint16_t threshold_raw, uint16_t hold_us) {
  if (threshold_raw > 4094u)
    threshold_raw = 4094u;
  uint32_t m = irq_off();
  if (!on) {
    oc_on = 0;
    ADC1->IER = 0;
  } else if (!oc_on && !oc_report) {
    /* A new stroke: clean flags, the threshold, PUMP_CMD back to its PWM after an earlier cut. */
    oc_thresholds(threshold_raw);
    pump_oc_arm(&oc, pump_oc_need(hold_us, ADC_SCAN_US));
    if (pump_forced) {
      TIM17->CCMR1 = (TIM17->CCMR1 & ~TIM_CCMR1_OC1M_LOW) | (TIM_CCMR1_OC1M_2 | TIM_CCMR1_OC1M_1);
      pump_forced = 0;
    }
    ADC1->ISR = ADC_ISR_AWD1 | ADC_ISR_AWD2;
    ADC1->IER = ADC_IER_AWD1IE;
    oc_on = 1;
  } else if (oc_on && !oc.cut && threshold_raw != oc_threshold) {
    oc_thresholds(threshold_raw); /* VDDA moved: the same current */
  }
  irq_restore(m);
}

uint8_t hal_pump_overcurrent(void) {
  uint32_t m = irq_off();
  uint8_t r = oc_report;
  oc_report = 0;
  hb_blocked = 0; /* the core has it now and holds the heartbeat off itself */
  irq_restore(m);
  return r;
}

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
  uint32_t isr = USART2->ISR;
  if (isr & USART_ISR_ORE)
    USART2->ICR = USART_ICR_ORECF; /* overrun: drop, the parser resynchronises */
  if (isr & USART_ISR_RXNE_RXFNE) {
    uint8_t next = (uint8_t) ((rx_head + 1u) & 127u);
    uint8_t c = (uint8_t) USART2->RDR;
    if (next != rx_tail) {
      rx_buf[rx_head] = c;
      rx_head = next;
    }
  }
  if ((isr & USART_ISR_TXE_TXFNF) && (USART2->CR1 & USART_CR1_TXEIE_TXFNFIE)) {
    if (tx_tail != tx_head) {
      USART2->TDR = tx_buf[tx_tail];
      tx_tail = (uint8_t) ((tx_tail + 1u) & 127u);
    } else {
      USART2->CR1 &= ~USART_CR1_TXEIE_TXFNFIE;
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
  USART2->CR1 |= USART_CR1_TXEIE_TXFNFIE;
}

/* ---------------------------------------------------------------- flash (the last two pages) */

static volatile uint8_t flash_unreadable; /* a bit per saved page */

/* A double ECC error raises the NMI. In a saved page (a write torn by a power loss): note the
 * page, clear ECCD and return; the load completes with corrupt data, which the images' CRCs
 * reject, and the board rewrites the page. Anything else waits for the watchdog, as before.
 * UNVERIFIED on silicon: that the NMI returns into the load, and ADDR_ECC's scale. */
void NMI_Handler(void) {
  uint32_t eccr = FLASH->ECCR;
  int page = hal_ecc_saved_page(eccr);
  if (page < 0)
    for (;;) {
    }
  flash_unreadable |= (uint8_t) (1u << page);
  FLASH->ECCR = (eccr & FLASH_ECCR_ECCCIE) | FLASH_ECCR_ECCD; /* clear ECCD only, keep ECCCIE */
}

uint8_t hal_flash_unreadable(uint8_t page) { return (uint8_t) ((flash_unreadable >> page) & 1u); }

static uint32_t page_addr(uint8_t page) {
  return FLASH_BASE + (HAL_SAVED_PAGE_FIRST + page) * HAL_FLASH_PAGE_SIZE;
}

size_t hal_flash_read(uint8_t page, uint8_t *out, size_t cap) {
  const uint8_t *p = (const uint8_t *) page_addr(page);
  size_t n = cap < HAL_FLASH_IMAGE_MAX ? cap : HAL_FLASH_IMAGE_MAX;
  memcpy(out, p, n);
  return n;
}

/* ST's sequence (RM0454, and HAL's FLASH_WaitForLastOperation): before touching FLASH->CR and after
 * each erase or double word, wait for BSY1, then for CFGBSY (FLASH->CR still taken by the
 * operation) unless an error flag is up, which ST's HAL does not wait past either; the caller
 * reads the errors at the end (flash_lock_result). */
static void flash_wait(void) {
  while (FLASH->SR & FLASH_SR_BSY1) {
  }
  while ((FLASH->SR & (FLASH_SR_CFGBSY | FLASH_SR_ERRORS)) == FLASH_SR_CFGBSY) {
  }
}

/* Unlocks FLASH->CR once the flash is idle, and clears EOP and any old errors. */
static void flash_unlock(void) {
  flash_wait();
  FLASH->KEYR = FLASH_KEY1;
  FLASH->KEYR = FLASH_KEY2;
  FLASH->SR = FLASH_SR_CLEAR;
}

/* n bytes from d to addr (erased, double-word aligned), 64 bits at a time; a last partial double
 * word is padded with 0xFF. */
static void flash_program_dwords(uint32_t addr, const uint8_t *d, size_t n) {
  FLASH->CR = FLASH_CR_PG;
  for (size_t i = 0; i < n; i += 8) {
    uint32_t w[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};
    memcpy(w, d + i, n - i < 8 ? n - i : 8);
    *(volatile uint32_t *) (addr + i) = w[0];
    *(volatile uint32_t *) (addr + i + 4) = w[1];
    flash_wait();
  }
}

/* Locks FLASH->CR again: 0, or -1 if any operation since flash_unlock raised an error. */
static int flash_lock_result(void) {
  FLASH->CR = FLASH_CR_LOCK;
  return (FLASH->SR & FLASH_SR_ERRORS) ? -1 : 0;
}

int hal_flash_write(uint8_t page, const uint8_t *d, size_t n) {
  if (n > HAL_FLASH_IMAGE_MAX)
    return -1;
  flash_unlock();
  FLASH->CR = FLASH_CR_PER | ((uint32_t) (HAL_SAVED_PAGE_FIRST + page) << FLASH_CR_PNB_Pos) | FLASH_CR_STRT; /* erase */
  flash_wait();
  flash_program_dwords(page_addr(page), d, n);
  if (flash_lock_result())
    return -1;
  flash_unreadable &= (uint8_t) ~(1u << page); /* erased and written afresh */
  return 0;
}

const uint8_t *hal_flash_page(uint8_t page) { return (const uint8_t *) page_addr(page); }

/* Double words into an erased part of the page, no erase (the CO sensor's R0 log). */
int hal_flash_program(uint8_t page, uint16_t offset, const uint8_t *d, size_t n) {
  if ((offset & 7u) || (n & 7u) || offset + n > HAL_FLASH_PAGE_SIZE)
    return -1;
  flash_unlock();
  flash_program_dwords(page_addr(page) + offset, d, n);
  return flash_lock_result();
}

/* ---------------------------------------------------------------- start-up */

void hal_init(void) {
  RCC->IOPENR = RCC_IOPENR_GPIOAEN | RCC_IOPENR_GPIOBEN | RCC_IOPENR_GPIOCEN;
  RCC->AHBENR |= RCC_AHBENR_DMA1EN; /* and its DMAMUX */
  RCC->APBENR1 |= RCC_APBENR1_TIM3EN | RCC_APBENR1_USART2EN;
  RCC->APBENR2 |= RCC_APBENR2_SYSCFGEN | RCC_APBENR2_TIM1EN | RCC_APBENR2_TIM16EN | RCC_APBENR2_TIM17EN |
                 RCC_APBENR2_ADCEN;

  /* Outputs default-off before they are driven (CFG-9). */
  pin_set(GPIOB, 0, 0);
  pin_mode(GPIOB, 0, GPIO_MODE_OUTPUT); /* GLOW_EN */
  pin_set(GPIOB, 6, 0);
  pin_mode(GPIOB, 6, GPIO_MODE_OUTPUT); /* LATCH_RESET */
  pin_set(GPIOC, 6, 0);
  pin_mode(GPIOC, 6, GPIO_MODE_OUTPUT); /* CO_HEAT_EN */
  /* Inputs: PC14, PC15 inputs only (CFG-5), pull-ups on PA7 and PC15 (CFG-11). */
  pin_mode(GPIOC, 14, GPIO_MODE_INPUT);
  pin_mode(GPIOC, 15, GPIO_MODE_INPUT);
  pin_pull_up(GPIOC, 15);
  pin_mode(GPIOA, 7, GPIO_MODE_INPUT);
  pin_pull_up(GPIOA, 7);
  GPIOC->LCKR = GPIO_LCKR_LCKK | GPIO_LCKR_LCK_PIN(14); /* OVH-5: lock PC14 as an input */
  GPIOC->LCKR = GPIO_LCKR_LCK_PIN(14);
  GPIOC->LCKR = GPIO_LCKR_LCKK | GPIO_LCKR_LCK_PIN(14);
  (void) GPIOC->LCKR;
  /* Analog inputs stay in their reset (analog) mode. */

  timers_init();
  pin_af(GPIOA, 8, 2u);  /* HEARTBEAT, TIM1_CH1 */
  pin_af(GPIOB, 3, 1u);  /* FAN_PWM, TIM1_CH2 */
  pin_af(GPIOB, 4, 1u);  /* GLOW_PWM, TIM3_CH1 */
  pin_af(GPIOB, 8, 2u);  /* HALL, TIM16_CH1 */
  pin_af(GPIOB, 9, 2u);  /* PUMP_CMD, TIM17_CH1 */

  adc_init();
  adc_schedule_init();
  rtc_init();

  /* USART2 at 115200 8N1 (PA2 TX, PA3 RX). */
  USART2->BRR = SYSCLK_HZ / 115200u;
  USART2->CR1 = USART_CR1_RXNEIE_RXFNEIE | USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
  pin_af(GPIOA, 2, 1u);
  pin_af(GPIOA, 3, 1u);
  NVIC->ISER[0] = NVIC_ISER_SETENA(USART2_IRQn);
}

void hal_ms_tick(void) { ms_count++; }
