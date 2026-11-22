/* The heater parameter table: see params.h. */
#include "params.h"

#include <string.h>

#include "../../common/link/link.h"

#define F(field) (uint16_t) offsetof(burn_params, field), (uint8_t) sizeof(((burn_params *) 0)->field)
#define OFF PARAM_FLAG_HEATER_OFF

/* id, name, unit, decimals, group, level, flags, min, max, step, field (offset, size), signed, help */
const param_def PARAM_TABLE[] = {
    {0x01, "Pump min", PARAM_UNIT_HZ, 2, PARAM_GROUP_PUMP, PARAM_LEVEL_BASIC, OFF, 50, 400, 5, F(pump_min_cHz), 0,
     "Fuel pump rate at the lowest power. Lower gives a smaller flame; too low and it may go out."},
    {0x02, "Pump max", PARAM_UNIT_HZ, 2, PARAM_GROUP_PUMP, PARAM_LEVEL_BASIC, OFF, 100, 600, 5, F(pump_max_cHz), 0,
     "Fuel pump rate at full power, at most 6 Hz. Higher gives more heat and more soot."},
    {0x03, "Pump start", PARAM_UNIT_HZ, 2, PARAM_GROUP_PUMP, PARAM_LEVEL_ADVANCED, OFF, 50, 400, 5,
     F(pump_start_cHz), 0, "Fuel pump rate while it lights. Too much fuel smokes; too little does not light."},
    {0x04, "Prime rate", PARAM_UNIT_HZ, 2, PARAM_GROUP_PUMP, PARAM_LEVEL_ADVANCED, OFF, 100, 1000, 10,
     F(pump_prime_cHz), 0, "Fuel pump rate when priming an empty fuel line, at most 10 Hz."},
    {0x05, "Prime time", PARAM_UNIT_S, 0, PARAM_GROUP_PUMP, PARAM_LEVEL_ADVANCED, OFF, 10, 180, 5, F(prime_s), 0,
     "How long one priming runs."},
    {0x10, "Fan min", PARAM_UNIT_RPM, 0, PARAM_GROUP_FAN, PARAM_LEVEL_BASIC, OFF, 1000, 4000, 50, F(fan_min_rpm), 0,
     "Fan speed at the lowest power. Too low for the fuel and it soots up."},
    {0x11, "Fan max", PARAM_UNIT_RPM, 0, PARAM_GROUP_FAN, PARAM_LEVEL_BASIC, OFF, 2000, 6000, 50, F(fan_max_rpm), 0,
     "Fan speed at full power. Higher moves more air and is louder."},
    {0x12, "Fan start", PARAM_UNIT_RPM, 0, PARAM_GROUP_FAN, PARAM_LEVEL_ADVANCED, OFF, 1200, 3000, 50,
     F(fan_start_rpm), 0, "Fan speed while it lights. The glow plug only heats with the fan turning."},
    {0x13, "Fan cooldown", PARAM_UNIT_RPM, 0, PARAM_GROUP_FAN, PARAM_LEVEL_ADVANCED, OFF, 1500, 6000, 50,
     F(fan_burn_off_rpm), 0, "Fan speed while it burns off and cools down after a run."},
    {0x20, "Preheat time", PARAM_UNIT_S, 0, PARAM_GROUP_START, PARAM_LEVEL_ADVANCED, OFF, 10, 110, 5, F(preheat_s), 0,
     "Glow plug heating before fuel goes in. Longer helps in the cold."},
    {0x21, "Ignition time", PARAM_UNIT_S, 0, PARAM_GROUP_START, PARAM_LEVEL_ADVANCED, OFF, 20, 120, 5,
     F(ignition_s), 0, "How long fuel and glow plug wait for the flame before it tries again."},
    {0x22, "Start attempts", PARAM_UNIT_NONE, 0, PARAM_GROUP_START, PARAM_LEVEL_ADVANCED, OFF, 1, 2, 1,
     F(attempts), 0, "Ignition attempts per start, with a purge between them."},
    {0x23, "Purge time", PARAM_UNIT_S, 0, PARAM_GROUP_START, PARAM_LEVEL_EXPERT, OFF, 20, 180, 5, F(purge_s), 0,
     "Fan only between two ignition attempts, to clear unburnt fuel."},
    {0x24, "Warm-up time", PARAM_UNIT_S, 0, PARAM_GROUP_START, PARAM_LEVEL_EXPERT, OFF, 10, 300, 5, F(warm_up_s), 0,
     "How long the flame takes to reach the asked power after it lights."},
    {0x25, "Flame rise", PARAM_UNIT_C_DIFF, 1, PARAM_GROUP_START, PARAM_LEVEL_EXPERT, OFF, 50, 500, 5,
     F(flame_rise_c10), 1, "Heat exchanger rise that counts as the flame lit."},
    {0x30, "Glow power", PARAM_UNIT_W, 0, PARAM_GROUP_GLOW, PARAM_LEVEL_ADVANCED, OFF, 40, 90, 5, F(glow_w), 0,
     "Power into the glow plug, at most 90 W. More lights sooner and wears the plug faster."},
    {0x32, "Glow plug type", PARAM_UNIT_V, 0, PARAM_GROUP_GLOW, PARAM_LEVEL_BASIC, OFF, 12, 24, 12, F(plug_v), 0,
     "The plug fitted, 12 or 24 V. A 12 V plug set as 24 V will not start on a 24 V supply."},
    {0x31, "Glow after flame", PARAM_UNIT_S, 0, PARAM_GROUP_GLOW, PARAM_LEVEL_EXPERT, OFF, 0, 60, 5,
     F(glow_after_flame_s), 0, "How long the glow plug stays on once the flame is seen."},
    {0x40, "Burn-off time", PARAM_UNIT_S, 0, PARAM_GROUP_STOP, PARAM_LEVEL_ADVANCED, OFF, 10, 120, 5, F(burn_off_s),
     0, "Fan at cooldown speed with no fuel, so the flame burns out cleanly."},
    {0x41, "Cooldown to", PARAM_UNIT_C, 1, PARAM_GROUP_STOP, PARAM_LEVEL_ADVANCED, OFF, 300, 800, 50, F(cool_c10), 1,
     "The heat exchanger temperature the cooldown runs down to."},
    {0x42, "Cooldown min", PARAM_UNIT_S, 0, PARAM_GROUP_STOP, PARAM_LEVEL_EXPERT, OFF, 30, 600, 10,
     F(cooldown_min_s), 0, "The shortest cooldown, however cool the heat exchanger reads."},
    {0x50, "Overheat derate", PARAM_UNIT_C, 1, PARAM_GROUP_OTHER, PARAM_LEVEL_EXPERT, OFF, 1500, 2500, 50,
     F(overheat_derate_c10), 1, "Above this the heater runs at its lowest power."},
    {0x51, "Overheat stop", PARAM_UNIT_C, 1, PARAM_GROUP_OTHER, PARAM_LEVEL_EXPERT, OFF, 1600, 2550, 50,
     F(overheat_stop_c10), 1, "Above this it stops. The board's own latch trips at about 270 C, whatever is set."},
    {0x52, "CO sensor fitted", PARAM_UNIT_NONE, 0, PARAM_GROUP_OTHER, PARAM_LEVEL_EXPERT, OFF, 0, 1, 1, F(co_fitted),
     0, "1 on a board with the CO sensor. 0 only on a board built without it: there is then no CO alarm."},
};
const size_t PARAM_COUNT = sizeof(PARAM_TABLE) / sizeof(PARAM_TABLE[0]);

