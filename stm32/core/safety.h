/* The safety MCU's side of the link (DESIGN.md): the requests from the ESP32 become burn
 * sequencer calls, under the start-number rule and the dead-man. Plain C99; the hardware layer
 * feeds received bytes and measurements in and sends the replies out. Host-tested in ../tests/. */
#ifndef FLAMEKEEPER_SAFETY_H
#define FLAMEKEEPER_SAFETY_H

#include <stddef.h>
#include <stdint.h>

#include "../../common/co/co_alarm.h"
#include "../../common/link/link.h"
#include "budget.h"
#include "burn.h"
#include "checks.h"
#include "fan.h"
#include "faultlog.h"
#include "glow.h"
#include "monitor.h"
#include "params.h"
#include "plug.h"
#include "pump.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Plain decimal numbers: SAFETY_VERSION_TEXT spells them out. */
#define SAFETY_VERSION_MAJOR 0
#define SAFETY_VERSION_MINOR 1
#define SAFETY_VERSION_PATCH 0
#define SAFETY_STR_(x) #x
#define SAFETY_STR(x) SAFETY_STR_(x)
#define SAFETY_VERSION_TEXT \
  SAFETY_STR(SAFETY_VERSION_MAJOR) "." SAFETY_STR(SAFETY_VERSION_MINOR) "." SAFETY_STR(SAFETY_VERSION_PATCH)

/* What this firmware can do today (link Info capabilities). */
#define SAFETY_CAPABILITIES \
  (LINK_CAP_FAN_ONLY | LINK_CAP_PRIME | LINK_CAP_PRIME_STOP | LINK_CAP_PARAMETERS | LINK_CAP_FAULT_LOG)

/* Supply present (link Status flag): above this the power stages have a supply, 0.01 V. */
#define SAFETY_SUPPLY_PRESENT_CV 600u

typedef struct {
  burn burn;
  glow glow;
  fan fan;
  checks chk;
  pump pump;
  params params;
  plug plug;
  monitor mon;            /* the MCU's own supply and tick (PWR-5, GLW-1) */
  budget budget;          /* the input-current budget (PWR-1, PWR-2) */
  uint16_t glow_limit_w;  /* ... the glow's power limit at the last glow period */
  co_alarm co;            /* the CO alarm, present as the CO-fitted parameter says (CO-1) */
  uint32_t co_r0_saved;   /* the R0 flash holds (0: none) */
  uint32_t co_r0_saved_ms; /* when it was written (or the reset) */
  uint8_t plug_judged;    /* the current start's plug-type verdict is in */
  uint8_t plug_refused;   /* PLUG_REFUSE_* for the sequencer, 0 none */
  uint8_t plug_warning;   /* the last start ran with a plug-type warning (12 V supply) */
  uint16_t hb_running_ms; /* the heartbeat as last asked: running this long (0 = not) */
  uint8_t power_up_started;
  uint8_t precheck_started; /* the pre-start test of the current start has begun */
  uint8_t precheck_result;  /* BURN_PRECHECK_* for the sequencer */
  uint16_t power_up_failed; /* CHECK_FAIL_* from the power-up test */
  uint8_t latch_armed;      /* the overheat latch's last reading (checks_obs.latch_armed) */
  uint8_t latch_was_armed;  /* ... armed since the power-up test passed (OVH-1) */
  link_mcu mcu;
  link_parser parser;
  link_start_gate gate;
  link_deadman deadman;
  uint8_t client_major;
  uint8_t reset_reason;  /* LINK_RESET_* */
  uint8_t checks;        /* LINK_CHECK_*: start-up checks that failed (any: no starts) */
  uint8_t start_result;
  uint8_t no_starts;     /* UPDATE_PREPARE agreed: no starts until the MCU resets */
  uint8_t esp_updating;  /* the ESP32 says it is updating something: no starts */
  burn_inputs in;        /* the latest measurements */
  link_detail detail;    /* the latest slow measurements (the hardware layer fills it) */
  uint32_t now;
  uint32_t run_ms;       /* lifetime burning time not yet counted in run_time_s */
  uint32_t run_time_s;
  faultlog log;          /* the fault log (faultlog.h); the board keeps it in flash */
#ifdef SAFETY_BENCH
  /* The bring-up build only (stm32/bringup/): single bench actions, honoured only while the burn
   * is off and no self-test runs, through the same stage drivers as a run, with every rule they
   * keep there (the heartbeat bursts, the pump's arming and current checks, the glow planner's
   * airflow, probe, caps and budget, the fan's cap, ramp and stall checks). */
  struct {
    uint8_t heartbeat; /* keep the heartbeat running */
    uint16_t pump_cHz; /* strokes through the pump driver (needs the heartbeat) */
    uint8_t glow_probe; /* one probe pulse through the glow planner, once the heartbeat has run
                         * 20 ms; the core clears it as soon as that pulse is out */
    uint16_t fan_rpm;  /* the fan's speed loop */
    uint16_t fan_duty; /* or a fixed fan duty (fan_step_open), permille; this wins */
  } bench;
#endif
} safety;

