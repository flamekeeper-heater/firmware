/* hal.h on the STM32G030K8 of the rev A2 board: pins from the board's pin map, timers per CFG-13,
 * the heartbeat burst per PMP-1, the watchdog per WDG-2/3/16. Bare registers (stm32g030.h), no
 * vendor HAL.
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
  const uint32_t runs = FLASH_OPTR_IWDG_STOP | FLASH_OPTR_IWDG_STDBY; /* both 1 (their default) */
  int iwdg_frozen = (optr & runs) != runs;
  return (uint8_t) ((iwdg_sw || iwdg_frozen || nboot_sel || optverr) ? LINK_CHECK_OPTION_BYTES : 0u);
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

/* Interrupts off around what an interrupt also writes; restores the previous state, so it is safe
 * before main enables them. */
static uint32_t irq_off(void) {
  uint32_t m;
  __asm__ volatile("mrs %0, primask\n cpsid i" : "=r"(m) : : "memory");
  return m;
}
static void irq_restore(uint32_t m) { __asm__ volatile("msr primask, %0" : : "r"(m) : "memory"); }

/* ---------------------------------------------------------------- timers */

static volatile uint16_t burst[26];
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
  /* TIM3, 200 Hz at 1 MHz: CH1 the glow pulse; CH2 and CH3 (no pins) pick the pulse and gap
   * readings from the ADC's scan (GLW-6, the ADC schedule below). */
  TIM_PSC(TIM3) = SYSCLK_HZ / 1000000u - 1u;
  TIM_ARR(TIM3) = GLOW_PERIOD_US - 1u;
  TIM_CCMR1(TIM3) = (6u << 4) | (1u << 3);
  TIM_CCR1(TIM3) = 0;
  TIM_CCR2(TIM3) = GLOW_PULSE_PICK_US;
  TIM_CCR3(TIM3) = GLOW_GAP_PICK_US;
  TIM_CCER(TIM3) = 1u << 0;
  TIM_DIER(TIM3) = TIM_DIER_CC2IE | TIM_DIER_CC3IE;
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
  /* TIM1's CCMR1 and the DMA channel are also written by the pump's overcurrent cut. */
  uint32_t m = irq_off();
  if (hb_blocked && mode != HAL_HB_STATIC_LOW)
    mode = HAL_HB_STOPPED;
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
    irq_restore(m);
    return;
  }
  DMA_CCR1 = 0;
  TIM_CCR1(TIM1) = 0;
  /* Static levels for the self-tests (PMP-11): forced active / inactive. */
  TIM_CCMR1(TIM1) = ccmr | ((mode == HAL_HB_STATIC_HIGH ? 5u : 4u) << 4);
  irq_restore(m);
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

/* Waits for the end of one scan, a few times its length at most. */
static void adc_wait_scan(void) {
  for (volatile int i = 0; i < 400 && !(ADC_ISR & ADC_ISR_EOS); i++) {
  }
}

/* (Re)starts the scan from its first channel, adc_buf[0] onwards. */
static void adc_start(void) {
  if (ADC_CR & ADC_CR_ADSTART) {
    ADC_CR = ADC_CR_ADVREGEN | ADC_CR_ADSTP;
    while (ADC_CR & ADC_CR_ADSTART) {
    }
  }
  DMA_CCR2 = 0;
  DMA_IFCR = DMA_IFCR_CGIF2 | DMA_IFCR_CTCIF2 | DMA_IFCR_CHTIF2 | DMA_IFCR_CTEIF2;
  DMA_CPAR2 = (uint32_t) &ADC_DR;
  DMA_CMAR2 = (uint32_t) adc_buf;
  DMA_CNDTR2 = HAL_ADC_COUNT;
  /* peripheral to memory, circular, memory increment, 16 bit both sides */
  DMA_CCR2 = DMA_CCR_CIRC | DMA_CCR_MINC | DMA_CCR_PSIZE_16 | DMA_CCR_MSIZE_16 | DMA_CCR_EN;
  ADC_ISR = ADC_ISR_OVR | ADC_ISR_EOS;
  ADC_CR = ADC_CR_ADVREGEN | ADC_CR_ADSTART;
}

static void irq_priority(unsigned irq, uint32_t level) {
  NVIC_IPR(irq / 4u) = (NVIC_IPR(irq / 4u) & ~(0xFFu << (8u * (irq % 4u)))) | (level << (8u * (irq % 4u)));
}

