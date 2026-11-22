/* The heater parameter table: the table itself (ids, names, ranges, defaults), staging and
 * applying, the rules between parameters, and the flash image. */
#include <stdio.h>
#include <string.h>

#include "../core/params.h"
#include "../../common/link/link.h"

static int checks = 0, failures = 0;
#define CHECK(c)                                                  \
  do {                                                            \
    checks++;                                                     \
    if (!(c)) {                                                   \
      failures++;                                                 \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);         \
    }                                                             \
  } while (0)

static void test_table(void) {
  params ps;
  params_init(&ps);
  CHECK(PARAM_COUNT > 0 && PARAM_COUNT <= 32);
  for (size_t i = 0; i < PARAM_COUNT; i++) {
    const param_def *d = &PARAM_TABLE[i];
    CHECK(d->id != 0 && d->id < 0x100); /* the ESP32's own settings start at 0x100 */
    for (size_t j = 0; j < i; j++)
      CHECK(PARAM_TABLE[j].id != d->id);
    CHECK(strlen(d->name) <= LINK_PARAM_NAME_MAX);
    CHECK(strlen(d->help) > 0 && strlen(d->help) <= LINK_PARAM_HELP_MAX);
    CHECK(d->min <= d->max && d->step >= 1);
    int32_t v = params_value(&ps, i);
    CHECK(v >= d->min && v <= d->max && (v - d->min) % d->step == 0); /* defaults in range */
    if (!(v >= d->min && v <= d->max && (v - d->min) % d->step == 0))
      printf("  %s: default %d outside %d..%d step %d\n", d->name, v, d->min, d->max, d->step);
  }
  /* The ceilings the board rests on are not settings. */
  CHECK(params_find(0x02)->max == 600);  /* pump at most 6 Hz */
  CHECK(params_find(0x04)->max == 1000); /* priming at most 10 Hz */
  CHECK(params_find(0x30)->max == 90);   /* glow at most 90 W */
  CHECK(params_find(0x51)->max <= 2550); /* below the hardware latch */
  /* CO-1: the CO sensor's presence is this setting, fitted by default (an unset or damaged table
   * never loses the alarm), applied only with the heater off. */
  const param_def *co = params_find(0x52);
  CHECK(co != NULL && co->min == 0 && co->max == 1 && (co->flags & PARAM_FLAG_HEATER_OFF));
  CHECK(ps.live.co_fitted == 1);
}

