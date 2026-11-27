#include "link.h"

#include <string.h>

/* ---------------------------------------------------------------- frames */

uint16_t link_crc16(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++)
      crc = (crc & 1) ? (uint16_t) ((crc >> 1) ^ 0xA001) : (uint16_t) (crc >> 1);
  }
  return crc;
}

static size_t finish(uint8_t *out, size_t n) {
  out[1] = (uint8_t) n;
  uint16_t crc = link_crc16(out, n);
  out[n] = (uint8_t) (crc >> 8);
  out[n + 1] = (uint8_t) crc;
  return n + 2;
}

size_t link_build_request(uint8_t seq, uint8_t op, const uint8_t *payload, size_t len, uint8_t *out) {
  if (len > LINK_REQUEST_PAYLOAD_MAX)
    return 0;
  out[0] = LINK_REQUEST;
  out[2] = seq;
  out[3] = op;
  if (len != 0)
    memcpy(out + 4, payload, len);
  return finish(out, 4 + len);
}

size_t link_build_reply(uint8_t seq, uint8_t op, uint8_t result, const uint8_t *payload, size_t len,
                        uint8_t *out) {
  if (len > LINK_REPLY_PAYLOAD_MAX)
    return 0;
  out[0] = LINK_REPLY;
  out[2] = seq;
  out[3] = op;
  out[4] = result;
  if (len != 0)
    memcpy(out + 5, payload, len);
  return finish(out, 5 + len);
}

void link_parser_init(link_parser *p, uint8_t header) {
  memset(p, 0, sizeof(*p));
  p->header = header;
}

/* Drop the first byte and move to the next possible header in what is buffered. */
static void resync(link_parser *p) {
  uint8_t i = 1;
  while (i < p->count && p->buf[i] != p->header)
    i++;
  memmove(p->buf, p->buf + i, p->count - i);
  p->count = (uint8_t) (p->count - i);
}

/* Checks what is buffered: -1 bad (resync), 0 incomplete, 1 a complete valid frame. */
static int check(link_parser *p) {
  if (p->count == 0)
    return 0;
  if (p->buf[0] != p->header)
    return -1;
  if (p->count < 2)
    return 0;
  uint8_t len = p->buf[1];
  uint8_t min = p->header == LINK_REPLY ? 5 : 4;
  if (len < min || (size_t) len + 2 > LINK_FRAME_MAX)
    return -1;
  if (p->count < (size_t) len + 2)
    return 0;
  uint16_t crc = link_crc16(p->buf, len);
  if (p->buf[len] != (uint8_t) (crc >> 8) || p->buf[len + 1] != (uint8_t) crc)
    return -1;
  return 1;
}

int link_parser_feed(link_parser *p, uint8_t byte) {
  /* The frame handed out last time is consumed now. After a resync it may have bytes behind it:
   * they stay, from their first header. */
  if (p->handed) {
    uint8_t i = p->handed;
    p->handed = 0;
    while (i < p->count && p->buf[i] != p->header)
      i++;
    memmove(p->buf, p->buf + i, p->count - i);
    p->count = (uint8_t) (p->count - i);
  }
  if (p->count == 0 && byte != p->header)
    return 0;
  if (p->count >= LINK_FRAME_MAX)
    resync(p);
  p->buf[p->count++] = byte;
  for (;;) {
    int r = check(p);
    if (r == 1) {
      p->handed = (uint8_t) link_frame_len(p->buf);
      return 1;
    }
    if (r == 0)
      return 0;
    p->bad++;
    resync(p);
  }
}

/* ---------------------------------------------------------------- records */

static size_t put_text(uint8_t *p, const char *s, size_t max) {
  size_t n = strlen(s);
  if (n > max)
    n = max;
  memcpy(p, s, n);
  return n;
}
static void get_text(const uint8_t *p, size_t n, char *out, size_t max) {
  if (n > max)
    n = max;
  memcpy(out, p, n);
  out[n] = 0;
}

