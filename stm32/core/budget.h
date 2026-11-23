/* The input-current budget (DESIGN.md, PWR-1, PWR-2): the board's estimated input current, glow +
 * fan + pump + logic, kept at or below 11 A averaged over 10 s. Each stage's share is its average
 * switch current (the stages are switched straight from the supply): the glow's pulses times the
 * current read in each, FAN_ISENSE (the fan's average switch current), PUMP_ISENSE each millisecond,
 * and the logic supply's input power over the supply voltage. The glow is derated first: its power
 * is held to the budget less the fan, pump and logic shares. Only if those alone leave too little
 * does the fan come down too, through the sequencer's power levels so the fuel follows the air.
 * Plain C99, integer maths, no hardware access. Host-tested in ../tests/test_budget.c. */
#ifndef FLAMEKEEPER_BUDGET_H
#define FLAMEKEEPER_BUDGET_H

#include <stdint.h>

#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint16_t budget_ma;   /* PWR-1: INPUT_BUDGET_A, 11 A */
  uint16_t headroom_ma; /* kept back from the glow's limit: its probe pulses, the pump share's lag */
  uint16_t logic_mw;    /* the logic supply's input power: its 5 V budget through the converter */
  uint16_t glow_min_w;  /* a glow limit below this switches the plug off */
} budget_params;

void budget_params_default(budget_params *p);

#define BUDGET_WINDOW_S 10u

typedef struct {
  budget_params p;
  uint32_t last_ms;
  uint16_t fan_ma;      /* FAN_ISENSE at the last pass */
  uint16_t vin_cv;      /* the supply at the last pass */
  uint32_t pump_acc;    /* PUMP_ISENSE averaged over about a second (x 1024) */
  uint32_t charge;      /* mA x ms of every share in the current second */
  uint32_t second_ms;   /* when the current second began */
  uint32_t window[BUDGET_WINDOW_S]; /* the last whole seconds' charge, mA x ms */
  uint8_t window_i, window_n;
  uint16_t avg_ma;      /* the estimate over the last 10 s (fewer just after a reset) */
} budget;

void budget_init(budget *b, const budget_params *p, uint32_t now);

/* Every millisecond: PUMP_ISENSE, mA. */
void budget_pump_ms(budget *b, uint16_t pump_ma);
/* Every glow period: the pulse that went out in the last one, its width and the current read in it. */
void budget_glow_pulse(budget *b, uint16_t i_ca, uint16_t width_us);
/* Every pass of the safety loop: FAN_ISENSE and the supply. */
void budget_pass(budget *b, uint32_t now, uint16_t fan_ma, uint16_t vin_cv);

/* The logic supply's share at this supply voltage, mA (0 with no supply). */
uint16_t budget_logic_ma(const budget *b, uint16_t vin_cv);
/* The pump's share: its current over about the last second, mA. */
static inline uint16_t budget_pump_ma(const budget *b) { return (uint16_t) (b->pump_acc / 1024u); }
/* PWR-2: the most power the glow may have at this supply, W (0: the plug off). */
uint16_t budget_glow_limit_w(const budget *b, uint16_t vin_cv);
/* PWR-2: the fan current the budget leaves with the glow off, mA. */
uint16_t budget_fan_allowed_ma(const budget *b);
/* PWR-1: the estimated input current over the last 10 s, mA. */
static inline uint16_t budget_avg_ma(const budget *b) { return b->avg_ma; }

#ifdef __cplusplus
}
#endif

#endif /* FLAMEKEEPER_BUDGET_H */
