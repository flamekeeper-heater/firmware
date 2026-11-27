/* Link codec, parser, server dispatch, start-number rule and dead-man, byte for byte against
 * DESIGN.md. */
#include <stdio.h>
#include <string.h>

#include "../link.h"
#include "../../test/check.h"

static void test_crc(void) {
  /* CRC16-Modbus check value. */
  const uint8_t s[] = "123456789";
  CHECK_EQ(link_crc16(s, 9), 0x4B37);
}

static int feed_all(link_parser *p, const uint8_t *data, size_t n, uint8_t *frame, size_t *frame_len) {
  int found = 0;
  for (size_t i = 0; i < n; i++)
    if (link_parser_feed(p, data[i])) {
      found++;
      *frame_len = link_frame_len(p->buf);
      memcpy(frame, p->buf, *frame_len);
    }
  return found;
}

static void test_frames(void) {
  uint8_t f[LINK_FRAME_MAX], got[LINK_FRAME_MAX];
  const uint8_t pl[] = {1, 2, 3};
  size_t n = link_build_request(9, LINK_OP_PARAM_SET, pl, 3, f);
  CHECK_EQ(n, 9);
  CHECK_EQ(f[0], 0x7A);
  CHECK_EQ(f[1], 7); /* len counts the bytes before the CRC */
  CHECK_EQ(f[2], 9);
  CHECK_EQ(f[3], 0x22);
  uint16_t crc = link_crc16(f, 7);
  CHECK_EQ(f[7], crc >> 8); /* high byte first */
  CHECK_EQ(f[8], crc & 0xFF);
  CHECK_EQ(link_build_request(1, 1, pl, LINK_REQUEST_PAYLOAD_MAX + 1, f), 0);

  link_parser p;
  link_parser_init(&p, LINK_REQUEST);
  size_t len = 0;
  n = link_build_request(9, LINK_OP_PARAM_SET, pl, 3, f);
  CHECK_EQ(feed_all(&p, f, n, got, &len), 1);
  CHECK_EQ(len, n);
  CHECK_EQ(link_request_payload_len(got), 3);
  CHECK(memcmp(link_request_payload(got), pl, 3) == 0);

  /* Noise, a broken frame and a fake header before a good one: resynchronised. */
  uint8_t stream[3 * LINK_FRAME_MAX];
  size_t k = 0;
  stream[k++] = 0x00;
  stream[k++] = 0x7A; /* a header with a silly length */
  stream[k++] = 0xFF;
  memcpy(stream + k, f, n);
  stream[k + 4] ^= 0x01; /* bad CRC */
  k += n;
  memcpy(stream + k, f, n);
  k += n;
  link_parser_init(&p, LINK_REQUEST);
  CHECK_EQ(feed_all(&p, stream, k, got, &len), 1);
  CHECK(memcmp(got, f, n) == 0);
  CHECK(p.bad >= 2);

  /* Two frames back to back, and a reply parser ignores requests. */
  memcpy(stream, f, n);
  memcpy(stream + n, f, n);
  link_parser_init(&p, LINK_REQUEST);
  CHECK_EQ(feed_all(&p, stream, 2 * n, got, &len), 2);
  link_parser_init(&p, LINK_REPLY);
  CHECK_EQ(feed_all(&p, stream, 2 * n, got, &len), 0);

  /* Replies */
  n = link_build_reply(9, LINK_OP_POLL, LINK_NOT_OFF, pl, 3, f);
  CHECK_EQ(n, 10);
  link_parser_init(&p, LINK_REPLY);
  CHECK_EQ(feed_all(&p, f, n, got, &len), 1);
  CHECK_EQ(link_reply_result(got), 0x20);
  CHECK_EQ(link_reply_payload_len(got), 3);
}

