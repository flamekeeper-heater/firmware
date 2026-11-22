/* The Flamekeeper link between the ESP32 and the safety MCU (DESIGN.md): frames,
 * records and the MCU-side helpers. Plain C99 with no hardware access, built by both firmware
 * projects (the ESP32 through symlinks in esp32/components/flamekeeper/, the STM32 directly) and
 * host-tested in tests/.
 *
 * Time is a wrapping uint32_t millisecond count; elapsed time is always (uint32_t)(now - then). */
#ifndef FLAMEKEEPER_LINK_H
#define FLAMEKEEPER_LINK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LINK_MAJOR 1
#define LINK_MINOR 0

/* ---------------------------------------------------------------- frames */

#define LINK_REQUEST 0x7A /* ESP32 -> MCU */
#define LINK_REPLY 0x7B   /* MCU -> ESP32 */
#define LINK_FRAME_MAX 64 /* whole frame, CRC included */
#define LINK_REQUEST_OVERHEAD 6 /* header, len, seq, op, CRC */
#define LINK_REPLY_OVERHEAD 7   /* the same plus the result byte */
#define LINK_REQUEST_PAYLOAD_MAX (LINK_FRAME_MAX - LINK_REQUEST_OVERHEAD)
#define LINK_REPLY_PAYLOAD_MAX (LINK_FRAME_MAX - LINK_REPLY_OVERHEAD)

enum {
  LINK_OP_HELLO = 0x01,
  LINK_OP_POLL = 0x02,
  LINK_OP_DETAIL = 0x03,
  LINK_OP_PARAM_DESCRIBE = 0x20,
  LINK_OP_PARAM_GET = 0x21,
  LINK_OP_PARAM_SET = 0x22,
  LINK_OP_PARAM_APPLY = 0x23,
  LINK_OP_PARAM_DISCARD = 0x24,
  LINK_OP_PARAM_DEFAULTS = 0x25,
  LINK_OP_PARAM_HELP = 0x26,
  LINK_OP_FAULT_RESET = 0x30,
  LINK_OP_UPDATE_PREPARE = 0x40,
};

/* Result codes: the app protocol's (PROTOCOL.md). */
enum {
  LINK_OK = 0x00,
  LINK_STORED = 0x01,
  LINK_UNKNOWN_OP = 0x10,
  LINK_BAD_LENGTH = 0x11,
  LINK_OUT_OF_RANGE = 0x12,
  LINK_NOT_SUPPORTED = 0x13,
  LINK_NOT_OFF = 0x20,
  LINK_BUSY = 0x22,
  LINK_REFUSED = 0x2F,
};

/* CRC16-Modbus (poly 0xA001 reflected, init 0xFFFF). */
uint16_t link_crc16(const uint8_t *data, size_t len);

/* Build a frame into out (LINK_FRAME_MAX bytes). Return its length, 0 if the payload is too long. */
size_t link_build_request(uint8_t seq, uint8_t op, const uint8_t *payload, size_t len, uint8_t *out);
size_t link_build_reply(uint8_t seq, uint8_t op, uint8_t result, const uint8_t *payload, size_t len,
                        uint8_t *out);

/* Byte-stream parser for one direction: finds frames that start with `header`, have a plausible
 * length and a good CRC, and resynchronises after noise or a bad frame. */
typedef struct {
  uint8_t header;
  uint8_t buf[LINK_FRAME_MAX];
  uint8_t count;
  uint32_t bad; /* frames dropped for a bad CRC or length */
} link_parser;

void link_parser_init(link_parser *p, uint8_t header);
/* Feed one byte. Returns 1 when buf holds a complete valid frame (length link_frame_len(buf)); the
 * frame stays there until the next byte is fed. */
int link_parser_feed(link_parser *p, uint8_t byte);

static inline size_t link_frame_len(const uint8_t *f) { return (size_t) f[1] + 2; }
static inline uint8_t link_frame_seq(const uint8_t *f) { return f[2]; }
static inline uint8_t link_frame_op(const uint8_t *f) { return f[3]; }
/* Requests: payload after op. Replies: result after op, then payload. */
static inline const uint8_t *link_request_payload(const uint8_t *f) { return f + 4; }
static inline size_t link_request_payload_len(const uint8_t *f) { return (size_t) f[1] - 4; }
static inline uint8_t link_reply_result(const uint8_t *f) { return f[4]; }
static inline const uint8_t *link_reply_payload(const uint8_t *f) { return f + 5; }
static inline size_t link_reply_payload_len(const uint8_t *f) { return (size_t) f[1] - 5; }

/* ---------------------------------------------------------------- records */

/* Unknown values (as in the app protocol). */
#define LINK_UNKNOWN_I16 ((int16_t) 0x8000)
#define LINK_UNKNOWN_U16 0xFFFFu
#define LINK_UNKNOWN_U8 0xFFu
#define LINK_UNKNOWN_U32 0xFFFFFFFFu

