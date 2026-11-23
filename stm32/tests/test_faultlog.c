/* The fault log (core/faultlog.h): what makes an entry, the ring, the reset entries, the clear,
 * the flash slots (torn ones skipped, the clear marker), and the log behind the safety core's link:
 * FAULT_LOG, FAULT_LOG_CLEAR and Status log_seq. */
#include <stdio.h>
#include <string.h>

#include "../core/faultlog.h"
#include "../core/safety.h"
#include "../../common/test/check.h"

static burn_inputs readings(int16_t hx, uint16_t supply, int16_t board) {
  burn_inputs in;
  memset(&in, 0, sizeof(in));
  in.hx_c10 = hx;
  in.supply_cv = supply;
  in.board_c10 = board;
  return in;
}

static link_fault_entry newest(const faultlog *l) {
  link_fault_entry e;
  memset(&e, 0, sizeof(e));
  CHECK_EQ(faultlog_read(l, 0, &e, 1), 1);
  return e;
}

static void test_notes(void) {
  faultlog l;
  faultlog_init(&l, 1000);
  burn_inputs in = readings(2513, 2380, 456);
  CHECK_EQ(l.count, 0);
  CHECK_EQ(faultlog_newest_seq(&l), 0);
  /* Nothing to note: no fault, no why. */
  faultlog_note(&l, 2000, 0, 0, BURN_STEP_RUNNING, &in);
  CHECK_EQ(l.count, 0);
  /* An overheat in the running step, 12.5 s after the reset. */
  faultlog_note(&l, 13500, BURN_FAULT_OVERHEAT, BURN_WHY_OVERHEAT_LIMIT, BURN_STEP_RUNNING, &in);
  CHECK_EQ(l.count, 1);
  link_fault_entry e = newest(&l);
  CHECK_EQ(e.kind, LINK_LOG_FAULT);
  CHECK_EQ(e.code, BURN_FAULT_OVERHEAT);
  CHECK_EQ(e.why, BURN_WHY_OVERHEAT_LIMIT);
  CHECK_EQ(e.step, BURN_STEP_RUNNING);
  CHECK_EQ(e.seq, 1);
  CHECK_EQ(e.hx, 2513);
  CHECK_EQ(e.supply, 2380);
  CHECK_EQ(e.board_c, 46); /* 45.6 C, rounded */
  CHECK_EQ(e.uptime_s, 12);
  CHECK_EQ(faultlog_newest_seq(&l), 1);
  CHECK_EQ(l.unsaved, 1);
  /* The same fault in the next passes (burn-off, cooldown): no new entry. */
  faultlog_note(&l, 13510, BURN_FAULT_OVERHEAT, BURN_WHY_OVERHEAT_LIMIT, BURN_STEP_BURN_OFF, &in);
  faultlog_note(&l, 60000, BURN_FAULT_OVERHEAT, BURN_WHY_OVERHEAT_LIMIT, BURN_STEP_COOLDOWN, &in);
  CHECK_EQ(l.count, 1);
  /* Another why for the same fault is news. */
  faultlog_note(&l, 60010, BURN_FAULT_OVERHEAT, BURN_WHY_BOARD_HOT, BURN_STEP_COOLDOWN, &in);
  CHECK_EQ(l.count, 2);
  /* A new start clears it; the same fault again is a new entry. */
  faultlog_note(&l, 70000, 0, 0, BURN_STEP_OFF, &in);
  faultlog_note(&l, 80000, BURN_FAULT_OVERHEAT, BURN_WHY_BOARD_HOT, BURN_STEP_RUNNING, &in);
  CHECK_EQ(l.count, 3);
  CHECK_EQ(newest(&l).seq, 3);
  /* A stop for a reason, without a fault: the dead-man. Unknown readings stay unknown. */
  faultlog_note(&l, 90000, 0, 0, BURN_STEP_OFF, &in);
  in = readings(BURN_UNKNOWN_TEMP, 1200, BURN_UNKNOWN_TEMP);
  faultlog_note(&l, 91000, BURN_FAULT_NONE, BURN_WHY_DEADMAN, BURN_STEP_WARM_UP, &in);
  e = newest(&l);
  CHECK_EQ(l.count, 4);
  CHECK(e.kind == LINK_LOG_FAULT && e.code == 0 && e.why == BURN_WHY_DEADMAN && e.step == BURN_STEP_WARM_UP);
  CHECK_EQ(e.hx, LINK_UNKNOWN_I16);
  CHECK_EQ(e.board_c, LINK_UNKNOWN_I8);
  CHECK_EQ(e.uptime_s, 90);
  /* Board temperatures beyond a byte are clamped; below zero they round the same way. */
  faultlog_note(&l, 92000, 0, 0, BURN_STEP_OFF, &in);
  in = readings(100, 1200, 2000);
  faultlog_note(&l, 92000, BURN_FAULT_FAN, BURN_WHY_FAN_STALL, BURN_STEP_SELF_TEST, &in);
  CHECK_EQ(newest(&l).board_c, 127);
  faultlog_note(&l, 92000, 0, 0, BURN_STEP_OFF, &in);
  in = readings(100, 1200, -456);
  faultlog_note(&l, 92000, BURN_FAULT_FAN, BURN_WHY_FAN_STALL, BURN_STEP_SELF_TEST, &in);
  CHECK_EQ(newest(&l).board_c, -46);
}

