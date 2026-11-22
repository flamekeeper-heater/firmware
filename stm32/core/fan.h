/* The fan stage controller (DESIGN.md): one low-side switch pulsed at 25 kHz, a Hall speed
 * sensor and a shunt that reads the AVERAGE switch current (motor current x duty). Called every
 * pass of the safety loop with the speed the sequencer wants, it returns the PWM duty. Plain C99,
 * integer maths, no hardware access. Host-tested in ../tests/test_fan.c against a motor model.
 *
 * - A speed loop on the Hall reading, under the 10 V cap on the motor (duty <= 10 V / supply).
 * - The start limit: the duty rises from 0 over at least ramp_ms, and while the fan turns below half
 *   its commanded speed the motor current is held at or below start_limit_ma (the fan's one-time
 *   fuse). Below estimate_min_permille of duty the current estimate is too coarse: the ramp runs on
 *   time alone there.
 * - Faults: a stalled motor (current, no Hall edges; it clears itself once the command is 0), current
 *   at zero duty (the switch stuck on), speed without a command or well above it, once the fan has
 *   had time to spin down.
 *
 * Gains and limits are PROVISIONAL until measured on the first boards. */
#ifndef FLAMEKEEPER_FAN_H
#define FLAMEKEEPER_FAN_H

#include <stdint.h>

#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint16_t v_max_cv;            /* the motor's highest average voltage, 0.01 V (FAN-1: 10 V) */
  uint16_t rpm_at_v_max;        /* the speed it reaches there: the feed-forward's scale */
  uint16_t kp_permille_per_krpm; /* proportional gain: duty permille per 1000 rpm of error */
  uint16_t ki_permille_per_krpm_s; /* integral gain: duty permille per 1000 rpm of error per s */
  uint16_t ramp_ms;             /* duty from 0 to its cap in no less than this */
  uint16_t start_limit_ma;      /* motor current while starting (FAN-9: 6 A) */
  uint16_t start_limit_ms;      /* above it this long: back off */
  uint16_t back_off_permille;   /* how much duty each back-off removes */
  uint16_t estimate_min_permille; /* below this duty the current estimate is not used */
  uint16_t turning_rpm;         /* Hall at or above this: the motor turns */
  uint16_t stall_ma;            /* average switch current above this with the motor not turning */
  uint16_t stall_ms;            /* ... this long: stalled (FAN-3: 1 s) */
  uint16_t stuck_on_ma;         /* average switch current at zero duty above this */
  uint16_t stuck_on_ms;
  uint16_t spin_down_ms;        /* time a fan gets to slow down before speed counts as a fault */
  uint8_t overspeed_percent;    /* above the command by more than this ... */
  uint16_t overspeed_ms;        /* ... this long (a fan freed from a short block overshoots briefly) */
} fan_params;

void fan_params_default(fan_params *p);

typedef struct {
  uint16_t rpm;        /* Hall */
  uint16_t vin_cv;     /* VIN_SENSE */
  uint16_t isense_ma;  /* FAN_ISENSE: the average switch current */
} fan_meas;

enum {
  FAN_FAULT_NONE = 0,
  FAN_FAULT_STALL = 1,     /* current but no Hall edges */
  FAN_FAULT_STUCK_ON = 2,  /* current with the switch off: the motor runs at full supply */
  FAN_FAULT_UNCOMMANDED = 3, /* turning with no command, after the spin-down time */
  FAN_FAULT_OVERSPEED = 4, /* well above the command, after the spin-down time */
};

typedef struct {
  fan_params p;
  uint32_t last_ms;
  uint16_t duty;        /* permille, what was set last pass */
  int32_t integ;        /* integral term, permille x 1000 */
  uint16_t target;
  uint32_t target_dropped_ms; /* when the command last went down (spin-down time counts from it) */
  held over;             /* start current above the limit (re-armed after each back-off) */
  held stall;            /* current, no turning */
  held stuck;            /* current at zero duty */
  held overspeed;        /* well above the command */
  uint8_t fault;
} fan;

/* At boot only. */
void fan_init(fan *f, const fan_params *p, uint32_t now_ms);
/* A user's fault reset: the fault, its timers and the loop's state go; the parameters stay. */
void fan_clear_fault(fan *f, uint32_t now_ms);

/* One pass: the speed wanted (0 = off) and the readings. Returns the duty, permille. After a fault
 * it returns 0: a stall until the speed wanted is 0, any other fault until fan_clear_fault. */
uint16_t fan_step(fan *f, uint32_t now_ms, uint16_t target_rpm, const fan_meas *m);

/* Open loop at a fixed duty (the bring-up build's bench tests): the same 10 V cap, ramp, start
 * current limit (always, as there is no speed to compare with), stall and stuck-on checks; no speed
 * loop and no speed faults. 0 is the speed loop's off. */
uint16_t fan_step_open(fan *f, uint32_t now_ms, uint16_t duty_permille, const fan_meas *m);

/* The highest duty at this supply (FAN-1), permille. */
uint16_t fan_duty_cap(const fan *f, uint16_t vin_cv);

#ifdef __cplusplus
}
#endif

#endif /* FLAMEKEEPER_FAN_H */