/* Capability bits (Info). */
#define LINK_CAP_FAN_ONLY (1u << 0)
#define LINK_CAP_PRIME (1u << 1)
#define LINK_CAP_PRIME_STOP (1u << 2)
#define LINK_CAP_PARAMETERS (1u << 3)
#define LINK_CAP_TRIMS (1u << 4)
#define LINK_CAP_SUPPLY_PRESENT (1u << 5)
#define LINK_CAP_CO (1u << 6)
#define LINK_CAP_GLOW_VI (1u << 7)
#define LINK_CAP_FAN_V (1u << 8)
#define LINK_CAP_FAN_I (1u << 9)
#define LINK_CAP_PUMP_I (1u << 10)
#define LINK_CAP_TEMPERATURES (1u << 11)
#define LINK_CAP_OVERHEAT_LATCH (1u << 12)
#define LINK_CAP_DEADMAN (1u << 13)
#define LINK_CAP_RUN_SWITCH (1u << 14) /* heat <-> fan during a run, without a new start */

enum { LINK_KIND_EMULATOR = 1, LINK_KIND_STM32 = 2 };
enum {
  LINK_RESET_UNKNOWN = 0,
  LINK_RESET_POWER_ON = 1,
  LINK_RESET_WATCHDOG = 2,
  LINK_RESET_PIN = 3,
  LINK_RESET_BROWN_OUT = 4,
  LINK_RESET_BOOTLOADER = 5,
  LINK_RESET_SOFTWARE = 6,
};
#define LINK_CHECK_OPTION_BYTES (1u << 0)
#define LINK_CHECK_SELF_TEST (1u << 1)
#define LINK_CHECK_NTC_OPEN (1u << 2)

#define LINK_TEXT_MAX 32
typedef struct {
  uint8_t major, minor, kind;
  uint8_t version[3];
  uint32_t capabilities;
  uint8_t param_count;
  uint8_t reset_reason;
  uint8_t checks;
  char text[LINK_TEXT_MAX + 1];
} link_info;

enum { LINK_MODE_OFF = 0, LINK_MODE_HEAT = 1, LINK_MODE_FAN = 2, LINK_MODE_PRIME = 3 };
#define LINK_WANT_ROOM_KNOWN (1u << 0)
#define LINK_WANT_UPDATING (1u << 1)

typedef struct {
  uint8_t start;   /* start number */
  uint8_t mode;    /* LINK_MODE_* */
  uint8_t power;   /* 0..100 % */
  int8_t fuel_trim; /* % */
  int16_t fan_trim; /* rpm */
  uint8_t flags;   /* LINK_WANT_* */
} link_desired;
#define LINK_DESIRED_SIZE 7

/* Heater states and faults: the app protocol's codes. */
#define LINK_STATUS_FAULT_LATCHED (1u << 0)
#define LINK_STATUS_SUPPLY_PRESENT (1u << 1)
#define LINK_STATUS_OVERHEAT_TRIPPED (1u << 2)
#define LINK_STATUS_DEADMAN_STOP (1u << 3)
#define LINK_STATUS_PARAMS_STAGED (1u << 4)
#define LINK_STATUS_NO_STARTS (1u << 5)

typedef struct {
  uint8_t state, detail, fault, fault_detail, flags;
  uint8_t start_ack, start_result;
  uint8_t power;          /* % running */
  int16_t heat_exchanger; /* 0.1 C */
  uint16_t supply;        /* 0.01 V */
  uint16_t fan_rpm;
  uint16_t pump;          /* 0.01 Hz */
  uint16_t glow_w;
  uint16_t prime_left_s;
  uint32_t run_time_s;    /* lifetime */
} link_status;
#define LINK_STATUS_SIZE 24

typedef struct {
  int16_t board, ts_fan, ts_glow; /* 0.1 C */
  uint16_t glow_v, glow_a, fan_v, fan_a; /* 0.01 V / A */
  uint16_t pump_ma, co_ppm, vdda_mv;
  uint8_t co_state; /* the app protocol's CO sensor state (0 not fitted .. 6 too cold) */
} link_detail;
#define LINK_DETAIL_SIZE 21

#define LINK_PARAM_NAME_MAX 24
typedef struct {
  uint8_t index, count;
  uint16_t id;
  uint8_t flags, unit, decimals;
  int32_t min, max, step;
  uint8_t group; /* the app protocol's parameter group (PROTOCOL.md) */
  uint8_t level; /* 0 basic, 1 advanced, 2 expert */
  char name[LINK_PARAM_NAME_MAX + 1];
} link_param_desc;
#define LINK_PARAM_DESC_FIXED 21