static void test_uptime(void) {
  /* Seconds since the reset, past the millisecond count's 49-day wrap. */
  faultlog l;
  faultlog_init(&l, 0xFFFFF000u);
  uint32_t t = 0xFFFFF000u;
  for (int i = 0; i < 820; i++) /* 8.2 s in passes of 10 ms, through the wrap */
    faultlog_tick(&l, t += 10u);
  CHECK_EQ(t, 0x00001008u);
  CHECK_EQ(l.uptime_s, 8);
  /* A now from before the last tick (a frame taken first) does not run the clock back. */
  faultlog_tick(&l, 0x00001000u);
  faultlog_tick(&l, 0x00001008u);
  CHECK_EQ(l.uptime_s, 8);
  faultlog_tick(&l, 0x00001008u + 800u);
  CHECK_EQ(l.uptime_s, 9);
}

static void test_resets(void) {
  faultlog l;
  faultlog_init(&l, 0);
  burn_inputs in = readings(1500, 2400, 300);
  /* A power-up with the heater cold: nothing to tell. */
  faultlog_reset(&l, LINK_RESET_POWER_ON, BURN_STEP_OFF, &in);
  CHECK_EQ(l.count, 0);
  /* A power-up with it hot (the supply went mid-run): an entry, with the cooldown it starts. */
  faultlog_reset(&l, LINK_RESET_POWER_ON, BURN_STEP_COOLDOWN, &in);
  CHECK_EQ(l.count, 1);
  link_fault_entry e = newest(&l);
  CHECK(e.kind == LINK_LOG_RESET && e.code == LINK_RESET_POWER_ON && e.why == 0 && e.step == BURN_STEP_COOLDOWN);
  CHECK(e.hx == 1500 && e.supply == 2400 && e.board_c == 30 && e.uptime_s == 0);
  /* The watchdog: an entry, once while nothing else happens in between. */
  faultlog_reset(&l, LINK_RESET_WATCHDOG, BURN_STEP_OFF, &in);
  faultlog_reset(&l, LINK_RESET_WATCHDOG, BURN_STEP_OFF, &in);
  faultlog_reset(&l, LINK_RESET_WATCHDOG, BURN_STEP_COOLDOWN, &in);
  CHECK_EQ(l.count, 2);
  faultlog_note(&l, 0, BURN_FAULT_CO_SENSOR, BURN_WHY_CO_SENSOR, BURN_STEP_OFF, &in);
  faultlog_reset(&l, LINK_RESET_WATCHDOG, BURN_STEP_OFF, &in);
  faultlog_reset(&l, LINK_RESET_SOFTWARE, BURN_STEP_OFF, &in);
  faultlog_reset(&l, LINK_RESET_PIN, BURN_STEP_OFF, &in);
  faultlog_reset(&l, LINK_RESET_UNKNOWN, BURN_STEP_OFF, &in);
  CHECK_EQ(l.count, 7);
  CHECK_EQ(newest(&l).code, LINK_RESET_UNKNOWN);
}

static void fill(faultlog *l, unsigned n) {
  burn_inputs in = readings(200, 1250, 250);
  for (unsigned i = 0; i < n; i++) {
    faultlog_note(l, 0, BURN_FAULT_FLAME_OUT, BURN_WHY_FLAME_LOST, BURN_STEP_RUNNING, &in);
    faultlog_note(l, 0, 0, 0, BURN_STEP_OFF, &in);
  }
}

