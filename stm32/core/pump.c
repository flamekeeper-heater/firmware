/* The fuel pump's stroke driver: see pump.h. */
#include "pump.h"

#include <string.h>

void pump_params_default(pump_params *p) {
  memset(p, 0, sizeof(*p));
  p->stroke_ms = 38;
  p->stroke_v_cv = 1200;
  p->max_cHz = 600;
  p->prime_max_cHz = 1000;
  p->hs_arm_ms = 20;
  p->decay_ms = 10;
  p->gap_ma = 100;
  p->rise_ma = 300;
  p->rearm_ms = 10;
  p->oc_ma = 3300;
  p->oc_hold_us = 50;
}

void pump_init(pump *pu, const pump_params *p) {
  memset(pu, 0, sizeof(*pu));
  pu->p = *p;
}

void pump_clear_fault(pump *pu) {
  pu->fault = PUMP_FAULT_NONE;
  pu->stroking = 0;
  pu->scheduled = 0;
  pu->after_stroke = 0;
  pu->gap_high = 0;
  pu->rearms = 0;
  pu->rearming = 0;
}

void pump_new_run(pump *pu) { pu->rearms = 0; }

static uint16_t stroke_duty(const pump *pu, uint16_t vin_cv) { return duty_permille_for(pu->p.stroke_v_cv, vin_cv); }

static pump_out fault(pump *pu, uint8_t f) {
  pump_out o = {0, 1, 0};
  pu->fault = f;
  pu->stroking = 0;
  return o;
}

void pump_cut(pump *pu) {
  if (pu->fault == PUMP_FAULT_NONE)
    (void) fault(pu, PUMP_FAULT_OVERCURRENT);
}

static pump_out step(pump *pu, uint32_t now, uint16_t rate_cHz, int priming, const pump_meas *m) {
  const pump_params *p = &pu->p;
  pump_out out = {0, 0, 0};

  if (pu->fault != PUMP_FAULT_NONE) {
    out.hb_hold_off = 1; /* PMP-7, PMP-8: the heartbeat stays stopped */
    return out;
  }
  if (m->overcurrent)
    return fault(pu, PUMP_FAULT_OVERCURRENT);

  /* A high-side fault (PMP-9): one heartbeat restart per run clears it, a second locks out. */
  if (pu->rearming) {
    out.hb_hold_off = 1;
    if (since(now, pu->rearm_ms) >= p->rearm_ms)
      pu->rearming = 0;
    return out;
  }
  if (m->hb_running_ms >= p->hs_arm_ms && !m->hs_ok) {
    pu->stroking = 0;
    if (pu->rearms >= 1)
      return fault(pu, PUMP_FAULT_HIGH_SIDE);
    pu->rearms++;
    pu->rearming = 1;
    pu->rearm_ms = now;
    out.hb_hold_off = 1;
    return out;
  }

  /* Between strokes (PMP-8): the current is gone within decay_ms, and stays gone. */
  if (!pu->stroking) {
    if (pu->after_stroke) {
      if (since(now, pu->ended_ms) >= p->decay_ms) {
        pu->after_stroke = 0;
        if (m->i_ma > p->gap_ma)
          return fault(pu, PUMP_FAULT_LOW_SIDE); /* no extension past the deadline */
      }
    } else if (m->i_ma > p->gap_ma) {
      if (++pu->gap_high >= 2)
        return fault(pu, PUMP_FAULT_LOW_SIDE);
    } else {
      pu->gap_high = 0;
    }
  }

  /* A stroke under way: until its length, or at once when no fuel is wanted (PMP-10). */
  if (pu->stroking) {
    if (m->i_ma > pu->stroke_peak_ma)
      pu->stroke_peak_ma = m->i_ma;
    int full = since(now, pu->stroke_ms) >= p->stroke_ms;
    if (full || rate_cHz == 0) {
      pu->stroking = 0;
      pu->ended_ms = now;
      pu->after_stroke = 1;
      pu->gap_high = 0;
      if (full && pu->stroke_peak_ma < p->rise_ma)
        return fault(pu, PUMP_FAULT_OPEN_COIL); /* PMP-15 */
    } else {
      out.duty = stroke_duty(pu, m->vin_cv);
      return out;
    }
  }

  /* The next stroke, at the rate asked within the limit (PMP-4). */
  uint16_t limit = priming ? p->prime_max_cHz : p->max_cHz;
  if (rate_cHz > limit)
    rate_cHz = limit;
  if (rate_cHz == 0) {
    pu->scheduled = 0;
    return out;
  }
  if (!pu->scheduled) {
    pu->scheduled = 1;
    pu->next_ms = now;
  }
  int ready = m->hb_running_ms >= p->hs_arm_ms && m->hs_ok; /* PMP-2 */
  int decaying = pu->after_stroke;
  if (ready && !decaying && (int32_t) (now - pu->next_ms) >= 0) {
    pu->stroking = 1;
    pu->stroke_ms = now;
    pu->stroke_peak_ma = 0;
    pu->strokes++;
    /* Spaced from the stroke itself: a delayed stroke never shortens the next gap. */
    pu->next_ms = now + 100000u / rate_cHz;
    out.duty = stroke_duty(pu, m->vin_cv);
  }
  return out;
}

pump_out pump_ms(pump *pu, uint32_t now, uint16_t rate_cHz, int priming, const pump_meas *m) {
  pump_out out = step(pu, now, rate_cHz, priming, m);
  /* PMP-7: from a stroke's first pulse until decay_ms after its last. */
  out.oc_armed = (uint8_t) (pu->fault == PUMP_FAULT_NONE && (pu->stroking || pu->after_stroke));
  return out;
}
