/* The glow plug type check: every verdict, the cold definition, the remembered check, and the
 * flash record. Resistances are what the probe measures (supply / probe current). */
#include <stdio.h>
#include <string.h>

#include "../core/plug.h"

static int checks = 0, failures = 0;
#define CHECK(c)                                                  \
  do {                                                            \
    checks++;                                                     \
    if (!(c)) {                                                   \
      failures++;                                                 \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);         \
    }                                                             \
  } while (0)

#define MIN (60u * 1000u)
static const int16_t UNKNOWN = (int16_t) 0x8000;

/* The probe current for a plug of r_ohm on vin volts (0.02 Ohm of wiring in series). */
static uint16_t probe_ca(double vin, double r_ohm) { return (uint16_t) (vin / (r_ohm + 0.02) * 100 + 0.5); }

static uint8_t judge(plug *pl, uint32_t now, uint8_t setting, double vin, double r_ohm, int16_t hx, int16_t board) {
  plug_begin(pl, now, hx, board);
  return plug_judge(pl, setting, (uint16_t) (vin * 100), probe_ca(vin, r_ohm));
}

static void setup(plug *pl) {
  plug_params p;
  plug_params_default(&p);
  plug_init(pl, &p, 0);
}

static void test_cold_checks(void) {
  plug pl;
  setup(&pl);
  /* A cold 12 V plug, set as 12 V: go, and remembered. */
  CHECK(judge(&pl, 11 * MIN, 12, 12.6, 0.2, 100, 100) == PLUG_GO);
  CHECK(pl.last_cold);
  CHECK(pl.rec.verified_type == 12 && pl.rec.verified_setting == 12 && pl.save_pending);

  /* A 12 V plug set as 24 V on a 24 V supply: refused; on a 12 V supply: a warning. */
  setup(&pl);
  CHECK(judge(&pl, 11 * MIN, 24, 26.0, 0.2, 100, 100) == PLUG_REFUSE_TYPE);
  CHECK(pl.rec.verified_type == 0);
  CHECK(judge(&pl, 11 * MIN, 24, 12.6, 0.2, 100, 100) == PLUG_WARN);
  /* A 24 V plug set as 24 V: go. */
  CHECK(judge(&pl, 11 * MIN, 24, 26.0, 1.6, 100, 100) == PLUG_GO);
  CHECK(pl.rec.verified_type == 24);
  /* Outside every range: open, shorted, or between the two types. */
  setup(&pl);
  plug_begin(&pl, 11 * MIN, 100, 100);
  CHECK(plug_judge(&pl, 12, 1260, 0) == PLUG_REFUSE_RANGE);
  CHECK(judge(&pl, 11 * MIN, 12, 12.6, 0.95, 100, 100) == PLUG_REFUSE_RANGE);
  CHECK(judge(&pl, 11 * MIN, 24, 26.0, 3.5, 100, 100) == PLUG_REFUSE_RANGE);
}

static void test_cold_definition(void) {
  plug pl;
  setup(&pl);
  /* Right after a reset nothing is cold, whatever the temperatures (the count starts at 0). */
  CHECK(judge(&pl, 9 * MIN, 12, 12.6, 0.2, 100, 100) != PLUG_GO || !pl.last_cold);
  CHECK(!pl.last_cold);
  /* 10 minutes on, and the heat exchanger below 40 C: cold. */
  judge(&pl, 10 * MIN, 12, 12.6, 0.2, 390, UNKNOWN);
  CHECK(pl.last_cold);
  /* Warm heat exchanger, but within 10 K of a warm board (a hot day): cold. */
  judge(&pl, 10 * MIN, 12, 12.6, 0.2, 600, 520);
  CHECK(pl.last_cold);
  /* Warm heat exchanger well above the board: not cold. */
  judge(&pl, 10 * MIN, 12, 12.6, 0.2, 600, 300);
  CHECK(!pl.last_cold);
  /* Glow 9 minutes ago: not cold, however cool it reads. */
  plug_glow_seen(&pl, 20 * MIN);
  judge(&pl, 29 * MIN, 12, 12.6, 0.2, 100, 100);
  CHECK(!pl.last_cold);
  judge(&pl, 30 * MIN, 12, 12.6, 0.2, 100, 100);
  CHECK(pl.last_cold);
  /* An open heat-exchanger sensor never counts as cold. */
  judge(&pl, 60 * MIN, 12, 12.6, 0.2, UNKNOWN, 100);
  CHECK(!pl.last_cold);
}

