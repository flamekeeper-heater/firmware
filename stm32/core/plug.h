/* The glow plug type check (DESIGN.md): a 12 V plug on a 24 V supply would overheat, so before a
 * start may heat the plug its type is checked against the setting, from the resistance the first
 * glow probe measures (R = supply / probe current). Plain C99. Host-tested in ../tests/test_plug.c.
 *
 * - Cold plug (no glow for cold_ms, counted from 0 after any reset, and the heat exchanger near the
 *   board's temperature or below cold_hx_c10): under r12_max is a 12 V plug, r24_min..r24_max a
 *   24 V plug, anything else no start. A match is remembered with the setting it was checked
 *   against, in a record the hardware layer keeps in flash.
 * - Warm plug: allowed when the remembered check matches the present setting, then sanity-checked
 *   (a 12 V plug reading above r12_warm_max, a 24 V plug below r24_warm_min, is refused).
 * - On a 24 V supply a mismatch, or no remembered check, refuses the start. On a 12 V supply a
 *   mismatch cannot overheat the plug: it is a warning only. A plug outside every range never
 *   starts.
 *
 * Every figure is PROVISIONAL until plugs are measured on the first boards. */
#ifndef FLAMEKEEPER_PLUG_H
#define FLAMEKEEPER_PLUG_H

#include <stddef.h>
#include <stdint.h>

#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint16_t r12_max_mohm;        /* GLW-10: under 0.8 Ohm cold: a 12 V plug */
  uint16_t r24_min_mohm, r24_max_mohm; /* 1.1-2.6 Ohm cold: a 24 V plug */
  uint16_t r12_warm_max_mohm;   /* GLW-12: a remembered 12 V plug above 1.4 Ohm: refused */
  uint16_t r24_warm_min_mohm;   /* ... a remembered 24 V plug below 1.3 Ohm: refused */
  uint32_t cold_ms;             /* GLW-11: no glow for 10 minutes */
  int16_t cold_hx_c10;          /* ... and the heat exchanger below 40 C */
  int16_t cold_near_c10;        /* ... or within 10 K of the board */
  uint16_t supply_24v_cv;       /* above this the supply counts as 24 V */
} plug_params;

void plug_params_default(plug_params *p);

enum {
  PLUG_GO = 0,
  PLUG_WARN = 1,            /* 12 V supply: a mismatch or no check, shown, not blocking (GLW-13) */
  PLUG_REFUSE_RANGE = 2,    /* outside every range (open, shorted, between the types) */
  PLUG_REFUSE_TYPE = 3,     /* cold check: the plug is not the type set */
  PLUG_REFUSE_UNVERIFIED = 4, /* warm: never checked cold against this setting */
  PLUG_REFUSE_SANITY = 5,   /* warm: the reading does not fit the remembered type (GLW-12) */
};

typedef struct {
  uint8_t verified_type;    /* 12 or 24; 0: nothing verified */
  uint8_t verified_setting; /* the plug-type setting the check was made against */
} plug_record;

typedef struct {
  plug_params p;
  plug_record rec;
  uint8_t save_pending;     /* rec changed: the hardware layer writes plug_pack to flash */
  uint32_t since_ms;        /* glow last seen, or the reset: the cold time counts from here */
  uint8_t last_verdict;
  uint16_t last_r_mohm;
  uint8_t last_cold;
} plug;

void plug_init(plug *pl, const plug_params *p, uint32_t now_ms);
/* Glow went out to the plug: the cold time starts again. */
void plug_glow_seen(plug *pl, uint32_t now_ms);
/* "New glow plug fitted": the remembered check no longer holds. */
void plug_new_plug(plug *pl);

/* A start begins: whether the plug is cold is decided now, before any glow. */
void plug_begin(plug *pl, uint32_t now_ms, int16_t hx_c10, int16_t board_c10);
/* The verdict for that start, from its first probe. setting_v: 12 or 24. */
uint8_t plug_judge(plug *pl, uint8_t setting_v, uint16_t vin_cv, uint16_t probe_ca);

/* The flash record: magic, version, type, setting, CRC16. */
#define PLUG_IMAGE_SIZE 7u
size_t plug_pack(const plug *pl, uint8_t *out, size_t cap);
int plug_unpack(plug *pl, const uint8_t *in, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* FLAMEKEEPER_PLUG_H */