const param_def *params_find(uint16_t id) {
  for (size_t i = 0; i < PARAM_COUNT; i++)
    if (PARAM_TABLE[i].id == id)
      return &PARAM_TABLE[i];
  return NULL;
}

static int32_t read_field(const burn_params *bp, const param_def *d) {
  const uint8_t *p = (const uint8_t *) bp + d->offset;
  if (d->size == 1)
    return d->is_signed ? (int32_t) * (const int8_t *) p : (int32_t) *p;
  int16_t s;
  uint16_t u;
  memcpy(&s, p, 2);
  memcpy(&u, p, 2);
  return d->is_signed ? (int32_t) s : (int32_t) u;
}

static void write_field(burn_params *bp, const param_def *d, int32_t v) {
  uint8_t *p = (uint8_t *) bp + d->offset;
  if (d->size == 1) {
    *p = (uint8_t) v;
  } else {
    uint16_t u = (uint16_t) v;
    memcpy(p, &u, 2);
  }
}

static int in_range(const param_def *d, int32_t v) {
  return v >= d->min && v <= d->max && (d->step <= 1 || (v - d->min) % d->step == 0);
}

/* Rules between parameters; 1 when they hold. */
static int consistent(const burn_params *p) {
  return p->pump_min_cHz <= p->pump_max_cHz && p->pump_start_cHz <= p->pump_max_cHz &&
         p->fan_min_rpm <= p->fan_max_rpm && p->overheat_derate_c10 < p->overheat_stop_c10 &&
         p->preheat_s < p->glow_attempt_max_s && p->fan_start_rpm > p->glow_min_fan_rpm;
}

