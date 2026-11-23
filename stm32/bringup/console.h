/* The bring-up build's text console, on the link UART (USART2, 115200 8N1; J_DEBUG pins 6 / 7).
 *
 * Why text on the link UART, beside the link: a person at the bench with a meter and a scope types
 * a command and reads plain numbers, from any serial terminal, with no tool and no ESP32; the same
 * image still answers the link, so the ESP32's link client works with it too. The two share the
 * byte stream safely:
 *   - the console takes a line only if every byte is printable ASCII (0x20-0x7E), ends in CR or LF
 *     and fits 63 bytes; anything else (a binary link frame) is dropped without a word;
 *   - the link takes a frame only with a valid CRC, so typed text never reaches it;
 *   - the console never sends '{' (0x7B, the link's reply header) and speaks only when spoken to,
 *     apart from its own events, so an ESP32 on the link only resynchronises.
 * Commands that drive a stage are the bench actions of bench.h (single, time-limited, through the
 * core's own stage drivers) and runs of the real burn sequencer through the link's POLL handler.
 * The console also watches, without changing them, the glow period's timed readings, the pump
 * overcurrent watchdog's arming and the CO heater pin; it may only hold that pin off, for a while.
 *
 * Every command line is echoed as "> line" and answered with lines ending in "ok" or "err ...";
 * unasked lines start with "* ". Output leaves at most CONSOLE_TX_PER_MS bytes a millisecond (the
 * link's replies share the UART's buffer).
 *
 *   help                         this list
 *   info                         firmware, reset reason, option bytes, self-tests, state, faults
 *   adc                          every ADC channel: raw count, mV, value; the Hall speed
 *   io                           the interlock lines, the heartbeat and the stage outputs
 *   watch <ms>                   a status line every <ms> (100-60000); watch 0 stops
 *   opt                          FLASH_OPTR decoded, against WDG-1
 *   flash <page> [<off> <len>]   hex dump of a flash page (0-31), read only
 *   par                          the parameter table
 *   par <id> <value> | par apply | par save | par discard
 *   tune [<name> <value>]        provisional figures outside the table, in RAM until a reset
 *   fan duty <permille> <s>      fixed fan duty (capped at 10 V on the motor), 1-600 s
 *   fan rpm <rpm> <s>            the fan's speed loop at <rpm>, 1-600 s
 *   hb <s>                       the heartbeat alone, 1-60 s
 *   pump <n> [<cHz>]             n pump strokes (1-20) at cHz (10-600, default 100)
 *   glow                         one glow probe pulse (a fan action must be turning)
 *   glow read                    the glow period's timed readings: in the pulse, in the gap
 *   pump oc <mA>                 one stroke with the overcurrent cut lowered to <mA> (100 up to
 *                                its setting): with a dummy load, the cut must stop the stroke
 *   co                           the CO alarm: state, reading, Rs, R0 (live and saved), ratio
 *   co heater off <s>            CO_HEAT_EN held low, 1-60 s (the safe side; the alarm may then
 *                                see a sensor fault: clear)
 *   precheck                     the pre-start self-test alone
 *   run fan|heat <power> <s>     the burn sequencer, as the ESP32 asks it, 1-3600 s
 *   run prime <s>                priming
 *   stop                         ends every action now; a run stops normally (burn-off, cooldown)
 *   clear                        fault reset
 *   wdg stop | wdg hang          stop refreshing the watchdog / hang the main loop: a reset follows
 *   reset                        software reset
 *
 * Plain C99 over board.h and bench.h. Host-tested in ../tests/test_bringup.c. */
#ifndef FLAMEKEEPER_CONSOLE_H
#define FLAMEKEEPER_CONSOLE_H

#include <stddef.h>
#include <stdint.h>

