/* The self-tests against a model of the board's interlocks: a healthy board passes both tests,
 * and every single fault the tests are there for is caught, by the right check. */
#include <stdio.h>
#include <string.h>

#include "../core/checks.h"

static int checks_n = 0, failures = 0;
#define CHECK(c)                                                  \
  do {                                                            \
    checks_n++;                                                   \
    if (!(c)) {                                                   \
      failures++;                                                 \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);         \
    }                                                             \
  } while (0)

/* The interlocks: the heartbeat detector, the overheat latch, the pump's and the plug's high
 * sides, and the faults that can break them. */
typedef struct {
  double vin;
  int16_t hx_c10;
  /* State. */
  int hb_running_ms, hb_stopped_ms; /* how long the heartbeat has run / been not running */
  int latch_armed;
  /* Faults. */
  int hb_ok_stuck;      /* the detector passes a static level */
  int latch_no_hold;    /* a tripped latch does not hold the pump off */
  int latch_broken;     /* the latch never re-arms */
  int pump_hs_short;    /* the pump's high side is on whatever its inputs */
  int pump_hs_dead;     /* ... never comes on */
  int pump_hs_flt;      /* ... reports a fault */
  int pump_coil_open;
  int glow_hs_short;    /* the plug's high side is on whatever its inputs */
  int glow_hb_via_open; /* HB_OK does not reach the glow high side's enable (GLW-22) */
  int glow_hs_dead;
  int glow_ls_stuck;    /* the plug's low side conducts with no pulses */
  /* What the test did. */
  uint32_t max_pulse_us, pulses;
  int glow_en_with_ls_current_ms;
} board;

static void board_init(board *b) {
  memset(b, 0, sizeof(*b));
  b->vin = 12.6;
  b->hx_c10 = 150;
  b->hb_stopped_ms = 1000;
}

static int hb_ok(const board *b) {
  if (b->hb_ok_stuck)
    return 1;
  return b->hb_running_ms >= 1; /* drops out within 3.5 ms of the last edge */
}

static int pump_on(const board *b) {
  if (b->pump_hs_short)
    return 1;
  if (b->pump_hs_dead || b->pump_hs_flt)
    return 0;
  return hb_ok(b) && (b->latch_armed || b->latch_no_hold);
}

static int glow_on(const board *b, int glow_en) {
  if (b->glow_hs_short)
    return 1;
  if (b->glow_hs_dead)
    return 0;
  return glow_en && (hb_ok(b) || b->glow_hb_via_open);
}

static checks_obs observe(const board *b, uint16_t pulse_us, int glow_en) {
  checks_obs o;
  o.vin_cv = (uint16_t) (b->vin * 100);
  o.pump_v_cv = (uint16_t) ((pump_on(b) ? b->vin : b->vin * 0.03) * 100);
  int g = glow_on(b, glow_en);
  o.glow_v_cv = (uint16_t) ((g ? b->vin : 0) * 100);
  o.glow_ca = (uint16_t) ((g && b->glow_ls_stuck) ? b->vin / 0.25 * 100 : 0);
  o.pump_peak_ma = (uint16_t) (pulse_us == 0 ? 0 : (pump_on(b) && !b->pump_coil_open) ? 400 : 4);
  o.latch_armed = (uint8_t) b->latch_armed;
  o.pump_hs_ok = (uint8_t) !b->pump_hs_flt;
  o.hx_c10 = b->hx_c10;
  return o;
}

/* Run a test to its end, one millisecond at a time. Returns its length in ms. */
static uint32_t run(checks *c, board *b, uint32_t *now, int power_up) {
  checks_obs o = observe(b, 0, 0);
  if (power_up)
    checks_begin_power_up(c, *now, &o);
  else
    checks_begin_pre_start(c, *now);
  uint32_t start = *now;
  uint16_t last_pulse = 0;
  int last_glow_en = 0;
  for (int ms = 0; ms < 600000 && checks_busy(c); ms++) {
    o = observe(b, last_pulse, last_glow_en);
    checks_out out = checks_step(c, *now, &o);
    if (out.heartbeat == CHECKS_HB_RUNNING)
      b->hb_running_ms++, b->hb_stopped_ms = 0;
    else
      b->hb_running_ms = 0, b->hb_stopped_ms++;
    if (out.latch_reset && !b->latch_broken && b->hx_c10 < 2700)
      b->latch_armed = 1;
    if (out.pump_pulse_us) {
      b->pulses++;
      if (out.pump_pulse_us > b->max_pulse_us)
        b->max_pulse_us = out.pump_pulse_us;
    }
    if (out.glow_en && glow_on(b, 1) && b->glow_ls_stuck)
      b->glow_en_with_ls_current_ms++;
    last_pulse = out.pump_pulse_us;
    last_glow_en = out.glow_en;
    (*now)++;
  }
  return *now - start;
}

static void setup(checks *c, board *b, uint32_t *now) {
  checks_params p;
  checks_params_default(&p);
  board_init(b);
  *now = 0;
  checks_init(c, &p, *now);
}

static void test_healthy(void) {
  checks c;
  board b;
  uint32_t now;
  setup(&c, &b, &now);
  uint32_t t = run(&c, &b, &now, 1);
  CHECK(c.failed == 0);
  CHECK(c.power_up_done);
  CHECK(b.latch_armed);
  CHECK(t >= 500 && t < 700);
  CHECK(b.max_pulse_us >= 100 && b.max_pulse_us <= 200);
  CHECK(b.pulses == 4); /* latch hold, static high, static low, running */
  b.pulses = 0;
  t = run(&c, &b, &now, 0);
  CHECK(c.failed == 0);
  CHECK(t >= 100 && t < 200);
  CHECK(b.pulses == 0); /* the pre-start test strokes nothing */
}

