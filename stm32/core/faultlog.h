/* The safety MCU's fault log (DESIGN.md): its last FAULTLOG_SIZE faults, stops for a reason (the
 * dead-man) and resets, newest first over the link (FAULT_LOG), and kept over resets by the board
 * layer in 16-byte flash slots. Plain C99, no hardware access; host-tested in ../tests/.
 *
 * Entries are the link's (link_fault_entry). A fault entry is noted when the sequencer's fault and
 * why change to something other than none: a run or a start that ended with a fault, a fault
 * latched while off, a fan stall that is retried, a dead-man stop. A fault and why already noted
 * makes no new entry until the burn has had neither in between (a new start, a fault reset), so
 * two latched causes the sequencer shows by turns make two entries, not one a pass. A
 * reset entry is noted at boot for every reset except a plain power-up with the heater cold, and
 * only once for the same reason in a row (a reset loop writes one slot, not one per reset).
 *
 * The RAM log is what the link reads at once. Flash takes it only with the heater off (the board's
 * rule for every write): the entries noted since the last write are unsaved until then, and a reset
 * before it loses them. A clear empties the RAM log at once and leaves a clear marker for flash, so
 * clearing costs one slot, not an erase. */
#ifndef FLAMEKEEPER_FAULTLOG_H
#define FLAMEKEEPER_FAULTLOG_H

#include <stdint.h>

#include "../../common/link/link.h"
#include "burn.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FAULTLOG_SIZE 32 /* entries kept, in RAM and over a page rewrite */
/* A flash slot: the entry's 14 link bytes and their CRC16 (link_crc16, low byte first). Erased:
 * all 0xFF. A slot whose kind is FAULTLOG_CLEARED is the clear marker: what came before it goes. */
#define FAULTLOG_SLOT 16u
#define FAULTLOG_CLEARED 0x43u /* 'C' */
#define FAULTLOG_SEEN 4         /* faults remembered as noted, until the burn has none */

typedef struct {
  link_fault_entry e[FAULTLOG_SIZE]; /* a ring: the newest at head - 1 */
  uint8_t head, count;
  uint8_t seq;           /* the last seq given out or loaded (0: none yet) */
  uint8_t unsaved;       /* the newest entries flash does not hold yet */
  uint8_t clear_unsaved; /* a clear flash does not hold yet ... */
  uint8_t clear_seq;     /* ... and the last seq given before it */
  uint16_t seen[FAULTLOG_SEEN]; /* fault << 8 | why noted since the burn last had neither ... */
  uint8_t seen_n;               /* ... how many (the oldest goes when full) */
  uint32_t last_ms, ms_part, uptime_s; /* the MCU's uptime, seconds (no 49-day wrap) */
} faultlog;

void faultlog_init(faultlog *l, uint32_t now);
/* The uptime follows now (every pass of the safety loop, through faultlog_note). */
void faultlog_tick(faultlog *l, uint32_t now);
/* After every call that may change the sequencer's fault or why: a new entry when they changed to
 * something other than none. step: the sequencer's step before that call; in: the readings now. */
void faultlog_note(faultlog *l, uint32_t now, uint8_t fault, uint8_t why, uint8_t step, const burn_inputs *in);
/* Once at boot, after the flash's entries are loaded: the reset's entry (LINK_RESET_*), unless it is
 * a power-up with the heater cold (step: the sequencer's after its start, BURN_STEP_COOLDOWN when the
 * heat exchanger was hot) or the newest entry is already a reset for the same reason. */
void faultlog_reset(faultlog *l, uint8_t reason, uint8_t step, const burn_inputs *in);
/* Empties the log (FAULT_LOG_CLEAR). */
void faultlog_clear(faultlog *l);

/* Up to max entries into out, from the from-th newest (0: the newest) towards the oldest. */
uint8_t faultlog_read(const faultlog *l, uint8_t from, link_fault_entry *out, uint8_t max);
/* The newest entry's seq, 0 when the log is empty (Status log_seq). */
uint8_t faultlog_newest_seq(const faultlog *l);

/* ---------------------------------------------------------------- flash (the board layer) */

/* One slot read at boot, in the order they were written: 1 an entry (added as saved), 2 the clear
 * marker (the log emptied), 0 not a slot (torn by a power loss, or anything else: skipped). */
int faultlog_load_slot(faultlog *l, const uint8_t slot[FAULTLOG_SLOT]);
/* The slots a flush writes: the clear marker if one is unsaved, then each unsaved entry, oldest
 * first. faultlog_unsaved_slot packs the i-th of them; faultlog_saved once they are written. */
unsigned faultlog_unsaved_slots(const faultlog *l);
void faultlog_unsaved_slot(const faultlog *l, unsigned i, uint8_t slot[FAULTLOG_SLOT]);
void faultlog_saved(faultlog *l);
/* The i-th oldest entry of the log as a slot (a page rewrite carries all count of them over). */
void faultlog_slot(const faultlog *l, unsigned i, uint8_t slot[FAULTLOG_SLOT]);

#ifdef __cplusplus
}
#endif

#endif /* FLAMEKEEPER_FAULTLOG_H */