static void adc_schedule_init(void) {
  /* Ascending channel numbers: IN0, 1, 4, 5, 6, 9, 10, 11, 13 (VREFINT), 15: the HAL_ADC_ order. */
  static const uint8_t ch[HAL_ADC_COUNT] = {0, 1, 4, 5, 6, 9, 10, 11, 13, 15};
  uint32_t mask = 0;
  for (int i = 0; i < HAL_ADC_COUNT; i++)
    mask |= 1u << ch[i];
  ADC_CHSELR = mask;
  while (!(ADC_ISR & ADC_ISR_CCRDY)) {
  }
  ADC_ISR = ADC_ISR_CCRDY;
  /* DMA in circular mode, continuous, the data kept on an overrun (the DMA then stops, so
   * adc_buf never shifts; hal_adc_scan restarts it), AWD1 on PUMP_ISENSE (IN6) alone. */
  ADC_CFGR1 = ADC_CFGR1_DMAEN | ADC_CFGR1_DMACFG | ADC_CFGR1_CONT | ADC_CFGR1_AWD1SGL |
              ADC_CFGR1_AWD1EN | ADC_CFGR1_AWD1CH(6);
  ADC_AWD1TR = ADC_AWD1TR_HT1(0xFFFu); /* nothing above full scale: quiet until armed */
  ADC_AWD2TR = ADC_AWD2TR_HT2(0xFFFu); /* nothing below 0 */
  ADC_AWD2CR = ADC_AWD2CR_AWD2CH6;
  ADC_IER = 0;
  DMAMUX_C1CR = DMAMUX_REQ_ADC;
  /* Priorities: the overcurrent cut first (0, the reset value), then the glow picks, the link. */
  irq_priority(IRQ_TIM3, 0x40u);
  irq_priority(IRQ_USART2, 0x80u);
  NVIC_ISER = 1u << IRQ_ADC;
  adc_start();
  adc_wait_scan();
}

void hal_adc_scan(uint16_t raw[HAL_ADC_COUNT]) {
  uint32_t isr = ADC_ISR;
  /* Stopped: an overrun, a DMA error, or no end of a scan since the last call. */
  if ((isr & ADC_ISR_OVR) || (DMA_ISR & DMA_ISR_TEIF2) || !(isr & ADC_ISR_EOS) || !(ADC_CR & ADC_CR_ADSTART)) {
    adc_start();
    adc_wait_scan();
  }
  ADC_ISR = ADC_ISR_EOS;
  for (int i = 0; i < HAL_ADC_COUNT; i++)
    raw[i] = adc_buf[i];
}

uint16_t hal_vrefint_cal(void) { return VREFINT_CAL; }

/* ---------------------------------------------------------------- glow readings (GLW-6) */

static volatile hal_glow_sample glow_ready;
static volatile uint8_t glow_fresh;
static uint16_t gap_i, gap_vin; /* the last gap pick, for the next period's hand-over */