static void test_records(void) {
  uint8_t b[LINK_FRAME_MAX];
  link_info i = {1, LINK_KIND_STM32, {0, 1, 2}, LINK_CAP_FAN_ONLY | LINK_CAP_RUN_SWITCH, 21,
                 LINK_RESET_WATCHDOG, LINK_CHECK_OPTION_BYTES, "Flamekeeper safety 0.1.2"};
  size_t n = link_encode_info(&i, b);
  CHECK_EQ(n, 12 + 24);
  CHECK_EQ(b[1], 2);
  CHECK_EQ(b[5], 0x01);
  CHECK_EQ(b[6], 0x40);
  link_info i2;
  CHECK(link_decode_info(b, n, &i2));
  CHECK(strcmp(i2.text, "Flamekeeper safety 0.1.2") == 0);
  CHECK_EQ(i2.capabilities, i.capabilities);
  CHECK_EQ(i2.checks, 1);
  CHECK(!link_decode_info(b, 11, &i2));

  link_desired d = {200, LINK_MODE_FAN, 55, -12, -500, LINK_WANT_UPDATING};
  CHECK_EQ(link_encode_desired(&d, b), 7);
  CHECK_EQ(b[3], 0xF4);               /* -12 */
  CHECK_EQ(b[4] | (b[5] << 8), 0xFE0C); /* -500, little-endian */
  link_desired d2;
  CHECK(link_decode_desired(b, 9, &d2)); /* trailing bytes from a newer minor are ignored */
  CHECK_EQ(d2.fuel_trim, -12);
  CHECK_EQ(d2.fan_trim, -500);
  CHECK_EQ(d2.flags, LINK_WANT_UPDATING);
  CHECK(!link_decode_desired(b, 6, &d2));

  link_status s;
  link_status_unknown(&s);
  CHECK_EQ(s.supply, 0xFFFF);
  s.state = 3;
  s.heat_exchanger = -123;
  s.supply = 1240;
  s.run_time_s = 3600000;
  CHECK_EQ(s.log_seq, 0); /* no fault log entry */
  s.log_seq = 201;
  CHECK_EQ(link_encode_status(&s, b), 25);
  CHECK_EQ(b[8] | (b[9] << 8), 0xFF85);
  CHECK_EQ(b[24], 201); /* the fault log's newest entry, after the run time */
  link_status s2;
  CHECK(link_decode_status(b, 25, &s2));
  CHECK_EQ(s2.heat_exchanger, -123);
  CHECK_EQ(s2.run_time_s, 3600000);
  CHECK_EQ(s2.detail, 0xFF);
  CHECK_EQ(s2.log_seq, 201);
  CHECK(!link_decode_status(b, 24, &s2));

  link_detail t;
  link_detail_unknown(&t);
  t.fan_v = 390;
  t.co_state = 3;
  CHECK_EQ(link_encode_detail(&t, b), 21);
  link_detail t2;
  CHECK(link_decode_detail(b, 21, &t2));
  CHECK_EQ(t2.co_state, 3);
  CHECK(!link_decode_detail(b, 20, &t2));
  CHECK_EQ(t2.fan_v, 390);
  CHECK_EQ(t2.co_ppm, 0xFFFF);
  CHECK_EQ(t2.board, (int16_t) 0x8000);

  link_param_desc pd = {3, 30, 0x110, 2, 5, 1, -50, 300, 5, 1, 2, "Thermostat band, 24 char"};
  n = link_encode_param_desc(&pd, b);
  CHECK_EQ(n, 21 + 24); /* names are at most 24 bytes */
  CHECK_EQ(b[19], 1);   /* group, then level, before the name */
  CHECK_EQ(b[20], 2);
  link_param_desc pd2;
  CHECK(link_decode_param_desc(b, n, &pd2));
  CHECK_EQ(pd2.group, 1);
  CHECK_EQ(pd2.level, 2);
  CHECK_EQ(pd2.min, -50);
  CHECK_EQ(pd2.id, 0x110);
  CHECK_EQ(strlen(pd2.name), 24);

  /* A fault log entry: 14 bytes, little-endian. */
  link_fault_entry fe = {LINK_LOG_FAULT, 6, 5, 6, 77, -12, 2513, 2380, 0x01020304};
  CHECK_EQ(link_encode_fault_entry(&fe, b), LINK_FAULT_ENTRY_SIZE);
  CHECK_EQ(LINK_FAULT_ENTRY_SIZE, 14);
  const uint8_t fe_bytes[14] = {1, 6, 5, 6, 77, 0xF4, 0xD1, 0x09, 0x4C, 0x09, 4, 3, 2, 1};
  CHECK(memcmp(b, fe_bytes, 14) == 0);
  link_fault_entry fe2;
  CHECK(link_decode_fault_entry(b, 14, &fe2));
  CHECK(fe2.kind == fe.kind && fe2.code == fe.code && fe2.why == fe.why && fe2.step == fe.step);
  CHECK(fe2.seq == fe.seq && fe2.board_c == -12 && fe2.hx == 2513 && fe2.supply == 2380);
  CHECK_EQ(fe2.uptime_s, 0x01020304);
  CHECK(!link_decode_fault_entry(b, 13, &fe2));
  /* A page of them fills a reply: the total, then four entries. */
  CHECK_EQ(1 + LINK_FAULT_LOG_PAGE * LINK_FAULT_ENTRY_SIZE, LINK_REPLY_PAYLOAD_MAX);

  link_param_value pv = {7, LINK_PARAM_KNOWN | LINK_PARAM_STAGED, 60, -70};
  CHECK_EQ(link_encode_param_value(&pv, b), 11);
  link_param_value pv2;
  CHECK(link_decode_param_value(b, 11, &pv2));
  CHECK_EQ(pv2.staged, -70);
}

