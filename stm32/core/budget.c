/* The input-current budget: see budget.h. */
#include "budget.h"

#include <string.h>

void budget_params_default(budget_params *p) {
  memset(p, 0, sizeof(*p));
  p->budget_ma = 11000;
  /* The probes the planner sends at any current, and the pump share's lag of about a second.
   * PROVISIONAL. */
  p->headroom_ma = 200;
  /* The 5 V converter's budget, 5 V x 1.2 A, at 80 % efficiency (the board's figures). PROVISIONAL
   * until measured. */
  p->logic_mw = 7500;
  p->glow_min_w = 10;
}

void budget_init(budget *b, const budget_params *p, uint32_t now) {
  memset(b, 0, sizeof(*b));
  b->p = *p;
  b->last_ms = now;
  b->second_ms = now;
}

/* No supply (USB only) draws no input current. */
#define BUDGET_SUPPLY_MIN_CV 600u

uint16_t budget_logic_ma(const budget *b, uint16_t vin_cv) {
  if (vin_cv < BUDGET_SUPPLY_MIN_CV)
    return 0;
  return (uint16_t) ((uint32_t) b->p.logic_mw * 100u / vin_cv);
}

void budget_pump_ms(budget *b, uint16_t pump_ma) {
  b->pump_acc = b->pump_acc - b->pump_acc / 1024u + pump_ma; /* about a second's average */
  b->charge += pump_ma;
}

void budget_glow_pulse(budget *b, uint16_t i_ca, uint16_t width_us) {
  b->charge += (uint32_t) ((uint64_t) i_ca * width_us / 100u); /* 10 mA x us / 1000 = mA x ms */
}

void budget_pass(budget *b, uint32_t now, uint16_t fan_ma, uint16_t vin_cv) {
  uint32_t dt = since(now, b->last_ms);
  b->last_ms = now;
  b->fan_ma = fan_ma;
  b->vin_cv = vin_cv;
  b->charge += ((uint32_t) fan_ma + budget_logic_ma(b, vin_cv)) * dt;
  if (since(now, b->second_ms) < 1000u)
    return;
  /* A second (or more, after a stall) is over: into the window, spread over the seconds it took. */
  uint32_t seconds = since(now, b->second_ms) / 1000u;
  if (seconds > BUDGET_WINDOW_S)
    seconds = BUDGET_WINDOW_S;
  for (uint32_t k = 0; k < seconds; k++) {
    b->window[b->window_i] = b->charge / seconds;
    b->window_i = (uint8_t) ((b->window_i + 1u) % BUDGET_WINDOW_S);
    if (b->window_n < BUDGET_WINDOW_S)
      b->window_n++;
  }
  b->charge = 0;
  b->second_ms = now;
  uint64_t sum = 0;
  for (unsigned i = 0; i < b->window_n; i++)
    sum += b->window[i];
  uint64_t avg = sum / ((uint64_t) b->window_n * 1000u);
  b->avg_ma = (uint16_t) (avg > 0xFFFFu ? 0xFFFFu : avg);
}

/* What the budget leaves once the fan, pump and logic shares and the headroom are taken, mA. */
static int32_t room_ma(const budget *b, uint16_t vin_cv, int with_fan) {
  return (int32_t) b->p.budget_ma - (int32_t) b->p.headroom_ma - (with_fan ? (int32_t) b->fan_ma : 0) -
         (int32_t) budget_pump_ma(b) - (int32_t) budget_logic_ma(b, vin_cv);
}

uint16_t budget_glow_limit_w(const budget *b, uint16_t vin_cv) {
  int32_t room = room_ma(b, b->vin_cv, 1);
  if (room <= 0)
    return 0;
  uint32_t w = (uint32_t) room * vin_cv / 100000u; /* mA x 0.01 V = 10 uW */
  if (w < b->p.glow_min_w)
    return 0;
  return (uint16_t) (w > 0xFFFFu ? 0xFFFFu : w);
}

uint16_t budget_fan_allowed_ma(const budget *b) {
  int32_t room = room_ma(b, b->vin_cv, 0);
  return (uint16_t) (room > 0 ? room : 0);
}
