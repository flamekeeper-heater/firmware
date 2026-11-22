/* The bring-up build against a fake chip with a fan, a pump and a glow plug: the console's parser
 * and its coexistence with the link, the readings, and the limits of every bench action (each
 * through the core's own stage drivers, never outside their rules), the watchdog tests and runs
 * of the burn sequencer. Also the fan stage's open-loop mode (fan_step_open). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../board/analog.h"
#include "../bringup/console.h"

#define TEXT_MAX (1u << 20)
static char text[TEXT_MAX]; /* everything the console sent */
static size_t text_len, text_mark;
static int n_checks = 0, failures = 0;
#define CHECK(c)                                                  \
  do {                                                            \
    n_checks++;                                                   \
    if (!(c)) {                                                   \
      failures++;                                                 \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);         \
      if (getenv("SHOW")) /* SHOW=1: the console's answer too */ \
        printf("---\n%s\n---\n", text + text_mark);            \
    }                                                             \
  } while (0)

/* ---------------------------------------------------------------- the fake chip */

static const double VDDA = 3.3, VREFINT = 1.212;
static uint16_t counts(double volts) {
  double c = volts / VDDA * 4095.0 + 0.5;
  return (uint16_t) (c > 4095 ? 4095 : c < 0 ? 0 : c);
}

static struct {
  uint32_t now;
  double vin;
  uint16_t ntc_raw, board_raw;
  uint8_t hb_mode, glow_en, latch_level, latch_armed;
  uint32_t burst_ms, hb_start_ms;
  uint16_t fan_duty, pump_duty, glow_us, test_pulse_us;
  uint32_t iwdg_refreshes, resets;
  uint8_t rx[512];
  size_t rx_len, rx_pos;
  uint8_t link_tx[512];
  size_t link_len;
  size_t max_text_write;
  /* models */
  double rpm;
  int fan_blocked, pump_open;
  double plug_ohm;
  /* what happened */
  uint16_t pending_glow_us;
  uint32_t glow_next_ms;
  /* the pump's overcurrent watchdog (PMP-7) */
  uint8_t oc_armed, oc_report, pump_forced;
  uint16_t oc_threshold;
  uint32_t oc_cuts;
  uint8_t co_heater;
  uint32_t glow_pulses, glow_bad, glow_last_us;
  int pump_was_on;
  uint32_t strokes, stroke_start, stroke_len_min, stroke_len_max, stroke_gap_min, last_stroke_end;
  uint32_t pump_unpowered_ms; /* PUMP_CMD high outside a heartbeat that has run 20 ms */
  uint32_t last_pump_on_ms;
  uint16_t pump_duty_seen;
} chip;

static int hb_ok(void) { return chip.hb_mode == HAL_HB_BURST && (uint32_t) (chip.now - chip.burst_ms) < 25u; }
static int pump_powered(void) { return hb_ok() && chip.latch_armed; }
static double motor_v(void) { return chip.fan_duty / 1000.0 * chip.vin; }
static double motor_i(void) { return chip.fan_blocked ? motor_v() / 1.5 : (chip.fan_duty ? 0.2 + 0.4 * motor_v() : 0); }

void hal_adc_scan(uint16_t raw[HAL_ADC_COUNT]) {
  raw[HAL_ADC_GLOW_V] = counts(chip.glow_en && hb_ok() ? chip.vin / 19.0 : 0);
  raw[HAL_ADC_GLOW_I] = 0;
  raw[HAL_ADC_PUMP_V] = counts(pump_powered() ? chip.vin / 19.0 : 0.02);
  raw[HAL_ADC_FAN_I] = counts(motor_i() * chip.fan_duty / 1000.0 * 0.015);
  raw[HAL_ADC_PUMP_I] = counts(pump_powered() && chip.pump_duty && !chip.pump_forced && !chip.pump_open ? 1.2 * 0.45 : 0);
  raw[HAL_ADC_NTC] = chip.ntc_raw;
  raw[HAL_ADC_BOARD] = chip.board_raw;
  raw[HAL_ADC_VIN] = counts(chip.vin * 6.8 / 106.8);
  raw[HAL_ADC_VREF] = counts(VREFINT);
  raw[HAL_ADC_CO] = counts(1.0);
}
uint16_t hal_vrefint_cal(void) { return (uint16_t) (VREFINT / 3.0 * 4095.0 + 0.5); }
/* The glow timer: a period every 5 ms; the pulse the planner set last goes out in it, with whatever
 * enable and heartbeat there are, and its reading comes with the period. */
