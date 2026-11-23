/* The bring-up build's text console: see console.h. */
#include "console.h"

#include <stddef.h>
#include <string.h>

#include "../board/analog.h"
#include "../hal/hal_stm32g0.h"

/* ---------------------------------------------------------------- the UART tap */

static console *tap;

int hal_uart_getc(void) {
  int c = bringup_chip_uart_getc();
  if (c >= 0 && tap)
    console_feed(tap, (uint8_t) c);
  return c;
}

void hal_iwdg_refresh(void) {
  if (!(tap && tap->wdg_stopped))
    bringup_chip_iwdg_refresh();
}

uint8_t hal_glow_period(hal_glow_sample *g) {
  uint8_t r = bringup_chip_glow_period(g);
  if (r && tap) {
    tap->glow_last = *g;
    tap->glow_periods++;
    if (g->pulse_i) {
      tap->glow_pulse = *g;
      tap->glow_pulse_ms = tap->now;
    }
  }
  return r;
}

void hal_pump_oc_arm(uint8_t on, uint16_t threshold_raw, uint16_t hold_us) {
  if (tap) {
    if (on && !tap->oc_on)
      tap->oc_arms++;
    tap->oc_on = on;
    tap->oc_threshold_raw = threshold_raw;
    tap->oc_hold_us = hold_us;
  }
  bringup_chip_pump_oc_arm(on, threshold_raw, hold_us);
}

/* CO_HEAT_EN as the board asks, unless "co heater off" holds it low (the safe side). */
void hal_co_heater(uint8_t on) {
  uint8_t pin = on;
  if (tap) {
    tap->co_wanted = on;
    if (tap->co_off)
      pin = 0;
    tap->co_pin = pin;
  }
  bringup_chip_co_heater(pin);
}

/* ---------------------------------------------------------------- output */

static uint16_t out_used(const console *c) {
  return (uint16_t) ((c->head + CONSOLE_OUT_SIZE - c->tail) % CONSOLE_OUT_SIZE);
}
static uint16_t out_free(const console *c) { return (uint16_t) (CONSOLE_OUT_SIZE - 1 - out_used(c)); }

static void put_ch(console *c, char ch) {
  if (ch == '{')
    ch = '('; /* never the link's reply header */
  if (out_free(c) == 0) {
    c->dropped++;
    return;
  }
  c->out[c->head] = ch;
  c->head = (uint16_t) ((c->head + 1u) % CONSOLE_OUT_SIZE);
}
static void put(console *c, const char *s) {
  while (*s)
    put_ch(c, *s++);
}
static void nl(console *c) { put(c, "\r\n"); }
static void put_u(console *c, uint32_t v) {
  char buf[11];
  int n = 0;
  do {
    buf[n++] = (char) ('0' + v % 10u);
    v /= 10u;
  } while (v);
  while (n)
    put_ch(c, buf[--n]);
}
static void put_i(console *c, int32_t v) {
  if (v < 0) {
    put_ch(c, '-');
    put_u(c, (uint32_t) (-(v + 1)) + 1u);
  } else {
    put_u(c, (uint32_t) v);
  }
}
/* v in units of 10^-dec, e.g. 1261 with 2 is 12.61. */
static void put_fix(console *c, int32_t v, uint8_t dec) {
  uint32_t scale = 1;
  for (uint8_t i = 0; i < dec; i++)
    scale *= 10u;
  uint32_t a;
  if (v < 0) {
    put_ch(c, '-');
    a = (uint32_t) (-(v + 1)) + 1u;
  } else {
    a = (uint32_t) v;
  }
  put_u(c, a / scale);
  if (dec) {
    put_ch(c, '.');
    uint32_t frac = a % scale;
    for (uint32_t d = scale / 10u; d > 0; d /= 10u)
      put_ch(c, (char) ('0' + (frac / d) % 10u));
  }
}
static void put_hex(console *c, uint32_t v, uint8_t digits) {
  static const char HEX[] = "0123456789abcdef";
  for (int i = digits - 1; i >= 0; i--)
    put_ch(c, HEX[(v >> (4 * i)) & 15u]);
}
static void put_0x(console *c, uint32_t v, uint8_t digits) {
  put(c, "0x");
  put_hex(c, v, digits);
}
static void kv(console *c, const char *key, int32_t v) {
  put_ch(c, ' ');
  put(c, key);
  put_ch(c, '=');
  put_i(c, v);
}
static void kvf(console *c, const char *key, int32_t v, uint8_t dec) {
  put_ch(c, ' ');
  put(c, key);
  put_ch(c, '=');
  put_fix(c, v, dec);
}

/* ---------------------------------------------------------------- names */

static const char *pick(const char *const *names, unsigned n, unsigned i) { return i < n ? names[i] : "?"; }
#define NAME(table, i) pick(table, sizeof(table) / sizeof(table[0]), (unsigned) (i))

static const char *const STEP[] = {"off",     "fan_check", "preheat",  "ignition", "purge",
                                   "warm_up", "running",   "burn_off", "cooldown", "fan_only",
                                   "priming", "pre_check", "fan_retry"};
static const char *const FAULT[] = {"none",     "?",      "supply_low", "supply_high", "glow",  "pump",
                                    "overheat", "fan",    "flame_out",  "sensor",      "start", "co"};
static const char *const WHY[] = {"none",          "asked",         "deadman",       "no_flame",
                                  "flame_lost",    "overheat_limit", "overheat_latch", "fan_stall",
                                  "fan_spin_up",   "sensor_open",   "no_temp_rise",  "supply",
                                  "cooldown_limit", "co",           "glow_stage",    "fan_stage",
                                  "self_test",     "pump_stage",    "board_hot",     "plug_type"};
