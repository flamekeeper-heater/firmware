/* The glow plug type check: see plug.h. */
#include "plug.h"

#include <string.h>

#include "../../common/link/link.h"

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
  pl->rec.verified_v = 0;
  pl->save_pending = 1;
}

static int is_cold(const plug *pl, uint32_t now, int16_t hx, int16_t board) {
  if (since(now, pl->since_ms) < pl->p.cold_ms || !temp_known(hx))
    return 0;
  if (hx < pl->p.cold_hx_c10)
    return 1;
  if (!temp_known(board))
    return 0;
  int32_t d = (int32_t) hx - (int32_t) board;
  return d <= pl->p.cold_near_c10 && d >= -pl->p.cold_near_c10;
}

void plug_begin(plug *pl, uint32_t now, int16_t hx_c10, int16_t board_c10) {
  pl->last_cold = (uint8_t) is_cold(pl, now, hx_c10, board_c10);
}

uint16_t plug_r_mohm(uint16_t vin_cv, uint16_t probe_ca) {
  uint32_t r = probe_ca == 0 ? 0xFFFFu : (uint32_t) vin_cv * 1000u / probe_ca;
  return (uint16_t) (r > 0xFFFFu ? 0xFFFFu : r);
}

uint8_t plug_type_of(const plug_params *p, uint16_t r_mohm) {
  if (r_mohm < p->r12_max_mohm)
    return 12;
  if (r_mohm >= p->r24_min_mohm && r_mohm <= p->r24_max_mohm)
    return 24;
  return 0;
}

uint8_t plug_judge(plug *pl, uint8_t setting_v, uint16_t vin_cv, uint16_t probe_ca) {
  const plug_params *p = &pl->p;
  int supply24 = vin_cv > p->supply_24v_cv;
  uint16_t r = plug_r_mohm(vin_cv, probe_ca);
  uint8_t type = plug_type_of(p, r);

  if (pl->last_cold) {
    if (type == 0)
      return PLUG_REFUSE_RANGE;
    if (type != setting_v)
      return supply24 ? PLUG_REFUSE_TYPE : PLUG_WARN;
    if (pl->rec.verified_v != type)
      pl->save_pending = 1;
    pl->rec.verified_v = type; /* the type and the setting it matched, at once */
    return PLUG_GO;
  }
  if (r == 0xFFFFu || r < 50u)
    return PLUG_REFUSE_RANGE; /* open, or a dead short: never, warm or not */
  if (pl->rec.verified_v != setting_v)
    return supply24 ? PLUG_REFUSE_UNVERIFIED : PLUG_WARN;
  int insane = (setting_v == 12 && r > p->r12_warm_max_mohm) || (setting_v == 24 && r < p->r24_warm_min_mohm);
  return !insane ? PLUG_GO : supply24 ? PLUG_REFUSE_SANITY : PLUG_WARN;
}

#define MAGIC0 'G'
#define MAGIC1 'P'
#define VERSION 2u

size_t plug_pack(const plug *pl, uint8_t *out, size_t cap) {
  if (cap < PLUG_IMAGE_SIZE)
    return 0;
  out[0] = MAGIC0, out[1] = MAGIC1, out[2] = VERSION;
  out[3] = pl->rec.verified_v;
  uint16_t crc = link_crc16(out, 4);
  out[4] = (uint8_t) crc, out[5] = (uint8_t) (crc >> 8);
  return PLUG_IMAGE_SIZE;
}

int plug_unpack(plug *pl, const uint8_t *in, size_t len) {
  if (len < PLUG_IMAGE_SIZE || in[0] != MAGIC0 || in[1] != MAGIC1 || in[2] != VERSION)
    return 0;
  if (link_crc16(in, 4) != (uint16_t) (in[4] | (in[5] << 8)))
    return 0;
  if (in[3] != 0 && in[3] != 12 && in[3] != 24)
    return 0;
  pl->rec.verified_v = in[3];
  return 1;
}