size_t link_encode_info(const link_info *in, uint8_t *out) {
  out[0] = in->major;
  out[1] = in->kind;
  memcpy(out + 2, in->version, 3);
  link_put32(out + 5, in->capabilities);
  out[9] = in->param_count;
  out[10] = in->reset_reason;
  out[11] = in->checks;
  return 12 + put_text(out + 12, in->text, LINK_TEXT_MAX);
}

int link_decode_info(const uint8_t *in, size_t len, link_info *out) {
  if (len < 12)
    return 0;
  memset(out, 0, sizeof(*out));
  out->major = in[0];
  out->kind = in[1];
  memcpy(out->version, in + 2, 3);
  out->capabilities = link_get32(in + 5);
  out->param_count = in[9];
  out->reset_reason = in[10];
  out->checks = in[11];
  get_text(in + 12, len - 12, out->text, LINK_TEXT_MAX);
  return 1;
}

size_t link_encode_desired(const link_desired *in, uint8_t *out) {
  out[0] = in->start;
  out[1] = in->mode;
  out[2] = in->power;
  out[3] = (uint8_t) in->fuel_trim;
  link_put16(out + 4, (uint16_t) in->fan_trim);
  out[6] = in->flags;
  return LINK_DESIRED_SIZE;
}

int link_decode_desired(const uint8_t *in, size_t len, link_desired *out) {
  if (len < LINK_DESIRED_SIZE)
    return 0;
  out->start = in[0];
  out->mode = in[1];
  out->power = in[2];
  out->fuel_trim = (int8_t) in[3];
  out->fan_trim = (int16_t) link_get16(in + 4);
  out->flags = in[6];
  return 1;
}

size_t link_encode_status(const link_status *in, uint8_t *out) {
  out[0] = in->state;
  out[1] = in->detail;
  out[2] = in->fault;
  out[3] = in->fault_detail;
  out[4] = in->flags;
  out[5] = in->start_ack;
  out[6] = in->start_result;
  out[7] = in->power;
  link_put16(out + 8, (uint16_t) in->heat_exchanger);
  link_put16(out + 10, in->supply);
  link_put16(out + 12, in->fan_rpm);
  link_put16(out + 14, in->pump);
  link_put16(out + 16, in->glow_w);
  link_put16(out + 18, in->prime_left_s);
  link_put32(out + 20, in->run_time_s);
  out[24] = in->log_seq;
  return LINK_STATUS_SIZE;
}

int link_decode_status(const uint8_t *in, size_t len, link_status *out) {
  if (len < LINK_STATUS_SIZE)
    return 0;
  out->state = in[0];
  out->detail = in[1];
  out->fault = in[2];
  out->fault_detail = in[3];
  out->flags = in[4];
  out->start_ack = in[5];
  out->start_result = in[6];
  out->power = in[7];
  out->heat_exchanger = (int16_t) link_get16(in + 8);
  out->supply = link_get16(in + 10);
  out->fan_rpm = link_get16(in + 12);
  out->pump = link_get16(in + 14);
  out->glow_w = link_get16(in + 16);
  out->prime_left_s = link_get16(in + 18);
  out->run_time_s = link_get32(in + 20);
  out->log_seq = in[24];
  return 1;
}

size_t link_encode_detail(const link_detail *in, uint8_t *out) {
  link_put16(out + 0, (uint16_t) in->board);
  link_put16(out + 2, (uint16_t) in->ts_fan);
  link_put16(out + 4, (uint16_t) in->ts_glow);
  link_put16(out + 6, in->glow_v);
  link_put16(out + 8, in->glow_a);
  link_put16(out + 10, in->fan_v);
  link_put16(out + 12, in->fan_a);
  link_put16(out + 14, in->pump_ma);
  link_put16(out + 16, in->co_ppm);
  link_put16(out + 18, in->vdda_mv);
  out[20] = in->co_state;
  return LINK_DETAIL_SIZE;
}