static const char *const RESET[] = {"unknown", "power_on", "watchdog", "pin", "brown_out", "bootloader", "software"};
static const char *const CHECK_FAIL[] = {"latch_hold",       "latch_rearm",       "static_heartbeat",
                                         "pump_hs_fault",    "pump_no_current",   "pump_hs_short",
                                         "pump_hs_no_output", "glow_hs_short",    "glow_hb_hold",
                                         "glow_hs_no_output", "glow_ls_short",    "timeout"};
static const char *const LINK_CHECK[] = {"option_bytes", "self_test", "ntc_open"};
static const char *const FAN_FAULT[] = {"none", "stall", "stuck_on", "uncommanded", "overspeed"};
static const char *const PUMP_FAULT[] = {"none", "overcurrent", "low_side", "open_coil", "high_side"};
static const char *const GLOW_FAULT[] = {"none", "stuck_low_side", "high_side_trips"};
static const char *const BENCH_ERR[] = {"ok",
                                        "busy",
                                        "not off",
                                        "out of range",
                                        "interlocks: power-up self-test, option bytes or no starts",
                                        "a fault needs clearing first (clear)",
                                        "no airflow: start a fan action at the glow's minimum speed"};
static const char *const ACTION[] = {"none", "fan_duty", "fan_rpm", "hb", "pump", "glow", "precheck", "pump_oc"};
static const char *const CO_STATE[] = {"not_fitted", "warming_up", "learning", "ok", "alarm", "sensor_fault",
                                       "too_cold"};
static const char *const END[] = {"?", "done", "stopped", "timeout", "failed", "fault", "burn_started", "no_probe",
                                  "no_air"};
static const char *const ADC_NAME[HAL_ADC_COUNT] = {"glow_v", "glow_i", "pump_v", "fan_i", "pump_i",
                                                    "ntc",    "board",  "vin",    "vref",  "co"};

static const char *link_result(int r) {
  switch (r) {
    case LINK_OK: return "ok";
    case LINK_STORED: return "stored";
    case LINK_UNKNOWN_OP: return "unknown op";
    case LINK_BAD_LENGTH: return "bad length";
    case LINK_OUT_OF_RANGE: return "out of range";
    case LINK_NOT_SUPPORTED: return "not supported";
    case LINK_NOT_OFF: return "not off";
    case LINK_BUSY: return "busy (starts refused, or the glow stage still resting)";
    default: return "refused";
  }
}

static void bits(console *c, uint32_t v, const char *const *names, unsigned n) {
  for (unsigned i = 0; i < n; i++)
    if (v & (1u << i)) {
      put_ch(c, ' ');
      put(c, names[i]);
    }
}
#define BITS(c, v, table) bits(c, v, table, sizeof(table) / sizeof(table[0]))

/* ---------------------------------------------------------------- input */

void console_feed(console *c, uint8_t ch) {
  if (ch == '\r' || ch == '\n') {
    if (!c->bad && c->len > 0 && !c->has_cmd) {
      memcpy(c->cmd, c->line, c->len);
      c->cmd[c->len] = 0;
      c->has_cmd = 1;
    }
    c->len = 0;
    c->bad = 0;
    return;
  }
  if (ch == 0x08 || ch == 0x7F) { /* backspace, for a person typing */
    if (c->len)
      c->len--;
    return;
  }
  if (ch < 0x20 || ch > 0x7E || c->len >= CONSOLE_LINE_MAX - 1) {
    c->bad = 1; /* binary (a link frame) or too long: the line is dropped */
    return;
  }
  c->line[c->len++] = (char) ch;
}

/* Words of the command line, split on spaces. */
#define MAX_WORDS 6
typedef struct {
  char *w[MAX_WORDS];
  int n;
} words;

static void split(char *s, words *ws) {
  ws->n = 0;
  while (*s) {
    while (*s == ' ')
      *s++ = 0;
    if (!*s)
      break;
    if (ws->n == MAX_WORDS) {
      ws->n++; /* too many: the caller refuses */
      return;
    }
    ws->w[ws->n++] = s;
    while (*s && *s != ' ')
      s++;
  }
}

static int eq(const char *a, const char *b) {
  while (*a && *a == *b)
    a++, b++;
  return *a == *b;
}

/* A decimal (or 0x hex) number that fits 32 bits, nothing else. */
static int parse_u32(const char *s, uint32_t *out) {
  uint32_t v = 0, base = 10;
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
    base = 16, s += 2;
  if (!*s)
    return 0;
  for (; *s; s++) {
    uint32_t d;
    if (*s >= '0' && *s <= '9')
      d = (uint32_t) (*s - '0');
    else if (base == 16 && *s >= 'a' && *s <= 'f')
      d = (uint32_t) (*s - 'a' + 10);
    else if (base == 16 && *s >= 'A' && *s <= 'F')
      d = (uint32_t) (*s - 'A' + 10);
    else
      return 0;
    if (v > (0xFFFFFFFFu - d) / base)
      return 0;
    v = v * base + d;
  }
  *out = v;
  return 1;
}
static int parse_i32(const char *s, int32_t *out) {
  uint32_t v;
  if (*s == '-') {
    if (!parse_u32(s + 1, &v) || v > 0x80000000u)
      return 0;
    *out = v == 0x80000000u ? INT32_MIN : -(int32_t) v;
    return 1;
  }
  if (!parse_u32(s, &v) || v > 0x7FFFFFFFu)
    return 0;
  *out = (int32_t) v;
  return 1;
}
/* A number within lo..hi. */
static int num(const char *s, uint32_t lo, uint32_t hi, uint32_t *out) {
  return parse_u32(s, out) && *out >= lo && *out <= hi;
}

/* ---------------------------------------------------------------- multi-line answers */

enum { JOB_NONE = 0, JOB_HELP, JOB_INFO, JOB_ADC, JOB_OPT, JOB_PAR, JOB_FLASH, JOB_TUNE };