/* A small MCU for the server dispatch. */
typedef struct {
  link_desired last;
  int polls, sets, applies, defaults;
  int32_t value;
} fake_mcu;

static void f_info(void *c, link_info *o) {
  (void) c;
  o->kind = LINK_KIND_EMULATOR;
  o->param_count = 1;
  strcpy(o->text, "fake");
}
static void f_poll(void *c, const link_desired *w, link_status *o) {
  fake_mcu *m = (fake_mcu *) c;
  m->last = *w;
  m->polls++;
  o->state = 1;
  o->supply = 1230;
}
static int f_new_plug(void *c) {
  (*(int *) c)++;
  return LINK_OK;
}

static int f_describe(void *c, uint8_t index, link_param_desc *o) {
  (void) c;
  if (index != 0)
    return LINK_OUT_OF_RANGE;
  o->index = 0;
  o->count = 1;
  o->id = 7;
  o->max = 100;
  strcpy(o->name, "Preheat");
  return LINK_OK;
}
/* 120 bytes: three pieces of 53, 53 and 14. */
static const char HELP[] =
    "How long the glow plug heats before fuel starts. Longer helps cold starts; shorter saves battery. And more text here!!!!";
static int f_help(void *c, uint16_t id, const char **text) {
  (void) c;
  if (id == 8) {
    *text = "";
    return LINK_OK;
  }
  if (id != 7)
    return LINK_OUT_OF_RANGE;
  *text = HELP;
  return LINK_OK;
}
static int f_set(void *c, uint16_t id, int32_t v) {
  fake_mcu *m = (fake_mcu *) c;
  if (id != 7)
    return LINK_OUT_OF_RANGE;
  m->sets++;
  m->value = v;
  return LINK_OK;
}
static int f_apply(void *c, uint8_t save) {
  ((fake_mcu *) c)->applies += save ? 10 : 1;
  return LINK_OK;
}
static int f_defaults(void *c) {
  ((fake_mcu *) c)->defaults++;
  return LINK_OK;
}