int link_decode_detail(const uint8_t *in, size_t len, link_detail *out) {
  if (len < LINK_DETAIL_SIZE)
    return 0;
  out->board = (int16_t) link_get16(in + 0);
  out->ts_fan = (int16_t) link_get16(in + 2);
  out->ts_glow = (int16_t) link_get16(in + 4);
  out->glow_v = link_get16(in + 6);
  out->glow_a = link_get16(in + 8);
  out->fan_v = link_get16(in + 10);
  out->fan_a = link_get16(in + 12);
  out->pump_ma = link_get16(in + 14);
  out->co_ppm = link_get16(in + 16);
  out->vdda_mv = link_get16(in + 18);
  out->co_state = in[20];
  return 1;
}

size_t link_encode_param_desc(const link_param_desc *in, uint8_t *out) {
  out[0] = in->index;
  out[1] = in->count;
  link_put16(out + 2, in->id);
  out[4] = in->flags;
  out[5] = in->unit;
  out[6] = in->decimals;
  link_put32(out + 7, (uint32_t) in->min);
  link_put32(out + 11, (uint32_t) in->max);
  link_put32(out + 15, (uint32_t) in->step);
  out[19] = in->group;
  out[20] = in->level;
  return LINK_PARAM_DESC_FIXED + put_text(out + LINK_PARAM_DESC_FIXED, in->name, LINK_PARAM_NAME_MAX);
}

int link_decode_param_desc(const uint8_t *in, size_t len, link_param_desc *out) {
  if (len < LINK_PARAM_DESC_FIXED)
    return 0;
  memset(out, 0, sizeof(*out));
  out->index = in[0];
  out->count = in[1];
  out->id = link_get16(in + 2);
  out->flags = in[4];
  out->unit = in[5];
  out->decimals = in[6];
  out->min = (int32_t) link_get32(in + 7);
  out->max = (int32_t) link_get32(in + 11);
  out->step = (int32_t) link_get32(in + 15);
  out->group = in[19];
  out->level = in[20];
  get_text(in + LINK_PARAM_DESC_FIXED, len - LINK_PARAM_DESC_FIXED, out->name, LINK_PARAM_NAME_MAX);
  return 1;
}

size_t link_encode_param_value(const link_param_value *in, uint8_t *out) {
  link_put16(out, in->id);
  out[2] = in->flags;
  link_put32(out + 3, (uint32_t) in->value);
  link_put32(out + 7, (uint32_t) in->staged);
  return LINK_PARAM_VALUE_SIZE;
}

int link_decode_param_value(const uint8_t *in, size_t len, link_param_value *out) {
  if (len < LINK_PARAM_VALUE_SIZE)
    return 0;
  out->id = link_get16(in);
  out->flags = in[2];
  out->value = (int32_t) link_get32(in + 3);
  out->staged = (int32_t) link_get32(in + 7);
  return 1;
}

size_t link_encode_fault_entry(const link_fault_entry *in, uint8_t *out) {
  out[0] = in->kind;
  out[1] = in->code;
  out[2] = in->why;
  out[3] = in->step;
  out[4] = in->seq;
  out[5] = (uint8_t) in->board_c;
  link_put16(out + 6, (uint16_t) in->hx);
  link_put16(out + 8, in->supply);
  link_put32(out + 10, in->uptime_s);
  return LINK_FAULT_ENTRY_SIZE;
}

int link_decode_fault_entry(const uint8_t *in, size_t len, link_fault_entry *out) {
  if (len < LINK_FAULT_ENTRY_SIZE)
    return 0;
  out->kind = in[0];
  out->code = in[1];
  out->why = in[2];
  out->step = in[3];
  out->seq = in[4];
  out->board_c = (int8_t) in[5];
  out->hx = (int16_t) link_get16(in + 6);
  out->supply = link_get16(in + 8);
  out->uptime_s = link_get32(in + 10);
  return 1;
}

void link_status_unknown(link_status *s) {
  memset(s, 0, sizeof(*s));
  s->detail = LINK_UNKNOWN_U8;
  s->fault_detail = 0;
  s->heat_exchanger = LINK_UNKNOWN_I16;
  s->supply = LINK_UNKNOWN_U16;
  s->fan_rpm = LINK_UNKNOWN_U16;
  s->pump = LINK_UNKNOWN_U16;
  s->glow_w = LINK_UNKNOWN_U16;
  s->run_time_s = LINK_UNKNOWN_U32;
}

