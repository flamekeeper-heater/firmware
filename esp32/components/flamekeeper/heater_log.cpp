#include "heater_log.h"

#include <cstdarg>
#include <cstdio>

namespace esphome {
namespace flamekeeper {

namespace {

// The safety MCU's codes (stm32/core/burn.h, common/link/link.h).
const char *const WHY[] = {nullptr,
                           "asked to stop",
                           "the ESP32 went silent (dead-man)",
                           "no flame after every ignition attempt",
                           "the flame went out",
                           "the heat exchanger over its limit",
                           "the hardware overheat cut-out tripped",
                           "the fan stalled",
                           "the fan did not spin up in time",
                           "the heat exchanger sensor is open",
                           "no temperature rise with fuel and glow",
                           "the supply out of range",
                           "the heat exchanger did not cool down in time",
                           "carbon monoxide alarm",
                           "the glow plug's switch failed",
                           "the fan's switch failed, or the fan turned by itself",
                           "the pre-start self-test failed",
                           "the fuel pump's drive failed",
                           "the board too hot even at the lowest power",
                           "the glow plug is not the type set",
                           "the CO sensor failed"};
const char *const STEP[] = {"off",          "checking the fan", "preheating",           "igniting",    "purging",
                            "warming up",   "running",          "burning off",          "cooling down",
                            "running the fan only", "priming",  "self-testing",         "waiting to retry the fan"};
const char *const RESET[] = {"unknown", "power-up", "watchdog", "reset pin", "brown-out", "bootloader", "software"};

template <size_t N> const char *name(const char *const (&table)[N], unsigned i) { return i < N ? table[i] : nullptr; }

struct Out {
  char *p;
  size_t cap, n = 0;
  void add(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
    if (this->n + 1 >= this->cap)
      return;
    va_list a;
    va_start(a, fmt);
    int k = vsnprintf(this->p + this->n, this->cap - this->n, fmt, a);
    va_end(a);
    if (k > 0)
      this->n = this->n + size_t(k) < this->cap ? this->n + size_t(k) : this->cap - 1;
  }
};

}  // namespace

void heater_log_text(const HeaterLogEntry *e, char *out, size_t cap) {
  if (cap == 0)
    return;
  out[0] = 0;
  Out o{out, cap};
  if (e == nullptr) {
    o.add("None");
    return;
  }
  o.add("#%u ", unsigned(e->seq));
  if (e->kind == HEATER_LOG_RESET) {
    const char *r = name(RESET, e->code);
    if (r != nullptr)
      o.add("Safety MCU reset: %s", r);
    else
      o.add("Safety MCU reset: code %u", unsigned(e->code));
    const char *st = name(STEP, e->step);
    if (e->step != 0)
      st != nullptr ? o.add(", %s after it", st) : o.add(", step %u after it", unsigned(e->step));
  } else {
    o.add("%s: ", e->code == 0 ? "Stopped" : fault_name(Fault(e->code)));
    const char *w = name(WHY, e->why);
    w != nullptr ? o.add("%s", w) : o.add("code %u", unsigned(e->why));
    const char *st = name(STEP, e->step);
    st != nullptr ? o.add(", while %s", st) : o.add(", in step %u", unsigned(e->step));
    uint32_t t = e->uptime_s;
    if (t < 60)
      o.add(", %u s", unsigned(t));
    else if (t < 3600)
      o.add(", %u min", unsigned(t / 60));
    else if (t < 86400)
      o.add(", %u h %u min", unsigned(t / 3600), unsigned(t % 3600 / 60));
    else
      o.add(", %u d %u h", unsigned(t / 86400), unsigned(t % 86400 / 3600));
    o.add(" after a reset");
  }
  const char *sep = " (";
  if (e->hx_c10 != INT16_MIN) {
    int v = e->hx_c10 < 0 ? -e->hx_c10 : e->hx_c10;
    o.add("%sheat exchanger %s%d.%d C", sep, e->hx_c10 < 0 ? "-" : "", v / 10, v % 10);
    sep = ", ";
  }
  if (e->supply_cv != 0xFFFF) {
    o.add("%ssupply %u.%02u V", sep, unsigned(e->supply_cv / 100), unsigned(e->supply_cv % 100));
    sep = ", ";
  }
  if (e->board_c != -128) {
    o.add("%sboard %d C", sep, int(e->board_c));
    sep = ", ";
  }
  if (sep[0] == ',')
    o.add(")");
}

}  // namespace flamekeeper
}  // namespace esphome