static const char *const HELP[] = {
    "help                        this list",
    "info                        firmware, reset, option bytes, self-tests, state",
    "adc                         every ADC channel: raw, mV, value; Hall speed",
    "io                          interlock lines, heartbeat, stage outputs",
    "watch <ms>                  a status line every <ms> (100-60000), 0 off",
    "opt                         option bytes (FLASH_OPTR) against WDG-1",
    "flash <page> [<off> <len>]  hex dump of flash page 0-31 (read only)",
    "par | par <id> <value> | par apply | par save | par discard",
    "tune [<name> <value>]       figures outside the table, RAM only",
    "fan duty <permille> <s>     fixed duty, capped at 10 V, 1-600 s",
    "fan rpm <rpm> <s>           speed loop, 200-6000 rpm, 1-600 s",
    "hb <s>                      heartbeat alone, 1-60 s",
    "pump <n> [<cHz>]            n strokes 1-20 at 10-600 cHz (100)",
    "glow                        one probe pulse (needs a fan action)",
    "glow read                   the glow period's in-pulse and gap readings",
    "pump oc <mA>                one stroke with the overcurrent cut lowered",
    "co | co heater off <s>      the CO alarm; CO_HEAT_EN held low 1-60 s",
    "precheck                    the pre-start self-test alone",
    "run fan|heat <power> <s>    the burn sequencer, 1-3600 s",
    "run prime <s>               priming",
    "stop                        ends every action; a run stops normally",
    "clear                       fault reset",
    "wdg stop | wdg hang         no watchdog refresh / hang: a reset follows",
    "reset                       software reset",
};

/* Provisional figures outside the parameter table that the bench may change, in RAM
 * (safety_bench_tune), described as the table's entries are (params.h): unsigned, min to max. */
#define TUNE(field, lo, hi)                                                                         \
  {.name = #field, .min = lo, .max = hi, .offset = (uint16_t) offsetof(burn_params, field),         \
   .size = (uint8_t) sizeof(((burn_params *) 0)->field)}
static const param_def TUNABLES[] = {
    TUNE(reset_fan_s, 0, 120),          /* GLW-21 */
    TUNE(reset_fan_within_s, 0, 600),   /* GLW-21 */
    TUNE(fan_retry_wait_s, 1, 60),      /* FAN-5 */
    TUNE(fan_retry_forget_s, 60, 3600), /* FAN-5 */
};
#define N_TUNABLES (sizeof(TUNABLES) / sizeof(TUNABLES[0]))

static void info_line(console *c, uint16_t i) {
  safety *s = &c->b->s;
  switch (i) {
    case 0:
      put(c, "fw flamekeeper-bringup " CONSOLE_VERSION " safety ");
      put_u(c, SAFETY_VERSION_MAJOR), put_ch(c, '.'), put_u(c, SAFETY_VERSION_MINOR), put_ch(c, '.');
      put_u(c, SAFETY_VERSION_PATCH);
      kv(c, "uptime_ms", (int32_t) c->now);
      break;
    case 1:
      put(c, "reset ");
      put(c, NAME(RESET, s->reset_reason));
      put(c, " csr ");
      put_0x(c, c->boot.csr, 8);
      break;
    case 2:
      put(c, "opt ");
      put_0x(c, c->boot.optr, 8);
      put(c, (s->checks & LINK_CHECK_OPTION_BYTES) ? " WRONG (see opt)" : " ok");
      break;
    case 3:
      put(c, "checks ");
      put_0x(c, s->checks, 2);
      BITS(c, s->checks, LINK_CHECK);
      put(c, s->checks ? " -> no starts" : "");
      break;
    case 4:
      put(c, "powerup ");
      put(c, !s->chk.power_up_done ? "running" : s->power_up_failed ? "failed" : "passed");
      put_ch(c, ' ');
      put_0x(c, s->power_up_failed, 4);
      BITS(c, s->power_up_failed, CHECK_FAIL);
      kv(c, "latch_armed", hal_overheat_n());
      break;
    case 5:
      put(c, "burn ");
      put(c, NAME(STEP, s->burn.step));
      put(c, " fault ");
      put(c, NAME(FAULT, s->burn.fault));
      put(c, " why ");
      put(c, NAME(WHY, s->burn.why));
      kv(c, "latched", s->burn.fault_latched);
      kv(c, "fan_stalls", s->burn.fan_stalls);
      break;
    case 6:
      put(c, "stages fan ");
      put(c, NAME(FAN_FAULT, s->fan.fault));
      put(c, " pump ");
      put(c, NAME(PUMP_FAULT, s->pump.fault));
      put(c, " glow ");
      put(c, NAME(GLOW_FAULT, s->glow.fault));
      kv(c, "glow_budget_a2s", (int32_t) glow_bucket_a2s(&s->glow));
      put(c, " co ");
      put(c, NAME(CO_STATE, s->co.state));
      break;
    default:
      put(c, "vdda_mv ");
      put_u(c, c->b->vdda_mv);
      kv(c, "vrefint_cal", c->b->vrefint_cal);
      kv(c, "plug_record", s->plug.rec.verified_v);
      kv(c, "plug_setting", s->burn.p.plug_v);
      kv(c, "link_bad_frames", (int32_t) s->parser.bad);
      kv(c, "out_dropped", (int32_t) c->dropped);
      break;
  }
}