uint8_t bringup_chip_glow_period(hal_glow_sample *g) {
  if ((int32_t) (chip.now - chip.glow_next_ms) < 0)
    return 0;
  chip.glow_next_ms = chip.now + 5;
  g->pulse_i = 0;
  g->gap_i = 0;
  g->gap_vin = counts(chip.vin * 6.8 / 106.8);
  if (chip.pending_glow_us) {
    chip.glow_pulses++;
    chip.glow_last_us = chip.pending_glow_us;
    int on = chip.glow_en && hb_ok();
    if (!on || (uint32_t) (chip.now - chip.hb_start_ms) < 20u)
      chip.glow_bad++;
    g->pulse_i = on ? counts(chip.vin / chip.plug_ohm * 0.009) : 0;
    chip.pending_glow_us = 0;
  }
  return 1;
}
uint8_t hal_pump_overcurrent(void) {
  uint8_t r = chip.oc_report;
  chip.oc_report = 0;
  return r;
}
void bringup_chip_pump_oc_arm(uint8_t on, uint16_t threshold_raw, uint16_t hold_us) {
  (void) hold_us;
  if (on && !chip.oc_armed && !chip.oc_report)
    chip.pump_forced = 0; /* a new stroke: PUMP_CMD back to its PWM */
  chip.oc_armed = on;
  chip.oc_threshold = threshold_raw;
}
void bringup_chip_co_heater(uint8_t on) { chip.co_heater = on; }
const uint8_t *hal_flash_page(uint8_t page) {
  static uint8_t erased[HAL_FLASH_PAGE_SIZE];
  (void) page;
  memset(erased, 0xFF, sizeof(erased));
  return erased;
}
int hal_flash_program(uint8_t page, uint16_t offset, const uint8_t *data, size_t len) {
  (void) page, (void) offset, (void) data, (void) len;
  return 0;
}
uint8_t hal_flash_unreadable(uint8_t page) {
  (void) page;
  return 0;
}
uint16_t hal_pump_peak_raw(void) {
  uint16_t r = chip.test_pulse_us && pump_powered() && !chip.pump_open ? counts(0.4 * 0.45) : 0;
  chip.test_pulse_us = 0;
  return r;
}
uint16_t hal_hall_rpm(void) { return chip.rpm < 50 ? 0 : (uint16_t) chip.rpm; }
uint8_t hal_overheat_n(void) { return chip.latch_armed; }
uint8_t hal_pump_hs_flt_n(void) { return 1; }
uint8_t hal_pwr_flt_n(void) { return 1; }
void hal_heartbeat(uint8_t mode) {
  if (mode == HAL_HB_BURST) {
    if (!hb_ok())
      chip.hb_start_ms = chip.now;
    chip.burst_ms = chip.now;
  }
  chip.hb_mode = mode;
}
void hal_glow_en(uint8_t on) { chip.glow_en = on; }
void hal_glow_next_pulse_us(uint16_t us) { chip.pending_glow_us = us; }
void hal_fan_duty(uint16_t p) { chip.fan_duty = p; }
void hal_pump_duty(uint16_t p) { chip.pump_duty = p; }
void hal_pump_test_pulse_us(uint16_t us) { chip.test_pulse_us = us; }
void hal_latch_reset(uint8_t level) {
  if (level && !chip.latch_level && chip.ntc_raw > 1000)
    chip.latch_armed = 1;
  chip.latch_level = level;
}
void hal_uart_write(const uint8_t *d, size_t n) {
  if (n >= 7 && d[0] == LINK_REPLY) { /* a link reply, whole */
    memcpy(chip.link_tx, d, n);
    chip.link_len = n;
    return;
  }
  if (n > chip.max_text_write)
    chip.max_text_write = n;
  if (text_len + n < TEXT_MAX) {
    memcpy(text + text_len, d, n);
    text_len += n;
    text[text_len] = 0;
  }
}
size_t hal_flash_read(uint8_t page, uint8_t *out, size_t cap) {
  (void) page, (void) out, (void) cap;
  return 0;
}
int hal_flash_write(uint8_t page, const uint8_t *d, size_t n) {
  (void) page, (void) d, (void) n;
  return 0;
}
int bringup_chip_uart_getc(void) { return chip.rx_pos < chip.rx_len ? chip.rx[chip.rx_pos++] : -1; }
void bringup_chip_iwdg_refresh(void) { chip.iwdg_refreshes++; }
void bringup_sys_reset(void) { chip.resets++; }
void bringup_sys_read(uint32_t addr, uint8_t *out, size_t n) {
  for (size_t i = 0; i < n; i++)
    out[i] = (uint8_t) ((addr + i) * 7u);
}