static void test_stage_apply(void) {
  params ps;
  params_init(&ps);
  CHECK(params_set(&ps, 0x02, 450) == LINK_STORED);
  CHECK(ps.live.pump_max_cHz == 550); /* staged, not applied */
  CHECK(params_staged(&ps, (size_t) (params_find(0x02) - PARAM_TABLE)) == 450);
  /* Every value in range fits the staged 16 bits, signed or not. */
  for (size_t i = 0; i < PARAM_COUNT; i++) {
    params t;
    params_init(&t);
    const param_def *d = &PARAM_TABLE[i];
    CHECK(d->size <= 2 && params_set(&t, d->id, d->max) == LINK_STORED && params_staged(&t, i) == d->max);
    CHECK(params_set(&t, d->id, d->min) == LINK_STORED && params_staged(&t, i) == d->min);
  }
  CHECK(params_set(&ps, 0x02, 650) == LINK_OUT_OF_RANGE); /* above 6 Hz */
  CHECK(params_set(&ps, 0x02, 452) == LINK_OUT_OF_RANGE); /* not on a step */
  CHECK(params_set(&ps, 0x7777, 1) == LINK_OUT_OF_RANGE); /* no such parameter */
  CHECK(params_apply(&ps, 0, 0) == LINK_NOT_OFF);         /* the heater runs */
  CHECK(ps.live.pump_max_cHz == 550 && ps.staged_mask != 0);
  CHECK(params_apply(&ps, 1, 0) == LINK_OK);
  CHECK(ps.live.pump_max_cHz == 450 && ps.staged_mask == 0 && !ps.save_pending);

  /* Rules between parameters: a minimum above its maximum is refused, the change stays staged. */
  CHECK(params_set(&ps, 0x01, 400) == LINK_STORED); /* pump min 4 Hz > max 4.5? no: allowed */
  CHECK(params_set(&ps, 0x02, 300) == LINK_STORED); /* max 3 Hz: now min > max */
  CHECK(params_apply(&ps, 1, 1) == LINK_OUT_OF_RANGE);
  CHECK(ps.live.pump_min_cHz == 160 && ps.live.pump_max_cHz == 450);
  CHECK(params_set(&ps, 0x01, 200) == LINK_STORED);
  CHECK(params_apply(&ps, 1, 1) == LINK_OK);
  CHECK(ps.live.pump_min_cHz == 200 && ps.live.pump_max_cHz == 300 && ps.save_pending);

  CHECK(params_set(&ps, 0x50, 2500) == LINK_STORED); /* derate at the stop's level */
  CHECK(params_set(&ps, 0x51, 2500) == LINK_STORED);
  CHECK(params_apply(&ps, 1, 0) == LINK_OUT_OF_RANGE);
  params_discard(&ps);
  CHECK(ps.staged_mask == 0);
  CHECK(ps.live.overheat_stop_c10 == 2550);

  /* Signed values (temperatures) round-trip. */
  CHECK(params_set(&ps, 0x41, 400) == LINK_STORED);
  CHECK(params_apply(&ps, 1, 0) == LINK_OK);
  CHECK(ps.live.cool_c10 == 400);
  CHECK(params_value(&ps, (size_t) (params_find(0x41) - PARAM_TABLE)) == 400);

  CHECK(params_set(&ps, 0x52, 0) == LINK_STORED);
  CHECK(params_apply(&ps, 1, 0) == LINK_OK);
  CHECK(ps.live.co_fitted == 0);

  params_defaults(&ps);
  CHECK(ps.live.pump_max_cHz == 550 && ps.live.cool_c10 == 500 && ps.save_pending);
  CHECK(ps.live.co_fitted == 1);
}

static void test_image(void) {
  params a, b;
  params_init(&a);
  params_set(&a, 0x11, 5000);
  params_set(&a, 0x41, 600);
  params_set(&a, 0x22, 1);
  CHECK(params_apply(&a, 1, 1) == LINK_OK);
  uint8_t img[PARAMS_IMAGE_MAX];
  size_t n = params_pack(&a, img, sizeof(img));
  CHECK(n == 4 + PARAM_COUNT * 6 + 2);
  params_init(&b);
  CHECK(params_unpack(&b, img, n) == 1);
  CHECK(memcmp(&a.live, &b.live, sizeof(a.live)) == 0);

  /* A damaged image changes nothing. */
  img[10] ^= 0x40;
  params_init(&b);
  CHECK(params_unpack(&b, img, n) == 0);
  CHECK(b.live.fan_max_rpm == 4500);
  img[10] ^= 0x40;

  /* An image whose CRC is right but a value is out of range: refused whole. */
  uint8_t bad[PARAMS_IMAGE_MAX];
  memcpy(bad, img, n);
  bad[4 + 2] = 0xFF, bad[4 + 3] = 0xFF; /* the first value: 65535 */
  uint16_t crc = link_crc16(bad, n - 2);
  bad[n - 2] = (uint8_t) crc, bad[n - 1] = (uint8_t) (crc >> 8);
  params_init(&b);
  CHECK(params_unpack(&b, bad, n) == 0);
  CHECK(b.live.pump_min_cHz == 160);

  /* Blank flash. */
  memset(bad, 0xFF, sizeof(bad));
  CHECK(params_unpack(&b, bad, sizeof(bad)) == 0);
  CHECK(params_pack(&a, img, 10) == 0); /* too small a buffer */
}

int main(void) {
  test_table();
  test_stage_apply();
  test_image();
  printf("params: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