static void adc_line(console *c, uint16_t i) {
  const board *b = c->b;
  uint16_t v = b->vdda_mv;
  if (i >= HAL_ADC_COUNT) {
    put(c, "hall rpm ");
    put_u(c, b->s.in.fan_rpm);
    put(c, " (one pulse a turn assumed)  glow_probe ");
    put_fix(c, b->s.glow.i_ca, 2);
    put(c, " A at ");
    put_fix(c, b->s.glow.vin_then_cv, 2);
    put(c, " V");
    return;
  }
  uint16_t raw = b->raw[i];
  put(c, "adc ");
  put(c, ADC_NAME[i]);
  kv(c, "raw", raw);
  kv(c, "mv", (int32_t) analog_mv(raw, v));
  put(c, " = ");
  switch (i) {
    case HAL_ADC_GLOW_V:
    case HAL_ADC_PUMP_V:
      put_fix(c, analog_div19_cv(raw, v), 2), put(c, " V (x19)");
      break;
    case HAL_ADC_GLOW_I:
      put_fix(c, analog_glow_ca(raw, v), 2), put(c, " A (9 mV/A)");
      break;
    case HAL_ADC_FAN_I:
      put_fix(c, analog_fan_ma(raw, v), 3), put(c, " A average switch current (15 mV/A)");
      break;
    case HAL_ADC_PUMP_I:
      put_u(c, analog_pump_ma(raw, v)), put(c, " mA (0.45 V/A)");
      break;
    case HAL_ADC_NTC: {
      int16_t t = analog_hx_c10(raw);
      if (t == ANALOG_UNKNOWN_TEMP)
        put(c, "open");
      else
        put_fix(c, t, 1), put(c, " C heat exchanger");
      break;
    }
    case HAL_ADC_BOARD: {
      int16_t t = analog_board_c10(raw);
      if (t == ANALOG_UNKNOWN_TEMP)
        put(c, "out of range");
      else
        put_fix(c, t, 1), put(c, " C board");
      break;
    }
    case HAL_ADC_VIN:
      put_fix(c, analog_vin_cv(raw, v), 2), put(c, " V supply (x106.8/6.8)");
      break;
    case HAL_ADC_VREF:
      put_u(c, v), put(c, " mV VDDA"), kv(c, "cal", b->vrefint_cal);
      break;
    default:
      put_fix(c, (int32_t) ((uint32_t) raw * 1000u / 4095u), 1), put(c, " % of VDDA");
      break;
  }
}

static void opt_line(console *c, uint16_t i) {
  uint32_t o = c->boot.optr;
  switch (i) {
    case 0:
      put(c, "optr ");
      put_0x(c, o, 8);
      put(c, " rdp ");
      put_0x(c, o & 0xFFu, 2);
      kv(c, "nrst_stop", (int32_t) ((o >> 13) & 1u));
      kv(c, "nrst_stdby", (int32_t) ((o >> 14) & 1u));
      kv(c, "iwdg_sw", (int32_t) ((o >> 16) & 1u));
      kv(c, "iwdg_stop", (int32_t) ((o >> 17) & 1u));
      kv(c, "iwdg_stdby", (int32_t) ((o >> 18) & 1u));
      kv(c, "wwdg_sw", (int32_t) ((o >> 19) & 1u));
      break;
    case 1:
      put(c, "optr");
      kv(c, "nboot_sel", (int32_t) ((o >> 24) & 1u));
      kv(c, "nboot1", (int32_t) ((o >> 25) & 1u));
      kv(c, "nboot0", (int32_t) ((o >> 26) & 1u));
      kv(c, "nrst_mode", (int32_t) ((o >> 27) & 3u));
      kv(c, "optverr", (int32_t) ((c->boot.flash_sr >> 15) & 1u));
      break;
    default: /* WDG-1: the driver's own decision (hal_option_faults) */
      put(c, "want iwdg_sw=0 iwdg_stop=1 iwdg_stdby=1 nboot_sel=0 nboot1=1 nboot0=1 optverr=0: ");
      put(c, hal_optr_wdg1_ok(o, c->boot.flash_sr) ? "ok" : "WRONG");
      break;
  }
}

static void par_line(console *c, uint16_t i) {
  const safety *s = &c->b->s;
  const param_def *d = &PARAM_TABLE[i];
  put(c, "par ");
  put_0x(c, d->id, 2);
  put_ch(c, ' ');
  put(c, d->name);
  put(c, " = ");
  put_fix(c, params_value(&s->params, i), d->decimals);
  if (s->params.staged_mask & (1u << i)) {
    put(c, " staged ");
    put_fix(c, params_staged(&s->params, i), d->decimals);
  }
}

static void tune_line(console *c, uint16_t i) {
  put(c, "tune ");
  put(c, TUNABLES[i].name);
  put(c, " = ");
  put_u(c, (uint32_t) params_read_field(&c->b->s.params.live, &TUNABLES[i]));
  put(c, " (");
  put_u(c, (uint32_t) TUNABLES[i].min);
  put_ch(c, '-');
  put_u(c, (uint32_t) TUNABLES[i].max);
  put_ch(c, ')');
}

static void flash_line(console *c, uint16_t i) {
  uint8_t buf[16];
  uint32_t n = c->job_n - i * 16u < 16u ? c->job_n - i * 16u : 16u;
  uint32_t addr = c->job_addr + i * 16u;
  bringup_sys_read(addr, buf, n);
  put(c, "flash ");
  put_hex(c, addr, 8);
  for (uint32_t k = 0; k < n; k++) {
    put_ch(c, ' ');
    put_hex(c, buf[k], 2);
  }
}

static uint16_t job_lines(const console *c) {
  switch (c->job) {
    case JOB_HELP: return (uint16_t) (sizeof(HELP) / sizeof(HELP[0]));
    case JOB_INFO: return 8;
    case JOB_ADC: return HAL_ADC_COUNT + 1;
    case JOB_OPT: return 3;
    case JOB_PAR: return (uint16_t) PARAM_COUNT;
    case JOB_TUNE: return (uint16_t) N_TUNABLES;
    case JOB_FLASH: return (uint16_t) ((c->job_n + 15u) / 16u);
    default: return 0;
  }
}