/* What the stages did in the millisecond that ended: the models and the rule checks. */
static void model_ms(void) {
  /* The fan: towards 500 rpm per motor volt, about 0.3 s to settle. */
  double target = chip.fan_blocked ? 0 : motor_v() * 500.0;
  chip.rpm += (target - chip.rpm) * 0.003;
  if (chip.fan_blocked)
    chip.rpm = 0;
  /* PMP-7: the armed watchdog cuts a stroke whose current is above its threshold: PUMP_CMD low
   * and the heartbeat stopped, by itself, then the report. */
  uint16_t i_raw = counts(pump_powered() && chip.pump_duty && !chip.pump_forced && !chip.pump_open ? 1.2 * 0.45 : 0);
  if (chip.oc_armed && !chip.pump_forced && i_raw > chip.oc_threshold) {
    chip.pump_forced = 1;
    chip.hb_mode = HAL_HB_STOPPED;
    chip.oc_report = 1;
    chip.oc_cuts++;
  }
  /* The pump: strokes and their spacing; PUMP_CMD high only on an armed high side. */
  int on = chip.pump_duty > 0 && !chip.pump_forced;
  if (on) {
    chip.pump_duty_seen = chip.pump_duty;
    chip.last_pump_on_ms = chip.now;
    if (!pump_powered() || (uint32_t) (chip.now - chip.hb_start_ms) < 20u)
      chip.pump_unpowered_ms++;
  }
  if (on && !chip.pump_was_on) {
    if (chip.strokes && chip.now - chip.last_stroke_end < chip.stroke_gap_min)
      chip.stroke_gap_min = chip.now - chip.last_stroke_end;
    chip.strokes++;
    chip.stroke_start = chip.now;
  }
  if (!on && chip.pump_was_on) {
    uint32_t len = chip.now - chip.stroke_start;
    if (len < chip.stroke_len_min)
      chip.stroke_len_min = len;
    if (len > chip.stroke_len_max)
      chip.stroke_len_max = len;
    chip.last_stroke_end = chip.now;
  }
  chip.pump_was_on = on;
}

static void chip_reset(double vin) {
  memset(&chip, 0, sizeof(chip));
  chip.vin = vin;
  chip.ntc_raw = 4057;   /* 25 C */
  chip.board_raw = 2048; /* 25 C */
  chip.plug_ohm = 0.5;
  chip.stroke_len_min = chip.stroke_gap_min = 0xFFFFFFFFu;
  text_len = text_mark = 0;
  text[0] = 0;
}

static board b;
static console con;

static void boot(double vin, uint32_t optr, uint8_t option_faults) {
  chip_reset(vin);
  board_init(&b, chip.now, LINK_RESET_WATCHDOG, option_faults);
  console_boot cb = {0x20000000u, optr, 0};
  console_init(&con, &b, &cb, chip.now);
}

static void run(uint32_t ms) {
  for (uint32_t i = 0; i < ms; i++) {
    chip.now++;
    model_ms();
    board_ms(&b, chip.now);
    console_ms(&con, chip.now);
  }
}

static void type(const char *line) {
  chip.rx_len = chip.rx_pos = 0;
  size_t n = strlen(line);
  memcpy(chip.rx, line, n);
  chip.rx[n] = '\n';
  chip.rx_len = n + 1;
}

/* Type a command and give it time to answer. */
static void cmd(const char *line) {
  text_mark = text_len;
  type(line);
  run(30);
}
static int said(const char *needle) { return strstr(text + text_mark, needle) != NULL; }
static int count(const char *needle) {
  int n = 0;
  for (const char *p = text + text_mark; (p = strstr(p, needle)) != NULL; p++)
    n++;
  return n;
}

static const uint32_t OPTR_OK = 0xDEFEE1AAu, OPTR_FACTORY = 0xDFFFE1AAu;

/* ---------------------------------------------------------------- the parser */

