// Fuzz targets for the link (DESIGN.md, "Link to the safety MCU"), both ends:
//
//   0  the frame parser and link_server_handle() behind a scripted MCU whose callbacks return any
//      result and any field (texts too long, null callbacks);
//   1  the safety core itself (safety_receive) over a byte stream of noise, well-formed requests
//      of every op and length, and passes of the safety loop in between;
//   2  the ESP32's link client fed arbitrary replies to its own requests, and the record decoders.
//
// Checks beyond "no crash, no undefined behaviour" (ASan, UBSan): every request frame gets exactly
// one reply, no larger than a frame, that the other side's parser takes in one piece and whose
// payload decodes for its op; bytes that complete no frame, and frames the MCU refuses, change
// nothing in the safety core; a refused or malformed request never reaches an MCU callback; a frame
// survives any noise before it; the client's own requests are always valid frames.
//
// Built two ways. With libFuzzer (clang -fsanitize=fuzzer,address,undefined -DUSE_LIBFUZZER), the
// first input byte picks the target; `--write-corpus DIR` on the standalone build writes the seed
// corpus. Standalone (the default), a deterministic mutation driver runs seeds made from a real
// exchange between the link client and the safety core, then mutated inputs from a fixed seed:
//   fuzz_link [--iters N] [--seed S] [--write-corpus DIR]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "../../esp32/components/flamekeeper/link_client.h"

extern "C" {
#include "../core/safety.h"
}

using esphome::flamekeeper::LinkClient;
using esphome::flamekeeper::LinkTransport;
using esphome::flamekeeper::Mode;