void TIM3_IRQHandler(void) {
  uint32_t sr = TIM_SR(TIM3) & (TIM_SR_CC2IF | TIM_SR_CC3IF);
  TIM_SR(TIM3) = ~sr;
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
  uint32_t ticks = us * (SYSCLK_HZ / 1000000u), reload = SYST_RVR + 1u;
  pin_mode(GPIOB, 9, 1u);
  pin_set(GPIOB, 9, 1);
  uint32_t start = SYST_CVR, now;
  do {
    uint16_t r = adc_buf[HAL_ADC_PUMP_I];
    if (r > peak)
      peak = r;
    now = SYST_CVR; /* counts down, wraps at the reload */
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
  ADC_AWD1TR = ADC_AWD1TR_HT1(t);                              /* AWD1: above t */
  ADC_AWD2TR = ADC_AWD2TR_HT2(0xFFFu) | ADC_AWD2TR_LT2(t + 1u); /* AWD2: below t + 1, at or below t */
  oc_threshold = t;
}

/* The cut, in the interrupt: PUMP_CMD low and the heartbeat stopped at once (PMP-7), each as
 * hal_pump_duty(0) and hal_heartbeat(HAL_HB_STOPPED) would, without waiting for the main loop. */
static void pump_cut(void) {
  TIM_CCMR1(TIM17) = (TIM_CCMR1(TIM17) & ~(7u << 4)) | (4u << 4); /* OC1REF forced low */
  TIM_CCR1(TIM17) = 0;
  DMA_CCR1 = 0;
  TIM_CCR1(TIM1) = 0;
  TIM_CCMR1(TIM1) = (TIM_CCMR1(TIM1) & ~(7u << 4)) | (4u << 4);
  ADC_IER = 0;
  pump_forced = 1;
  hb_blocked = 1;
  oc_report = 1;
}

void ADC_IRQHandler(void) {
  uint32_t isr = ADC_ISR & (ADC_ISR_AWD1 | ADC_ISR_AWD2), ier = ADC_IER;
  ADC_ISR = isr;
  if (!oc_on) {
    ADC_IER = 0;
    return;
  }
  int high = (isr & ADC_ISR_AWD1) && (ier & ADC_IER_AWD1IE);
  int low = (isr & ADC_ISR_AWD2) && (ier & ADC_IER_AWD2IE);
  if (pump_oc_event(&oc, high, low)) {
    pump_cut();
    return;
  }
  ADC_IER = ADC_IER_AWD1IE | (pump_oc_listening(&oc) ? ADC_IER_AWD2IE : 0u);
}

void hal_pump_oc_arm(uint8_t on, uint16_t threshold_raw, uint16_t hold_us) {
  if (threshold_raw > 4094u)
    threshold_raw = 4094u;
  uint32_t m = irq_off();
  if (!on) {
    oc_on = 0;
    ADC_IER = 0;
  } else if (!oc_on && !oc_report) {
    /* A new stroke: clean flags, the threshold, PUMP_CMD back to its PWM after an earlier cut. */
    oc_thresholds(threshold_raw);
    pump_oc_arm(&oc, pump_oc_need(hold_us, ADC_SCAN_US));
    if (pump_forced) {
      TIM_CCMR1(TIM17) = (TIM_CCMR1(TIM17) & ~(7u << 4)) | (6u << 4);
      pump_forced = 0;
    }
    ADC_ISR = ADC_ISR_AWD1 | ADC_ISR_AWD2;
    ADC_IER = ADC_IER_AWD1IE;
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

static volatile uint8_t flash_unreadable; /* a bit per saved page */

/* A double ECC error raises the NMI. In a saved page (a write torn by a power loss): note the
 * page, clear ECCD and return; the load completes with corrupt data, which the images' CRCs
 * reject, and the board rewrites the page. Anything else waits for the watchdog, as before.
 * UNVERIFIED on silicon: that the NMI returns into the load, and ADDR_ECC's scale. */
void NMI_Handler(void) {
  uint32_t eccr = FLASH_ECCR;
  int page = hal_ecc_saved_page(eccr);
  if (page < 0)
    for (;;) {
    }
  flash_unreadable |= (uint8_t) (1u << page);
  FLASH_ECCR = (eccr & FLASH_ECCR_ECCCIE) | FLASH_ECCR_ECCD; /* clear ECCD only, keep ECCCIE */
}

uint8_t hal_flash_unreadable(uint8_t page) { return (uint8_t) ((flash_unreadable >> page) & 1u); }

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
  FLASH_SR = 0x83FBu; /* clear old errors and EOP */
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
  if (FLASH_SR & 0x83FAu) /* OPERR..FASTERR, OPTVERR */
    return -1;
  flash_unreadable &= (uint8_t) ~(1u << page); /* erased and written afresh */
  return 0;
}

const uint8_t *hal_flash_page(uint8_t page) { return (const uint8_t *) page_addr(page); }

/* Double words into an erased part of the page, no erase (the CO sensor's R0 log). */
int hal_flash_program(uint8_t page, uint16_t offset, const uint8_t *d, size_t n) {
  if ((offset & 7u) || (n & 7u) || offset + n > 2048u)
    return -1;
  while (FLASH_SR & (1u << 16)) {
  }
  FLASH_KEYR = 0x45670123u;
  FLASH_KEYR = 0xCDEF89ABu;
  FLASH_SR = 0x83FBu; /* clear EOP and the error flags */
  FLASH_CR = 1u; /* program */
  uint32_t a = page_addr(page) + offset;
  for (size_t i = 0; i < n; i += 8) {
    uint32_t w[2];
    memcpy(w, d + i, 8);
    REG(a + i) = w[0];
    REG(a + i + 4) = w[1];
    while (FLASH_SR & (1u << 16)) {
    }
  }
  FLASH_CR = 1u << 31; /* lock */
  return (FLASH_SR & 0x83FAu) ? -1 : 0; /* OPERR..FASTERR, OPTVERR */
}

/* ---------------------------------------------------------------- start-up */

void hal_init(void) {
  RCC_IOPENR = 7u;                                      /* GPIOA, B, C */
  RCC_AHBENR |= 1u;                                     /* DMA1 (and its DMAMUX) */
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
  adc_schedule_init();

  /* USART2 at 115200 8N1 (PA2 TX, PA3 RX). */
  USART_BRR = SYSCLK_HZ / 115200u;
  USART_CR1 = (1u << 5) | (1u << 3) | (1u << 2) | 1u;
  pin_af(GPIOA, 2, 1u);
  pin_af(GPIOA, 3, 1u);
  NVIC_ISER = 1u << IRQ_USART2;
}

void hal_ms_tick(void) { ms_count++; }