void link_detail_unknown(link_detail *d) {
  d->board = d->ts_fan = d->ts_glow = LINK_UNKNOWN_I16;
  d->glow_v = d->glow_a = d->fan_v = d->fan_a = LINK_UNKNOWN_U16;
  d->pump_ma = d->co_ppm = d->vdda_mv = LINK_UNKNOWN_U16;
  d->co_state = 0; /* not fitted */
}

/* ---------------------------------------------------------------- MCU side */

size_t link_server_handle(const link_mcu *m, const uint8_t *req, uint8_t *reply) {
  uint8_t seq = link_frame_seq(req), op = link_frame_op(req);
  const uint8_t *in = link_request_payload(req);
  size_t n = link_request_payload_len(req);
  uint8_t out[LINK_REPLY_PAYLOAD_MAX];
  size_t len = 0;
  int result = LINK_OK;
#define NEED(want)            \
  if (n != (want)) {          \
    result = LINK_BAD_LENGTH; \
    break;                    \
  }
#define HAVE(fn) (m->fn != NULL)

  switch (op) {
    case LINK_OP_HELLO: {
      NEED(1); /* the client's link major */
      if (m->client_major != NULL)
        *m->client_major = in[0];
      link_info info;
      memset(&info, 0, sizeof(info));
      info.major = LINK_MAJOR;
      if (m->info != NULL)
        m->info(m->ctx, &info);
      len = link_encode_info(&info, out);
      break;
    }
    case LINK_OP_POLL: {
      link_desired want;
      if (m->client_major != NULL && *m->client_major != LINK_MAJOR) {
        result = LINK_NOT_SUPPORTED; /* no HELLO yet, or one of a major this MCU does not speak */
        break;
      }
      if (!link_decode_desired(in, n, &want)) {
        result = LINK_BAD_LENGTH;
        break;
      }
      link_status st;
      link_status_unknown(&st);
      if (m->poll != NULL)
        m->poll(m->ctx, &want, &st);
      len = link_encode_status(&st, out);
      break;
    }
    case LINK_OP_DETAIL: {
      NEED(0);
      link_detail d;
      link_detail_unknown(&d);
      if (m->detail != NULL)
        m->detail(m->ctx, &d);
      len = link_encode_detail(&d, out);
      break;
    }
    case LINK_OP_PARAM_DESCRIBE: {
      NEED(1);
      link_param_desc d;
      memset(&d, 0, sizeof(d));
      result = HAVE(param_describe) ? m->param_describe(m->ctx, in[0], &d) : LINK_NOT_SUPPORTED;
      if (result == LINK_OK)
        len = link_encode_param_desc(&d, out);
      break;
    }
    case LINK_OP_PARAM_GET: {
      NEED(2);
      link_param_value v;
      memset(&v, 0, sizeof(v));
      result = HAVE(param_get) ? m->param_get(m->ctx, link_get16(in), &v) : LINK_NOT_SUPPORTED;
      if (result == LINK_OK)
        len = link_encode_param_value(&v, out);
      break;
    }
    case LINK_OP_PARAM_SET:
      NEED(6);
      result = HAVE(param_set) ? m->param_set(m->ctx, link_get16(in), (int32_t) link_get32(in + 2)) : LINK_NOT_SUPPORTED;
      break;
    case LINK_OP_PARAM_APPLY:
      NEED(1);
      if (in[0] > 1) {
        result = LINK_OUT_OF_RANGE;
        break;
      }
      result = HAVE(param_apply) ? m->param_apply(m->ctx, in[0]) : LINK_NOT_SUPPORTED;
      break;
    case LINK_OP_PARAM_DISCARD:
      NEED(0);
      result = HAVE(param_discard) ? m->param_discard(m->ctx) : LINK_NOT_SUPPORTED;
      break;
    case LINK_OP_PARAM_DEFAULTS:
      NEED(2);
      if (in[0] != 'D' || in[1] != 'F') {
        result = LINK_REFUSED;
        break;
      }
      result = HAVE(param_defaults) ? m->param_defaults(m->ctx) : LINK_NOT_SUPPORTED;
      break;
    case LINK_OP_PARAM_HELP: {
      NEED(3);
      const char *text = "";
      uint16_t id = link_get16(in);
      result = HAVE(param_help) ? m->param_help(m->ctx, id, &text) : LINK_NOT_SUPPORTED;
      if (result != LINK_OK)
        break;
      size_t total = text != NULL ? strlen(text) : 0, off = in[2], chunk;
      if (total > LINK_PARAM_HELP_MAX)
        total = LINK_PARAM_HELP_MAX;
      if (off > total) {
        result = LINK_OUT_OF_RANGE;
        break;
      }
      chunk = total - off;
      if (chunk > LINK_PARAM_HELP_CHUNK)
        chunk = LINK_PARAM_HELP_CHUNK;
      link_put16(out, id);
      out[2] = (uint8_t) off;
      out[3] = (uint8_t) total;
      if (chunk != 0)
        memcpy(out + LINK_PARAM_HELP_FIXED, text + off, chunk);
      len = LINK_PARAM_HELP_FIXED + chunk;
      break;
    }
    case LINK_OP_FAULT_RESET:
      NEED(0);
      result = HAVE(fault_reset) ? m->fault_reset(m->ctx) : LINK_NOT_SUPPORTED;
      break;
    case LINK_OP_GLOW_NEW_PLUG:
      NEED(0);
      result = HAVE(glow_new_plug) ? m->glow_new_plug(m->ctx) : LINK_NOT_SUPPORTED;
      break;
    case LINK_OP_FAULT_LOG: {
      NEED(1);
      if (!HAVE(fault_log)) {
        result = LINK_NOT_SUPPORTED;
        break;
      }
      link_fault_entry e[LINK_FAULT_LOG_PAGE];
      uint8_t total = 0, got = m->fault_log(m->ctx, in[0], e, &total);
      out[0] = total;
      len = 1;
      for (uint8_t i = 0; i < got && i < LINK_FAULT_LOG_PAGE; i++)
        len += link_encode_fault_entry(&e[i], out + len);
      break;
    }
    case LINK_OP_FAULT_LOG_CLEAR:
      NEED(2);
      if (in[0] != 'C' || in[1] != 'L') {
        result = LINK_REFUSED;
        break;
      }
      result = HAVE(fault_log_clear) ? m->fault_log_clear(m->ctx) : LINK_NOT_SUPPORTED;
      break;
    case LINK_OP_UPDATE_PREPARE: {
      NEED(0);
      uint8_t method = 0;
      result = HAVE(update_prepare) ? m->update_prepare(m->ctx, &method) : LINK_NOT_SUPPORTED;
      if (result == LINK_OK) {
        out[0] = method;
        len = 1;
      }
      break;
    }
    default:
      result = LINK_UNKNOWN_OP;
      break;
  }
#undef NEED
#undef HAVE
  return link_build_reply(seq, op, (uint8_t) result, out, len, reply);
}

void link_start_gate_init(link_start_gate *g) { g->last = 0, g->seen = 0; }

int link_start_gate_check(link_start_gate *g, uint8_t start, uint8_t mode) {
  if (!g->seen) {
    g->seen = 1;
    g->last = start;
    return 0;
  }
  if (start == g->last)
    return 0;
  g->last = start;
  return mode != LINK_MODE_OFF;
}

int link_deadman_expired(const link_deadman *d, uint32_t now, int burning) {
  if (!burning)
    return 0;
  if (!d->heard)
    return 1;
  /* A frame stamped after `now` was taken (the caller read the clock first) is not 49 days old. */
  int32_t elapsed = (int32_t) (now - d->last_ms);
  return elapsed >= (int32_t) LINK_DEADMAN_MS;
}