#define STR2(x) #x
#define STR(x) STR2(x)
#define REQUIRE(c)                                                         \
  do {                                                                     \
    if (!(c)) {                                                            \
      std::fprintf(stderr, "fuzz_link: FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      std::abort();                                                        \
    }                                                                      \
  } while (0)

namespace {

// The input, read from the front; reads past the end give zeros.
struct In {
  const uint8_t *d;
  size_t n, i = 0;
  In(const uint8_t *data, size_t size) : d(data), n(size) {}
  bool more() const { return i < n; }
  uint8_t u8() { return i < n ? d[i++] : 0; }
  uint16_t u16() { return uint16_t(u8() | (u8() << 8)); }
  uint32_t u32() { return uint32_t(u16()) | (uint32_t(u16()) << 16); }
  void bytes(uint8_t *out, size_t len) {
    for (size_t k = 0; k < len; k++)
      out[k] = u8();
  }
};

const uint8_t KNOWN_OPS[] = {LINK_OP_HELLO,         LINK_OP_POLL,           LINK_OP_DETAIL,
                             LINK_OP_PARAM_DESCRIBE, LINK_OP_PARAM_GET,     LINK_OP_PARAM_SET,
                             LINK_OP_PARAM_APPLY,    LINK_OP_PARAM_DISCARD, LINK_OP_PARAM_DEFAULTS,
                             LINK_OP_PARAM_HELP,     LINK_OP_FAULT_RESET,   LINK_OP_GLOW_NEW_PLUG,
                             LINK_OP_UPDATE_PREPARE};

uint8_t pick_op(In &in) {
  uint8_t b = in.u8();
  if (b < 0xD0)  // mostly ops the MCU knows
    return KNOWN_OPS[b % sizeof(KNOWN_OPS)];
  return in.u8();
}

bool frame_valid(const uint8_t *f, uint8_t header) {
  size_t len = f[1];
  if (f[0] != header || len < (header == LINK_REPLY ? 5u : 4u) || len + 2 > LINK_FRAME_MAX)
    return false;
  uint16_t crc = link_crc16(f, len);
  return f[len] == uint8_t(crc >> 8) && f[len + 1] == uint8_t(crc);
}

// The ESP32 side's view of a reply to `req`: one frame, all of it, decodable for its op.
void check_reply(const uint8_t *req, const uint8_t *reply, size_t n) {
  REQUIRE(n >= LINK_REPLY_OVERHEAD && n <= LINK_FRAME_MAX);
  link_parser p;
  link_parser_init(&p, LINK_REPLY);
  for (size_t i = 0; i < n; i++) {
    int got = link_parser_feed(&p, reply[i]);
    REQUIRE(got == (i + 1 == n ? 1 : 0));
  }
  REQUIRE(link_frame_len(p.buf) == n && std::memcmp(p.buf, reply, n) == 0);
  REQUIRE(link_frame_seq(reply) == link_frame_seq(req));
  REQUIRE(link_frame_op(reply) == link_frame_op(req));
  const uint8_t result = link_reply_result(reply), *pl = link_reply_payload(reply);
  const size_t len = link_reply_payload_len(reply);
  if (result != LINK_OK) {
    REQUIRE(len == 0);  // refusals carry nothing
    return;
  }
  const uint8_t *q = link_request_payload(req);
  switch (link_frame_op(req)) {
    case LINK_OP_HELLO: {
      link_info info;
      REQUIRE(link_decode_info(pl, len, &info));
      REQUIRE(info.major == LINK_MAJOR);
      REQUIRE(std::strlen(info.text) <= LINK_TEXT_MAX);
      break;
    }
    case LINK_OP_POLL: {
      link_status st;
      REQUIRE(len == LINK_STATUS_SIZE && link_decode_status(pl, len, &st));
      break;
    }
    case LINK_OP_DETAIL: {
      link_detail d;
      REQUIRE(len == LINK_DETAIL_SIZE && link_decode_detail(pl, len, &d));
      break;
    }
    case LINK_OP_PARAM_DESCRIBE: {
      link_param_desc d;
      REQUIRE(link_decode_param_desc(pl, len, &d));
      REQUIRE(len <= LINK_PARAM_DESC_FIXED + LINK_PARAM_NAME_MAX);
      break;
    }
    case LINK_OP_PARAM_GET: {
      link_param_value v;
      REQUIRE(len == LINK_PARAM_VALUE_SIZE && link_decode_param_value(pl, len, &v));
      break;
    }
    case LINK_OP_PARAM_HELP: {
      REQUIRE(len >= LINK_PARAM_HELP_FIXED);
      REQUIRE(pl[0] == q[0] && pl[1] == q[1] && pl[2] == q[2]);  // id and offset echoed
      size_t off = pl[2], total = pl[3], chunk = len - LINK_PARAM_HELP_FIXED;
      REQUIRE(total <= LINK_PARAM_HELP_MAX && off <= total);
      size_t want = total - off < LINK_PARAM_HELP_CHUNK ? total - off : LINK_PARAM_HELP_CHUNK;
      REQUIRE(chunk == want);
      for (size_t i = 0; i < chunk; i++)
        REQUIRE(pl[LINK_PARAM_HELP_FIXED + i] != 0);
      break;
    }
    case LINK_OP_UPDATE_PREPARE:
      REQUIRE(len == 1);
      break;
    default:
      REQUIRE(len == 0);
      break;
  }
}

// A request the MCU must refuse before any callback runs: wrong length for a fixed-size op, or
// an op it does not know.
bool refused_by_shape(const uint8_t *req) {
  size_t n = link_request_payload_len(req);
  switch (link_frame_op(req)) {
    case LINK_OP_HELLO: return n != 2;
    case LINK_OP_POLL: return n < LINK_DESIRED_SIZE;
    case LINK_OP_DETAIL: return n != 0;
    case LINK_OP_PARAM_DESCRIBE: return n != 1;
    case LINK_OP_PARAM_GET: return n != 2;
    case LINK_OP_PARAM_SET: return n != 6;
    case LINK_OP_PARAM_APPLY: return n != 1 || link_request_payload(req)[0] > 1;
    case LINK_OP_PARAM_DISCARD: return n != 0;
    case LINK_OP_PARAM_DEFAULTS:
      return n != 2 || link_request_payload(req)[0] != 'D' || link_request_payload(req)[1] != 'F';
    case LINK_OP_PARAM_HELP: return n != 3;
    case LINK_OP_FAULT_RESET:
    case LINK_OP_GLOW_NEW_PLUG:
    case LINK_OP_UPDATE_PREPARE: return n != 0;
    default: return true;
  }
}

// Bytes for a record: a raw run, a well-formed request, a request with one byte broken, or noise
// then a request (the parser must still find it).
size_t make_request(In &in, uint8_t *frame) {
  uint8_t op = pick_op(in), seq = in.u8();
  size_t len;
  switch (op) {  // mostly the right length for the op
    case LINK_OP_HELLO: len = 2; break;
    case LINK_OP_POLL: len = LINK_DESIRED_SIZE; break;
    case LINK_OP_PARAM_DESCRIBE:
    case LINK_OP_PARAM_APPLY: len = 1; break;
    case LINK_OP_PARAM_GET:
    case LINK_OP_PARAM_DEFAULTS: len = 2; break;
    case LINK_OP_PARAM_SET: len = 6; break;
    case LINK_OP_PARAM_HELP: len = 3; break;
    default: len = 0; break;
  }
  uint8_t lb = in.u8();
  if (lb >= 0xC0)
    len = lb % (LINK_REQUEST_PAYLOAD_MAX + 1);
  uint8_t payload[LINK_REQUEST_PAYLOAD_MAX];
  in.bytes(payload, len);
  if (op == LINK_OP_PARAM_DEFAULTS && len == 2 && (lb & 1))
    payload[0] = 'D', payload[1] = 'F';
  if (op == LINK_OP_HELLO && len == 2 && (lb & 1))
    payload[0] = LINK_MAJOR;
  return link_build_request(seq, op, payload, len, frame);
}

// Occurrences of `f` in `s` that start before `before` and end after `after`.
size_t occurrences(const std::vector<uint8_t> &s, const uint8_t *f, size_t n, size_t before, size_t after) {
  size_t k = 0;
  for (size_t q = 0; q + n <= s.size(); q++)
    if (q < before && q + n > after && std::memcmp(s.data() + q, f, n) == 0)
      k++;
  return k;
}

// Liveness: noise then a frame then enough filler: the frame comes out (perhaps late, once a bogus
// length in the noise has run out), unless a valid frame the noise made up overlaps it. No frame
// comes out more often than it is in the stream.
void check_resync(In &in, uint8_t header) {
  std::vector<uint8_t> s;
  size_t noise = in.u8() % 80;
  for (size_t i = 0; i < noise; i++)
    s.push_back(in.u8());
  uint8_t frame[LINK_FRAME_MAX];
  size_t start = s.size(), n;
  uint8_t op = pick_op(in), len = uint8_t(in.u8() % (LINK_REPLY_PAYLOAD_MAX + 1)), payload[LINK_REQUEST_PAYLOAD_MAX];
  in.bytes(payload, len);
  n = header == LINK_REQUEST ? link_build_request(in.u8(), op, payload, len, frame)
                             : link_build_reply(in.u8(), op, in.u8(), payload, len, frame);
  REQUIRE(n > 0);
  s.insert(s.end(), frame, frame + n);
  size_t end = s.size();
  for (size_t i = 0; i < 2 * LINK_FRAME_MAX; i++)
    s.push_back(uint8_t(header ^ 0x80));  // filler that is never a header
  link_parser p;
  link_parser_init(&p, header);
  bool found = false, overlapped = false;
  std::vector<std::vector<uint8_t>> got;
  for (size_t i = 0; i < s.size(); i++) {
    if (!link_parser_feed(&p, s[i]))
      continue;
    REQUIRE(frame_valid(p.buf, header));
    size_t got_len = link_frame_len(p.buf);
    REQUIRE(occurrences(s, p.buf, got_len, i + 1, 0) > 0);  // it is in what came so far
    got.emplace_back(p.buf, p.buf + got_len);
    if (i + 1 >= end && got_len == n && std::memcmp(p.buf, frame, n) == 0)
      found = true;
    else if (occurrences(s, p.buf, got_len, end, start) > 0)
      overlapped = true;
  }
  for (const auto &g : got) {
    size_t times = 0;
    for (const auto &h : got)
      times += h == g;
    REQUIRE(times <= occurrences(s, g.data(), g.size(), s.size(), 0));  // never handed out twice
  }
  if (!(found || overlapped)) {
    std::fprintf(stderr, "header %02x start %zu end %zu n %zu:", header, start, end, n);
    for (size_t i = 0; i < end; i++)
      std::fprintf(stderr, " %02x", s[i]);
    std::fprintf(stderr, "\n");
  }
  REQUIRE(found || overlapped);
}

// ---------------------------------------------------------------- target 0: the server dispatch

struct Script {
  In *in;
  uint8_t client_major = 0;
  int calls = 0;
  char help[256];
};

int script_result(Script *s) {
  uint8_t b = s->in->u8();
  return b < 0xA0 ? LINK_OK : b;  // mostly OK, otherwise anything
}

void sc_info(void *c, link_info *o) {
  Script *s = static_cast<Script *>(c);
  s->calls++;
  o->kind = s->in->u8();
  s->in->bytes(o->version, 3);
  o->capabilities = s->in->u32();
  o->param_count = s->in->u8();
  size_t n = s->in->u8() % (LINK_TEXT_MAX + 1);
  for (size_t i = 0; i < n; i++)
    o->text[i] = char(s->in->u8() | 1);
  o->text[n] = 0;
}
void sc_poll(void *c, const link_desired *, link_status *o) {
  Script *s = static_cast<Script *>(c);
  s->calls++;
  s->in->bytes(reinterpret_cast<uint8_t *>(o), sizeof(*o));
}
void sc_detail(void *c, link_detail *o) {
  Script *s = static_cast<Script *>(c);
  s->calls++;
  s->in->bytes(reinterpret_cast<uint8_t *>(o), sizeof(*o));
}
int sc_describe(void *c, uint8_t, link_param_desc *o) {
  Script *s = static_cast<Script *>(c);
  s->calls++;
  o->id = s->in->u16();
  o->min = int32_t(s->in->u32());
  size_t n = s->in->u8() % (LINK_PARAM_NAME_MAX + 1);
  for (size_t i = 0; i < n; i++)
    o->name[i] = char(s->in->u8() | 1);
  o->name[n] = 0;
  return script_result(s);
}
int sc_get(void *c, uint16_t id, link_param_value *o) {
  Script *s = static_cast<Script *>(c);
  s->calls++;
  o->id = id;
  o->value = int32_t(s->in->u32());
  return script_result(s);
}
int sc_plain(void *c) {
  Script *s = static_cast<Script *>(c);
  s->calls++;
  return script_result(s);
}
int sc_set(void *c, uint16_t, int32_t) { return sc_plain(c); }
int sc_apply(void *c, uint8_t) { return sc_plain(c); }
int sc_help(void *c, uint16_t, const char **text) {
  Script *s = static_cast<Script *>(c);
  s->calls++;
  uint8_t mode = s->in->u8();
  if (mode >= 0xF0) {
    *text = nullptr;  // a careless MCU: the server treats it as no description
  } else {
    size_t n = s->in->u8();  // up to 255: longer than the link carries
    for (size_t i = 0; i < n; i++)
      s->help[i] = char('a' + i % 26);
    s->help[n] = 0;
    *text = s->help;
  }
  return script_result(s);
}
int sc_update(void *c, uint8_t *method) {
  Script *s = static_cast<Script *>(c);
  s->calls++;
  *method = s->in->u8();
  return script_result(s);
}

void target_server(In &in) {
  Script sc;
  sc.in = &in;
  link_mcu m;
  std::memset(&m, 0, sizeof(m));
  m.ctx = &sc;
  uint16_t present = in.u16();  // which callbacks this MCU has
  if (present & (1u << 0)) m.info = sc_info;
  if (present & (1u << 1)) m.poll = sc_poll;
  if (present & (1u << 2)) m.detail = sc_detail;
  if (present & (1u << 3)) m.param_describe = sc_describe;
  if (present & (1u << 4)) m.param_get = sc_get;
  if (present & (1u << 5)) m.param_set = sc_set;
  if (present & (1u << 6)) m.param_apply = sc_apply;
  if (present & (1u << 7)) m.param_discard = sc_plain;
  if (present & (1u << 8)) m.param_defaults = sc_plain;
  if (present & (1u << 9)) m.param_help = sc_help;
  if (present & (1u << 10)) m.fault_reset = sc_plain;
  if (present & (1u << 11)) m.glow_new_plug = sc_plain;
  if (present & (1u << 12)) m.update_prepare = sc_update;
  if (present & (1u << 13)) m.client_major = &sc.client_major;

  link_parser p;
  link_parser_init(&p, LINK_REQUEST);
  // The reply buffer on the heap at its exact size: any write past it is caught.
  uint8_t *reply = static_cast<uint8_t *>(std::malloc(LINK_FRAME_MAX));
  auto feed = [&](uint8_t byte) {
    if (!link_parser_feed(&p, byte))
      return;
    REQUIRE(frame_valid(p.buf, LINK_REQUEST));
    uint8_t req[LINK_FRAME_MAX];
    std::memcpy(req, p.buf, link_frame_len(p.buf));
    int calls = sc.calls;
    uint8_t major = sc.client_major;
    size_t n = link_server_handle(&m, req, reply);
    check_reply(req, reply, n);
    uint8_t result = link_reply_result(reply);
    if (refused_by_shape(req)) {
      REQUIRE(result != LINK_OK && result != LINK_STORED);
      REQUIRE(sc.calls == calls && sc.client_major == major);  // nothing reached the MCU
    }
    if (link_frame_op(req) == LINK_OP_POLL && m.client_major != nullptr && major != LINK_MAJOR) {
      REQUIRE(result == LINK_NOT_SUPPORTED && sc.calls == calls);  // no HELLO of our major yet
    }
  };
  while (in.more()) {
    uint8_t tag = in.u8();
    switch (tag % 5) {
      case 0: {
        size_t n = in.u8() % 24;
        for (size_t i = 0; i < n; i++)
          feed(in.u8());
        break;
      }
      case 1:
      case 2: {
        uint8_t f[LINK_FRAME_MAX];
        size_t n = make_request(in, f);
        if (tag & 0x80 && n > 0)
          f[in.u8() % n] ^= uint8_t(1u << (in.u8() % 8));  // one bit broken somewhere
        for (size_t i = 0; i < n; i++)
          feed(f[i]);
        break;
      }
      case 3:
        check_resync(in, LINK_REQUEST);
        break;
      default:
        check_resync(in, LINK_REPLY);
        break;
    }
  }
  std::free(reply);
}

// ---------------------------------------------------------------- target 1: the safety core

struct Core {
  safety s;
  uint32_t now = 0;
  int16_t hx = 200;
  uint16_t supply = 1260;
  int16_t board = 300;
  uint8_t latch = 0, co = 0;
  uint16_t rpm = 0;

  Core() {
    burn_params p;
    burn_params_default(&p);
    glow_params gp;
    glow_params_default(&gp);
    fan_params fp;
    fan_params_default(&fp);
    checks_params cp;
    checks_params_default(&cp);
    pump_params pp;
    pump_params_default(&pp);
    burn_inputs in = inputs();
    safety_init(&s, &p, &gp, &fp, &cp, &pp, now, &in, LINK_RESET_POWER_ON, 0);
  }
  burn_inputs inputs() const {
    burn_inputs in;
    std::memset(&in, 0, sizeof(in));
    in.hx_c10 = hx;
    in.fan_rpm = rpm;
    in.supply_cv = supply;
    in.overheat_latch = latch;
    in.co_alarm = co;
    in.board_c10 = board;
    return in;
  }
  // One 10 ms pass of a healthy board, the fan at whatever speed is asked.
  void pass() {
    for (uint32_t i = 0; i < 10; i++) {
      checks_obs o;
      std::memset(&o, 0, sizeof(o));
      o.vin_cv = supply;
      o.latch_armed = 1;
      o.pump_hs_ok = 1;
      o.hx_c10 = hx;
      o.pump_v_cv = supply;
      o.pump_peak_ma = 400;
      pump_meas pm;
      std::memset(&pm, 0, sizeof(pm));
      pm.vin_cv = supply;
      pm.hs_ok = 1;
      pm.i_ma = 400;
      safety_io io = safety_ms(&s, now + i, &o, &pm);
      if (io.pump_duty)  // no stroke outside a fuel step, nor without the heartbeat
        REQUIRE(io.heartbeat == CHECKS_HB_RUNNING && s.burn.out.pump_cHz > 0);
    }
    for (int k = 0; k < 2; k++) {
      glow_meas gm;
      std::memset(&gm, 0, sizeof(gm));
      gm.vin_cv = supply;
      gm.pulse_ca = 6000;
      gm.airflow_ok = rpm >= 1000;
      gm.board_c10 = board;
      glow_cmd c = safety_glow_period(&s, now + uint32_t(5 * k), &gm);
      if (c.width_us)
        REQUIRE(s.in.fan_rpm >= s.burn.p.glow_min_fan_rpm);
    }
    now += 10;
    burn_inputs in = inputs();
    safety_step(&s, now, &in);
    if (s.burn.out.glow_w)
      REQUIRE(in.fan_rpm >= s.burn.p.glow_min_fan_rpm);
    fan_meas fm = {rpm, supply, 0};
    safety_fan_step(&s, now, &fm);
    rpm = s.burn.out.fan_rpm;
  }
};

// The safety core with the parts a received byte may always touch put back: the parser, the
// dead-man (fed by any valid frame), the clock.
void normalised(const safety &s, safety &out) {
  std::memcpy(&out, &s, sizeof(out));
  std::memset(&out.parser, 0, sizeof(out.parser));
  std::memset(&out.deadman, 0, sizeof(out.deadman));
  out.now = 0;
}

void target_core(In &in) {
  Core *c = new Core;
  safety *before = new safety, *after = new safety;
  uint8_t *reply = static_cast<uint8_t *>(std::malloc(LINK_FRAME_MAX));
  link_parser mirror;  // what the MCU's parser should see
  link_parser_init(&mirror, LINK_REQUEST);
  uint8_t setup = in.u8();
  for (int i = 0; i < (setup & 1 ? 120 : 2); i++)  // with or without the power-up test done
    c->pass();
  auto feed = [&](uint8_t byte) {
    normalised(c->s, *before);
    int framed = link_parser_feed(&mirror, byte);
    size_t n = safety_receive(&c->s, byte, c->now, reply);
    REQUIRE((n > 0) == (framed == 1));
    if (n == 0) {
      normalised(c->s, *after);
      REQUIRE(std::memcmp(before, after, sizeof(safety)) == 0);  // noise changes nothing
      return;
    }
    check_reply(mirror.buf, reply, n);
    uint8_t result = link_reply_result(reply);
    if (refused_by_shape(mirror.buf))
      REQUIRE(result != LINK_OK && result != LINK_STORED);
    if (result != LINK_OK && result != LINK_STORED) {
      normalised(c->s, *after);
      REQUIRE(std::memcmp(before, after, sizeof(safety)) == 0);  // a refused request changes nothing
    }
  };
  while (in.more()) {
    uint8_t tag = in.u8();
    switch (tag % 8) {
      case 0: {
        size_t n = in.u8() % 24;
        for (size_t i = 0; i < n; i++)
          feed(in.u8());
        break;
      }
      case 1:
      case 2: {
        uint8_t f[LINK_FRAME_MAX];
        size_t n = make_request(in, f);
        if (tag & 0x80 && n > 0)
          f[in.u8() % n] ^= uint8_t(1u << (in.u8() % 8));
        for (size_t i = 0; i < n; i++)
          feed(f[i]);
        break;
      }
      case 3: {
        size_t n = 1 + in.u8() % 64;
        for (size_t i = 0; i < n; i++)
          c->pass();
        break;
      }
      case 4: {
        uint8_t what = in.u8();
        int16_t v = int16_t(in.u16());
        switch (what % 6) {
          case 0: c->hx = v; break;
          case 1: c->supply = uint16_t(v); break;
          case 2: c->board = v; break;
          case 3: c->latch = uint8_t(v & 1); break;
          case 4: c->co = uint8_t(v & 1); break;
          default: c->rpm = uint16_t(v); break;
        }
        break;
      }
      case 5: {
        size_t n = 1 + in.u8() % 4;  // seconds of passes
        for (size_t i = 0; i < n * 100; i++)
          c->pass();
        break;
      }
      case 6: {  // a minute of passes: a start gets through its preheat
        for (size_t i = 0; i < 6000; i++)
          c->pass();
        break;
      }
      default:
        break;
    }
  }
  std::free(reply);
  delete before;
  delete after;
  delete c;
}

// ---------------------------------------------------------------- target 2: the link client

struct Wire : LinkTransport {
  std::deque<uint8_t> rx;
  std::vector<std::vector<uint8_t>> sent;
  void send(const uint8_t *data, size_t len) override {
    // Every request the client sends is one valid frame.
    REQUIRE(len >= LINK_REQUEST_OVERHEAD && len <= LINK_FRAME_MAX);
    link_parser p;
    link_parser_init(&p, LINK_REQUEST);
    for (size_t i = 0; i < len; i++)
      REQUIRE(link_parser_feed(&p, data[i]) == (i + 1 == len ? 1 : 0));
    REQUIRE(!refused_by_shape(data));
    sent.emplace_back(data, data + len);
  }
  int read() override {
    if (rx.empty())
      return -1;
    int b = rx.front();
    rx.pop_front();
    return b;
  }
};

void decoders(In &in) {
  uint8_t buf[80];
  size_t n = in.u8() % sizeof(buf);
  in.bytes(buf, n);
  // The decoder reads only what it is given (an exact heap copy).
  uint8_t *h = static_cast<uint8_t *>(std::malloc(n ? n : 1));
  std::memcpy(h, buf, n);
  link_info info;
  if (link_decode_info(h, n, &info))
    REQUIRE(std::strlen(info.text) <= LINK_TEXT_MAX);
  link_desired d;
  link_decode_desired(h, n, &d);
  link_status st;
  link_decode_status(h, n, &st);
  link_detail de;
  link_decode_detail(h, n, &de);
  link_param_desc pd;
  if (link_decode_param_desc(h, n, &pd))
    REQUIRE(std::strlen(pd.name) <= LINK_PARAM_NAME_MAX);
  link_param_value pv;
  link_decode_param_value(h, n, &pv);
  std::free(h);
}

void target_client(In &in) {
  Wire w;
  LinkClient client(&w);
  uint32_t now = 1000;
  size_t answered = 0;
  while (in.more()) {
    client.loop(now);
    uint8_t tag = in.u8();
    // A reply to the latest request, as the MCU might send it.
    if (w.sent.size() > answered) {
      const std::vector<uint8_t> &req = w.sent.back();
      answered = w.sent.size();
      uint8_t kind = tag % 8;
      if (kind <= 4) {
        uint8_t payload[LINK_REPLY_PAYLOAD_MAX], result = in.u8();
        result = result < 0xC0 ? LINK_OK : result;
        size_t len;
        uint8_t op = link_frame_op(req.data());
        if (op == LINK_OP_HELLO && kind < 3) {
          link_info info;
          std::memset(&info, 0, sizeof(info));
          info.major = in.u8() < 0xF0 ? LINK_MAJOR : in.u8();
          info.kind = in.u8() & 3;
          info.capabilities = in.u32() | (kind == 0 ? LINK_CAP_PARAMETERS : 0);
          info.param_count = uint8_t(in.u8() % 40);
          size_t tn = in.u8() % (LINK_TEXT_MAX + 1);
          for (size_t i = 0; i < tn; i++)
            info.text[i] = char(in.u8() | 1);
          len = link_encode_info(&info, payload);
        } else if (op == LINK_OP_PARAM_HELP && kind < 3) {
          const uint8_t *q = link_request_payload(req.data());
          payload[0] = q[0], payload[1] = q[1];
          payload[2] = in.u8() < 0xE0 ? q[2] : in.u8();  // the offset asked for, mostly
          payload[3] = in.u8();                         // any total, shorter than asked included
          len = LINK_PARAM_HELP_FIXED + in.u8() % (LINK_PARAM_HELP_CHUNK + 1);
          for (size_t i = LINK_PARAM_HELP_FIXED; i < len; i++)
            payload[i] = uint8_t('a' + i % 26);
        } else if (op == LINK_OP_PARAM_DESCRIBE && kind < 3) {
          link_param_desc pd;
          std::memset(&pd, 0, sizeof(pd));
          pd.index = in.u8() < 0xE0 ? link_request_payload(req.data())[0] : in.u8();
          pd.id = uint16_t(in.u8() % 8);
          pd.flags = in.u8();
          pd.min = int32_t(in.u32()), pd.max = int32_t(in.u32());
          size_t nn = in.u8() % (LINK_PARAM_NAME_MAX + 1);
          for (size_t i = 0; i < nn; i++)
            pd.name[i] = char(in.u8() | 1);
          len = link_encode_param_desc(&pd, payload);
        } else if (op == LINK_OP_PARAM_GET && kind < 3) {
          link_param_value pv;
          pv.id = uint16_t(in.u8() % 8);
          pv.flags = in.u8();
          pv.value = int32_t(in.u32()), pv.staged = int32_t(in.u32());
          len = link_encode_param_value(&pv, payload);
        } else {
          len = in.u8() % (LINK_REPLY_PAYLOAD_MAX + 1);
          in.bytes(payload, len);
        }
        uint8_t seq = link_frame_seq(req.data());
        if (kind == 4)  // late or foreign
          seq = uint8_t(seq + 1 + in.u8() % 3), op = in.u8();
        uint8_t f[LINK_FRAME_MAX];
        size_t n = link_build_reply(seq, op, result, payload, len, f);
        REQUIRE(n > 0);
        uint8_t split = in.u8();
        for (size_t i = 0; i < n; i++) {
          w.rx.push_back(f[i]);
          if (split == i)  // a reply cut by noise
            w.rx.push_back(in.u8());
        }
      } else if (kind == 5) {
        size_t n = in.u8() % 24;
        for (size_t i = 0; i < n; i++)
          w.rx.push_back(in.u8());
      }  // else: no reply (a timeout)
    }
    // The user, now and then.
    uint8_t act = in.u8();
    switch (act % 16) {
      case 0: client.set_mode(Mode(in.u8() % 4)); break;
      case 1: client.set_power_demand(in.u8()); break;
      case 2: client.param_set(uint16_t(in.u8() % 8), int32_t(in.u32())); break;
      case 3: client.param_apply(in.u8() & 1); break;
      case 4: client.reset_fault(); break;
      case 5: client.glow_plug_fitted(); break;
      case 6: client.param_discard(); break;
      case 7: client.reset_parameters(); break;
      case 8: decoders(in); break;
      default: break;
    }
    now += 10 + in.u8() % 120;
    // What the client reports stays sane whatever it was told.
    esphome::flamekeeper::HeaterStatus hs;
    client.read_status(hs);
    REQUIRE(client.param_count() <= 255);
    for (size_t i = 0; i < client.param_count(); i++) {
      esphome::flamekeeper::ParamInfo pi;
      REQUIRE(client.param_info(i, pi));
      REQUIRE(std::strlen(client.param_help(pi.id)) <= LINK_PARAM_HELP_MAX);
      REQUIRE(std::strlen(pi.name) <= LINK_PARAM_NAME_MAX);
    }
    (void) client.param_status_text();
    (void) client.capabilities();
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size == 0)
    return 0;
  In in(data + 1, size - 1);
  switch (data[0] % 3) {
    case 0: target_server(in); break;
    case 1: target_core(in); break;
    default: target_client(in); break;
  }
  return 0;
}

#ifndef USE_LIBFUZZER

namespace {

struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
  uint32_t next() {
    s ^= s << 13, s ^= s >> 7, s ^= s << 17;
    return uint32_t(s >> 11);
  }
  uint32_t below(uint32_t n) { return n ? next() % n : 0; }
};

// Seeds from a real exchange: the link client against the safety core, with the user starting,
// changing and stopping the heater and its parameters. Requests become target 1 records (and
// target 0's), replies target 2's.
struct Capture : LinkTransport {
  safety s;
  uint32_t now = 0;
  std::vector<std::vector<uint8_t>> requests, replies;
  std::deque<uint8_t> to_esp;
  std::vector<uint8_t> pending;
  Capture() {
    burn_params p;
    burn_params_default(&p);
    glow_params gp;
    glow_params_default(&gp);
    fan_params fp;
    fan_params_default(&fp);
    checks_params cp;
    checks_params_default(&cp);
    pump_params pp;
    pump_params_default(&pp);
    burn_inputs in;
    std::memset(&in, 0, sizeof(in));
    in.hx_c10 = 200, in.supply_cv = 1260, in.board_c10 = 300;
    safety_init(&s, &p, &gp, &fp, &cp, &pp, 0, &in, LINK_RESET_POWER_ON, 0);
  }
  void send(const uint8_t *data, size_t len) override {
    requests.emplace_back(data, data + len);
    uint8_t reply[LINK_FRAME_MAX];
    for (size_t i = 0; i < len; i++) {
      size_t n = safety_receive(&s, data[i], now, reply);
      if (n) {
        replies.emplace_back(reply, reply + n);
        to_esp.insert(to_esp.end(), reply, reply + n);
      }
    }
  }
  int read() override {
    if (to_esp.empty())
      return -1;
    int b = to_esp.front();
    to_esp.pop_front();
    return b;
  }
};

std::vector<std::vector<uint8_t>> make_seeds() {
  Capture cap;
  LinkClient client(&cap);
  for (uint32_t t = 0; t < 20000; t += 10) {
    cap.now = t;
    if (t == 3000)
      client.set_mode(Mode::FAN);
    if (t == 4000)
      client.param_set(0x11, 5000), client.param_apply(true);
    if (t == 5000)
      client.set_mode(Mode::OFF);
    if (t == 6000)
      client.glow_plug_fitted(), client.reset_fault(), client.reset_parameters(), client.param_discard();
    if (t == 7000)
      client.set_mode(Mode::PRIME);
    client.loop(t);
  }
  std::vector<std::vector<uint8_t>> seeds;
  // Target 0 and 1: one record per captured request, in order; the whole stream raw, too.
  for (int target = 0; target < 2; target++) {
    std::vector<uint8_t> structured{uint8_t(target)}, raw{uint8_t(target)};
    if (target == 0)
      structured.push_back(0xFF), structured.push_back(0xFF), raw.push_back(0xFF), raw.push_back(0xFF);
    else
      structured.push_back(1), raw.push_back(1);
    size_t k = 0;
    for (const auto &r : cap.requests) {
      if (k++ % 7 != 0 && link_frame_op(r.data()) == LINK_OP_POLL)
        continue;  // fewer polls
      // tag 1: op (as a known-op index), seq, length marker, payload
      structured.push_back(1);
      uint8_t idx = 0;
      for (uint8_t i = 0; i < sizeof(KNOWN_OPS); i++)
        if (KNOWN_OPS[i] == link_frame_op(r.data()))
          idx = i;
      structured.push_back(idx);
      structured.push_back(link_frame_seq(r.data()));
      structured.push_back(0);
      structured.insert(structured.end(), r.begin() + 4, r.end() - 2);
      if (target == 1 && k % 5 == 0)
        structured.push_back(3), structured.push_back(50);  // half a second of passes
      raw.push_back(0);
      raw.push_back(uint8_t(r.size()));
      raw.insert(raw.end(), r.begin(), r.end());
    }
    seeds.push_back(structured);
    seeds.push_back(raw);
  }
  // Target 2: the captured replies, as answers to the client's requests in order.
  std::vector<uint8_t> client_seed{2};
  for (const auto &r : cap.replies) {
    client_seed.push_back(5 * 8 + 3);  // kind 3: a raw reply payload
    client_seed.push_back(link_reply_result(r.data()));
    size_t len = link_reply_payload_len(r.data());
    client_seed.push_back(uint8_t(len));
    client_seed.insert(client_seed.end(), r.begin() + 5, r.begin() + 5 + long(len));
    client_seed.push_back(0xFF);  // no split
    client_seed.push_back(15);    // no user action
    client_seed.push_back(0);
  }
  seeds.push_back(client_seed);
  for (int t = 0; t < 3; t++)
    seeds.push_back({uint8_t(t)});
  return seeds;
}

const uint8_t MAGIC[] = {LINK_REQUEST, LINK_REPLY, 0x00, 0xFF, 0x04, 0x05, 0x06, 0x0B, 0x3E, 0x3F, 0x40,
                         0x01, 0x02, 0x03, 0x20, 0x21, 0x22, 0x23, 0x26, 0x30, 0x31, 0x7F, 0x80};

std::vector<uint8_t> mutate(const std::vector<std::vector<uint8_t>> &seeds, Rng &r) {
  std::vector<uint8_t> v = seeds[r.below(uint32_t(seeds.size()))];
  if (r.below(16) == 0) {  // now and then, pure noise for a target
    v.assign(1, uint8_t(r.below(3)));
    size_t n = r.below(400);
    for (size_t i = 0; i < n; i++)
      v.push_back(uint8_t(r.next()));
    return v;
  }
  int count = 1 + int(r.below(8));
  for (int k = 0; k < count; k++) {
    size_t n = v.size();
    size_t at = n > 1 ? 1 + r.below(uint32_t(n - 1)) : 1;  // byte 0 is the target: kept
    switch (r.below(8)) {
      case 0:
        if (at < n) v[at] ^= uint8_t(1u << r.below(8));
        break;
      case 1:
        if (at < n) v[at] = uint8_t(r.next());
        break;
      case 2:
        if (at < n) v[at] = MAGIC[r.below(sizeof(MAGIC))];
        break;
      case 3: {
        size_t len = 1 + r.below(16);
        for (size_t i = 0; i < len; i++)
          v.insert(v.begin() + long(at > v.size() ? v.size() : at), uint8_t(r.next()));
        break;
      }
      case 4:
        if (at < n) v.erase(v.begin() + long(at), v.begin() + long(at + r.below(uint32_t(n - at)) / 4 + 1));
        break;
      case 5:
        if (at < n) {
          size_t len = 1 + r.below(uint32_t(n - at));
          std::vector<uint8_t> chunk(v.begin() + long(at), v.begin() + long(at + len));
          v.insert(v.begin() + long(at), chunk.begin(), chunk.end());
        }
        break;
      case 6: {  // splice another seed's tail
        const std::vector<uint8_t> &o = seeds[r.below(uint32_t(seeds.size()))];
        if (o.size() > 1 && at <= n) {
          size_t from = 1 + r.below(uint32_t(o.size() - 1));
          v.resize(at);
          v.insert(v.end(), o.begin() + long(from), o.end());
        }
        break;
      }
      default:
        if (at < n) v[at] = uint8_t(v[at] + 1 + r.below(4) - 2);
        break;
    }
    if (v.size() > 4096)
      v.resize(4096);
  }
  return v;
}

}  // namespace

int main(int argc, char **argv) {
  unsigned long iters = 3000, seed = 1;
  const char *corpus = nullptr;
  for (int i = 1; i < argc; i++) {
    if (!std::strcmp(argv[i], "--iters") && i + 1 < argc)
      iters = std::strtoul(argv[++i], nullptr, 0);
    else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc)
      seed = std::strtoul(argv[++i], nullptr, 0);
    else if (!std::strcmp(argv[i], "--write-corpus") && i + 1 < argc)
      corpus = argv[++i];
  }
  std::vector<std::vector<uint8_t>> seeds = make_seeds();
  if (corpus != nullptr) {
    for (size_t i = 0; i < seeds.size(); i++) {
      std::string path = std::string(corpus) + "/seed-" + std::to_string(i);
      FILE *f = std::fopen(path.c_str(), "wb");
      if (f == nullptr)
        return 1;
      std::fwrite(seeds[i].data(), 1, seeds[i].size(), f);
      std::fclose(f);
    }
    std::printf("fuzz_link: %zu seeds written to %s\n", seeds.size(), corpus);
    return 0;
  }
  for (const auto &s : seeds)
    LLVMFuzzerTestOneInput(s.data(), s.size());
  Rng r(seed);
  unsigned long per_target[3] = {0, 0, 0};
  for (unsigned long i = 0; i < iters; i++) {
    std::vector<uint8_t> v = mutate(seeds, r);
    per_target[v[0] % 3]++;
    LLVMFuzzerTestOneInput(v.data(), v.size());
  }
  std::printf("fuzz_link: %zu seeds, %lu mutated inputs (server %lu, core %lu, client %lu), seed %lu, 0 failures\n",
              seeds.size(), iters, per_target[0], per_target[1], per_target[2], seed);
  return 0;
}

#endif
