/* The safety MCU's side of the link (DESIGN.md): the requests from the ESP32 become burn
 * sequencer calls, under the start-number rule and the dead-man. Plain C99; the hardware layer
 * feeds received bytes and measurements in and sends the replies out. Host-tested in ../tests/. */
#ifndef FLAMEKEEPER_SAFETY_H
#define FLAMEKEEPER_SAFETY_H

#include <stddef.h>
#include <stdint.h>

#include "../../common/link/link.h"
#include "burn.h"
#include "checks.h"
#include "fan.h"
#include "glow.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SAFETY_VERSION_MAJOR 0
#define SAFETY_VERSION_MINOR 1
#define SAFETY_VERSION_PATCH 0

/* What this firmware can do today (link Info capabilities). */
#define SAFETY_CAPABILITIES                                                                     \
  (LINK_CAP_FAN_ONLY | LINK_CAP_PRIME | LINK_CAP_PRIME_STOP | LINK_CAP_SUPPLY_PRESENT |        \
   LINK_CAP_OVERHEAT_LATCH | LINK_CAP_DEADMAN)

/* Supply present (link Status flag): above this the power stages have a supply, 0.01 V. */
#define SAFETY_SUPPLY_PRESENT_CV 600u

typedef struct {
  burn burn;
  glow glow;
  fan fan;
  checks chk;
  uint8_t power_up_started;
  uint8_t precheck_started; /* the pre-start test of the current start has begun */
  uint8_t precheck_result;  /* BURN_PRECHECK_* for the sequencer */
  uint16_t power_up_failed; /* CHECK_FAIL_* from the power-up test */
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
} safety;

/* reset_reason and checks come from the start-up code. */
void safety_init(safety *s, const burn_params *p, const glow_params *gp, const fan_params *fp,
                 const checks_params *cp, uint32_t now, const burn_inputs *in, uint8_t reset_reason,
                 uint8_t checks);

/* One received byte. When it completes a request, the reply is written to reply (LINK_FRAME_MAX
 * bytes) and its length returned; otherwise 0. */
size_t safety_receive(safety *s, uint8_t byte, uint32_t now, uint8_t *reply);

/* One pass of the safety loop with fresh measurements: the dead-man, then the sequencer. */
void safety_step(safety *s, uint32_t now, const burn_inputs *in);

const burn_outputs *safety_outputs(const safety *s);

/* One glow PWM period (5 ms): the planner turns the sequencer's glow request into the next pulse.
 * The hardware layer fills m with that period's readings. */
glow_cmd safety_glow_period(safety *s, uint32_t now, glow_meas *m);

/* One pass of the fan stage: the speed the sequencer wants, as a PWM duty (permille), from that
 * pass's fan readings. Call it every pass, after safety_step. */
uint16_t safety_fan_step(safety *s, uint32_t now, const fan_meas *m);

/* Every millisecond: the self-tests while one runs (the power-up test by itself after a reset,
 * the pre-start test when the sequencer asks), else the heartbeat the burn needs. The hardware
 * layer drives the heartbeat, LATCH_RESET and any test pump pulse from the result, and sets
 * GLOW_EN when either this or the glow planner asks for it. */
checks_out safety_ms(safety *s, uint32_t now, const checks_obs *o);

/* A heat start waits while more than this share of the glow plug's heat budget is used (after any
 * reset it starts full and drains in 240 s). PROVISIONAL. */
#define SAFETY_GLOW_START_FREE_PERCENT 50u

#ifdef __cplusplus
}
#endif

#endif /* FLAMEKEEPER_SAFETY_H */