static void test_parser(void) {
  boot(12.6, OPTR_OK, 0);
  run(1000);
  CHECK(strstr(text, "* boot flamekeeper-bringup") && strstr(text, "reset watchdog"));
  CHECK(strstr(text, "* powerup passed"));

  cmd("help");
  run(500);
  CHECK(said("> help") && said("fan duty <permille> <s>") && said("ok"));
  cmd("bogus");
  CHECK(said("err unknown command"));
  type("info\r\n"); /* CRLF: one command, not two */
  text_mark = text_len;
  run(200);
  CHECK(count("> info") == 1 && said("powerup passed") && said("opt 0xdefee1aa ok"));
  cmd("infx\bo"); /* a backspace from a person typing */
  run(100);
  CHECK(said("> info"));

  /* Numbers: decimal or 0x, in range, nothing trailing. */
  cmd("fan duty 1001 10");
  CHECK(said("err"));
  cmd("fan duty 300 0");
  CHECK(said("err"));
  cmd("fan duty 300 601");
  CHECK(said("err"));
  cmd("fan duty 30a 10");
  CHECK(said("err"));
  cmd("fan duty -3 10");
  CHECK(said("err"));
  cmd("flash 4294967296");
  CHECK(said("err"));
  cmd("watch 99");
  CHECK(said("err"));
  cmd("pump 21");
  CHECK(said("err"));
  cmd("pump 1 601");
  CHECK(said("err"));
  cmd("hb 61");
  CHECK(said("err"));
  cmd("a b c d e f g");
  CHECK(said("err too many words"));
  cmd("par 0x01 99999");
  CHECK(said("err out of range"));
  CHECK(b.s.fan.duty == 0 && b.s.pump.strokes == 0); /* nothing started */

  /* A binary link frame: the link answers, the console says nothing. */
  text_mark = text_len;
  const uint8_t hello[2] = {LINK_MAJOR, LINK_MINOR};
  chip.rx_len = link_build_request(9, LINK_OP_HELLO, hello, 2, chip.rx);
  chip.rx_pos = 0;
  chip.link_len = 0;
  run(20);
  CHECK(chip.link_len > 7 && link_frame_op(chip.link_tx) == LINK_OP_HELLO && link_frame_seq(chip.link_tx) == 9);
  link_info info;
  CHECK(link_decode_info(link_reply_payload(chip.link_tx), link_reply_payload_len(chip.link_tx), &info) &&
        strstr(info.text, "bring-up")); /* the ESP32 can tell this image from the production one */
  CHECK(text_len == text_mark);
  /* A line with a control byte in it, or too long: dropped without a word. */
  memcpy(chip.rx, "info\x01x\n", 7);
  chip.rx_len = 7, chip.rx_pos = 0;
  run(20);
  char longline[200];
  memset(longline, 'a', sizeof(longline) - 1);
  longline[sizeof(longline) - 1] = 0;
  type(longline);
  run(50);
  CHECK(text_len == text_mark);
  /* Typed text never reaches the link: no reply to it. */
  chip.link_len = 0;
  cmd("zzz z");
  CHECK(chip.link_len == 0);

  /* Output: never the link's reply header, never more than 8 bytes a millisecond. */
  CHECK(strchr(text, '{') == NULL);
  CHECK(chip.max_text_write <= CONSOLE_TX_PER_MS);
}

/* ---------------------------------------------------------------- readings */

static void test_readings(void) {
  boot(12.6, OPTR_OK, 0);
  run(1000);
  cmd("adc");
  run(300);
  CHECK(said("adc vin raw=") && said("12.5") && said("V supply"));
  CHECK(said("adc ntc raw=4057") && said("C heat exchanger"));
  CHECK(said("adc vref") && said("mV VDDA"));
  CHECK(said("hall rpm 0"));
  CHECK(count("adc ") == HAL_ADC_COUNT + 0);
  cmd("io");
  CHECK(said("latch_armed=1") && said("pump_hs_ok=1"));
  cmd("opt");
  run(100);
  CHECK(said("iwdg_sw=0") && said("nboot_sel=0") && said(": ok"));
  cmd("flash 30 0 20");
  run(100);
  CHECK(said("flash 0800f000 00 07 0e") && said("flash 0800f010 70"));
  cmd("flash 32");
  CHECK(said("err"));
  cmd("par");
  run(500);
  CHECK(said("par 0x01 Pump min = 1.60") && said("par 0x51 Overheat stop = 255.0"));
  cmd("tune");
  run(100);
  CHECK(said("tune reset_fan_s = 30") && said("tune fan_retry_wait_s = 5") && said("fan_retry_forget_s = 600"));
  cmd("tune reset_fan_s 45");
  CHECK(said("ok") && b.s.burn.p.reset_fan_s == 45 && b.s.params.live.reset_fan_s == 45);
  cmd("tune reset_fan_s 121");
  CHECK(said("err out of range") && b.s.burn.p.reset_fan_s == 45);
  cmd("par 0x05 30");
  cmd("par apply");
  CHECK(b.s.burn.p.prime_s == 30 && b.s.burn.p.reset_fan_s == 45); /* the tune survives an apply */
  cmd("watch 100");
  run(1000);
  CHECK(count("w t=") >= 8 && said("vin=12.") && said(" off "));
  cmd("watch 0");
  run(500);
  CHECK(count("w t=") == 0);

  /* Wrong option bytes: shown, and the interlocks they stand for refuse the pump and the glow. */
  boot(12.6, OPTR_FACTORY, LINK_CHECK_OPTION_BYTES);
  run(1000);
  CHECK(strstr(text, "opt 0xdfffe1aa WRONG"));
  cmd("opt");
  run(100);
  CHECK(said("WRONG"));
  cmd("pump 1");
  CHECK(said("err interlocks"));
  cmd("fan duty 200 1"); /* the fan needs no interlock */
  CHECK(said("ok"));
}