static void job_step(console *c) {
  while (c->job && out_free(c) >= 160) {
    if (c->job_i >= job_lines(c)) {
      c->job = JOB_NONE;
      put(c, "ok");
      nl(c);
      return;
    }
    uint16_t i = c->job_i++;
    switch (c->job) {
      case JOB_HELP: put(c, HELP[i]); break;
      case JOB_INFO: info_line(c, i); break;
      case JOB_ADC: adc_line(c, i); break;
      case JOB_OPT: opt_line(c, i); break;
      case JOB_PAR: par_line(c, i); break;
      case JOB_TUNE: tune_line(c, i); break;
      case JOB_FLASH: flash_line(c, i); break;
    }
    nl(c);
  }
}

static const char *job(console *c, uint8_t kind) {
  c->job = kind;
  c->job_i = 0;
  return NULL;
}

/* ---------------------------------------------------------------- runs (the burn sequencer) */

/* As the ESP32 asks: a POLL through the link's own handler, and the dead-man fed. */
static void poll(console *c, uint8_t start, uint8_t mode) {
  safety *s = &c->b->s;
  link_desired w;
  memset(&w, 0, sizeof(w));
  w.start = start;
  w.mode = mode;
  w.power = c->run_power;
  link_status st;
  s->mcu.poll(s->mcu.ctx, &w, &st);
  link_deadman_feed(&s->deadman, c->now);
  c->run_poll_ms = c->now;
}

static const char *run_start(console *c, uint8_t mode, uint8_t power, uint32_t seconds) {
  safety *s = &c->b->s;
  if (c->run)
    return "busy: a run is on (stop)";
  if (bench_busy(&c->bench))
    return "busy: a bench action is on (stop)";
  if (s->burn.step != BURN_STEP_OFF)
    return "not off";
  c->run_power = power;
  if (!s->gate.seen)
    poll(c, 0, LINK_MODE_OFF); /* the first poll after a reset only records its number */
  c->run_start = (uint8_t) (s->gate.last + 1u);
  poll(c, c->run_start, mode);
  if (s->start_result != LINK_OK)
    return link_result(s->start_result);
  c->run = CONSOLE_RUN_ON;
  c->run_mode = mode;
  c->run_begin_ms = c->now;
  c->run_end_ms = c->now + seconds * 1000u;
  c->run_step = 0xFF;
  return NULL;
}

static void run_stop(console *c) {
  if (c->run == CONSOLE_RUN_ON) {
    poll(c, c->run_start, LINK_MODE_OFF); /* a normal stop: burn-off and cooldown follow */
    c->run = CONSOLE_RUN_STOPPING;
  }
}

static void run_ms(console *c) {
  const safety *s = &c->b->s;
  if (!c->run)
    return;
  if (c->run == CONSOLE_RUN_ON) {
    if ((int32_t) (c->now - c->run_end_ms) >= 0) {
      run_stop(c);
      put(c, "* run time up: stopping");
      nl(c);
    } else if (since(c->now, c->run_poll_ms) >= CONSOLE_POLL_MS) {
      poll(c, c->run_start, c->run_mode);
    }
  }
  if (s->burn.step != c->run_step && out_free(c) >= 160) {
    c->run_step = s->burn.step;
    put(c, "* step ");
    put(c, NAME(STEP, s->burn.step));
    kvf(c, "t", (int32_t) (since(c->now, c->run_begin_ms) / 100u), 1);
    nl(c);
    if (s->burn.step == BURN_STEP_OFF) {
      put(c, "* run ended fault ");
      put(c, NAME(FAULT, s->burn.fault));
      put(c, " why ");
      put(c, NAME(WHY, s->burn.why));
      nl(c);
      c->run = CONSOLE_RUN_NONE;
    }
  }
}

/* ---------------------------------------------------------------- commands */

static const char *bench_result(int r) { return r == BENCH_OK ? NULL : NAME(BENCH_ERR, r); }

static const char *cmd_par(console *c, words *ws) {
  safety *s = &c->b->s;
  if (ws->n == 1)
    return job(c, JOB_PAR);
  int r;
  if (ws->n == 2 && eq(ws->w[1], "apply"))
    r = s->mcu.param_apply(s->mcu.ctx, 0);
  else if (ws->n == 2 && eq(ws->w[1], "save"))
    r = s->mcu.param_apply(s->mcu.ctx, 1);
  else if (ws->n == 2 && eq(ws->w[1], "discard"))
    r = s->mcu.param_discard(s->mcu.ctx);
  else if (ws->n == 3) {
    uint32_t id;
    int32_t v;
    if (!num(ws->w[1], 0, 0xFFFF, &id) || !parse_i32(ws->w[2], &v))
      return "usage: par <id> <value> (raw units, as listed without the point)";
    r = s->mcu.param_set(s->mcu.ctx, (uint16_t) id, v);
  } else {
    return "usage: par | par <id> <value> | par apply | par save | par discard";
  }
  return r == LINK_OK ? NULL : link_result(r);
}

static const char *cmd_tune(console *c, words *ws) {
  safety *s = &c->b->s;
  if (ws->n == 1)
    return job(c, JOB_TUNE);
  if (ws->n != 3)
    return "usage: tune [<name> <value>]";
  for (unsigned i = 0; i < N_TUNABLES; i++) {
    const param_def *t = &TUNABLES[i];
    if (!eq(ws->w[1], t->name))
      continue;
    uint32_t v;
    if (!num(ws->w[2], (uint32_t) t->min, (uint32_t) t->max, &v))
      return "out of range";
    return safety_bench_tune(s, t, (int32_t) v) == LINK_OK ? NULL : "not off";
  }
  return "unknown name (tune lists them)";
}

static const char *cmd_flash(console *c, words *ws) {
  uint32_t page, off = 0, len = 2048;
  if ((ws->n != 2 && ws->n != 4) || !num(ws->w[1], 0, 31, &page))
    return "usage: flash <page 0-31> [<offset> <length>]";
  if (ws->n == 4 && (!num(ws->w[2], 0, 2047, &off) || !num(ws->w[3], 1, 2048 - off, &len)))
    return "out of range";
  c->job_addr = 0x08000000u + page * 2048u + off;
  c->job_n = (uint16_t) len;
  return job(c, JOB_FLASH);
}