/* A fault log of `n` entries, seq 1..n (n newest). */
typedef struct {
  uint8_t n, clears;
} fake_log;
static uint8_t f_log(void *c, uint8_t from, link_fault_entry *out, uint8_t *total) {
  fake_log *l = (fake_log *) c;
  *total = l->n;
  uint8_t got = 0;
  for (unsigned i = from; i < l->n && got < LINK_FAULT_LOG_PAGE; i++, got++) {
    memset(&out[got], 0, sizeof(out[got]));
    out[got].kind = LINK_LOG_FAULT;
    out[got].seq = (uint8_t) (l->n - i);
  }
  return got;
}
static int f_log_clear(void *c) {
  fake_log *l = (fake_log *) c;
  l->n = 0;
  l->clears++;
  return LINK_OK;
}

static size_t roundtrip(const link_mcu *m, uint8_t op, const uint8_t *pl, size_t n, uint8_t *reply) {
  uint8_t req[LINK_FRAME_MAX];
  link_build_request(42, op, pl, n, req);
  size_t len = link_server_handle(m, req, reply);
  CHECK_EQ(link_frame_seq(reply), 42);
  CHECK_EQ(link_frame_op(reply), op);
  CHECK_EQ(reply[0], 0x7B);
  CHECK_EQ(len, link_frame_len(reply));
  return len;
}

