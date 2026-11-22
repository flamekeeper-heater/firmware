/* The safety MCU's self-tests of its own interlocks (DESIGN.md): a power-up test once after every
 * reset, and a pre-start test before every start that may stroke the pump or heat the plug. Each is
 * a short sequence stepped once a millisecond by the hardware layer, which does what checks_out
 * asks (heartbeat mode, glow enable, one short pump pulse, the latch reset line) and reports what it
 * measured in checks_obs. Plain C99, no hardware access. Host-tested in ../tests/test_checks.c
 * against a model of the board's interlocks.
 *
 * Power-up (PMP-11 to PMP-13, OVH-2):
 *   1. Latch still tripped from power-up, heartbeat running: a pump pulse draws no current.
 *   2. Re-arm the overheat latch (a plausible, cool sensor; 500 ms after power-up) and confirm it.
 *   3. Heartbeat held static high, then static low: a pump pulse draws no current.
 *   4. Heartbeat running, latch armed: the pump's high side reports no fault and a pulse draws
 *      current.
 * Pre-start (PMP-14, GLW-22, GLW-8):
 *   1. Heartbeat stopped 50 ms: the pump's and the plug's + sides are off.
 *   2. Glow enable high with the heartbeat stopped: the plug's + side stays off.
 *   3. Heartbeat running: the pump's high side is up and reports no fault.
 *   4. Glow enable high, no pulses: the plug's + side reads the supply and no current flows.
 *
 * Thresholds are PROVISIONAL until measured on the first boards. */
#ifndef FLAMEKEEPER_CHECKS_H
#define FLAMEKEEPER_CHECKS_H

#include <stdint.h>

#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { CHECKS_HB_STOPPED = 0, CHECKS_HB_STATIC_HIGH = 1, CHECKS_HB_STATIC_LOW = 2, CHECKS_HB_RUNNING = 3 };

/* What failed (a bit each). */
#define CHECK_FAIL_LATCH_HOLD (1u << 0)       /* PMP-12: pump current with the latch tripped */
#define CHECK_FAIL_LATCH_REARM (1u << 1)      /* OVH-2: re-arm refused (sensor) or not confirmed */
#define CHECK_FAIL_STATIC_HEARTBEAT (1u << 2) /* PMP-11: pump current with a static heartbeat */
#define CHECK_FAIL_PUMP_HS_FAULT (1u << 3)    /* PMP-13 / PMP-2: the pump's high side reports a fault */
#define CHECK_FAIL_PUMP_NO_CURRENT (1u << 4)  /* PMP-13: no current rise (open coil, dead stage) */
#define CHECK_FAIL_PUMP_HS_SHORT (1u << 5)    /* PMP-14: pump + side on without the heartbeat */
#define CHECK_FAIL_PUMP_HS_NO_OUTPUT (1u << 6) /* PMP-14: pump + side off with the heartbeat */
#define CHECK_FAIL_GLOW_HS_SHORT (1u << 7)    /* GLW-8: plug + side on with the enable low */
#define CHECK_FAIL_GLOW_HB_HOLD (1u << 8)     /* GLW-22: plug + side on without the heartbeat */
#define CHECK_FAIL_GLOW_HS_NO_OUTPUT (1u << 9) /* GLW-8: plug + side off with enable and heartbeat */
#define CHECK_FAIL_GLOW_LS_SHORT (1u << 10)   /* GLW-8: plug current with no pulses */
#define CHECK_FAIL_TIMEOUT (1u << 11)
#define CHECK_FAILS_PUMP                                                                            \
  (CHECK_FAIL_LATCH_HOLD | CHECK_FAIL_STATIC_HEARTBEAT | CHECK_FAIL_PUMP_HS_FAULT |                \
   CHECK_FAIL_PUMP_NO_CURRENT | CHECK_FAIL_PUMP_HS_SHORT | CHECK_FAIL_PUMP_HS_NO_OUTPUT)