static void test_ring_and_seq(void) {
  faultlog l;
  faultlog_init(&l, 0);
  fill(&l, 40);
  CHECK_EQ(l.count, FAULTLOG_SIZE);
  CHECK_EQ(faultlog_newest_seq(&l), 40);
  link_fault_entry e[8];
  CHECK_EQ(faultlog_read(&l, 0, e, 4), 4);
  CHECK(e[0].seq == 40 && e[1].seq == 39 && e[3].seq == 37);
  CHECK_EQ(faultlog_read(&l, 30, e, 4), 2);
  CHECK(e[0].seq == 10 && e[1].seq == 9); /* the oldest kept: 40 - 32 + 1 */
  CHECK_EQ(faultlog_read(&l, 32, e, 4), 0);
  CHECK_EQ(faultlog_read(&l, 255, e, 4), 0);
  /* The unsaved count never exceeds what the log still holds. */
  CHECK_EQ(l.unsaved, FAULTLOG_SIZE);
  CHECK_EQ(faultlog_unsaved_slots(&l), FAULTLOG_SIZE);
  /* seq wraps 255 to 1: 0 means an empty log. */
  fill(&l, 215);
  CHECK_EQ(faultlog_newest_seq(&l), 255);
  fill(&l, 1);
  CHECK_EQ(faultlog_newest_seq(&l), 1);
}

static void test_clear(void) {
  faultlog l;
  faultlog_init(&l, 0);
  fill(&l, 3);
  faultlog_saved(&l);
  faultlog_clear(&l);
  CHECK_EQ(l.count, 0);
  CHECK_EQ(faultlog_newest_seq(&l), 0);
  link_fault_entry e;
  CHECK_EQ(faultlog_read(&l, 0, &e, 1), 0);
  /* One slot to write: the marker, carrying the last seq so the numbers go on after a reset. */
  CHECK_EQ(faultlog_unsaved_slots(&l), 1);
  uint8_t slot[FAULTLOG_SLOT];
  faultlog_unsaved_slot(&l, 0, slot);
  CHECK_EQ(slot[0], FAULTLOG_CLEARED);
  CHECK_EQ(slot[4], 3);
  fill(&l, 1);
  CHECK_EQ(faultlog_newest_seq(&l), 4);
  CHECK_EQ(faultlog_unsaved_slots(&l), 2);
  faultlog_unsaved_slot(&l, 1, slot);
  CHECK(slot[0] == LINK_LOG_FAULT && slot[4] == 4);
}

