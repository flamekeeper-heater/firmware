#include "co_alarm.h"

#include <string.h>

/* The R0 decay: towards the current reading by 1/64 every 10 minutes, so one high reading (a
 * hand-fitted part still burning in) does not hold R0 up for ever. */
#define CO_R0_AGE_MS (10u * 60u * 1000u)

void co_default_config(co_config *cfg) {
  memset(cfg, 0, sizeof(*cfg));
  cfg->fitted = 1;
  cfg->warmup_ms = 3u * 60u * 1000u;
  cfg->learn_ms = 10u * 60u * 1000u;
  cfg->alarm_ratio = 30;
  cfg->alarm_ms = 60u * 1000u;
  cfg->alarm_fast_ratio = 10;
  cfg->alarm_fast_ms = 10u * 1000u;
  cfg->clear_ratio = 300;
  cfg->clear_ms = 60u * 1000u;
  cfg->open_ms = 5u * 1000u;
  cfg->check_off_ms = 5u * 1000u;
  cfg->check_drop_pct = 30;
  cfg->cold_c10 = -300;
}

void co_init(co_alarm *c, const co_config *cfg) {
  memset(c, 0, sizeof(*c));
  c->cfg = *cfg;
  c->state = cfg->fitted ? CO_WARMING_UP : CO_NOT_FITTED;
  c->heater_on = cfg->fitted ? 1 : 0;
  c->ratio = 0xFFFFu;
  c->ppm = CO_UNKNOWN_PPM;
}

void co_load_r0(co_alarm *c, uint32_t r0_ohm) {
  c->r0 = r0_ohm;
  if (r0_ohm != 0)
    c->learned_ms = c->cfg.learn_ms;
}

uint8_t co_heater_on(const co_alarm *c) { return c->heater_on; }

uint32_t co_rs_ohm(uint16_t adc) {
  if (adc == 0)
    return UINT32_MAX;
  if (adc >= CO_ADC_FULL)
    return 0;
  /* ADC / FULL = RL / (RL + Rs)  =>  Rs = RL (FULL - ADC) / ADC */
  return (uint32_t) (((uint64_t) CO_LOAD_OHM * (CO_ADC_FULL - adc)) / adc);
}

/* Rs / R0 against ppm from the rev A2 notes' line (0.03 at 300 ppm, 0.01 at 1000 ppm), extended:
 * log-log, so interpolating linearly between close points is close enough for a display. */
static const uint16_t CURVE_RATIO[] = {5, 10, 20, 30, 50, 100, 200, 300};
static const uint16_t CURVE_PPM[] = {2140, 1000, 468, 300, 171, 80, 38, 24};

uint16_t co_ppm_from_ratio(uint16_t ratio) {
  const unsigned n = (unsigned) (sizeof(CURVE_RATIO) / sizeof(CURVE_RATIO[0]));
  unsigned i;
  if (ratio > CURVE_RATIO[n - 1])
    return CO_UNKNOWN_PPM; /* cleaner than the curve says anything about */
  if (ratio <= CURVE_RATIO[0])
    return CURVE_PPM[0];
  for (i = 1; i < n; i++) {
    if (ratio <= CURVE_RATIO[i]) {
      uint32_t span = (uint32_t) (CURVE_RATIO[i] - CURVE_RATIO[i - 1]);
      uint32_t into = (uint32_t) (ratio - CURVE_RATIO[i - 1]);
      uint32_t hi = CURVE_PPM[i - 1], lo = CURVE_PPM[i];
      return (uint16_t) (hi - (hi - lo) * into / span);
    }
  }
  return CO_UNKNOWN_PPM;
}

/* Tracks "cond held since": returns 1 once it has held for `ms`. */
static int held(uint8_t *flag, uint32_t *since, int cond, uint32_t now, uint32_t ms) {
  if (!cond) {
    *flag = 0;
    return 0;
  }
  if (!*flag) {
    *flag = 1;
    *since = now;
  }
  return (uint32_t) (now - *since) >= ms;
}