static void io_line(console *c) {
  const board *b = c->b;
  const safety *s = &b->s;
  put(c, "io");
  kv(c, "latch_armed", hal_overheat_n());
  kv(c, "pump_hs_ok", hal_pump_hs_flt_n());
  kv(c, "glow_hs_ok", hal_pwr_flt_n());
  kv(c, "hb_ms", s->hb_running_ms);
  kv(c, "bursts", (int32_t) b->bursts);
  kv(c, "loops", (int32_t) b->loops);
  kv(c, "refreshes", (int32_t) b->loops); /* the watchdog, once per loop pass */
  nl(c);
  put(c, "io");
  kv(c, "fan_duty", s->fan.duty);
  kv(c, "pump_stroking", s->pump.stroking);
  kv(c, "pump_strokes", (int32_t) s->pump.strokes);
  kv(c, "glow_on", s->glow.on);
  kv(c, "glow_en", b->glow_en_test || b->glow_en_planner);
  kv(c, "glow_budget_a2s", (int32_t) glow_bucket_a2s(&s->glow));
  nl(c);
  put(c, "io");
  kv(c, "pump_oc_armed", c->oc_on);
  kv(c, "pump_oc_ma", s->pump.p.oc_ma);
  kv(c, "pump_oc_raw", c->oc_threshold_raw);
  kv(c, "pump_oc_hold_us", c->oc_hold_us);
  kv(c, "pump_oc_arms", (int32_t) c->oc_arms);
  kv(c, "co_heat_en", c->co_pin);
  kv(c, "co_heat_wanted", c->co_wanted);
  nl(c);
}

static void glow_sample_line(console *c, const char *what, const hal_glow_sample *g) {
  uint16_t v = c->b->vdda_mv;
  put(c, what);
  kv(c, "pulse_raw", g->pulse_i);
  kvf(c, "pulse_A", analog_glow_ca(g->pulse_i, v), 2);
  kv(c, "gap_raw", g->gap_i);
  kvf(c, "gap_A", analog_glow_ca(g->gap_i, v), 2);
  kv(c, "gap_vin_raw", g->gap_vin);
  kvf(c, "gap_vin", analog_vin_cv(g->gap_vin, v), 2);
  nl(c);
}

static void glow_read(console *c) {
  put(c, "glow periods=");
  put_u(c, c->glow_periods);
  kv(c, "last_pulse_ms_ago", c->glow_pulse_ms ? (int32_t) since(c->now, c->glow_pulse_ms) : -1);
  nl(c);
  glow_sample_line(c, "glow last", &c->glow_last);
  glow_sample_line(c, "glow last_with_pulse", &c->glow_pulse);
}

static void co_lines(console *c) {
  const safety *s = &c->b->s;
  const co_alarm *a = &s->co;
  uint16_t raw = c->b->raw[HAL_ADC_CO];
  put(c, "co ");
  put(c, NAME(CO_STATE, a->state));
  kv(c, "fitted", a->cfg.fitted);
  kv(c, "param_0x52", s->burn.p.co_fitted);
  kv(c, "raw", raw);
  uint32_t rs = co_rs_ohm(raw);
  if (rs == 0xFFFFFFFFu)
    put(c, " rs=open");
  else
    kv(c, "rs_ohm", (int32_t) (rs > 0x7FFFFFFFu ? 0x7FFFFFFFu : rs));
  nl(c);
  put(c, "co");
  kv(c, "r0_ohm", (int32_t) a->r0);
  kv(c, "r0_saved_ohm", (int32_t) s->co_r0_saved);
  if (a->ratio == 0xFFFFu)
    put(c, " ratio=unknown");
  else
    kvf(c, "ratio", a->ratio, 3);
  if (a->ppm == CO_UNKNOWN_PPM)
    put(c, " ppm=unknown");
  else
    kv(c, "ppm", a->ppm);
  nl(c);
  put(c, "co");
  kv(c, "alarm", a->alarm);
  kv(c, "fault", a->fault);
  kv(c, "heater_checked", a->checked);
  kv(c, "checking", a->checking);
  kv(c, "learned_s", (int32_t) (a->learned_ms / 1000u));
  kv(c, "heat_wanted", c->co_wanted);
  kv(c, "heat_en", c->co_pin);
  if (c->co_off)
    kv(c, "held_off_s", (int32_t) ((c->co_off_end_ms - c->now) / 1000u));
  nl(c);
}

static void watch_line(console *c) {
  const board *b = c->b;
  const safety *s = &b->s;
  uint16_t v = b->vdda_mv;
  put(c, "w");
  kv(c, "t", (int32_t) c->now);
  put_ch(c, ' ');
  put(c, NAME(STEP, s->burn.step));
  put(c, " f=");
  put(c, NAME(FAULT, s->burn.fault));
  kvf(c, "vin", s->in.supply_cv, 2);
  if (s->in.hx_c10 == BURN_UNKNOWN_TEMP)
    put(c, " hx=open");
  else
    kvf(c, "hx", s->in.hx_c10, 1);
  kvf(c, "brd", s->in.board_c10, 1);
  kv(c, "rpm", s->in.fan_rpm);
  kv(c, "duty", s->fan.duty);
  kvf(c, "fanA", (int32_t) analog_fan_ma(b->raw[HAL_ADC_FAN_I], v), 3);
  kv(c, "pump", s->burn.out.pump_cHz);
  kv(c, "pmA", analog_pump_ma(b->raw[HAL_ADC_PUMP_I], v));
  kv(c, "strokes", (int32_t) s->pump.strokes);
  kv(c, "glowW", s->burn.out.glow_w);
  kvf(c, "glowA", s->glow.i_ca, 2);
  kv(c, "hb", s->hb_running_ms);
  nl(c);
}