/* After a reset, with every stage on its defaults (safety_load_params then brings the saved
 * parameters). in: the first measurements; reset_reason and checks come from the start-up code. */
void safety_init(safety *s, uint32_t now, const burn_inputs *in, uint8_t reset_reason, uint8_t checks);

/* One received byte. When it completes a request, the reply is written to reply (LINK_FRAME_MAX
 * bytes) and its length returned; otherwise 0. */
size_t safety_receive(safety *s, uint8_t byte, uint32_t now, uint8_t *reply);

/* One pass of the safety loop with fresh measurements: the dead-man, then the sequencer. The core
 * adds its own findings to in: the stages' faults, the CO alarm, and the overheat latch's trip (a
 * fall of the latch the power-up test re-armed, OVH-1; from safety_ms's readings). */
void safety_step(safety *s, uint32_t now, const burn_inputs *in);

const burn_outputs *safety_outputs(const safety *s);

/* One glow PWM period (5 ms): the planner turns the sequencer's glow request into the next pulse.
 * The hardware layer fills m with that period's readings. */
glow_cmd safety_glow_period(safety *s, uint32_t now, glow_meas *m);

/* One pass of the fan stage: the speed the sequencer wants, as a PWM duty (permille), from that
 * pass's fan readings. Call it every pass, after safety_step. */
uint16_t safety_fan_step(safety *s, uint32_t now, const fan_meas *m);

/* The saved parameter image from flash, once at boot before any start (ignored when invalid). */
void safety_load_params(safety *s, const uint8_t *image, size_t len);
/* 1 when the parameters changed and should be written (params_pack); call safety_params_saved
 * once they are. */
static inline int safety_params_save_pending(const safety *s) { return s->params.save_pending; }
static inline void safety_params_saved(safety *s) { s->params.save_pending = 0; }
/* The plug-type check's record, the same way (plug_pack / plug_unpack). */
static inline int safety_plug_save_pending(const safety *s) { return s->plug.save_pending; }
static inline void safety_plug_saved(safety *s) { s->plug.save_pending = 0; }
void safety_load_plug(safety *s, const uint8_t *image, size_t len);
/* "New glow plug fitted": the plug's remembered type check no longer holds. */
static inline void safety_new_glow_plug(safety *s) { plug_new_plug(&s->plug); }

/* The CO alarm (common/co/). Once a second: the CO_ADC count, taken with CO_HEAT_EN as
 * safety_co_heater says, and the board temperature in 0.1 C (BURN_UNKNOWN_TEMP if not known). An
 * alarm stops the heater with the CO fault, a sensor fault with the CO sensor fault (CO-2); both
 * latch until FAULT_RESET finds the cause gone. */
void safety_co_step(safety *s, uint32_t now, uint16_t adc, int16_t board_c10);
/* What CO_HEAT_EN should be now: on from power-up, off for the heater check and when not fitted. */
static inline uint8_t safety_co_heater(const safety *s) { return co_heater_on(&s->co); }
/* CO-7: the clean-air R0 from flash, once at boot (0: none). */
void safety_load_co_r0(safety *s, uint32_t r0_ohm);
/* The R0 worth writing to flash now, or 0: once learnt in clean, trusted air, then after a change
 * of at least 1/16, at most once an hour (flash wear). Call safety_co_r0_saved once written. */
