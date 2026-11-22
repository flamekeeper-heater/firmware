/* The glow plug type check: see plug.h. */
#include "plug.h"

#include <string.h>

#include "../../common/link/link.h"

#define UNKNOWN_TEMP ((int16_t) 0x8000)

void plug_params_default(plug_params *p) {
  memset(p, 0, sizeof(*p));
  p->r12_max_mohm = 800;
  p->r24_min_mohm = 1100;
  p->r24_max_mohm = 2600;
  p->r12_warm_max_mohm = 1400;
  p->r24_warm_min_mohm = 1300;
  p->cold_ms = 10u * 60u * 1000u;
  p->cold_hx_c10 = 400;
  p->cold_near_c10 = 100;
  p->supply_24v_cv = 1800;
}

void plug_init(plug *pl, const plug_params *p, uint32_t now_ms) {
  memset(pl, 0, sizeof(*pl));
  pl->p = *p;
  pl->since_ms = now_ms; /* GLW-11: counted from 0 after any reset */
}

void plug_glow_seen(plug *pl, uint32_t now_ms) { pl->since_ms = now_ms; }

void plug_new_plug(plug *pl) {
  pl->rec.verified_type = 0;
  pl->rec.verified_setting = 0;
  pl->save_pending = 1;
}

static int is_cold(const plug *pl, uint32_t now, int16_t hx, int16_t board) {
  if ((uint32_t) (now - pl->since_ms) < pl->p.cold_ms || hx == UNKNOWN_TEMP)
    return 0;
  if (hx < pl->p.cold_hx_c10)
    return 1;
  if (board == UNKNOWN_TEMP)
    return 0;
  int32_t d = (int32_t) hx - (int32_t) board;
  return d <= pl->p.cold_near_c10 && d >= -pl->p.cold_near_c10;
}

void plug_begin(plug *pl, uint32_t now, int16_t hx_c10, int16_t board_c10) {
  pl->last_cold = (uint8_t) is_cold(pl, now, hx_c10, board_c10);
}

uint8_t plug_judge(plug *pl, uint8_t setting_v, uint16_t vin_cv, uint16_t probe_ca) {
  const plug_params *p = &pl->p;
  int supply24 = vin_cv > p->supply_24v_cv;
  uint32_t r = probe_ca == 0 ? 0xFFFFu : (uint32_t) vin_cv * 1000u / probe_ca; /* mOhm */
  if (r > 0xFFFFu)
    r = 0xFFFFu;
  pl->last_r_mohm = (uint16_t) r;
  uint8_t type = r < p->r12_max_mohm ? 12 : (r >= p->r24_min_mohm && r <= p->r24_max_mohm) ? 24 : 0;
  uint8_t v;

  if (pl->last_cold) {
    if (type == 0)
      v = PLUG_REFUSE_RANGE;
    else if (type == setting_v) {
      if (pl->rec.verified_type != type || pl->rec.verified_setting != setting_v)
        pl->save_pending = 1;
      pl->rec.verified_type = type;
      pl->rec.verified_setting = setting_v;
      v = PLUG_GO;
    } else {
      v = supply24 ? PLUG_REFUSE_TYPE : PLUG_WARN;
    }
  } else if (r == 0xFFFFu || r < 50u) {
    v = PLUG_REFUSE_RANGE; /* open, or a dead short: never, warm or not */
  } else if (pl->rec.verified_type == setting_v && pl->rec.verified_setting == setting_v) {
    int insane = (setting_v == 12 && r > p->r12_warm_max_mohm) || (setting_v == 24 && r < p->r24_warm_min_mohm);
    v = !insane ? PLUG_GO : supply24 ? PLUG_REFUSE_SANITY : PLUG_WARN;
  } else {
    v = supply24 ? PLUG_REFUSE_UNVERIFIED : PLUG_WARN;
  }
  pl->last_verdict = v;
  return v;
}

#define MAGIC0 'G'
#define MAGIC1 'P'
#define VERSION 1u

size_t plug_pack(const plug *pl, uint8_t *out, size_t cap) {
  if (cap < PLUG_IMAGE_SIZE)
    return 0;
  out[0] = MAGIC0, out[1] = MAGIC1, out[2] = VERSION;
  out[3] = pl->rec.verified_type, out[4] = pl->rec.verified_setting;
  uint16_t crc = link_crc16(out, 5);
  out[5] = (uint8_t) crc, out[6] = (uint8_t) (crc >> 8);
  return PLUG_IMAGE_SIZE;
}

int plug_unpack(plug *pl, const uint8_t *in, size_t len) {
  if (len < PLUG_IMAGE_SIZE || in[0] != MAGIC0 || in[1] != MAGIC1 || in[2] != VERSION)
    return 0;
  if (link_crc16(in, 5) != (uint16_t) (in[5] | (in[6] << 8)))
    return 0;
  if ((in[3] != 0 && in[3] != 12 && in[3] != 24) || (in[4] != 0 && in[4] != 12 && in[4] != 24))
    return 0;
  pl->rec.verified_type = in[3];
  pl->rec.verified_setting = in[4];
  return 1;
}