/* PARAM_HELP: a parameter's description, in pieces (DESIGN.md). */
#define LINK_PARAM_HELP_MAX 120
#define LINK_PARAM_HELP_FIXED 4 /* id, offset, total length */
#define LINK_PARAM_HELP_CHUNK (LINK_REPLY_PAYLOAD_MAX - LINK_PARAM_HELP_FIXED)

#define LINK_PARAM_KNOWN (1u << 0)
#define LINK_PARAM_STAGED (1u << 1)
typedef struct {
  uint16_t id;
  uint8_t flags;
  int32_t value, staged;
} link_param_value;
#define LINK_PARAM_VALUE_SIZE 11

/* encode: return the length written into out (at least the record's size). decode: return 1 when
 * len is long enough (extra trailing bytes are ignored: newer minor versions append fields). */
size_t link_encode_info(const link_info *in, uint8_t *out);
int link_decode_info(const uint8_t *in, size_t len, link_info *out);
size_t link_encode_desired(const link_desired *in, uint8_t *out);
int link_decode_desired(const uint8_t *in, size_t len, link_desired *out);
size_t link_encode_status(const link_status *in, uint8_t *out);
int link_decode_status(const uint8_t *in, size_t len, link_status *out);
size_t link_encode_detail(const link_detail *in, uint8_t *out);
int link_decode_detail(const uint8_t *in, size_t len, link_detail *out);
size_t link_encode_param_desc(const link_param_desc *in, uint8_t *out);
int link_decode_param_desc(const uint8_t *in, size_t len, link_param_desc *out);
size_t link_encode_param_value(const link_param_value *in, uint8_t *out);
int link_decode_param_value(const uint8_t *in, size_t len, link_param_value *out);

/* Every field unknown / zero, as an MCU fills them before it knows anything. */
void link_status_unknown(link_status *s);
void link_detail_unknown(link_detail *d);

/* ---------------------------------------------------------------- MCU side */

/* What an MCU implementation provides; link_server_handle() decodes requests and calls these.
 * Calls returning int return a result code (LINK_OK ...). */
typedef struct {
  void *ctx;
  void (*info)(void *ctx, link_info *out);
  void (*poll)(void *ctx, const link_desired *want, link_status *out);
  void (*detail)(void *ctx, link_detail *out);
  int (*param_describe)(void *ctx, uint8_t index, link_param_desc *out);
  int (*param_get)(void *ctx, uint16_t id, link_param_value *out);
  int (*param_set)(void *ctx, uint16_t id, int32_t value);
  int (*param_apply)(void *ctx, uint8_t save);
  int (*param_discard)(void *ctx);
  int (*param_defaults)(void *ctx);
  /* *text: the description (static storage, NUL-terminated, at most LINK_PARAM_HELP_MAX bytes are
   * sent); "" when there is none. */
  int (*param_help)(void *ctx, uint16_t id, const char **text);
  int (*fault_reset)(void *ctx);
  int (*update_prepare)(void *ctx, uint8_t *method);
  /* Where the server keeps the ESP32's link major from its last HELLO (0 = none yet). With it set,
   * POLL is refused with LINK_NOT_SUPPORTED until a HELLO of this major (DESIGN.md);
   * NULL = not checked. */
  uint8_t *client_major;
} link_mcu;

/* One complete, CRC-checked request (from link_parser) in, the reply frame out (LINK_FRAME_MAX
 * bytes). Returns the reply's length. A null callback answers LINK_NOT_SUPPORTED. */
size_t link_server_handle(const link_mcu *mcu, const uint8_t *request, uint8_t *reply);

/* The start-number rule (DESIGN.md). */
typedef struct {
  uint8_t last;
  uint8_t seen; /* 0 until the first poll after a reset */
} link_start_gate;

void link_start_gate_init(link_start_gate *g);
/* Call with every poll. Returns 1 when this poll asks for a new start (a number different from
 * the last acted on, and a mode other than off); the number is then taken as acted on whatever the
 * MCU decides, so a refused start is not retried by the same number. The first poll after a reset
 * only records its number. */
int link_start_gate_check(link_start_gate *g, uint8_t start, uint8_t mode);

/* The dead-man (DESIGN.md). */
#define LINK_DEADMAN_MS 2000u
typedef struct {
  uint32_t last_ms;
  uint8_t heard;
} link_deadman;

static inline void link_deadman_init(link_deadman *d) { d->last_ms = 0, d->heard = 0; }
/* Any valid frame from the ESP32. */
static inline void link_deadman_feed(link_deadman *d, uint32_t now) { d->last_ms = now, d->heard = 1; }
/* 1 when the link has been silent for LINK_DEADMAN_MS (or never spoke) and the heater is burning,
 * i.e. the MCU must burn off and cool down on its own. A frame fed after `now` was read counts as
 * just heard (not as wrapped-around silence). */
int link_deadman_expired(const link_deadman *d, uint32_t now, int burning);

#ifdef __cplusplus
}
#endif

#endif /* FLAMEKEEPER_LINK_H */