static void test_slots(void) {
  /* Entries noted, written as slots, loaded by a fresh log after a reset. */
  faultlog a;
  faultlog_init(&a, 0);
  fill(&a, 5);
  burn_inputs in = readings(-405, 1180, -50);
  faultlog_reset(&a, LINK_RESET_WATCHDOG, BURN_STEP_OFF, &in);
  uint8_t flash[64][FAULTLOG_SLOT];
  unsigned n = faultlog_unsaved_slots(&a);
  CHECK_EQ(n, 6);
  for (unsigned i = 0; i < n; i++)
    faultlog_unsaved_slot(&a, i, flash[i]);
  faultlog_saved(&a);
  CHECK_EQ(faultlog_unsaved_slots(&a), 0);
  /* The slot: the link's 14 bytes, then their CRC16, low byte first. */
  link_fault_entry e;
  CHECK(link_decode_fault_entry(flash[5], FAULTLOG_SLOT, &e));
  CHECK(e.kind == LINK_LOG_RESET && e.code == LINK_RESET_WATCHDOG && e.seq == 6 && e.hx == -405);
  uint16_t crc = link_crc16(flash[5], 14);
  CHECK(flash[5][14] == (uint8_t) crc && flash[5][15] == (uint8_t) (crc >> 8));

  faultlog b;
  faultlog_init(&b, 0);
  for (unsigned i = 0; i < n; i++)
    CHECK_EQ(faultlog_load_slot(&b, flash[i]), 1);
  CHECK_EQ(b.count, 6);
  CHECK_EQ(b.unsaved, 0);
  CHECK_EQ(faultlog_newest_seq(&b), 6);
  link_fault_entry x[6], y[6];
  CHECK_EQ(faultlog_read(&a, 0, x, 6), 6);
  CHECK_EQ(faultlog_read(&b, 0, y, 6), 6);
  for (unsigned i = 0; i < 6; i++) {
    uint8_t bx[14], by[14];
    link_encode_fault_entry(&x[i], bx);
    link_encode_fault_entry(&y[i], by);
    CHECK(memcmp(bx, by, 14) == 0);
  }
  /* The numbers go on from the last slot. */
  fill(&b, 1);
  CHECK_EQ(faultlog_newest_seq(&b), 7);

  /* A torn slot: the first double word written, the second still erased. Or a flipped bit. */
  uint8_t torn[FAULTLOG_SLOT];
  memcpy(torn, flash[2], FAULTLOG_SLOT);
  memset(torn + 8, 0xFF, 8);
  faultlog c;
  faultlog_init(&c, 0);
  CHECK_EQ(faultlog_load_slot(&c, torn), 0);
  memcpy(torn, flash[2], FAULTLOG_SLOT);
  torn[7] ^= 0x10;
  CHECK_EQ(faultlog_load_slot(&c, torn), 0);
  /* A kind that is neither, with a good CRC: skipped too. */
  memcpy(torn, flash[2], FAULTLOG_SLOT);
  torn[0] = 9;
  crc = link_crc16(torn, 14);
  torn[14] = (uint8_t) crc, torn[15] = (uint8_t) (crc >> 8);
  CHECK_EQ(faultlog_load_slot(&c, torn), 0);
  CHECK_EQ(c.count, 0);

  /* The clear marker: what came before goes, the numbers go on. */
  faultlog_clear(&b);
  uint8_t mark[FAULTLOG_SLOT];
  faultlog_unsaved_slot(&b, 0, mark);
  faultlog d;
  faultlog_init(&d, 0);
  for (unsigned i = 0; i < n; i++)
    faultlog_load_slot(&d, flash[i]);
  CHECK_EQ(faultlog_load_slot(&d, mark), 2);
  CHECK_EQ(d.count, 0);
  CHECK_EQ(faultlog_newest_seq(&d), 0);
  fill(&d, 1);
  CHECK_EQ(faultlog_newest_seq(&d), 8);

  /* More slots than the log holds: the newest 32 stay. */
  faultlog f;
  faultlog_init(&f, 0);
  fill(&f, 40);
  faultlog g;
  faultlog_init(&g, 0);
  for (unsigned i = 0; i < 40; i++) {
    uint8_t s[FAULTLOG_SLOT];
    if (i < 8) { /* the 8 oldest were in flash before; rebuild them as the ring lost them */
      faultlog h;
      faultlog_init(&h, 0);
      h.seq = (uint8_t) i;
      fill(&h, 1);
      faultlog_slot(&h, 0, s);
    } else {
      faultlog_slot(&f, i - 8, s);
    }
    CHECK_EQ(faultlog_load_slot(&g, s), 1);
  }
  CHECK_EQ(g.count, FAULTLOG_SIZE);
  CHECK_EQ(faultlog_newest_seq(&g), 40);
  link_fault_entry o;
  CHECK_EQ(faultlog_read(&g, FAULTLOG_SIZE - 1, &o, 1), 1);
  CHECK_EQ(o.seq, 9);
  /* faultlog_slot: the i-th oldest. */
  uint8_t s0[FAULTLOG_SLOT];
  faultlog_slot(&g, 0, s0);
  CHECK_EQ(s0[4], 9);
}

/* ---------------------------------------------------------------- behind the safety core's link */

static uint8_t seq_no = 0;
static size_t ask(safety *s, uint32_t now, uint8_t op, const uint8_t *pl, size_t n, uint8_t *reply) {
  uint8_t f[LINK_FRAME_MAX];
  size_t len = link_build_request(++seq_no, op, pl, n, f), r = 0;
  for (size_t i = 0; i < len; i++)
    r = safety_receive(s, f[i], now, reply);
  CHECK(r > 0);
  return r;
}

static link_status poll(safety *s, uint32_t now, uint8_t start, uint8_t mode) {
  link_desired d = {start, mode, 50, 0, 0, 0};
  uint8_t pl[LINK_DESIRED_SIZE], r[LINK_FRAME_MAX];
  link_encode_desired(&d, pl);
  ask(s, now, LINK_OP_POLL, pl, sizeof(pl), r);
  link_status st;
  memset(&st, 0, sizeof(st));
  CHECK(link_decode_status(link_reply_payload(r), link_reply_payload_len(r), &st));
  return st;
}

