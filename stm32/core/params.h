/* The heater parameters the safety MCU owns (DESIGN.md): the sequencer's tunable figures as one
 * described table, so the apps show them with names, units and ranges and never hold a list of
 * their own. Values are staged, then applied together while the heater is off, and optionally
 * saved: the hardware layer writes the saved image to flash. Plain C99. Host-tested in
 * ../tests/test_params.c.
 *
 * What the board's safety rests on is not in the table: the glow time caps, the glow's minimum fan
 * speed, the pump's 6 Hz ceiling (a bound on the pump range here, not a setting). */
#ifndef FLAMEKEEPER_PARAMS_H
#define FLAMEKEEPER_PARAMS_H

#include <stddef.h>
#include <stdint.h>

#include "burn.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The app protocol's codes. */
enum { PARAM_UNIT_NONE = 0, PARAM_UNIT_S = 1, PARAM_UNIT_PERCENT = 2, PARAM_UNIT_HZ = 3, PARAM_UNIT_RPM = 4,
       PARAM_UNIT_C = 5, PARAM_UNIT_W = 6, PARAM_UNIT_C_DIFF = 10, PARAM_UNIT_V = 13 };
enum { PARAM_GROUP_PUMP = 2, PARAM_GROUP_FAN = 3, PARAM_GROUP_START = 4, PARAM_GROUP_GLOW = 5,
       PARAM_GROUP_STOP = 6, PARAM_GROUP_OTHER = 0 };
enum { PARAM_LEVEL_BASIC = 0, PARAM_LEVEL_ADVANCED = 1, PARAM_LEVEL_EXPERT = 2 };
#define PARAM_FLAG_HEATER_OFF (1u << 0) /* applied only while the heater is off */

typedef struct {
  uint16_t id;          /* stable: the ESP32 merges its own settings (0x100 up) after these */
  const char *name;     /* at most 24 bytes */
  uint8_t unit, decimals, group, level, flags;
  int32_t min, max, step;
  uint16_t offset;      /* where the value lives in burn_params */
  uint8_t size;         /* 1 or 2 bytes */
  uint8_t is_signed;
  const char *help;     /* at most 120 bytes */
} param_def;

extern const param_def PARAM_TABLE[];
extern const size_t PARAM_COUNT;

const param_def *params_find(uint16_t id);

typedef struct {
  burn_params live;       /* what the sequencer runs with */
  uint16_t staged[32];    /* per table entry, the field's 16 bits (params_staged reads it) */
  uint32_t staged_mask;   /* entries with a staged change */
  uint8_t save_pending;   /* applied with "save": the hardware layer writes params_pack to flash */
} params;

/* Defaults; then params_unpack if flash holds a valid image. */
void params_init(params *ps);

int32_t params_value(const params *ps, size_t index);
/* The value staged for a table entry (meaningful while its staged_mask bit is set). */
int32_t params_staged(const params *ps, size_t index);

/* The link's requests. They return the app protocol's result codes. */
int params_set(params *ps, uint16_t id, int32_t value);        /* stage */
int params_apply(params *ps, int heater_off, int save);       /* the staged changes, together */
void params_discard(params *ps);
void params_defaults(params *ps);                             /* every value back, and saved */

/* The flash image: magic, version, count, id/value pairs, CRC16. At most 32 entries (staged_mask). */
#define PARAMS_IMAGE_MAX (4u + 32u * 6u + 2u)
size_t params_pack(const params *ps, uint8_t *out, size_t cap);
/* 1 when the image was valid and every value in range (then applied); 0 leaves the defaults. */
int params_unpack(params *ps, const uint8_t *in, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* FLAMEKEEPER_PARAMS_H */