void co_step(co_alarm *c, uint32_t now, uint16_t adc, int16_t board_c10) {
  const co_config *k = &c->cfg;
  uint32_t rs, dt;
  if (!k->fitted) {
    c->state = CO_NOT_FITTED;
    c->heater_on = 0;
    return;
  }
  if (!c->started) {
    c->started = 1;
    c->start_ms = now;
    c->last_ms = now;
    c->r0_aged_ms = now;
  }
  dt = (uint32_t) (now - c->last_ms);
  c->last_ms = now;
  if (adc > CO_ADC_FULL)
    adc = CO_ADC_FULL;

  /* Warm-up: heated, readings ignored. */
  if ((uint32_t) (now - c->start_ms) < k->warmup_ms) {
    c->heater_on = 1;
    c->state = c->fault ? CO_SENSOR_FAULT : (c->alarm ? CO_ALARM : CO_WARMING_UP);
    return;
  }

  /* The heater check, once per warm-up: off for a few seconds, the reading must fall. */
  if (!c->checked && !c->fault) {
    if (!c->checking) {
      c->checking = 1;
      c->check_start_ms = now;
      c->check_adc_before = adc;
      c->heater_on = 0;
      return;
    }
    if ((uint32_t) (now - c->check_start_ms) < k->check_off_ms)
      return;
    c->checking = 0;
    c->heater_on = 1;
    if ((uint32_t) adc * 100u > (uint32_t) c->check_adc_before * (uint32_t) (100u - k->check_drop_pct)) {
      c->fault = 1; /* no change: a broken heater, or a stuck input; a reset checks again */
      c->state = CO_SENSOR_FAULT;
    } else {
      c->checked = 1;
    }
    return; /* this reading was taken cooling: skip it */
  }
  c->heater_on = 1;

  /* Plausibility: an open reading for open_ms is a sensor fault, latched. */
  if (held(&c->open, &c->open_since_ms, adc < CO_OPEN_ADC, now, k->open_ms))
    c->fault = 1;
  if (adc < CO_OPEN_ADC) {
    c->ratio = 0xFFFFu;
    c->ppm = CO_UNKNOWN_PPM;
    c->state = c->fault ? CO_SENSOR_FAULT : (c->alarm ? CO_ALARM : c->state);
    return;
  }

  rs = co_rs_ohm(adc);
  /* R0: the clean-air (highest) resistance, slowly aged towards the reading. */
  if (rs > c->r0) {
    c->r0 = rs;
    c->r0_aged_ms = now;
  } else if ((uint32_t) (now - c->r0_aged_ms) >= CO_R0_AGE_MS) {
    c->r0 -= (c->r0 - rs) / 64u;
    c->r0_aged_ms = now;
  }
  if (c->learned_ms < k->learn_ms)
    c->learned_ms += dt;

  if (c->r0 == 0) {
    c->ratio = 0xFFFFu;
  } else {
    uint64_t q = (uint64_t) rs * 1000u / c->r0;
    c->ratio = (uint16_t) (q > 0xFFFEu ? 0xFFFEu : q);
  }
  c->ppm = co_ppm_from_ratio(c->ratio);

  /* The alarm, latched. A reading at the rail reads Rs about 0: the alarm (safe side). */
  if (held(&c->low, &c->low_since_ms, c->ratio <= k->alarm_ratio, now, k->alarm_ms) ||
      held(&c->fast, &c->fast_since_ms, c->ratio <= k->alarm_fast_ratio, now, k->alarm_fast_ms))
    c->alarm = 1;
  (void) held(&c->clean, &c->clean_since_ms, c->ratio >= k->clear_ratio, now, k->clear_ms);

  if (c->fault)
    c->state = CO_SENSOR_FAULT;
  else if (c->alarm)
    c->state = CO_ALARM;
  else if (board_c10 != CO_UNKNOWN_TEMP && board_c10 < k->cold_c10)
    c->state = CO_TOO_COLD; /* still judged (the safe side) */
  else if (c->learned_ms < k->learn_ms)
    c->state = CO_LEARNING;
  else
    c->state = CO_OK;
}

int co_reset(co_alarm *c, uint32_t now) {
  if (c->alarm && c->clean && (uint32_t) (now - c->clean_since_ms) >= c->cfg.clear_ms) {
    c->alarm = 0;
    c->low = c->fast = 0;
  }
  if (c->fault && !c->open) {
    c->fault = 0; /* a failed heater check runs again from the next reading */
    c->checking = 0;
  }
  if (!c->alarm && !c->fault && c->state != CO_NOT_FITTED && c->state != CO_WARMING_UP)
    c->state = CO_OK;
  return !c->alarm && !c->fault;
}
