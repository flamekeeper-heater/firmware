/* The pump's overcurrent cut: see pump_oc.h. */
#include "pump_oc.h"

uint8_t pump_oc_need(uint16_t hold_us, uint16_t scan_us) {
  if (scan_us == 0)
    return 1;
  uint32_t n = (uint32_t) hold_us / scan_us;
  return (uint8_t) (n < 1u ? 1u : n > 255u ? 255u : n);
}

void pump_oc_arm(pump_oc *f, uint8_t need) {
  f->need = need ? need : 1u;
  f->above = 0;
  f->cut = 0;
}

int pump_oc_event(pump_oc *f, int high, int low) {
  if (f->cut)
    return 0;
  if (high) {
    if (f->above < 255u)
      f->above++;
    if (f->above >= f->need) {
      f->cut = 1;
      return 1;
    }
  } else if (low) {
    f->above = 0;
  }
  return 0;
}