#define CHECK_FAILS_GLOW                                                                            \
  (CHECK_FAIL_GLOW_HS_SHORT | CHECK_FAIL_GLOW_HB_HOLD | CHECK_FAIL_GLOW_HS_NO_OUTPUT |            \
   CHECK_FAIL_GLOW_LS_SHORT)

typedef struct {
  uint16_t settle_ms;          /* a static heartbeat level is held this long before the pulse */
  uint16_t hs_arm_ms;          /* heartbeat running this long before the high sides count as up */
  uint16_t stopped_ms;         /* heartbeat stopped this long before the "off" readings */
  uint16_t pulse_us;           /* the test pump pulse (100-200 us) */
  uint16_t no_current_ma;      /* a pulse that should be blocked draws less than this */
  uint16_t rise_ma;            /* a pulse that should pass draws at least this */
  uint8_t pump_off_percent;    /* pump + side below this share of the supply: off */
  uint8_t pump_on_percent;     /* ... within this many percent of the supply: on */
  uint8_t glow_off_percent;    /* plug + side below this share of the supply: off (GLW-22: 20) */
  uint8_t glow_on_percent;     /* plug + side above this share: on */
  uint16_t glow_ls_ca;         /* plug current with no pulses above this: the low side is stuck */
  uint16_t glow_test_ms;       /* GLW-22 and GLW-8: glow enable held this long */
  uint16_t rearm_after_ms;     /* the latch is re-armed no sooner after power-up (OVH-2: 500) */
  uint16_t latch_pulse_ms;
  int16_t rearm_max_c10;       /* re-arm only below this heat-exchanger reading */
  uint16_t timeout_ms;
} checks_params;

void checks_params_default(checks_params *p);

typedef struct {
  uint16_t vin_cv;
  uint16_t pump_v_cv;      /* the pump's + wire (PUMP_VSENSE x 19), 0.01 V */
  uint16_t glow_v_cv;      /* the plug's + side (GLOW_VSENSE x 19), 0.01 V */
  uint16_t pump_peak_ma;   /* the highest PUMP_ISENSE during the pulse asked for last step */
  uint16_t glow_ca;        /* GLOW_ISENSE now, 0.01 A */
  uint8_t latch_armed;     /* OVERHEAT_N high */
  uint8_t pump_hs_ok;      /* PUMP_HS_FLT_N high */
  int16_t hx_c10;          /* heat-exchanger sensor, UNKNOWN_TEMP when open */
} checks_obs;

typedef struct {
  uint8_t heartbeat;      /* CHECKS_HB_* */
  uint8_t glow_en;
  uint16_t pump_pulse_us; /* one PUMP_CMD pulse this step (0 none) */
  uint8_t latch_reset;    /* LATCH_RESET level */
} checks_out;

enum { CHECKS_IDLE = 0, CHECKS_POWER_UP = 1, CHECKS_PRE_START = 2 };

typedef struct {
  checks_params p;
  uint8_t kind;          /* CHECKS_IDLE when no test runs */
  uint8_t stage;
  uint32_t begin_ms, stage_ms;
  uint32_t boot_ms;      /* power-up, for the latch's re-arm delay */
  uint16_t failed;       /* CHECK_FAIL_* of the last test */
  uint8_t pulse_pending; /* a pulse went out last step: its peak comes in now */
  uint8_t latch_skipped; /* power-up: the latch was already armed, so step 1 could not run */
  uint8_t power_up_done;
} checks;

void checks_init(checks *c, const checks_params *p, uint32_t boot_ms);
void checks_begin_power_up(checks *c, uint32_t now_ms, const checks_obs *o);
void checks_begin_pre_start(checks *c, uint32_t now_ms);
/* Every millisecond while a test runs (idle: all off). */
checks_out checks_step(checks *c, uint32_t now_ms, const checks_obs *o);
static inline int checks_busy(const checks *c) { return c->kind != CHECKS_IDLE; }

#ifdef __cplusplus
}
#endif

#endif /* FLAMEKEEPER_CHECKS_H */