/* ---------------------------------------------------------------- the fan */

static void test_fan_duty(void) {
  boot(24.0, OPTR_OK, 0);
  run(1000);
  cmd("fan duty 1000 2");
  CHECK(said("ok"));
  run(200);
  uint16_t cap = (uint16_t) (1000u * 1000u / 2400u);
  CHECK(chip.fan_duty > 0 && chip.fan_duty <= cap / 4); /* the ramp: 0 to the cap in no less than 1 s */
  uint16_t top = 0;
  for (int i = 0; i < 1500; i++) {
    run(1);
    if (chip.fan_duty > top)
      top = chip.fan_duty;
  }
  CHECK(top <= cap + 1 && top >= cap - 5); /* FAN-1: 10 V at the motor whatever is asked */
  CHECK(chip.rpm > 3000);
  run(600);
  CHECK(said("* fan_duty done"));
  CHECK(chip.fan_duty == 0);
  /* The coasting fan is not a fault (it has its spin-down time). */
  run(5000);
  CHECK(b.s.fan.fault == FAN_FAULT_NONE && b.s.burn.fault == BURN_FAULT_NONE);

  /* A stall: off within about a second, the event, and the core's stall count. */
  chip.fan_blocked = 1;
  cmd("fan duty 300 10");
  run(1500);
  CHECK(said("* fan_duty fault fault stall"));
  CHECK(chip.fan_duty == 0 && !con.bench.fan);
  CHECK(b.s.burn.fault == BURN_FAULT_FAN && b.s.burn.fan_stalls == 1);
  /* The start limit held the motor current at about 6 A while it tried (FAN-9). */
  chip.fan_blocked = 0;
  run(100);
  /* The stage restarted with its own parameters (it once restarted with them zeroed: every retry
   * then ended in a latched "uncommanded" fault). */
  CHECK(b.s.fan.fault == FAN_FAULT_NONE && !b.s.burn.fault_latched && b.s.fan.p.v_max_cv == 1000);

  /* stop ends it at once. */
  cmd("fan duty 300 10");
  run(500);
  CHECK(chip.fan_duty > 0);
  cmd("stop");
  run(20);
  CHECK(said("* fan_duty stopped") && chip.fan_duty == 0);
  cmd("fan rpm 2000 3");
  CHECK(said("ok"));
  run(2800);
  CHECK(chip.rpm > 1800 && chip.rpm < 2200);
  cmd("fan off");
  run(20);
  CHECK(chip.fan_duty == 0 && !con.bench.fan);
}

/* fan_step_open on its own: the cap, the ramp, the start limit and the stall, and the spin-down
 * grace afterwards. */
static void test_fan_open_loop(void) {
  fan_params p;
  fan_params_default(&p);
  fan f;
  fan_init(&f, &p, 0);
  fan_meas m = {0, 2400, 0};
  uint16_t d = 0;
  for (uint32_t t = 10; t <= 100; t += 10)
    d = fan_step_open(&f, t, 1000, &m);
  CHECK(d > 0 && d <= 42 + 1); /* 100 ms of a 1 s ramp to 416 */
  for (uint32_t t = 110; t <= 2000; t += 10)
    d = fan_step_open(&f, t, 1000, &m);
  CHECK(d == 416);
  /* Motor current above 6 A while it does not turn: no rise, then backing off. */
  fan_init(&f, &p, 0);
  m.rpm = 0;
  for (uint32_t t = 10; t <= 800; t += 10) {
    m.isense_ma = (uint16_t) (8000u * f.duty / 1000u); /* an 8 A motor */
    d = fan_step_open(&f, t, 1000, &m);
  }
  CHECK(f.fault == FAN_FAULT_STALL || d * 8u <= 6000u * 1000u / 1000u + 200u);
  /* No Hall edges with current for 1 s: a stall. */
  fan_init(&f, &p, 0);
  for (uint32_t t = 10; t <= 3000 && f.fault == FAN_FAULT_NONE; t += 10) {
    m.isense_ma = 500;
    fan_step_open(&f, t, 300, &m);
  }
  CHECK(f.fault == FAN_FAULT_STALL);
  /* After an open run, the speed loop's off with the fan still turning: no fault for 30 s. */
  fan_init(&f, &p, 0);
  m.isense_ma = 0;
  m.rpm = 2000;
  for (uint32_t t = 10; t <= 1000; t += 10)
    fan_step_open(&f, t, 300, &m);
  for (uint32_t t = 1010; t <= 25000; t += 10)
    fan_step(&f, t, 0, &m);
  CHECK(f.fault == FAN_FAULT_NONE);
  CHECK(fan_step_open(&f, 25010, 0, &m) == 0);
}