#include "../board/board.h"
#include "bench.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CONSOLE_VERSION "0.1.0"
#define CONSOLE_LINE_MAX 64
#define CONSOLE_OUT_SIZE 768
#define CONSOLE_TX_PER_MS 8
#define CONSOLE_RUN_MAX_S 3600u
#define CONSOLE_POLL_MS 100u
#define CONSOLE_CO_OFF_MAX_S 60u

/* What the start-up code read before anything changed it. */
typedef struct {
  uint32_t csr;      /* RCC_CSR, before the reset flags were cleared */
  uint32_t optr;     /* FLASH_OPTR */
  uint32_t flash_sr; /* FLASH_SR (OPTVERR) */
} console_boot;

enum { CONSOLE_RUN_NONE = 0, CONSOLE_RUN_ON = 1, CONSOLE_RUN_STOPPING = 2 };

typedef struct {
  board *b;
  bench bench;
  console_boot boot;
  uint32_t now;
  char line[CONSOLE_LINE_MAX];
  uint8_t len, bad;
  char cmd[CONSOLE_LINE_MAX];
  uint8_t has_cmd;
  char out[CONSOLE_OUT_SIZE];
  uint16_t head, tail;
  uint32_t dropped;     /* output bytes lost to a full buffer (never expected) */
  uint8_t job;          /* a multi-line answer under way */
  uint16_t job_i, job_n;
  uint32_t job_addr;
  uint16_t watch_ms;
  uint32_t watch_last;
  uint8_t run, run_mode, run_power, run_start, run_step;
  uint32_t run_begin_ms, run_end_ms, run_poll_ms;
  uint8_t powerup_told;
  uint8_t wdg_stopped;  /* hal_iwdg_refresh does nothing: the watchdog resets the chip */
  uint8_t hang_asked, reset_asked;
  uint8_t hang;         /* the main loop must stop here (with every timer still running) */
  /* What the taps saw. */
  hal_glow_sample glow_last;   /* the last glow period's readings (raw) */
  hal_glow_sample glow_pulse;  /* the last period that had an in-pulse reading */
  uint32_t glow_periods, glow_pulse_ms;
  uint8_t oc_on;               /* the pump's overcurrent watchdog is armed now */
  uint16_t oc_threshold_raw, oc_hold_us;
  uint32_t oc_arms;            /* strokes it guarded (arming edges) */
  uint8_t co_pin, co_wanted;   /* CO_HEAT_EN as driven, and as the board asked */
  uint32_t co_off_end_ms;
  uint8_t co_off;              /* "co heater off" holds the pin low until co_off_end_ms */
} console;

/* After board_init. Prints the boot line. */
void console_init(console *c, board *b, const console_boot *boot, uint32_t now);
/* Once a millisecond, after board_ms: the pending command, the bench, a run, the output. */
void console_ms(console *c, uint32_t now);
/* One received byte (the UART tap below calls it for every byte the board reads). */
void console_feed(console *c, uint8_t byte);

/* The console defines five of the HAL's functions (board/hal.h) over the chip's own, which
 * `build.sh bringup` renames to these when it compiles the driver (and run.sh when it compiles the
 * tests' fake chip):
 *   hal_uart_getc      every byte goes to console_feed, then to the link
 *   hal_iwdg_refresh   skipped after "wdg stop"
 *   hal_glow_period    its readings noted, passed on unchanged
 *   hal_pump_oc_arm    its arming noted, passed on unchanged
 *   hal_co_heater      passed on, or held low ("co heater off") */
int bringup_chip_uart_getc(void);
void bringup_chip_iwdg_refresh(void);
uint8_t bringup_chip_glow_period(hal_glow_sample *g);
void bringup_chip_pump_oc_arm(uint8_t on, uint16_t threshold_raw, uint16_t hold_us);
void bringup_chip_co_heater(uint8_t on);
void bringup_sys_reset(void);
void bringup_sys_read(uint32_t addr, uint8_t *out, size_t n); /* main flash, read only */

#ifdef __cplusplus
}
#endif

#endif /* FLAMEKEEPER_CONSOLE_H */