static const char *cmd_fan(console *c, words *ws) {
  safety *s = &c->b->s;
  uint32_t v, sec;
  if (ws->n == 2 && eq(ws->w[1], "off")) {
    if (c->bench.fan)
      c->bench.fan_end_ms = c->now; /* ends at the next step, reported as done */
    return NULL;
  }
  if (ws->n != 4 || !num(ws->w[3], 1, BENCH_FAN_MAX_S, &sec))
    return "usage: fan duty <permille> <s> | fan rpm <rpm> <s> | fan off";
  if (eq(ws->w[1], "duty") && num(ws->w[2], 1, 1000, &v))
    return bench_result(bench_fan(&c->bench, s, c->now, BENCH_ACT_FAN_DUTY, (uint16_t) v, (uint16_t) sec));
  if (eq(ws->w[1], "rpm") && num(ws->w[2], BENCH_FAN_RPM_MIN, BENCH_FAN_RPM_MAX, &v))
    return bench_result(bench_fan(&c->bench, s, c->now, BENCH_ACT_FAN_RPM, (uint16_t) v, (uint16_t) sec));
  return "out of range";
}

static const char *bench_or_run_busy(const console *c) { return c->run ? "busy: a run is on (stop)" : NULL; }

static const char *execute(console *c, char *line) {
  safety *s = &c->b->s;
  words ws;
  split(line, &ws);
  if (ws.n == 0)
    return NULL;
  if (ws.n > MAX_WORDS)
    return "too many words";
  const char *w0 = ws.w[0];
  uint32_t v, v2;

  if (eq(w0, "help"))
    return job(c, JOB_HELP);
  if (eq(w0, "info"))
    return job(c, JOB_INFO);
  if (eq(w0, "adc"))
    return job(c, JOB_ADC);
  if (eq(w0, "opt"))
    return job(c, JOB_OPT);
  if (eq(w0, "io")) {
    io_line(c);
    return NULL;
  }
  if (eq(w0, "watch")) {
    if (ws.n != 2 || !parse_u32(ws.w[1], &v) || (v != 0 && (v < 100 || v > 60000)))
      return "usage: watch <ms 100-60000> | watch 0";
    c->watch_ms = (uint16_t) v;
    c->watch_last = c->now;
    return NULL;
  }
  if (eq(w0, "flash"))
    return cmd_flash(c, &ws);
  if (eq(w0, "glow") && ws.n == 2 && eq(ws.w[1], "read")) {
    glow_read(c);
    return NULL;
  }
  if (eq(w0, "co") && ws.n == 1) {
    co_lines(c);
    return NULL;
  }
  if (eq(w0, "co")) {
    if (ws.n != 4 || !eq(ws.w[1], "heater") || !eq(ws.w[2], "off") || !num(ws.w[3], 1, CONSOLE_CO_OFF_MAX_S, &v))
      return "usage: co | co heater off <s 1-60>";
    if (s->burn.step != BURN_STEP_OFF || c->run)
      return "not off";
    c->co_off = 1;
    c->co_off_end_ms = c->now + v * 1000u;
    return NULL;
  }
  if (eq(w0, "par"))
    return cmd_par(c, &ws);
  if (eq(w0, "tune"))
    return cmd_tune(c, &ws);
  if (eq(w0, "stop")) {
    bench_stop(&c->bench, s, c->now);
    run_stop(c);
    return NULL;
  }
  if (eq(w0, "clear")) {
    int r = s->mcu.fault_reset(s->mcu.ctx);
    return r == LINK_OK ? NULL : "refused (nothing latched, or its cause is still there)";
  }
  if (eq(w0, "wdg")) {
    if (ws.n == 2 && eq(ws.w[1], "stop")) {
      c->wdg_stopped = 1;
      put(c, "watchdog refresh stopped: expect a reset in about 80 ms");
      nl(c);
      return NULL;
    }
    if (ws.n == 2 && eq(ws.w[1], "hang")) {
      c->hang_asked = 1;
      put(c, "hanging the main loop, timers left running: expect a reset in about 80 ms");
      nl(c);
      return NULL;
    }
    return "usage: wdg stop | wdg hang";
  }
  if (eq(w0, "reset")) {
    c->reset_asked = 1;
    return NULL;
  }

  /* Commands that drive a stage. */
  const char *busy = bench_or_run_busy(c);
  if (eq(w0, "fan"))
    return busy ? busy : cmd_fan(c, &ws);
  if (eq(w0, "hb")) {
    if (ws.n != 2 || !num(ws.w[1], 1, BENCH_HB_MAX_S, &v))
      return "usage: hb <s 1-60>";
    return busy ? busy : bench_result(bench_hb(&c->bench, s, c->now, (uint16_t) v));
  }
  if (eq(w0, "pump") && ws.n == 3 && eq(ws.w[1], "oc")) {
    if (!num(ws.w[2], BENCH_PUMP_OC_MIN_MA, 0xFFFFu, &v) || v >= s->pump.p.oc_ma)
      return "usage: pump oc <mA from 100 to below the cut's setting>";
    return busy ? busy : bench_result(bench_pump_oc(&c->bench, s, c->now, (uint16_t) v));
  }
  if (eq(w0, "pump")) {
    v2 = BENCH_PUMP_DEFAULT_CHZ;
    if (ws.n < 2 || ws.n > 3 || !num(ws.w[1], 1, BENCH_PUMP_MAX_STROKES, &v) ||
        (ws.n == 3 && !num(ws.w[2], BENCH_PUMP_MIN_CHZ, s->pump.p.max_cHz, &v2)))
      return "usage: pump <strokes 1-20> [<cHz 10-600>]";
    return busy ? busy : bench_result(bench_pump(&c->bench, s, c->now, (uint16_t) v, (uint16_t) v2));
  }
  if (eq(w0, "glow")) {
    if (ws.n != 1)
      return "usage: glow";
    return busy ? busy : bench_result(bench_glow(&c->bench, s, c->now));
  }
  if (eq(w0, "precheck"))
    return busy ? busy : bench_result(bench_precheck(&c->bench, s, c->now));
  if (eq(w0, "run")) {
    if (ws.n == 3 && eq(ws.w[1], "prime") && num(ws.w[2], 1, CONSOLE_RUN_MAX_S, &v))
      return run_start(c, LINK_MODE_PRIME, 0, v);
    if (ws.n == 4 && num(ws.w[2], 0, 100, &v) && num(ws.w[3], 1, CONSOLE_RUN_MAX_S, &v2)) {
      if (eq(ws.w[1], "fan"))
        return run_start(c, LINK_MODE_FAN, (uint8_t) v, v2);
      if (eq(ws.w[1], "heat"))
        return run_start(c, LINK_MODE_HEAT, (uint8_t) v, v2);
    }
    return "usage: run fan|heat <power 0-100> <s 1-3600> | run prime <s>";
  }
  return "unknown command (help)";
}