static void test_server(void) {
  fake_mcu fm;
  memset(&fm, 0, sizeof(fm));
  link_mcu m;
  memset(&m, 0, sizeof(m));
  m.ctx = &fm;
  m.info = f_info;
  m.poll = f_poll;
  m.param_describe = f_describe;
  m.param_set = f_set;
  m.param_apply = f_apply;
  m.param_defaults = f_defaults;
  m.param_help = f_help;
  uint8_t r[LINK_FRAME_MAX];

  const uint8_t ver[1] = {1};
  roundtrip(&m, LINK_OP_HELLO, ver, 1, r);
  CHECK_EQ(link_reply_result(r), LINK_OK);
  link_info info;
  CHECK(link_decode_info(link_reply_payload(r), link_reply_payload_len(r), &info));
  CHECK_EQ(info.major, LINK_MAJOR); /* filled in by the server */
  CHECK_EQ(info.kind, LINK_KIND_EMULATOR);
  CHECK(strcmp(info.text, "fake") == 0);

  uint8_t want[7];
  link_desired d = {5, LINK_MODE_HEAT, 40, 0, 0, 0};
  link_encode_desired(&d, want);
  roundtrip(&m, LINK_OP_POLL, want, 7, r);
  CHECK_EQ(fm.polls, 1);
  CHECK_EQ(fm.last.power, 40);
  link_status st;
  CHECK(link_decode_status(link_reply_payload(r), link_reply_payload_len(r), &st));
  CHECK_EQ(st.supply, 1230);
  CHECK_EQ(st.fan_rpm, 0xFFFF); /* not set by the MCU: unknown */
  roundtrip(&m, LINK_OP_POLL, want, 6, r);
  CHECK_EQ(link_reply_result(r), LINK_BAD_LENGTH);

  /* DETAIL with no callback: every value unknown. */
  roundtrip(&m, LINK_OP_DETAIL, NULL, 0, r);
  link_detail dt;
  CHECK(link_decode_detail(link_reply_payload(r), link_reply_payload_len(r), &dt));
  CHECK_EQ(dt.glow_v, 0xFFFF);

  const uint8_t idx0[1] = {0}, idx1[1] = {1};
  roundtrip(&m, LINK_OP_PARAM_DESCRIBE, idx0, 1, r);
  link_param_desc pd;
  CHECK(link_decode_param_desc(link_reply_payload(r), link_reply_payload_len(r), &pd));
  CHECK(strcmp(pd.name, "Preheat") == 0);
  roundtrip(&m, LINK_OP_PARAM_DESCRIBE, idx1, 1, r);
  CHECK_EQ(link_reply_result(r), LINK_OUT_OF_RANGE);
  CHECK_EQ(link_reply_payload_len(r), 0);

  /* PARAM_HELP in pieces. */
  CHECK_EQ(strlen(HELP), LINK_PARAM_HELP_MAX);
  CHECK_EQ(LINK_PARAM_HELP_CHUNK, 53);
  {
    char got[LINK_PARAM_HELP_MAX + 1];
    size_t have = 0;
    int pieces = 0;
    for (;;) {
      const uint8_t q[3] = {7, 0, (uint8_t) have};
      roundtrip(&m, LINK_OP_PARAM_HELP, q, 3, r);
      CHECK_EQ(link_reply_result(r), LINK_OK);
      const uint8_t *pl = link_reply_payload(r);
      size_t pn = link_reply_payload_len(r);
      CHECK_EQ(pl[0], 7);
      CHECK_EQ(pl[2], have);
      CHECK_EQ(pl[3], LINK_PARAM_HELP_MAX);
      memcpy(got + have, pl + 4, pn - 4);
      have += pn - 4;
      pieces++;
      if (have >= pl[3] || pn == 4 || pieces > 5)
        break;
    }
    got[have] = 0;
    CHECK_EQ(pieces, 3);
    CHECK(strcmp(got, HELP) == 0);
  }
  {
    const uint8_t past[3] = {7, 0, 121}, none[3] = {8, 0, 0}, unknown[3] = {9, 0, 0};
    roundtrip(&m, LINK_OP_PARAM_HELP, past, 3, r);
    CHECK_EQ(link_reply_result(r), LINK_OUT_OF_RANGE);
    roundtrip(&m, LINK_OP_PARAM_HELP, none, 3, r);
    CHECK_EQ(link_reply_result(r), LINK_OK);
    CHECK_EQ(link_reply_payload_len(r), 4);
    CHECK_EQ(link_reply_payload(r)[3], 0);
    roundtrip(&m, LINK_OP_PARAM_HELP, unknown, 3, r);
    CHECK_EQ(link_reply_result(r), LINK_OUT_OF_RANGE);
    roundtrip(&m, LINK_OP_PARAM_HELP, none, 2, r);
    CHECK_EQ(link_reply_result(r), LINK_BAD_LENGTH);
  }

  const uint8_t set[6] = {7, 0, 0x2C, 0x01, 0, 0}; /* id 7, 300 */
  roundtrip(&m, LINK_OP_PARAM_SET, set, 6, r);
  CHECK_EQ(link_reply_result(r), LINK_OK);
  CHECK_EQ(fm.value, 300);
  const uint8_t two[1] = {2}, save[1] = {1};
  roundtrip(&m, LINK_OP_PARAM_APPLY, two, 1, r);
  CHECK_EQ(link_reply_result(r), LINK_OUT_OF_RANGE);
  roundtrip(&m, LINK_OP_PARAM_APPLY, save, 1, r);
  CHECK_EQ(fm.applies, 10);
  const uint8_t df[2] = {'D', 'F'}, xx[2] = {'D', 'X'};
  roundtrip(&m, LINK_OP_PARAM_DEFAULTS, xx, 2, r);
  CHECK_EQ(link_reply_result(r), LINK_REFUSED);
  CHECK_EQ(fm.defaults, 0);
  roundtrip(&m, LINK_OP_PARAM_DEFAULTS, df, 2, r);
  CHECK_EQ(fm.defaults, 1);

  /* Not provided by this MCU / unknown. */
  roundtrip(&m, LINK_OP_FAULT_RESET, NULL, 0, r);
  CHECK_EQ(link_reply_result(r), LINK_NOT_SUPPORTED);
  roundtrip(&m, LINK_OP_PARAM_GET, set, 2, r);
  CHECK_EQ(link_reply_result(r), LINK_NOT_SUPPORTED);
  roundtrip(&m, LINK_OP_GLOW_NEW_PLUG, NULL, 0, r);
  CHECK_EQ(link_reply_result(r), LINK_NOT_SUPPORTED);
  roundtrip(&m, 0x77, NULL, 0, r);
  CHECK_EQ(link_reply_result(r), LINK_UNKNOWN_OP);

  /* A new glow plug fitted: no payload. */
  int new_plugs = 0;
  link_mcu np;
  memset(&np, 0, sizeof(np));
  np.ctx = &new_plugs;
  np.glow_new_plug = f_new_plug;
  roundtrip(&np, LINK_OP_GLOW_NEW_PLUG, NULL, 0, r);
  CHECK_EQ(link_reply_result(r), LINK_OK);
  CHECK_EQ(new_plugs, 1);
  roundtrip(&np, LINK_OP_GLOW_NEW_PLUG, set, 1, r);
  CHECK_EQ(link_reply_result(r), LINK_BAD_LENGTH);
  CHECK_EQ(new_plugs, 1);

  /* The fault log: not provided by this MCU. */
  const uint8_t from0[1] = {0}, cl[2] = {'C', 'L'};
  roundtrip(&m, LINK_OP_FAULT_LOG, from0, 1, r);
  CHECK_EQ(link_reply_result(r), LINK_NOT_SUPPORTED);
  CHECK_EQ(link_reply_payload_len(r), 0);
  roundtrip(&m, LINK_OP_FAULT_LOG_CLEAR, cl, 2, r);
  CHECK_EQ(link_reply_result(r), LINK_NOT_SUPPORTED);

  /* A fault log of 6: pages of 4 newest first, then 2, then none. */
  {
    fake_log fl = {6, 0};
    link_mcu lm;
    memset(&lm, 0, sizeof(lm));
    lm.ctx = &fl;
    lm.fault_log = f_log;
    lm.fault_log_clear = f_log_clear;
    roundtrip(&lm, LINK_OP_FAULT_LOG, from0, 1, r);
    CHECK_EQ(link_reply_result(r), LINK_OK);
    CHECK_EQ(link_reply_payload_len(r), 1 + 4 * LINK_FAULT_ENTRY_SIZE);
    CHECK_EQ(link_reply_payload(r)[0], 6);
    link_fault_entry e;
    CHECK(link_decode_fault_entry(link_reply_payload(r) + 1, LINK_FAULT_ENTRY_SIZE, &e));
    CHECK_EQ(e.seq, 6);
    CHECK(link_decode_fault_entry(link_reply_payload(r) + 1 + 3 * LINK_FAULT_ENTRY_SIZE, LINK_FAULT_ENTRY_SIZE, &e));
    CHECK_EQ(e.seq, 3);
    const uint8_t from4[1] = {4}, from6[1] = {6}, from9[1] = {9};
    roundtrip(&lm, LINK_OP_FAULT_LOG, from4, 1, r);
    CHECK_EQ(link_reply_payload_len(r), 1 + 2 * LINK_FAULT_ENTRY_SIZE);
    CHECK(link_decode_fault_entry(link_reply_payload(r) + 1 + LINK_FAULT_ENTRY_SIZE, LINK_FAULT_ENTRY_SIZE, &e));
    CHECK_EQ(e.seq, 1);
    roundtrip(&lm, LINK_OP_FAULT_LOG, from6, 1, r);
    CHECK_EQ(link_reply_result(r), LINK_OK);
    CHECK_EQ(link_reply_payload_len(r), 1);
    roundtrip(&lm, LINK_OP_FAULT_LOG, from9, 1, r);
    CHECK_EQ(link_reply_payload_len(r), 1);
    roundtrip(&lm, LINK_OP_FAULT_LOG, NULL, 0, r);
    CHECK_EQ(link_reply_result(r), LINK_BAD_LENGTH);
    /* Clearing needs its two confirmation bytes, as PARAM_DEFAULTS does. */
    const uint8_t bad[2] = {'C', 'X'};
    roundtrip(&lm, LINK_OP_FAULT_LOG_CLEAR, bad, 2, r);
    CHECK_EQ(link_reply_result(r), LINK_REFUSED);
    roundtrip(&lm, LINK_OP_FAULT_LOG_CLEAR, cl, 1, r);
    CHECK_EQ(link_reply_result(r), LINK_BAD_LENGTH);
    CHECK_EQ(fl.clears, 0);
    roundtrip(&lm, LINK_OP_FAULT_LOG_CLEAR, cl, 2, r);
    CHECK_EQ(link_reply_result(r), LINK_OK);
    CHECK_EQ(fl.clears, 1);
    roundtrip(&lm, LINK_OP_FAULT_LOG, from0, 1, r);
    CHECK_EQ(link_reply_payload(r)[0], 0);
    CHECK_EQ(link_reply_payload_len(r), 1);
  }

  /* With the major checked: no POLL before a HELLO, nor after one of an unknown major. */
  uint8_t client_major = 0;
  m.client_major = &client_major;
  int polls = fm.polls;
  roundtrip(&m, LINK_OP_POLL, want, 7, r);
  CHECK_EQ(link_reply_result(r), LINK_NOT_SUPPORTED);
  const uint8_t ver2[1] = {2};
  roundtrip(&m, LINK_OP_HELLO, ver2, 1, r);
  roundtrip(&m, LINK_OP_POLL, want, 7, r);
  CHECK_EQ(link_reply_result(r), LINK_NOT_SUPPORTED);
  CHECK_EQ(fm.polls, polls);
  roundtrip(&m, LINK_OP_HELLO, ver, 1, r);
  roundtrip(&m, LINK_OP_POLL, want, 7, r);
  CHECK_EQ(link_reply_result(r), LINK_OK);
  CHECK_EQ(fm.polls, polls + 1);
}

