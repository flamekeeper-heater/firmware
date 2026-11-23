/* The fault log: see faultlog.h. */
#include "faultlog.h"

#include <string.h>

void faultlog_init(faultlog *l, uint32_t now) {
  memset(l, 0, sizeof(*l));
  l->last_ms = now;
}

void faultlog_tick(faultlog *l, uint32_t now) {
  int32_t dt = (int32_t) (now - l->last_ms);
  if (dt <= 0)
    return; /* a frame's now taken before the last pass's: the clock does not go back */
  l->last_ms = now;
  l->ms_part += (uint32_t) dt;
  l->uptime_s += l->ms_part / 1000u;
  l->ms_part %= 1000u;
}

/* The i-th newest (0: the newest); i < count. */
static const link_fault_entry *nth(const faultlog *l, unsigned i) {
  return &l->e[(l->head + FAULTLOG_SIZE - 1u - i) % FAULTLOG_SIZE];
}

static int8_t whole_c(int16_t c10) {
  if (c10 == BURN_UNKNOWN_TEMP)
    return LINK_UNKNOWN_I8;
  int32_t c = c10 >= 0 ? (c10 + 5) / 10 : -((5 - c10) / 10);
  return (int8_t) (c > 127 ? 127 : c < -127 ? -127 : c);
}

static void add(faultlog *l, uint8_t kind, uint8_t code, uint8_t why, uint8_t step, const burn_inputs *in) {
  link_fault_entry *e = &l->e[l->head];
  l->seq = (uint8_t) (l->seq == 255u ? 1u : l->seq + 1u);
  e->kind = kind;
  e->code = code;
  e->why = why;
  e->step = step;
  e->seq = l->seq;
  e->board_c = whole_c(in->board_c10);
  e->hx = in->hx_c10;
  e->supply = in->supply_cv;
  e->uptime_s = l->uptime_s;
  l->head = (uint8_t) ((l->head + 1u) % FAULTLOG_SIZE);
  if (l->count < FAULTLOG_SIZE)
    l->count++;
  if (l->unsaved < FAULTLOG_SIZE)
    l->unsaved++;
}

void faultlog_note(faultlog *l, uint32_t now, uint8_t fault, uint8_t why, uint8_t step, const burn_inputs *in) {
  faultlog_tick(l, now);
  if (fault == 0 && why == 0) {
    l->fault = l->why = 0;
    return;
  }
  if (fault == l->fault && why == l->why)
    return;
  l->fault = fault;
  l->why = why;
  add(l, LINK_LOG_FAULT, fault, why, step, in);
}

void faultlog_reset(faultlog *l, uint8_t reason, uint8_t step, const burn_inputs *in) {
  if (reason == LINK_RESET_POWER_ON && step == BURN_STEP_OFF)
    return;
  if (l->count > 0 && nth(l, 0)->kind == LINK_LOG_RESET && nth(l, 0)->code == reason)
    return;
  add(l, LINK_LOG_RESET, reason, 0, step, in);
}

void faultlog_clear(faultlog *l) {
  l->head = l->count = 0;
  l->unsaved = 0;
  l->clear_unsaved = 1;
  l->clear_seq = l->seq;
}

uint8_t faultlog_read(const faultlog *l, uint8_t from, link_fault_entry *out, uint8_t max) {
  uint8_t n = 0;
  for (unsigned i = from; i < l->count && n < max; i++)
    out[n++] = *nth(l, i);
  return n;
}

uint8_t faultlog_newest_seq(const faultlog *l) { return l->count ? nth(l, 0)->seq : 0; }

/* ---------------------------------------------------------------- flash */

static void pack(const link_fault_entry *e, uint8_t slot[FAULTLOG_SLOT]) {
  link_encode_fault_entry(e, slot);
  uint16_t crc = link_crc16(slot, LINK_FAULT_ENTRY_SIZE);
  slot[14] = (uint8_t) crc;
  slot[15] = (uint8_t) (crc >> 8);
}

int faultlog_load_slot(faultlog *l, const uint8_t slot[FAULTLOG_SLOT]) {
  uint16_t crc = link_crc16(slot, LINK_FAULT_ENTRY_SIZE);
  if (slot[14] != (uint8_t) crc || slot[15] != (uint8_t) (crc >> 8))
    return 0;
  link_fault_entry e;
  link_decode_fault_entry(slot, FAULTLOG_SLOT, &e);
  if (e.kind == FAULTLOG_CLEARED) {
    l->head = l->count = 0;
    l->seq = e.seq;
    return 2;
  }
  if (e.kind != LINK_LOG_FAULT && e.kind != LINK_LOG_RESET)
    return 0;
  l->e[l->head] = e;
  l->head = (uint8_t) ((l->head + 1u) % FAULTLOG_SIZE);
  if (l->count < FAULTLOG_SIZE)
    l->count++;
  l->seq = e.seq;
  return 1;
}

unsigned faultlog_unsaved_slots(const faultlog *l) { return (unsigned) l->clear_unsaved + l->unsaved; }

void faultlog_unsaved_slot(const faultlog *l, unsigned i, uint8_t slot[FAULTLOG_SLOT]) {
  if (l->clear_unsaved) {
    if (i == 0) {
      /* The marker: the last seq given before the clear (the entries after it are newer). */
      link_fault_entry m;
      memset(&m, 0, sizeof(m));
      m.kind = FAULTLOG_CLEARED;
      m.seq = l->clear_seq;
      pack(&m, slot);
      return;
    }
    i--;
  }
  pack(nth(l, l->unsaved - 1u - i), slot);
}

void faultlog_saved(faultlog *l) { l->unsaved = 0, l->clear_unsaved = 0; }

void faultlog_slot(const faultlog *l, unsigned i, uint8_t slot[FAULTLOG_SLOT]) { pack(nth(l, l->count - 1u - i), slot); }