/* ---------------------------------------------------------------- events */

static void bench_event_line(console *c, const bench_event *e) {
  put(c, "* ");
  put(c, NAME(ACTION, e->action));
  put_ch(c, ' ');
  put(c, NAME(END, e->end));
  if (e->end == BENCH_END_FAULT) {
    put(c, " fault ");
    put(c, e->action <= BENCH_ACT_FAN_RPM ? NAME(FAN_FAULT, e->fault)
            : e->action == BENCH_ACT_GLOW ? NAME(GLOW_FAULT, e->fault)
                                          : NAME(PUMP_FAULT, e->fault));
  }
  if (e->end == BENCH_END_PRECHECK_FAILED) {
    put(c, " precheck ");
    put_0x(c, e->fail, 4);
    BITS(c, e->fail, CHECK_FAIL);
  }
  if (e->action == BENCH_ACT_PUMP_OC)
    put(c, e->end == BENCH_END_FAULT && e->fault == PUMP_FAULT_OVERCURRENT ? " (the cut acted)"
                                                                            : " (NO CUT)");
  if (e->action == BENCH_ACT_PUMP || e->action == BENCH_ACT_PUMP_OC) {
    kv(c, "strokes", e->a);
    kv(c, "peak_min_ma", e->b);
    kv(c, "peak_max_ma", e->c);
  }
  if (e->action == BENCH_ACT_GLOW && e->end == BENCH_END_DONE) {
    kvf(c, "i", e->a, 2);
    kvf(c, "vin", e->b, 2);
    if (e->c == 0xFFFFu)
      put(c, " r=none");
    else
      kv(c, "r_mohm", e->c);
    kv(c, "cold_type", e->d);
  }
  nl(c);
}

static void events(console *c) {
  safety *s = &c->b->s;
  if (!c->powerup_told && s->chk.power_up_done && out_free(c) >= 160) {
    c->powerup_told = 1;
    put(c, "* powerup ");
    put(c, s->power_up_failed ? "failed " : "passed ");
    put_0x(c, s->power_up_failed, 4);
    BITS(c, s->power_up_failed, CHECK_FAIL);
    nl(c);
  }
  bench_event e;
  while (out_free(c) >= 160 && bench_next_event(&c->bench, &e))
    bench_event_line(c, &e);
}

/* ---------------------------------------------------------------- the step */

void console_init(console *c, board *b, const console_boot *boot, uint32_t now) {
  memset(c, 0, sizeof(*c));
  c->b = b;
  c->boot = *boot;
  c->now = now;
  bench_init(&c->bench);
  tap = c;
  put(c, "* boot flamekeeper-bringup " CONSOLE_VERSION " reset ");
  put(c, NAME(RESET, b->s.reset_reason));
  put(c, " csr ");
  put_0x(c, boot->csr, 8);
  put(c, " opt ");
  put_0x(c, boot->optr, 8);
  put(c, (b->s.checks & LINK_CHECK_OPTION_BYTES) ? " WRONG" : " ok");
  nl(c);
}

void console_ms(console *c, uint32_t now) {
  safety *s = &c->b->s;
  c->now = now;

  if (c->has_cmd && !c->job && out_free(c) >= 256) {
    put(c, "> ");
    put(c, c->cmd);
    nl(c);
    const char *err = execute(c, c->cmd);
    c->has_cmd = 0;
    if (err) {
      put(c, "err ");
      put(c, err);
      nl(c);
    } else if (!c->job) {
      put(c, "ok");
      nl(c);
    }
  }
  job_step(c);

  bench_ms(&c->bench, s, now);
  run_ms(c);
  /* "co heater off" ends on time, or when the burn leaves off. */
  if (c->co_off && ((int32_t) (now - c->co_off_end_ms) >= 0 || s->burn.step != BURN_STEP_OFF)) {
    c->co_off = 0;
    if (out_free(c) >= 160) {
      put(c, "* co heater back");
      nl(c);
    }
  }
  if (!c->job)
    events(c);
  if (c->watch_ms && since(now, c->watch_last) >= c->watch_ms && !c->job) {
    c->watch_last = now;
    if (out_free(c) >= 200)
      watch_line(c);
  }

  /* Out, at a pace the UART keeps up with whatever the link sends meanwhile. */
  uint8_t chunk[CONSOLE_TX_PER_MS];
  size_t n = 0;
  while (n < sizeof(chunk) && c->tail != c->head) {
    chunk[n++] = (uint8_t) c->out[c->tail];
    c->tail = (uint16_t) ((c->tail + 1u) % CONSOLE_OUT_SIZE);
  }
  if (n)
    hal_uart_write(chunk, n);

  /* A hang or a reset waits until its message is out. */
  if (c->tail == c->head) {
    if (c->hang_asked)
      c->hang = 1;
    if (c->reset_asked)
      bringup_sys_reset();
  }
}