static void test_safety_link(void) {
  burn_inputs in = readings(200, 1250, 250);
  safety s;
  safety_init(&s, 0, &in, LINK_RESET_WATCHDOG, 0);
  safety_log_reset(&s);
  uint8_t r[LINK_FRAME_MAX];
  const uint8_t ver[2] = {LINK_MAJOR, LINK_MINOR};
  ask(&s, 10, LINK_OP_HELLO, ver, 2, r);
  link_info info;
  CHECK(link_decode_info(link_reply_payload(r), link_reply_payload_len(r), &info));
  CHECK(info.capabilities & LINK_CAP_FAULT_LOG);
  /* The reset's entry is there at once. */
  link_status st = poll(&s, 20, 5, LINK_MODE_OFF);
  CHECK_EQ(st.log_seq, 1);
  /* The overheat latch trips while off: an entry from that pass, in the off step. */
  in.overheat_latch = 1;
  in.supply_cv = 1180;
  safety_step(&s, 30, &in);
  st = poll(&s, 35, 5, LINK_MODE_OFF);
  CHECK_EQ(st.fault, BURN_FAULT_OVERHEAT);
  CHECK_EQ(st.log_seq, 2);
  const uint8_t from0[1] = {0};
  ask(&s, 40, LINK_OP_FAULT_LOG, from0, 1, r);
  CHECK_EQ(link_reply_result(r), LINK_OK);
  CHECK_EQ(link_reply_payload_len(r), 1 + 2 * LINK_FAULT_ENTRY_SIZE);
  CHECK_EQ(link_reply_payload(r)[0], 2);
  link_fault_entry e;
  CHECK(link_decode_fault_entry(link_reply_payload(r) + 1, LINK_FAULT_ENTRY_SIZE, &e));
  CHECK(e.kind == LINK_LOG_FAULT && e.code == BURN_FAULT_OVERHEAT && e.why == BURN_WHY_OVERHEAT_LATCH);
  CHECK(e.step == BURN_STEP_OFF && e.supply == 1180 && e.seq == 2);
  CHECK(link_decode_fault_entry(link_reply_payload(r) + 1 + LINK_FAULT_ENTRY_SIZE, LINK_FAULT_ENTRY_SIZE, &e));
  CHECK(e.kind == LINK_LOG_RESET && e.code == LINK_RESET_WATCHDOG && e.seq == 1);
  /* Still latched in the next passes, and a start refused for it: no new entry. */
  safety_step(&s, 50, &in);
  st = poll(&s, 55, 6, LINK_MODE_HEAT);
  safety_step(&s, 60, &in);
  st = poll(&s, 70, 6, LINK_MODE_OFF);
  CHECK_EQ(st.log_seq, 2);
  /* Cleared (with its confirmation): empty, and Status says so. */
  const uint8_t cl[2] = {'C', 'L'};
  ask(&s, 80, LINK_OP_FAULT_LOG_CLEAR, cl, 2, r);
  CHECK_EQ(link_reply_result(r), LINK_OK);
  st = poll(&s, 90, 7, LINK_MODE_OFF);
  CHECK_EQ(st.log_seq, 0);
  ask(&s, 100, LINK_OP_FAULT_LOG, from0, 1, r);
  CHECK_EQ(link_reply_payload_len(r), 1);
  CHECK_EQ(link_reply_payload(r)[0], 0);
}

static void test_safety_fault_reset(void) {
  /* A latched fault, reset while its cause is gone, then latched again at once: two entries. */
  burn_inputs in = readings(200, 1250, 250);
  safety s;
  safety_init(&s, 0, &in, LINK_RESET_POWER_ON, 0);
  safety_log_reset(&s);
  CHECK_EQ(s.log.count, 0);
  in.overheat_latch = 1;
  safety_step(&s, 10, &in);
  CHECK(s.burn.fault_latched && s.burn.fault == BURN_FAULT_OVERHEAT);
  CHECK_EQ(s.log.count, 1);
  in.overheat_latch = 0;
  safety_step(&s, 20, &in);
  uint8_t r[LINK_FRAME_MAX];
  ask(&s, 30, LINK_OP_FAULT_RESET, NULL, 0, r);
  CHECK_EQ(link_reply_result(r), LINK_OK);
  in.overheat_latch = 1;
  safety_step(&s, 40, &in);
  CHECK_EQ(s.log.count, 2);
}

int main(void) {
  test_notes();
  test_uptime();
  test_resets();
  test_ring_and_seq();
  test_clear();
  test_slots();
  test_safety_link();
  test_safety_fault_reset();
  return check_report("faultlog");
}
