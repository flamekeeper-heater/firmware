/* The burn sequencer of the Flamekeeper safety MCU (DESIGN.md): start, run and stop the heater
 * from measured inputs, and say what the power stages should do. Plain C99, no hardware access:
 * the caller reads the sensors, calls burn_step() every pass of the safety loop, and drives the
 * outputs from burn_outputs. Host-tested in ../tests/.
 *
 * Time is a wrapping uint32_t millisecond count; elapsed time is always (uint32_t)(now - then).
 *
 * Every timing and limit is in burn_params. The defaults are PROVISIONAL: they keep the board's
 * rules (glow time caps, airflow before glow) but the ignition and warm-up figures still have to
 * be tuned on a real heater. */
#ifndef FLAMEKEEPER_BURN_H
#define FLAMEKEEPER_BURN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BURN_UNKNOWN_TEMP ((int16_t) 0x8000) /* heat exchanger sensor open or not read */

/* The heater states the app protocol reports (link_status.state). */
enum {
  BURN_STATE_OFF = 1,
  BURN_STATE_STARTING = 2,
  BURN_STATE_HEATING = 3,
  BURN_STATE_FAN_ONLY = 4,
  BURN_STATE_STOPPING = 5,
  BURN_STATE_COOLING = 6,
  BURN_STATE_PRIMING = 7,
};

/* The sequencer's own step (link_status.detail). */
enum {
  BURN_STEP_OFF = 0,
  BURN_STEP_SELF_TEST = 1, /* fan spinning up: it must turn before anything else happens */
  BURN_STEP_PREHEAT = 2,   /* glow plug heating, fan at start speed, no fuel */
  BURN_STEP_IGNITION = 3,  /* glow plug on, fuel at the start rate, waiting for the flame */
  BURN_STEP_PURGE = 4,     /* no flame: fuel off, fan clears the chamber before the retry */
  BURN_STEP_WARM_UP = 5,   /* flame seen: ramp fuel and fan to the asked power */
  BURN_STEP_RUNNING = 6,
  BURN_STEP_BURN_OFF = 7,  /* fuel off, fan high: the flame burns out */
  BURN_STEP_COOLDOWN = 8,
  BURN_STEP_FAN_ONLY = 9,
  BURN_STEP_PRIMING = 10,
};

/* Faults: the app protocol's codes (link_status.fault). */
enum {
  BURN_FAULT_NONE = 0,
  BURN_FAULT_SUPPLY_LOW = 2,
  BURN_FAULT_SUPPLY_HIGH = 3,
  BURN_FAULT_GLOW = 4,
  BURN_FAULT_OVERHEAT = 6,
  BURN_FAULT_FAN = 7,
  BURN_FAULT_FLAME_OUT = 8,
  BURN_FAULT_SENSOR = 9,
  BURN_FAULT_START = 10,
  BURN_FAULT_CO = 11,
};

/* Why a run stopped, beyond the fault code (link_status.fault_detail). */
enum {
  BURN_WHY_NONE = 0,
  BURN_WHY_ASKED = 1,           /* a normal stop */
  BURN_WHY_DEADMAN = 2,         /* the ESP32 went silent during a burn */
  BURN_WHY_NO_FLAME = 3,        /* every ignition attempt failed */
  BURN_WHY_FLAME_LOST = 4,
  BURN_WHY_OVERHEAT_LIMIT = 5,  /* the firmware's limit */
  BURN_WHY_OVERHEAT_LATCH = 6,  /* the hardware latch tripped */
  BURN_WHY_FAN_STALL = 7,       /* a turning fan was asked for and none turned */
  BURN_WHY_FAN_SPIN_UP = 8,     /* the fan did not reach the glow's minimum speed in time */
  BURN_WHY_SENSOR_OPEN = 9,     /* heat-exchanger sensor open */
  BURN_WHY_NO_TEMP_RISE = 10,   /* fuel and glow, but the sensor never moved (OVH-7) */
  BURN_WHY_SUPPLY = 11,
  BURN_WHY_COOLDOWN_LIMIT = 12, /* the heat exchanger never cooled within the cooldown limit */
  BURN_WHY_CO = 13,
  BURN_WHY_GLOW_STAGE = 14,     /* the glow planner stopped: stuck low side or repeated trips */
  BURN_WHY_FAN_STAGE = 15,      /* the fan's switch stuck on, or the fan turning against its command */
};