/* A stage restarted with its own parameters keeps them (safety.c's stall retry and fault reset). */
static void test_restart_keeps_params(void) {
  fan_params fp;
  fan_params_default(&fp);
  fan f;
  fan_init(&f, &fp, 0);
  fan_init(&f, &f.p, 10);
  CHECK(memcmp(&f.p, &fp, sizeof(fp)) == 0);
  glow_params gp;
  glow_params_default(&gp);
  glow g;
  glow_init(&g, &gp, 0);
  glow_init(&g, &g.p, 10);
  CHECK(memcmp(&g.p, &gp, sizeof(gp)) == 0);
  pump_params pp;
  pump_params_default(&pp);
  pump pu;
  pump_init(&pu, &pp);
  pump_init(&pu, &pu.p);
  CHECK(memcmp(&pu.p, &pp, sizeof(pp)) == 0);
}

/* ---------------------------------------------------------------- heartbeat and pump */

static void test_hb(void) {
  boot(12.6, OPTR_OK, 0);
  run(1000);
  uint32_t bursts = b.bursts, loops = b.loops;
  cmd("hb 1");
  run(500);
  CHECK(hb_ok());
  run(600);
  CHECK(said("* hb done"));
  CHECK(!hb_ok() && chip.hb_mode == HAL_HB_STOPPED);
  /* Re-armed once per completed pass (PMP-1), never more. */
  CHECK(b.bursts - bursts <= b.loops - loops + 1 && b.bursts - bursts >= 95);
  CHECK(chip.pump_duty_seen == 0);
}

static void test_pump(void) {
  boot(12.6, OPTR_OK, 0);
  cmd("pump 1"); /* the power-up self-test still runs */
  CHECK(said("err busy"));
  run(1000);
  cmd("pump 3 200");
  CHECK(said("ok"));
  run(3000);
  CHECK(said("* pump done") && said("strokes=3") && said("peak_min_ma=1"));
  CHECK(chip.strokes == 3);
  CHECK(chip.stroke_len_min >= 37 && chip.stroke_len_max <= 39);   /* PMP-3: 38 ms */
  CHECK(chip.stroke_gap_min >= 500 - 40);                          /* 2 Hz, spaced from the stroke */
  CHECK(chip.pump_duty_seen >= 940 && chip.pump_duty_seen <= 960); /* 12 V / 12.6 V */
  CHECK(chip.pump_unpowered_ms == 0); /* PMP-2: only on a heartbeat that has run 20 ms */
  CHECK(!hb_ok() && chip.hb_mode == HAL_HB_STOPPED);
  CHECK(b.s.pump.fault == PUMP_FAULT_NONE);

  /* At 24 V the duty halves; the rate is the driver's to cap (6 Hz). */
  boot(24.0, OPTR_OK, 0);
  run(1000);
  cmd("pump 4 600");
  run(2500);
  CHECK(chip.strokes == 4 && chip.pump_duty_seen >= 495 && chip.pump_duty_seen <= 505);
  CHECK(chip.stroke_gap_min >= 166 - 40);
  CHECK(chip.pump_unpowered_ms == 0);

  /* An open coil: the driver's fault, the event, and no stroke until a fault reset. */
  chip.pump_open = 1;
  cmd("pump 2");
  run(2000);
  CHECK(said("* pump fault fault open_coil"));
  CHECK(b.s.pump.fault == PUMP_FAULT_OPEN_COIL && b.s.burn.fault_latched);
  CHECK(!hb_ok());
  cmd("pump 1");
  CHECK(said("err a fault needs clearing"));
  chip.pump_open = 0;
  cmd("clear");
  CHECK(said("ok") && b.s.pump.fault == PUMP_FAULT_NONE);
  uint32_t before = chip.strokes;
  cmd("pump 1");
  run(1500);
  CHECK(chip.strokes == before + 1 && said("* pump done"));

  /* stop in the middle: PUMP_CMD low at once, the heartbeat a decay wait later (PMP-10). */
  cmd("pump 20 100");
  run(400);
  for (int i = 0; i < 2000 && !chip.pump_duty; i++)
    run(1);
  CHECK(chip.pump_duty > 0);
  run(5);
  cmd("stop");
  CHECK(said("* pump stopped"));
  CHECK(chip.pump_duty == 0 && !hb_ok());
  CHECK(chip.burst_ms >= chip.last_pump_on_ms + 10u - 1u); /* the last burst came after the decay wait */

  /* The pre-start test runs first, and a failure refuses the strokes: a pump + side stuck on. */
  boot(12.6, OPTR_OK, 0);
  run(1000);
  uint32_t s0 = chip.strokes;
  b.s.chk.p.pump_off_percent = 0; /* stands in for a shorted high side: any reading fails */
  cmd("pump 1");
  run(1000);
  CHECK(said("* pump failed precheck") && said("pump_hs_short"));
  CHECK(chip.strokes == s0);
}