static void test_start_gate(void) {
  link_start_gate g;
  link_start_gate_init(&g);
  /* After a reset the first number is only recorded, whatever the mode: an MCU reset never starts. */
  CHECK(!link_start_gate_check(&g, 7, LINK_MODE_HEAT));
  CHECK(!link_start_gate_check(&g, 7, LINK_MODE_HEAT)); /* repeated polls */
  CHECK(link_start_gate_check(&g, 8, LINK_MODE_HEAT));  /* a new start */
  CHECK(!link_start_gate_check(&g, 8, LINK_MODE_HEAT)); /* once */
  /* The heater stopped by itself: the same number does not start it again. */
  CHECK(!link_start_gate_check(&g, 8, LINK_MODE_FAN));
  /* A new number with mode off (the ESP32 re-synchronising) is recorded, not a start. */
  CHECK(!link_start_gate_check(&g, 9, LINK_MODE_OFF));
  CHECK(!link_start_gate_check(&g, 9, LINK_MODE_HEAT));
  CHECK(link_start_gate_check(&g, 10, LINK_MODE_PRIME));
  /* Wrapping */
  link_start_gate_init(&g);
  link_start_gate_check(&g, 255, LINK_MODE_OFF);
  CHECK(link_start_gate_check(&g, 0, LINK_MODE_HEAT));
}