void params_init(params *ps) {
  memset(ps, 0, sizeof(*ps));
  burn_params_default(&ps->live);
}

int32_t params_value(const params *ps, size_t index) { return read_field(&ps->live, &PARAM_TABLE[index]); }

static size_t index_of(uint16_t id) {
  for (size_t i = 0; i < PARAM_COUNT; i++)
    if (PARAM_TABLE[i].id == id)
      return i;
  return PARAM_COUNT;
}

int params_set(params *ps, uint16_t id, int32_t value) {
  size_t i = index_of(id);
  if (i == PARAM_COUNT || !in_range(&PARAM_TABLE[i], value))
    return LINK_OUT_OF_RANGE;
  ps->staged[i] = value;
  ps->staged_mask |= 1u << i;
  return LINK_STORED;
}

int params_apply(params *ps, int heater_off, int save) {
  if (ps->staged_mask == 0) {
    if (save)
      ps->save_pending = 1;
    return LINK_OK;
  }
  if (!heater_off)
    return LINK_NOT_OFF; /* every parameter here applies only with the heater off */
  burn_params next = ps->live;
  for (size_t i = 0; i < PARAM_COUNT; i++)
    if (ps->staged_mask & (1u << i))
      write_field(&next, &PARAM_TABLE[i], ps->staged[i]);
  if (!consistent(&next))
    return LINK_OUT_OF_RANGE; /* kept staged: the app can correct one and apply again */
  ps->live = next;
  ps->staged_mask = 0;
  if (save)
    ps->save_pending = 1;
  return LINK_OK;
}

void params_discard(params *ps) { ps->staged_mask = 0; }

void params_defaults(params *ps) {
  burn_params_default(&ps->live);
  ps->staged_mask = 0;
  ps->save_pending = 1;
}

/* ---------------------------------------------------------------- the flash image */

#define MAGIC0 'F'
#define MAGIC1 'P'
#define VERSION 1u

size_t params_pack(const params *ps, uint8_t *out, size_t cap) {
  size_t n = 4 + PARAM_COUNT * 6 + 2;
  if (cap < n)
    return 0;
  out[0] = MAGIC0, out[1] = MAGIC1, out[2] = VERSION, out[3] = (uint8_t) PARAM_COUNT;
  uint8_t *p = out + 4;
  for (size_t i = 0; i < PARAM_COUNT; i++) {
    uint32_t v = (uint32_t) params_value(ps, i);
    p[0] = (uint8_t) PARAM_TABLE[i].id, p[1] = (uint8_t) (PARAM_TABLE[i].id >> 8);
    p[2] = (uint8_t) v, p[3] = (uint8_t) (v >> 8), p[4] = (uint8_t) (v >> 16), p[5] = (uint8_t) (v >> 24);
    p += 6;
  }
  uint16_t crc = link_crc16(out, n - 2);
  p[0] = (uint8_t) crc, p[1] = (uint8_t) (crc >> 8);
  return n;
}

int params_unpack(params *ps, const uint8_t *in, size_t len) {
  if (len < 6 || in[0] != MAGIC0 || in[1] != MAGIC1 || in[2] != VERSION)
    return 0;
  size_t count = in[3], n = 4 + count * 6 + 2;
  if (len < n || link_crc16(in, n - 2) != (uint16_t) (in[n - 2] | (in[n - 1] << 8)))
    return 0;
  burn_params next = ps->live;
  const uint8_t *p = in + 4;
  for (size_t i = 0; i < count; i++, p += 6) {
    uint16_t id = (uint16_t) (p[0] | (p[1] << 8));
    int32_t v = (int32_t) ((uint32_t) p[2] | ((uint32_t) p[3] << 8) | ((uint32_t) p[4] << 16) | ((uint32_t) p[5] << 24));
    const param_def *d = params_find(id);
    if (d == NULL)
      continue; /* a parameter this build no longer has */
    if (!in_range(d, v))
      return 0;
    write_field(&next, d, v);
  }
  if (!consistent(&next))
    return 0;
  ps->live = next;
  return 1;
}