/* The overcurrent cut (PMP-7) on the bench: armed for every stroke, and acting on a lowered
 * threshold for one stroke, which then goes back. */
static void test_pump_oc(void) {
  boot(12.6, OPTR_OK, 0);
  run(1000);
  cmd("pump 2 200");
  run(2000);
  CHECK(said("* pump done") && chip.oc_cuts == 0 && con.oc_arms == 2);
  cmd("io");
  CHECK(said("pump_oc_armed=0") && said("pump_oc_ma=3300") && said("pump_oc_raw=184"));
  cmd("pump oc 3300");
  CHECK(said("err") && b.s.pump.p.oc_ma == 3300);
  cmd("pump oc 99");
  CHECK(said("err"));
  /* Below the coil's 1.2 A: the cut acts, the stroke ends, the pump faults until a fault reset. */
  uint32_t strokes = chip.strokes;
  cmd("pump oc 500");
  CHECK(said("ok") && b.s.pump.p.oc_ma == 500);
  run(2000);
  CHECK(said("* pump_oc fault fault overcurrent (the cut acted)"));
  CHECK(chip.oc_cuts == 1 && chip.strokes <= strokes + 1); /* cut in its first millisecond */
  CHECK(b.s.pump.p.oc_ma == 3300); /* back */
  CHECK(!hb_ok() && b.s.pump.fault == PUMP_FAULT_OVERCURRENT);
  cmd("pump 1");
  CHECK(said("err a fault needs clearing"));
  cmd("clear");
  CHECK(said("ok") && b.s.pump.p.oc_ma == 3300);
  /* Above the coil's current: no cut, said so. */
  cmd("pump oc 2000");
  run(2000);
  CHECK(said("* pump_oc done (NO CUT)") && chip.oc_cuts == 1 && b.s.pump.p.oc_ma == 3300);
  /* stop in the middle puts the threshold back too. */
  cmd("pump oc 500");
  run(5);
  cmd("stop");
  run(50);
  CHECK(b.s.pump.p.oc_ma == 3300 && !con.bench.out);
}

/* ---------------------------------------------------------------- the CO sensor */

static void test_co(void) {
  boot(12.6, OPTR_OK, 0);
  run(1000);
  cmd("co");
  CHECK(said("co warming_up") && said("fitted=1") && said("param_0x52=1") && said("raw=1241"));
  CHECK(said("heat_wanted=1") && said("heat_en=1") && chip.co_heater == 1);
  cmd("co heater off 2");
  CHECK(said("ok"));
  run(100);
  CHECK(chip.co_heater == 0 && con.co_wanted == 1);
  cmd("co");
  CHECK(said("heat_en=0") && said("held_off_s="));
  run(2000);
  CHECK(said("* co heater back") && chip.co_heater == 1);
  cmd("co heater off 61");
  CHECK(said("err"));
  cmd("co heater on 5");
  CHECK(said("err"));
}

/* ---------------------------------------------------------------- the glow probe */

static uint16_t board_raw_for(int16_t c10) {
  for (uint16_t r = 4095; r > 0; r--)
    if (analog_board_c10(r) != ANALOG_UNKNOWN_TEMP && analog_board_c10(r) >= c10)
      return r;
  return 0;
}