static void test_deadman(void) {
  link_deadman d;
  link_deadman_init(&d);
  CHECK(!link_deadman_expired(&d, 100, 0));
  CHECK(link_deadman_expired(&d, 100, 1)); /* never heard and burning */
  link_deadman_feed(&d, 1000);
  CHECK(!link_deadman_expired(&d, 2999, 1));
  CHECK(link_deadman_expired(&d, 3000, 1));
  CHECK(!link_deadman_expired(&d, 99999, 0)); /* off: silence changes nothing */
  /* Fed a moment after the caller read the clock: just heard, not 49 days of silence (a stamp a few ms after
   * the read must not stop every run). */
  link_deadman_feed(&d, 5000);
  CHECK(!link_deadman_expired(&d, 4990, 1));
  CHECK(!link_deadman_expired(&d, 6999, 1));
  CHECK(link_deadman_expired(&d, 7000, 1));
  link_deadman_feed(&d, 0xFFFFFF00u);        /* millis() wrapping */
  CHECK(!link_deadman_expired(&d, 0x00000100u, 1));
  CHECK(link_deadman_expired(&d, 0x00000800u, 1));
}

int main(void) {
  test_crc();
  test_frames();
  test_records();
  test_server();
  test_start_gate();
  test_deadman();
  return check_report("link");
}