typedef struct {
  /* Fan, rpm. Power 0..100 % maps linearly onto min..max (heat), fan_only_min..max (fan only). */
  uint16_t fan_start_rpm;     /* self-test, preheat and ignition */
  uint16_t fan_min_rpm, fan_max_rpm;
  uint16_t fan_burn_off_rpm;  /* burn-off and cooldown */
  uint16_t glow_min_fan_rpm;  /* no glow below this MEASURED speed (GLW-14) */
  uint16_t fan_stall_rpm;     /* measured below this while asked to turn: stalled */
  uint16_t fan_spin_up_ms;    /* time the fan gets to reach glow_min_fan_rpm */
  uint16_t fan_stall_ms;      /* stalled this long: fault (FAN-3: 1 s) */

  /* Fuel pump, 0.01 Hz. */
  uint16_t pump_start_cHz;    /* ignition */
  uint16_t pump_min_cHz, pump_max_cHz;
  uint16_t pump_prime_cHz;
  uint16_t prime_s;

  /* Glow plug. */
  uint16_t glow_w;            /* power asked of the glow stage (GLW-2) */
  uint16_t preheat_s;
  uint16_t ignition_s;        /* fuel and glow, waiting for the flame */
  uint16_t glow_after_flame_s; /* glow stays on this long after the flame is seen */
  uint16_t glow_attempt_max_s; /* glow on-time cap per attempt (GLW-1: 120 s) */
  uint16_t glow_window_max_s;  /* glow on-time cap in any 10 minutes (GLW-1: 240 s) */
  uint8_t attempts;            /* ignition attempts per start */
  uint16_t purge_s;

  /* Flame, from the heat exchanger, 0.1 C. */
  int16_t flame_rise_c10;     /* rise over the start temperature that means flame */
  int16_t flame_lost_c10;     /* fall below the run's peak that means the flame went out */
  uint16_t warm_up_s;         /* fuel and fan ramp to the asked power */

  /* Limits. */
  int16_t overheat_derate_c10; /* above: power held at the minimum */
  int16_t overheat_stop_c10;   /* above: fault, burn-off and cooldown */
  int16_t cool_c10;            /* cooldown ends below this */
  uint16_t cooldown_min_s, cooldown_max_s;
  uint16_t burn_off_s;
  int16_t hot_start_c10;       /* heat exchanger above this after a reset: cool down first */
  uint16_t supply_low_cv, supply_high_cv; /* 0.01 V */
  uint16_t supply_fault_ms;    /* outside the range this long: fault */
} burn_params;

void burn_params_default(burn_params *p);

/* One pass's measurements. */
typedef struct {
  int16_t hx_c10;          /* heat exchanger, 0.1 C, or BURN_UNKNOWN_TEMP */
  uint16_t fan_rpm;        /* measured (Hall) */
  uint16_t supply_cv;      /* 0.01 V */
  uint8_t overheat_latch;  /* 1: the hardware overheat latch has tripped (OVERHEAT_N low) */
  uint8_t co_alarm;        /* 1: the CO alarm is on */
  uint8_t glow_fault;      /* 1: the glow stage failed (low side stuck on, repeated trips) */
  uint8_t fan_fault;       /* the fan stage's fault (fan.h FAN_FAULT_*), 0 none */
} burn_inputs;

/* What the power stages should do now. */
typedef struct {
  uint16_t fan_rpm;  /* target speed; 0 = fan off */
  uint16_t pump_cHz; /* 0 = no fuel */
  uint16_t glow_w;   /* 0 = glow plug off */
} burn_outputs;

typedef struct {
  burn_params p;
  uint8_t step, mode;      /* mode: what was asked (LINK_MODE_* values) */
  uint8_t power;           /* asked power, 0..100 % */
  uint8_t running_power;   /* power the heater runs at now, % */
  uint8_t fault, why;
  uint8_t attempt;
  uint8_t fault_latched;   /* needs burn_fault_reset() (hardware latch, CO) */
  uint32_t step_ms;        /* when this step began */
  uint32_t fan_low_ms;     /* when the fan was first seen below the stall speed (0 = not low) */
  uint8_t fan_low;
  uint32_t supply_bad_ms;
  uint8_t supply_bad;
  int16_t start_c10, peak_c10;
  uint8_t flame;
  uint32_t flame_ms;
  /* Glow on-time: this attempt's, and the last 10 minutes' in one-minute buckets (ms). */
  uint32_t glow_attempt_ms;
  uint32_t glow_bucket[10];
  uint32_t glow_bucket_start; /* when the current bucket began */
  uint8_t glow_bucket_i;
  uint32_t fan_on_ms;      /* when the fan was last asked to turn from standstill */
  uint32_t last_ms;
  uint8_t glow_on;
  burn_outputs out;
} burn;

/* After a reset: the heat exchanger's first reading decides whether it cools down first. */
void burn_init(burn *b, const burn_params *p, uint32_t now, const burn_inputs *in);

/* Requests. They return the app protocol's result codes: 0 OK, 0x20 not off, 0x13 not supported,
 * 0x2F refused (fault and why say what stands in the way). */
int burn_start(burn *b, uint8_t mode, uint8_t power, uint32_t now, const burn_inputs *in);
void burn_stop(burn *b, uint8_t why, uint32_t now);
void burn_set_power(burn *b, uint8_t power);
int burn_fault_reset(burn *b, const burn_inputs *in);

/* One pass of the safety loop. Updates b->out. */
void burn_step(burn *b, uint32_t now, const burn_inputs *in);

uint8_t burn_state(const burn *b);
/* Seconds of priming left (0 when not priming). */
uint16_t burn_prime_left_s(const burn *b, uint32_t now);
/* Glow seconds counted in the last 10 minutes. */
uint16_t burn_glow_window_s(const burn *b);
/* 1 from a start until the stop is asked: the dead-man stops an active run. */
int burn_active(const burn *b);
/* 1 while fuel may be in the chamber (ignition to burn-off): the pump interlock's business. */
int burn_fuel_steps(const burn *b);

#ifdef __cplusplus
}
#endif

#endif /* FLAMEKEEPER_BURN_H */