uint32_t safety_co_r0_to_save(const safety *s);
void safety_co_r0_saved(safety *s, uint32_t r0_ohm);
#define SAFETY_CO_R0_SAVE_MIN_MS (60u * 60u * 1000u)

/* The MCU's checks of itself (monitor.h). PWR-5: VDDA every millisecond, mV, not clamped
 * (analog_vdda_check_mv), before that millisecond's safety_ms; out of 3.0-3.6 V it holds every
 * output off at once and latches a fault the sequencer acts on in its next pass. */
void safety_vdda(safety *s, uint32_t now, uint16_t vdda_mv);
/* GLW-1: the second clock's count, every pass before safety_step: it runs at SAFETY_CLOCK_HZ
 * (nominal) and wraps at SAFETY_CLOCK_WRAP, on an oscillator of its own. */
void safety_clock_check(safety *s, uint32_t now, uint32_t count);
/* 1 while PWR-5 holds every output off: the board drops GLOW_EN that same millisecond too. */
static inline int safety_held(const safety *s) { return monitor_holding(&s->mon); }
#define SAFETY_CLOCK_HZ 250u
#define SAFETY_CLOCK_WRAP 32768u
/* The fault log (faultlog.h). Once at boot, after the board has loaded its slots into s->log: the
 * reset's own entry. */
void safety_log_reset(safety *s);

/* What the millisecond step asks of the hardware. */
typedef struct {
  uint8_t heartbeat;      /* CHECKS_HB_* */
  uint8_t glow_en;        /* a self-test's glow enable (GLOW_EN = this OR the glow planner's) */
  uint8_t latch_reset;    /* LATCH_RESET level */
  uint16_t pump_pulse_us; /* a self-test's single pump pulse */
  uint16_t pump_duty;     /* PUMP_CMD duty for this millisecond, permille (0 = low) */
  uint8_t pump_oc_armed;  /* the pump's overcurrent watchdog guards this millisecond (PMP-7) */
} safety_io;

/* Every millisecond: the self-tests while one runs (the power-up test by itself after a reset,
 * the pre-start test when the sequencer asks), else the pump strokes and the heartbeat the burn
 * needs. o: the self-tests' readings (latch_armed every time, the rest while a test may run); pm:
 * the pump's (its hb_running_ms is filled in here). An overcurrent cut in pm latches the pump's
 * fault at once, test or not (PMP-7). */
safety_io safety_ms(safety *s, uint32_t now, const checks_obs *o, pump_meas *pm);
/* A self-test runs or may start at the next safety_ms: it reads the whole of checks_obs then. */
static inline int safety_checks_may_run(const safety *s) {
  return !s->power_up_started || checks_busy(&s->chk) || s->burn.step == BURN_STEP_PRE_CHECK;
}

#ifdef SAFETY_BENCH
/* The bring-up console's tune: a burn_params figure outside the parameter table (d describes it),
 * in RAM. It goes into the applied values (params.live) and to the sequencer as an apply does, so
 * a later apply or "defaults" (the table's values only) keeps it; the flash image never holds it,
 * and a reset forgets it. LINK_NOT_OFF unless the heater is off; d's range is the caller's. */
int safety_bench_tune(safety *s, const param_def *d, int32_t value);

/* A bench glow probe waits for the heartbeat to have run this long (GLW-7: the gate supply). */
#define SAFETY_BENCH_GLOW_HB_MS 20u
#endif

/* A heat start waits while more than this share of the glow plug's heat budget is used (a start
 * straight after a long glow; the budget drains in 240 s). PROVISIONAL. */
#define SAFETY_GLOW_START_FREE_PERCENT 50u

#ifdef __cplusplus
}
#endif

#endif /* FLAMEKEEPER_SAFETY_H */