static void test_glow(void) {
  boot(12.6, OPTR_OK, 0);
  run(1000);
  cmd("glow");
  CHECK(said("err no airflow"));
  cmd("fan rpm 600 20"); /* turning, but below the glow's minimum (1000 rpm) */
  run(3000);
  cmd("glow");
  CHECK(said("err no airflow"));
  cmd("stop");
  cmd("fan rpm 1500 20");
  run(3000);
  cmd("glow");
  CHECK(said("ok"));
  run(600);
  CHECK(said("* glow done") && said("i=25.") && said("r_mohm=50") && said("cold_type=12"));
  CHECK(chip.glow_pulses == 1 && chip.glow_last_us == 150); /* one 150 us probe */
  cmd("glow read");
  CHECK(said("glow last_with_pulse pulse_raw=") && said("pulse_A=25.") && said("gap_A=0.00") && said("gap_vin=12."));
  CHECK(con.glow_periods > 100);
  CHECK(chip.glow_bad == 0);   /* with GLOW_EN, on a heartbeat that had run 20 ms (GLW-7) */
  CHECK(!chip.glow_en && !hb_ok());
  run(1000);
  CHECK(chip.glow_pulses == 1);
  /* A 24 V plug reads as one. */
  chip.plug_ohm = 1.6;
  cmd("glow");
  run(600);
  CHECK(said("cold_type=24") && chip.glow_pulses == 2);

  /* A hot board: the planner gives no pulse (GLW-15). */
  chip.board_raw = board_raw_for(900);
  run(20);
  cmd("glow");
  run(600);
  CHECK(said("* glow no_probe"));
  CHECK(chip.glow_pulses == 2);
  chip.board_raw = 2048;
  cmd("stop");

  /* The core's own rules, whatever the bench asks: a probe request without a heartbeat, or with
   * the burn running, gives no pulse. */
  run(100);
  for (int i = 0; i < 200; i++) { /* the bench controller clears it every millisecond: insist */
    b.s.bench.glow_probe = 1;
    run(1);
  }
  CHECK(chip.glow_pulses == 2);
}

/* ---------------------------------------------------------------- watchdog, reset */

static void test_watchdog(void) {
  boot(12.6, OPTR_OK, 0);
  run(500);
  uint32_t r = chip.iwdg_refreshes;
  CHECK(r >= 45);
  cmd("wdg stop");
  CHECK(said("expect a reset"));
  uint32_t r2 = chip.iwdg_refreshes;
  run(500);
  CHECK(chip.iwdg_refreshes == r2);
  CHECK(b.loops >= 95); /* the loop itself still runs */

  boot(12.6, OPTR_OK, 0);
  run(1000);
  cmd("fan duty 200 5");
  cmd("wdg hang");
  CHECK(con.hang == 1 && said("hanging"));
  CHECK(chip.fan_duty > 0); /* the timers are left as they were */

  boot(12.6, OPTR_OK, 0);
  run(100);
  cmd("reset");
  CHECK(chip.resets >= 1);
}

/* ---------------------------------------------------------------- runs */

static void test_runs(void) {
  boot(12.6, OPTR_OK, 0);
  run(1000);
  cmd("run fan 50 5");
  CHECK(said("ok"));
  run(1000);
  CHECK(b.s.burn.step == BURN_STEP_FAN_ONLY || b.s.burn.step == BURN_STEP_SELF_TEST);
  CHECK(said("* step"));
  /* Bench actions and a second run are refused while it runs. */
  cmd("fan duty 300 5");
  CHECK(said("err busy"));
  cmd("run heat 50 5");
  CHECK(said("err busy"));
  /* The dead-man is fed: still running after 3 s with no link at all. */
  run(3000);
  CHECK(b.s.burn.step != BURN_STEP_OFF && b.s.burn.why != BURN_WHY_DEADMAN);
  text_mark = text_len;
  run(2000);
  CHECK(said("* run time up") && said("* run ended"));
  CHECK(b.s.burn.step == BURN_STEP_OFF && b.s.burn.why != BURN_WHY_DEADMAN && b.s.burn.fault == BURN_FAULT_NONE);
  CHECK(!con.run);

  /* A heat run through the pre-check to the fan check, then a normal stop. */
  cmd("run heat 40 600");
  CHECK(said("ok"));
  run(3000);
  CHECK(said("* step pre_check") && said("* step fan_check"));
  cmd("stop");
  run(200);
  CHECK(b.s.burn.step == BURN_STEP_OFF && !con.run && said("* run ended"));
  /* The bench is refused while the burn is not off; the core ignores its requests then too. */
  cmd("run fan 30 60");
  run(500);
  uint32_t strokes = chip.strokes, glows = chip.glow_pulses;
  for (int i = 0; i < 1000; i++) {
    b.s.bench.pump_cHz = 500, b.s.bench.heartbeat = 1, b.s.bench.glow_probe = 1;
    run(1);
  }
  CHECK(chip.strokes == strokes && chip.glow_pulses == glows);
  cmd("stop");
  run(500);
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  test_parser();
  test_readings();
  test_fan_duty();
  test_fan_open_loop();
  test_restart_keeps_params();
  test_hb();
  test_pump();
  test_glow();
  test_pump_oc();
  test_co();
  test_watchdog();
  test_runs();
  printf("bringup: %d checks, %d failures\n", n_checks, failures);
  return failures ? 1 : 0;
}
