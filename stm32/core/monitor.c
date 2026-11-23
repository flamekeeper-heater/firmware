/* The safety MCU's checks of itself: see monitor.h. */
#include "monitor.h"

#include <string.h>

void monitor_params_default(monitor_params *p, uint16_t clock_hz, uint32_t clock_wrap) {
  memset(p, 0, sizeof(*p));
  p->vdda_min_mv = 3000;
  p->vdda_max_mv = 3600;
  p->vdda_up_margin_mv = 50;
  /* Ten readings 1 ms apart, each its own VREFINT conversion: a stray one does not count, and the
   * fault still acts within one pass of the safety loop. PROVISIONAL. */
  p->vdda_bad_ms = 10;
  /* The 3.3 V converter's soft start is a few ms; a slow supply gets far more. PROVISIONAL. */
  p->vdda_rise_ms = 500;
  p->tick_check_ms = 1000;
  p->tick_tolerance_percent = 20;
  p->clock_hz = clock_hz;
  p->clock_wrap = clock_wrap;
}

void monitor_init(monitor *m, const monitor_params *p, uint32_t now) {
  memset(m, 0, sizeof(*m));
  m->p = *p;
  m->reset_ms = now;
  m->tick_permille = 1000;
}

static int in_window(const monitor *m, uint16_t mv) { return mv >= m->p.vdda_min_mv && mv <= m->p.vdda_max_mv; }

void monitor_vdda(monitor *m, uint32_t now, uint16_t mv) {
  const monitor_params *p = &m->p;
  int high = mv > p->vdda_max_mv, low = mv < p->vdda_min_mv;
  m->vdda_mv = mv;
  m->vdda_seen = 1;
  /* After a reset the rail may still be rising: a low reading holds the outputs off but is no
   * fault until the rail has been up, or the rise has taken too long. */
  if (!m->vdda_up && !high && mv >= p->vdda_min_mv + p->vdda_up_margin_mv)
    m->vdda_up = 1;
  /* A VDDA fault takes the place of a tick fault: it holds every output off. */
  int vdda_latched = m->fault == MONITOR_VDDA_HIGH || m->fault == MONITOR_VDDA_LOW;
  if (!m->vdda_up && !vdda_latched && since(now, m->reset_ms) >= p->vdda_rise_ms)
    m->fault = MONITOR_VDDA_LOW, vdda_latched = 1;
  int bad = high || (low && m->vdda_up);
  if (held_for(&m->vdda_bad, bad, now, p->vdda_bad_ms) && !vdda_latched)
    m->fault = high ? MONITOR_VDDA_HIGH : MONITOR_VDDA_LOW;
}

void monitor_clock(monitor *m, uint32_t now, uint32_t count) {
  const monitor_params *p = &m->p;
  uint32_t wrap = p->clock_wrap ? p->clock_wrap : 1u;
  count %= wrap;
  if (!m->clock_seen) {
    m->clock_seen = 1;
    m->clock_ms = now;
    m->clock_then = count;
    return;
  }
  uint32_t elapsed = since(now, m->clock_ms);
  if (elapsed < p->tick_check_ms)
    return;
  /* The second clock's count over the tick's elapsed time, against its nominal rate. */
  uint32_t counts = count >= m->clock_then ? count - m->clock_then : count + wrap - m->clock_then;
  uint64_t expect = (uint64_t) elapsed * p->clock_hz / 1000u;
  if (expect == 0)
    expect = 1;
  uint64_t permille = (uint64_t) counts * 1000u / expect;
  m->tick_permille = (uint16_t) (permille > 0xFFFFu ? 0xFFFFu : permille);
  m->tick_bad = (uint64_t) counts * 100u < expect * (100u - p->tick_tolerance_percent) ||
                (uint64_t) counts * 100u > expect * (100u + p->tick_tolerance_percent);
  m->tick_checks++;
  if (m->tick_bad && m->fault == MONITOR_OK)
    m->fault = MONITOR_TICK;
  m->clock_ms = now;
  m->clock_then = count;
}

int monitor_clear(monitor *m) {
  if (m->fault == MONITOR_VDDA_HIGH || m->fault == MONITOR_VDDA_LOW) {
    if (m->vdda_up && in_window(m, m->vdda_mv)) {
      /* A tick check that failed meanwhile (a VDDA fault holds the place) takes it now. */
      m->fault = m->tick_bad ? MONITOR_TICK : MONITOR_OK;
      m->vdda_bad.on = 0;
    }
  } else if (m->fault == MONITOR_TICK && !m->tick_bad) {
    m->fault = MONITOR_OK;
  }
  return m->fault == MONITOR_OK;
}