static void test_warm_starts(void) {
  plug pl;
  setup(&pl);
  /* Never checked cold: refused on 24 V, a warning on 12 V. */
  CHECK(judge(&pl, 1 * MIN, 24, 26.0, 3.0, 1500, 300) == PLUG_REFUSE_UNVERIFIED);
  CHECK(judge(&pl, 1 * MIN, 12, 12.6, 0.9, 1500, 300) == PLUG_WARN);

  /* Checked cold as a 24 V plug: a warm 24 V plug (3 Ohm) goes. */
  CHECK(judge(&pl, 11 * MIN, 24, 26.0, 1.6, 100, 100) == PLUG_GO);
  plug_glow_seen(&pl, 11 * MIN);
  CHECK(judge(&pl, 12 * MIN, 24, 26.0, 3.0, 1500, 300) == PLUG_GO);
  /* ... but a reading below 1.3 Ohm does not fit a 24 V plug: someone fitted a 12 V one. */
  CHECK(judge(&pl, 12 * MIN, 24, 26.0, 1.0, 1500, 300) == PLUG_REFUSE_SANITY);
  /* The setting changed since the check: refused until a cold check against the new one. */
  CHECK(judge(&pl, 12 * MIN, 12, 26.0, 1.0, 1500, 300) == PLUG_REFUSE_UNVERIFIED);

  /* A remembered 12 V plug reading above 1.4 Ohm (a 24 V plug fitted): refused on 24 V. */
  setup(&pl);
  judge(&pl, 11 * MIN, 12, 12.6, 0.2, 100, 100);
  plug_glow_seen(&pl, 11 * MIN);
  CHECK(judge(&pl, 12 * MIN, 12, 26.0, 1.6, 1500, 300) == PLUG_REFUSE_SANITY);
  CHECK(judge(&pl, 12 * MIN, 12, 12.6, 1.6, 1500, 300) == PLUG_WARN); /* 12 V: a warning */
  CHECK(judge(&pl, 12 * MIN, 12, 26.0, 1.0, 1500, 300) == PLUG_GO);   /* hot 12 V plug: fits */
  /* An open plug never starts, warm or cold. */
  plug_begin(&pl, 12 * MIN, 1500, 300);
  CHECK(plug_judge(&pl, 12, 1260, 0) == PLUG_REFUSE_RANGE);

  /* A new plug fitted: the remembered check is gone. */
  plug_new_plug(&pl);
  CHECK(pl.rec.verified_type == 0 && pl.save_pending);
  CHECK(judge(&pl, 12 * MIN, 12, 26.0, 1.0, 1500, 300) == PLUG_REFUSE_UNVERIFIED);
}

static void test_record(void) {
  plug a, b;
  setup(&a);
  judge(&a, 11 * MIN, 24, 26.0, 1.6, 100, 100);
  uint8_t img[PLUG_IMAGE_SIZE];
  CHECK(plug_pack(&a, img, sizeof(img)) == PLUG_IMAGE_SIZE);
  setup(&b);
  CHECK(plug_unpack(&b, img, sizeof(img)) == 1);
  CHECK(b.rec.verified_type == 24 && b.rec.verified_setting == 24);
  img[3] ^= 1;
  setup(&b);
  CHECK(plug_unpack(&b, img, sizeof(img)) == 0);
  CHECK(b.rec.verified_type == 0);
  uint8_t blank[PLUG_IMAGE_SIZE];
  memset(blank, 0xFF, sizeof(blank));
  CHECK(plug_unpack(&b, blank, sizeof(blank)) == 0);
}

int main(void) {
  test_cold_checks();
  test_cold_definition();
  test_warm_starts();
  test_record();
  printf("plug: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