#define POWER_UP_WITH(field, want)          \
  do {                                      \
    checks c;                               \
    board b;                                \
    uint32_t now;                           \
    setup(&c, &b, &now);                    \
    b.field = 1;                            \
    run(&c, &b, &now, 1);                   \
    CHECK(c.failed & (want));               \
    CHECK(c.power_up_done);                 \
    if (!(c.failed & (want)))               \
      printf("  %s: failed 0x%x\n", #field, c.failed); \
  } while (0)

#define PRE_START_WITH(field, want)         \
  do {                                      \
    checks c;                               \
    board b;                                \
    uint32_t now;                           \
    setup(&c, &b, &now);                    \
    run(&c, &b, &now, 1);                   \
    CHECK(c.failed == 0);                   \
    b.field = 1;                            \
    run(&c, &b, &now, 0);                   \
    CHECK(c.failed == (want));              \
    if (c.failed != (want))                 \
      printf("  %s: failed 0x%x\n", #field, c.failed); \
  } while (0)

static void test_power_up_faults(void) {
  POWER_UP_WITH(latch_no_hold, CHECK_FAIL_LATCH_HOLD);
  POWER_UP_WITH(latch_broken, CHECK_FAIL_LATCH_REARM);
  POWER_UP_WITH(hb_ok_stuck, CHECK_FAIL_STATIC_HEARTBEAT | CHECK_FAIL_LATCH_HOLD);
  POWER_UP_WITH(pump_hs_flt, CHECK_FAIL_PUMP_HS_FAULT);
  POWER_UP_WITH(pump_coil_open, CHECK_FAIL_PUMP_NO_CURRENT);
  POWER_UP_WITH(pump_hs_dead, CHECK_FAIL_PUMP_NO_CURRENT);
}

static void test_pre_start_faults(void) {
  PRE_START_WITH(pump_hs_short, CHECK_FAIL_PUMP_HS_SHORT);
  PRE_START_WITH(glow_hs_short, CHECK_FAIL_GLOW_HS_SHORT);
  PRE_START_WITH(glow_hb_via_open, CHECK_FAIL_GLOW_HB_HOLD);
  PRE_START_WITH(pump_hs_dead, CHECK_FAIL_PUMP_HS_NO_OUTPUT);
  PRE_START_WITH(pump_hs_flt, CHECK_FAIL_PUMP_HS_FAULT);
  PRE_START_WITH(glow_hs_dead, CHECK_FAIL_GLOW_HS_NO_OUTPUT);
  PRE_START_WITH(glow_ls_stuck, CHECK_FAIL_GLOW_LS_SHORT);
}

static void test_stuck_low_side_drops_enable(void) {
  checks c;
  board b;
  uint32_t now;
  setup(&c, &b, &now);
  run(&c, &b, &now, 1);
  b.glow_ls_stuck = 1;
  run(&c, &b, &now, 0);
  CHECK(c.failed == CHECK_FAIL_GLOW_LS_SHORT);
  CHECK(b.glow_en_with_ls_current_ms <= 1); /* the enable drops within 1 ms (GLW-8) */
}

static void test_latch_already_armed(void) {
  checks c;
  board b;
  uint32_t now;
  setup(&c, &b, &now);
  b.latch_armed = 1; /* armed by a brown-out, or by the bootloader's pull-up (OVH-3) */
  run(&c, &b, &now, 1);
  CHECK(c.failed == 0);
  CHECK(b.pulses == 3); /* step 1 (the latch hold) cannot run */
}

static void test_sensor_and_heat(void) {
  checks c;
  board b;
  uint32_t now;
  /* An open sensor: the latch is not re-armed (it could never trip). */
  setup(&c, &b, &now);
  b.hx_c10 = (int16_t) 0x8000;
  run(&c, &b, &now, 1);
  CHECK(c.failed == CHECK_FAIL_LATCH_REARM);
  CHECK(!b.latch_armed);

  /* A reset while hot: the test waits for the heater to cool, then re-arms. */
  setup(&c, &b, &now);
  b.hx_c10 = 2500;
  checks_obs o = observe(&b, 0, 0);
  checks_begin_power_up(&c, now, &o);
  int armed_while_hot = 0;
  uint16_t last_pulse = 0;
  for (int ms = 0; ms < 120000; ms++, now++) {
    if (ms == 90000)
      b.hx_c10 = 900; /* cooled down */
    o = observe(&b, last_pulse, 0);
    checks_out out = checks_step(&c, now, &o);
    last_pulse = out.pump_pulse_us;
    if (out.heartbeat == CHECKS_HB_RUNNING)
      b.hb_running_ms++;
    else
      b.hb_running_ms = 0;
    if (out.latch_reset && !b.latch_broken)
      b.latch_armed = 1;
    if (ms < 90000 && b.latch_armed)
      armed_while_hot = 1;
    if (!checks_busy(&c))
      break;
  }
  CHECK(!armed_while_hot);
  CHECK(c.failed == 0);
  CHECK(b.latch_armed);
}

int main(void) {
  test_healthy();
  test_power_up_faults();
  test_pre_start_faults();
  test_stuck_low_side_drops_enable();
  test_latch_already_armed();
  test_sensor_and_heat();
  printf("checks: %d checks, %d failures\n", checks_n, failures);
  return failures ? 1 : 0;
}
