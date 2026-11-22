#include "automation.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <utility>

namespace esphome {
namespace flamekeeper {

namespace {

// An alert's text goes out in one Bluetooth result (PROTOCOL.md): RESULT_MAX less the
// header, the alert bit and the log sequence number.
const size_t ALERT_TEXT_MAX = 124;
// A refused automatic start's reason, as the disarm alert quotes it.
const size_t WHY_MAX = 96;

// Days since 1970-01-01 of a civil date (Howard Hinnant's days_from_civil).
int32_t days_from_civil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = unsigned(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + int32_t(doe) - 719468;
}

// A POSIX TZ string the C library can read: printable, from the characters such strings use.
bool tz_ok(const char *tz) {
  size_t n = std::strlen(tz);
  if (n == 0 || n > Automation::TZ_MAX)
    return false;
  if (!((tz[0] >= 'A' && tz[0] <= 'Z') || (tz[0] >= 'a' && tz[0] <= 'z') || tz[0] == '<'))
    return false;
  for (size_t i = 0; i < n; i++) {
    char ch = tz[i];
    bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
              std::strchr("<>+-,./:", ch) != nullptr;
    if (!ok)
      return false;
  }
  return true;
}

bool burning(HeaterState s) { return s == HeaterState::STARTING || s == HeaterState::HEATING; }

}  // namespace

Automation::Automation(Controller &controller) : c_(controller) {
  this->c_.set_automation(this);
  // Low fuel refuses every start, from every source.
  this->c_.set_start_check([this](Source) { return this->start_check_(); });
  this->c_.on_stop_asked([this](Source source) {
    this->stop_asked_ = true;
    if (!is_automatic(source) && !this->own_stop_)
      this->person_off_ = true;
    this->log_(LogKind::STOP_ASKED, source, 0, 0);  // the board's own stops carry theirs (timer, frost, board)
  });
  this->c_.on_event([this](Event e, Result, uint16_t, const char *) {
    if (e == Event::FAILSAFE_STOP)
      this->stop_asked_ = true;
  });
  this->c_.on_refresh([this](Status &s) {
    s.alerts = this->alerts_;
    uint8_t a = 0;
    if (this->c_.tuning().extra(FR_ENABLED) != 0)
      a |= automatic::FROST_ON;
    if (this->disarmed_)
      a |= automatic::DISARMED;
    if (this->utc_base_ != 0)
      a |= automatic::CLOCK_KNOWN;
    for (const TimerSpec &t : this->timers_)
      if (t.enabled())
        a |= automatic::TIMER_ENABLED;
    s.automatic = a;
    s.fuel_left = this->fuel_left_();
    s.burnoff_left_min = this->burnoff_minutes_left(this->c_.now());
    const bool timer = this->auto_run_ == Source::TIMER && this->timer_slot_ >= 0;
    s.timer_slot = timer ? uint8_t(this->timer_slot_) : 0xFF;
    s.timer_end_utc = timer ? this->timer_end_utc_ : 0;
  });
}

Outcome Automation::start_check_() const {
  HeaterStatus s;  // the heater's own, now (the controller's copy is from the last tick)
  this->c_.heater()->read_status(s);
  const int stop = this->c_.tuning().extra(F_STOP_AT);
  const uint8_t left = this->fuel_left_();
  if (stop > 0 && left != 0xFF && left <= stop)
    return Outcome::refuse(Result::REFUSED, "fuel low: fill the tank, then Mark tank as filled");
  if (s.fault == Fault::CO || s.co_state == uint8_t(CoState::ALARM))
    return Outcome::refuse(Result::REFUSED, "CO alarm: air the space, then reset the heater fault");
  if (s.fault == Fault::CO_SENSOR || s.co_state == uint8_t(CoState::SENSOR_FAULT))
    return Outcome::refuse(Result::REFUSED, "CO sensor fault: the heater cannot run until it reads again");
  const float cutoff = this->c_.tuning().extra(B_CUTOFF) / 10.0f;
  if (cutoff > 0.0f && s.answering && std::isfinite(s.supply_v) && s.supply_v < cutoff + 0.2f) {
    static char why[64];
    std::snprintf(why, sizeof(why), "battery low (%.1f V): charge it first", s.supply_v);
    return Outcome::refuse(Result::REFUSED, why);
  }
  return Outcome::ok();
}

void Automation::log_(LogKind kind, Source source, uint16_t a, uint32_t b) {
  this->log_book_.add(kind, uint8_t(source), a, b, this->utc(this->c_.now()));
}

// ---------------------------------------------------------------- clock

Outcome Automation::set_clock(uint32_t utc, const char *tz, ClockSource source, uint32_t now) {
  if (utc < EARLIEST_UTC)
    return Outcome::refuse(Result::OUT_OF_RANGE, "the time is before 2025");
  bool tz_changed = false;
  if (tz != nullptr && tz[0] != '\0') {
    if (!tz_ok(tz))
      return Outcome::refuse(Result::OUT_OF_RANGE, "the time zone is not a POSIX TZ string");
    tz_changed = this->tz_ != tz;
    this->tz_ = tz;
  }
  // Apps set the clock on every connection and Home Assistant every 10 minutes: only a clock that
  // was unknown or wrong, or a new time zone, is an event worth the log and a save.
  const uint32_t was = this->utc(now);
  const uint32_t off = was > utc ? was - utc : utc - was;
  const bool source_changed = source != this->clock_source_;
  this->clock_source_ = source;  // CLOCK_GET names whoever set it last
  this->restore_clock(utc, now);
  if (this->clock_cb_)
    this->clock_cb_(utc);
  if (tz_changed || source_changed)
    this->save_();
  if (was == 0 || off > CLOCK_LOG_S || tz_changed)
    this->log_(LogKind::CLOCK_SET, Source::NONE, uint16_t(source), 0);
  return Outcome::ok();
}

void Automation::restore_clock(uint32_t utc, uint32_t now) {
  this->utc_base_ = utc;
  this->utc_base_ms_ = now;
  if (this->boot_utc_ == 0)
    this->boot_utc_ = utc - now / 1000;
  this->apply_tz_();
}

uint32_t Automation::utc(uint32_t now) const {
  if (this->utc_base_ == 0)
    return 0;
  return this->utc_base_ + uint32_t(now - this->utc_base_ms_) / 1000;
}

void Automation::apply_tz_() const {
  if (this->tz_.empty())
    return;  // the C library's own (Home Assistant's time sets one)
  const char *cur = std::getenv("TZ");
  if (cur != nullptr && this->tz_ == cur)
    return;
  setenv("TZ", this->tz_.c_str(), 1);
  tzset();
}

int32_t Automation::local_day_(uint32_t utc) const {
  if (utc == 0)
    return 0;
  this->apply_tz_();
  time_t t = time_t(utc);
  struct tm tm {};
  localtime_r(&t, &tm);
  return days_from_civil(tm.tm_year + 1900, unsigned(tm.tm_mon + 1), unsigned(tm.tm_mday));
}

uint32_t Automation::weekly_start_(const TimerSpec &t, uint32_t utc, int days_ago) const {
  time_t at = time_t(utc) - time_t(days_ago) * 86400;
  struct tm tm {};
  localtime_r(&at, &tm);
  const int bit = (tm.tm_wday + 6) % 7;  // Monday = bit 0
  if (!(t.days & (1u << bit)))
    return 0;
  tm.tm_hour = t.start_min / 60;
  tm.tm_min = t.start_min % 60;
  tm.tm_sec = 0;
  tm.tm_isdst = -1;
  time_t start = mktime(&tm);
  return start <= 0 ? 0 : uint32_t(start);
}

// ---------------------------------------------------------------- fuel

uint8_t Automation::fuel_left_() const {
  const float tank = this->c_.tuning().tank_ml();
  if (!(tank > 0.0f))
    return 0xFF;
  float pct = 100.0f * (1.0f - this->meter_.tank_used_ml() / tank);
  if (pct <= 0.0f)
    return 0;
  if (pct >= 100.0f)
    return 100;
  return uint8_t(std::floor(pct + 0.001f));  // 40 % set is 40 %, not 39.99
}

FuelMeter::Totals Automation::meters(uint32_t now) const {
  const uint32_t utc = this->utc(now);
  return this->meter_.totals(utc, this->local_day_(utc));
}

Outcome Automation::tank_set(uint8_t percent, Source source) {
  const float tank = this->c_.tuning().tank_ml();
  if (!(tank > 0.0f))
    return Outcome::refuse(Result::REFUSED, "set the fuel tank size first");
  if (percent > 100)
    return Outcome::refuse(Result::OUT_OF_RANGE, "0 to 100 %");
  this->meter_.set_tank(tank * float(100 - percent) / 100.0f);
  this->save_meter_();
  this->log_(LogKind::TANK_SET, source, percent, 0);
  if (this->started_)
    this->step_fuel_(this->last_ms_, this->c_.status(), 0);  // the alerts follow at once
  this->c_.refresh();
  return Outcome::ok();
}

void Automation::step_fuel_(uint32_t now, const Status &s, uint32_t utc) {
  const float dt = float(uint32_t(now - this->last_ms_)) / 1000.0f;
  const float pump = s.answering ? s.pump_hz : 0.0f;
  const bool burn = s.answering && burning(s.state);
  const bool glow = s.answering && std::isfinite(s.glow_w) && s.glow_w > 1.0f;
  this->meter_.add(dt, pump, this->c_.tuning().pump_ml_per_stroke(), burn, glow, utc, this->local_day_(utc));
  const bool moving = (std::isfinite(pump) && pump > 0.0f) || burn;
  if (moving && uint32_t(now - this->meter_saved_ms_) >= METER_SAVE_MS)
    this->save_meter_();

  const uint8_t left = this->fuel_left_();
  if (left == 0xFF) {
    this->set_alert_(alert::FUEL_LOW | alert::FUEL_STOP, false, "");
    return;
  }
  const int low = this->c_.tuning().extra(F_LOW_ALERT);
  const int stop = this->c_.tuning().extra(F_STOP_AT);
  char text[64];
  std::snprintf(text, sizeof(text), "fuel low: about %u %% left", unsigned(left));
  this->set_alert_(alert::FUEL_LOW, low > 0 && left <= low, text);
  const bool too_low = stop > 0 && left <= stop;
  this->set_alert_(alert::FUEL_STOP, too_low, "fuel too low to run: fill the tank, then Mark tank as filled");
  if (too_low && s.mode != Mode::OFF && !this->stop_asked_)
    this->stop_(Source::BOARD, "fuel too low to run");
}

// ---------------------------------------------------------------- alerts

void Automation::set_alert_(uint16_t bits, bool on, const char *text) {
  if (!on) {
    this->alerts_ &= uint16_t(~bits);
    return;
  }
  if ((this->alerts_ & bits) == bits)
    return;
  this->raise_(bits, text);
}

void Automation::raise_(uint16_t bit, const char *text) {
  this->alerts_ |= bit;
  uint16_t index = 0;
  while (index < 15 && !(bit & (1u << index)))
    index++;
  this->log_(LogKind::ALERT, Source::NONE, index, 0);
  this->c_.emit_event(Event::ALERT, Result::OK, index, text, this->log_book_.newest());
}

Outcome Automation::clear_alerts(uint16_t mask) {
  this->alerts_ &= uint16_t(~(mask & (alert::BURN | alert::GLOW_DRIFT)));  // the others follow their cause
  this->c_.refresh();
  return Outcome::ok();
}

Outcome Automation::rearm(Source source) {
  if (this->disarmed_)
    this->log_(LogKind::REARMED, source, 0, 0);
  this->disarmed_ = false;
  this->alerts_ &= uint16_t(~alert::AUTO_OFF);
  this->save_();
  this->c_.refresh();
  return Outcome::ok();
}

void Automation::disarm_(const char *why) {
  if (this->disarmed_)
    return;
  this->disarmed_ = true;
  this->save_();
  this->log_(LogKind::DISARMED, Source::NONE, 0, 0);
  // The whole reason fits, brackets closed: the app shows it only when they balance. The app and
  // Home Assistant offer the re-arm themselves.
  static const char TEMPLATE[] = "automatic starts are off (%s)";
  char text[sizeof(TEMPLATE) - 2 + WHY_MAX];
  static_assert(sizeof(text) <= ALERT_TEXT_MAX + 1, "an alert text goes over Bluetooth in one result");
  std::snprintf(text, sizeof(text), TEMPLATE, why);
  this->raise_(alert::AUTO_OFF, text);
}

// ---------------------------------------------------------------- runs

bool Automation::start_(Source source, Mode mode, HeatMode heat_mode, uint8_t power, float target_c, bool *wait) {
  *wait = false;
  const Status &s = this->c_.status();
  if (s.state != HeaterState::OFF && s.state != HeaterState::UNKNOWN) {
    *wait = true;  // cooling down from an earlier run
    return false;
  }
  this->saved_settings_ = Restore{true, s.heat_mode, s.target_c, s.power};
  Heater *h = this->c_.heater();
  float t = std::max(h->min_target(), std::min(h->max_target(), target_c));
  if (mode == Mode::HEAT) {
    this->c_.set_heat_mode(heat_mode, source);
    if (heat_mode == HeatMode::TEMPERATURE)
      this->c_.set_target(t, source);
  }
  if (mode == Mode::FAN || heat_mode == HeatMode::POWER)
    this->c_.set_power(power, source);
  this->stop_asked_ = false;
  this->person_off_ = false;
  this->auto_run_ = source;
  Outcome o = this->c_.set_mode(mode, source);
  if (o.accepted())
    return true;
  this->auto_run_ = Source::NONE;
  this->restore_();
  if (o.result == Result::NOT_OFF || o.result == Result::BUSY) {  // cooling down, or an update runs
    *wait = true;
    return false;
  }
  char why[WHY_MAX];
  std::snprintf(why, sizeof(why), "%s could not start the heater: %s", source_name(source), o.reason);
  this->disarm_(why);
  return false;
}

void Automation::stop_(Source source, const char *why) {
  (void) why;
  this->own_stop_ = true;
  this->c_.set_mode(Mode::OFF, source);
  this->own_stop_ = false;
}

void Automation::restore_() {
  if (!this->saved_settings_.set)
    return;
  Restore r = this->saved_settings_;
  this->saved_settings_.set = false;
  this->c_.set_heat_mode(r.heat_mode, Source::BOARD);
  this->c_.set_target(r.target_c, Source::BOARD);
  this->c_.set_power(r.power, Source::BOARD);
}

void Automation::step_runs_(uint32_t now, const Status &s) {
  const Source run = s.run_source;
  if (this->prev_run_ == Source::NONE && run != Source::NONE) {
    this->meter_.new_run();
    this->log_(LogKind::RUN_STARTED, run, uint16_t(s.mode), 0);
    this->glow_read_this_run_ = false;
    this->glow_on_since_ms_ = 0;
    this->glow_window_n_ = 0;
    if (!is_automatic(run)) {
      this->auto_run_ = Source::NONE;
      this->stop_asked_ = false;
      this->person_off_ = false;
    }
    this->burn_warned_ = false;
    this->heating_since_ms_ = 0;
    this->hx_count_ = 0;
    this->priming_run_ = s.mode == Mode::PRIME;
  } else if (this->prev_run_ != Source::NONE && run == Source::NONE) {
    this->save_meter_();
    // Priming ends on its own when its time is up: only a fault makes that a problem.
    if (!this->stop_asked_ && !(this->priming_run_ && s.fault == Fault::NONE)) {
      char text[96];
      if (s.fault != Fault::NONE)
        std::snprintf(text, sizeof(text), "the heater stopped by itself: %s", fault_name(s.fault));
      else
        std::snprintf(text, sizeof(text), "the heater stopped by itself");
      this->log_(LogKind::STOPPED_BY_ITSELF, Source::NONE, uint16_t(s.fault), 0);
      this->raise_(alert::BURN, text);
      if (this->auto_run_ != Source::NONE)
        this->disarm_("the heater stopped by itself during an automatic run");
    } else if (this->auto_run_ == Source::FROST && this->person_off_ &&
               this->c_.tuning().extra(FR_ENABLED) != 0) {
      // Someone stopped a frost run in the cold: it must not start again at once.
      this->c_.param_set(FR_ENABLED, 0, Source::BOARD);
    }
    if (this->auto_run_ != Source::NONE || this->burnoff_active_)
      this->restore_();
    if (this->burnoff_active_) {
      if (this->burnoff_done_) {
        this->maint_.low_power_s = 0;
        this->set_alert_(alert::BURNOFF_DUE, false, "");
        this->log_(LogKind::BURNOFF, Source::NONE, 1, this->burnoff_minutes_);
        this->save_maintenance_now_();
      }
      this->burnoff_active_ = false;
      this->burnoff_done_ = false;
    }
    if (this->log_cb_) {
      this->log_cb_(this->log_book_.saved());
      this->log_book_.saved_now();
      this->log_saved_ms_ = now;
    }
    this->auto_run_ = Source::NONE;
    this->timer_slot_ = -1;
    this->timer_end_utc_ = 0;
  }
  this->prev_run_ = run;
}

void Automation::step_burn_(uint32_t now, const Status &s) {
  if (s.fault != Fault::NONE && this->prev_fault_ == Fault::NONE) {
    char text[64];
    std::snprintf(text, sizeof(text), "heater fault: %s", fault_name(s.fault));
    this->log_(LogKind::FAULT, Source::NONE, uint16_t(s.fault), s.panel_fault);
    this->raise_(alert::BURN, text);
  }
  this->prev_fault_ = s.fault;

  if (s.state != HeaterState::HEATING || !std::isfinite(s.heat_exchanger_c)) {
    this->heating_since_ms_ = 0;
    this->hx_count_ = 0;
    return;
  }
  if (this->heating_since_ms_ == 0)
    this->heating_since_ms_ = now == 0 ? 1 : now;
  if (this->hx_count_ != 0 && uint32_t(now - this->hx_last_ms_) < 10000)
    return;
  this->hx_last_ms_ = now;
  const size_t N = sizeof(this->hx_window_) / sizeof(this->hx_window_[0]);
  if (this->hx_count_ == N) {
    for (size_t i = 1; i < N; i++)
      this->hx_window_[i - 1] = this->hx_window_[i];
    this->hx_count_--;
  }
  this->hx_window_[this->hx_count_++] = s.heat_exchanger_c;
  float top = s.heat_exchanger_c;
  for (size_t i = 0; i < this->hx_count_; i++)
    top = std::max(top, this->hx_window_[i]);
  if (!this->burn_warned_ && this->hx_count_ == N && uint32_t(now - this->heating_since_ms_) >= 5 * 60 * 1000 &&
      top - s.heat_exchanger_c >= 30.0f) {
    this->burn_warned_ = true;
    this->raise_(alert::BURN, "the heat exchanger is cooling while the heater burns: the flame may be failing");
  }
}

void Automation::step_frost_(uint32_t now, const Status &s) {
  (void) now;
  const Tuning &t = this->c_.tuning();
  if (t.extra(FR_ENABLED) == 0) {
    this->set_alert_(alert::FROST_NO_TEMP, false, "");
    if (this->auto_run_ == Source::FROST && s.mode != Mode::OFF && !this->stop_asked_)
      this->stop_(Source::FROST, "frost protection turned off");
    return;
  }
  const float room = s.room_c;
  this->set_alert_(alert::FROST_NO_TEMP, std::isnan(room), "frost protection is on, but there is no room temperature");
  const float frost = t.extra(FR_TEMP) / 10.0f;
  const float stop_at = frost + t.extra(FR_STOP_ABOVE) / 10.0f;
  if (this->auto_run_ == Source::FROST) {
    if (!std::isnan(room) && room >= stop_at && s.mode != Mode::OFF && !this->stop_asked_)
      this->stop_(Source::FROST, "frost protection: the room is warm enough");
    return;
  }
  if (this->disarmed_ || s.run_source != Source::NONE || std::isnan(room) || room > frost)
    return;
  bool wait = false;
  this->start_(Source::FROST, Mode::HEAT, HeatMode::TEMPERATURE, s.power, stop_at, &wait);
}

void Automation::step_timers_(uint32_t now, const Status &s, uint32_t utc) {
  (void) now;
  bool any = false;
  for (const TimerSpec &t : this->timers_)
    any = any || t.enabled();
  this->set_alert_(alert::NO_CLOCK, any && utc == 0, "timers are set, but the board does not know the time: open the app");
  if (utc == 0)
    return;
  if (this->auto_run_ == Source::TIMER && this->timer_end_utc_ != 0 && utc >= this->timer_end_utc_ &&
      s.mode != Mode::OFF && !this->stop_asked_)
    this->stop_(Source::TIMER, "timer finished");

  for (size_t i = 0; i < TIMERS; i++) {
    TimerSpec &t = this->timers_[i];
    if (!t.enabled())
      continue;
    const uint32_t length = uint32_t(t.duration_min) * 60;
    uint32_t occ = 0, end = 0;
    if (t.one_off()) {
      if (utc >= t.once_utc + length) {
        t.flags &= uint8_t(~TimerSpec::ENABLED);  // its time has passed
        this->save_();
        continue;
      }
      if (utc >= t.once_utc) {
        occ = t.once_utc;
        end = occ + length;
      }
    } else {
      for (int days_ago = 0; days_ago <= 1 && occ == 0; days_ago++) {
        uint32_t st = this->weekly_start_(t, utc, days_ago);
        if (st != 0 && utc >= st && utc < st + length) {
          occ = st;
          end = st + length;
        }
      }
    }
    if (occ == 0 || this->fired_[i] == occ)
      continue;
    if (occ < this->boot_utc_ || s.run_source != Source::NONE || this->disarmed_) {
      // Under way before a restart (not resumed), someone else's run, or disarmed: this occurrence
      // does nothing.
      this->fired_[i] = occ;
      continue;
    }
    bool wait = false;
    const Mode mode = t.mode == 2 ? Mode::FAN : Mode::HEAT;
    if (this->start_(Source::TIMER, mode, HeatMode(t.heat_mode), t.power, t.target_x10 / 10.0f, &wait)) {
      this->fired_[i] = occ;
      this->timer_slot_ = int(i);
      this->timer_end_utc_ = end;
      return;  // one run at a time
    }
    if (!wait)
      this->fired_[i] = occ;
  }
}

// ---------------------------------------------------------------- timers

bool Automation::timer_get(size_t slot, TimerSpec &out) const {
  if (slot >= TIMERS)
    return false;
  out = this->timers_[slot];
  return true;
}

Outcome Automation::timer_set(size_t slot, const TimerSpec &t) {
  if (slot >= TIMERS)
    return Outcome::refuse(Result::OUT_OF_RANGE, "no such timer slot");
  if (!t.empty()) {
    Heater *h = this->c_.heater();
    if ((t.flags & ~(TimerSpec::ENABLED | TimerSpec::ONE_OFF)) != 0 || t.duration_min < 1 || t.duration_min > 1440 ||
        t.mode < 1 || t.mode > 2 || t.heat_mode > 1 || t.power > 100)
      return Outcome::refuse(Result::OUT_OF_RANGE, "a timer field is out of range");
    if (t.mode == 1 && t.heat_mode == 1 &&
        (t.target_x10 < int(std::lround(h->min_target() * 10)) || t.target_x10 > int(std::lround(h->max_target() * 10))))
      return Outcome::refuse(Result::OUT_OF_RANGE, "target temperature out of range");
    if (t.one_off() ? t.once_utc < EARLIEST_UTC : (t.days == 0 || t.days > 0x7F || t.start_min > 1439))
      return Outcome::refuse(Result::OUT_OF_RANGE, t.one_off() ? "the start is before 2025" : "days or start time out of range");
  }
  this->timers_[slot] = t;
  this->fired_[slot] = 0;
  this->save_();
  return Outcome::ok();
}

Outcome Automation::timer_delete(size_t slot) { return this->timer_set(slot, TimerSpec{}); }

// ---------------------------------------------------------------- sensors

void Automation::sensor_use(const uint8_t address[6], ThermoKind kind) {
  this->sensors_.use(address, kind);
  this->sensor_chosen_ms_ = this->last_ms_ == 0 ? 1 : this->last_ms_;
  this->save_();
}

// ---------------------------------------------------------------- persistence

void Automation::load(const Saved &in) {
  if (in.check != SAVED_CHECK)
    return;
  for (size_t i = 0; i < TIMERS; i++)
    this->timers_[i] = in.timers[i];
  this->disarmed_ = in.disarmed != 0;
  if (this->disarmed_)
    this->alerts_ |= alert::AUTO_OFF;  // still disarmed after the restart (no event: nothing new)
  this->sensors_.use(in.sensor_address, ThermoKind(in.sensor_kind));
  this->clock_source_ = ClockSource(in.clock_source <= 2 ? in.clock_source : 0);
  char tz[TZ_MAX + 1];
  std::memcpy(tz, in.tz, sizeof(tz));
  tz[TZ_MAX] = '\0';
  if (tz_ok(tz)) {
    this->tz_ = tz;
    this->apply_tz_();
  }
}

void Automation::save_() {
  if (!this->save_cb_)
    return;
  Saved out{};
  out.check = SAVED_CHECK;
  for (size_t i = 0; i < TIMERS; i++)
    out.timers[i] = this->timers_[i];
  out.disarmed = this->disarmed_ ? 1 : 0;
  std::memcpy(out.sensor_address, this->sensors_.chosen().address, 6);
  out.sensor_kind = uint8_t(this->sensors_.chosen().kind);
  out.clock_source = uint8_t(this->clock_source_);
  std::strncpy(out.tz, this->tz_.c_str(), TZ_MAX);
  this->save_cb_(out);
}

void Automation::save_meter_() {
  this->meter_saved_ms_ = this->last_ms_;
  if (this->meter_cb_)
    this->meter_cb_(this->meter_.saved());
  this->save_maintenance_now_();
}

void Automation::save_maintenance_now_() {
  if (this->maint_cb_)
    this->maint_cb_(this->maint_);
}

// ---------------------------------------------------------------- tick

void Automation::tick(uint32_t now) {
  if (!this->started_) {
    this->started_ = true;
    this->last_ms_ = now;
    this->meter_saved_ms_ = now;
    if (this->sensor_chosen_ms_ == 0)
      this->sensor_chosen_ms_ = now == 0 ? 1 : now;
    return;
  }
  if (uint32_t(now - this->last_ms_) < 1000)
    return;
  this->step_(now);
}

void Automation::step_(uint32_t now) {
  const uint32_t utc = this->utc(now);
  this->apply_tz_();

  // The chosen Bluetooth thermometer into the room temperature (NAN once it is stale).
  const float bt = this->sensors_.temperature(now);
  this->c_.room().bluetooth(bt, std::isnan(bt) ? now : this->sensors_.chosen().heard_ms);
  bool lost = false;
  if (this->sensors_.has_chosen()) {
    const uint16_t age = this->sensors_.age_s(now);
    lost = age == 0xFFFF ? uint32_t(now - this->sensor_chosen_ms_) >= RoomSensors::STALE_MS
                         : uint32_t(age) * 1000 >= RoomSensors::STALE_MS;
  }
  this->set_alert_(alert::SENSOR_LOST, lost, "the Bluetooth thermometer has not been heard for 10 minutes");

  const Status s = this->c_.status();
  const float dt = float(uint32_t(now - this->last_ms_)) / 1000.0f;
  this->step_fuel_(now, s, utc);
  this->step_runs_(now, s);
  this->step_burn_(now, s);
  this->step_co_(s);
  this->step_battery_(now, s);
  this->step_glow_(now, s);
  this->step_maintenance_(now, s, dt);
  this->step_frost_(now, this->c_.status());
  this->step_timers_(now, this->c_.status(), utc);
  this->last_ms_ = now;
  // The log every minute when something was added (and at every stop, in step_runs_).
  if (this->log_book_.dirty() && this->log_cb_ && uint32_t(now - this->log_saved_ms_) >= 60 * 1000) {
    this->log_cb_(this->log_book_.saved());
    this->log_book_.saved_now();
    this->log_saved_ms_ = now;
  }
}

// ---------------------------------------------------------------- battery

void Automation::step_battery_(uint32_t now, const Status &s) {
  const float cutoff = this->c_.tuning().extra(B_CUTOFF) / 10.0f;
  if (!(cutoff > 0.0f) || !s.answering || !std::isfinite(s.supply_v)) {
    this->battery_low_since_ms_ = 0;
    if (!(cutoff > 0.0f))
      this->set_alert_(alert::BATTERY_LOW, false, "");
    return;
  }
  const float v = s.supply_v;
  if ((this->alerts_ & alert::BATTERY_LOW) && v >= cutoff + 0.5f)
    this->set_alert_(alert::BATTERY_LOW, false, "");
  const bool running = s.state == HeaterState::HEATING || s.state == HeaterState::FAN_ONLY;
  const bool starting = s.state == HeaterState::STARTING;
  // The glow plug pulls any battery down while starting: only a collapsing one goes 1 V lower.
  const float limit = starting ? cutoff - 1.0f : cutoff;
  if (!(running || starting) || s.mode == Mode::OFF || v >= limit) {
    this->battery_low_since_ms_ = 0;
    return;
  }
  if (this->battery_low_since_ms_ == 0)
    this->battery_low_since_ms_ = now == 0 ? 1 : now;
  const uint32_t delay_ms = uint32_t(this->c_.tuning().extra(B_DELAY)) * 1000;
  if (uint32_t(now - this->battery_low_since_ms_) < delay_ms || this->stop_asked_)
    return;
  char text[96];
  std::snprintf(text, sizeof(text), "battery low (%.1f V): the heater was stopped", v);
  this->log_(LogKind::LOW_BATTERY_STOP, Source::NONE, 0, uint32_t(std::lround(v * 1000.0f)));
  this->stop_(Source::BOARD, "battery low");
  this->raise_(alert::BATTERY_LOW, text);
  this->battery_low_since_ms_ = 0;
}

// ---------------------------------------------------------------- glow plug

void Automation::step_glow_(uint32_t now, const Status &s) {
  // The heater started with a plug-type warning (a 12 V supply: it could not overheat the plug).
  if (s.answering && s.plug_warning)
    this->set_alert_(alert::GLOW_PLUG_TYPE, true,
                     "the glow plug may not be the type set: check the plug type setting, then New glow plug fitted");
  const bool on = s.answering && s.state == HeaterState::STARTING && std::isfinite(s.glow_v) &&
                  std::isfinite(s.glow_a) && s.glow_v > 4.0f && s.glow_a > 2.0f;
  if (on) {
    if (this->glow_on_since_ms_ == 0)
      this->glow_on_since_ms_ = now == 0 ? 1 : now;
    if (uint32_t(now - this->glow_on_since_ms_) >= 15 * 1000) {
      const float r = s.glow_v / s.glow_a * 1000.0f;
      if (r > 0.0f && r < 60000.0f) {
        const size_t N = sizeof(this->glow_window_) / sizeof(this->glow_window_[0]);
        if (this->glow_window_n_ == N) {
          for (size_t i = 1; i < N; i++)
            this->glow_window_[i - 1] = this->glow_window_[i];
          this->glow_window_n_--;
        }
        this->glow_window_[this->glow_window_n_++] = uint16_t(std::lround(r));
      }
    }
    return;
  }
  // The plug went off: one reading for this start, the median of its last 10 s.
  if (this->glow_on_since_ms_ != 0 && !this->glow_read_this_run_ && this->glow_window_n_ >= 5) {
    uint16_t sorted[10];
    const size_t n = this->glow_window_n_;
    std::memcpy(sorted, this->glow_window_, n * sizeof(uint16_t));
    for (size_t i = 1; i < n; i++)
      for (size_t j = i; j > 0 && sorted[j - 1] > sorted[j]; j--)
        std::swap(sorted[j - 1], sorted[j]);
    this->glow_read_this_run_ = true;
    this->glow_reading_(sorted[n / 2]);
  }
  this->glow_on_since_ms_ = 0;
  this->glow_window_n_ = 0;
}

void Automation::glow_reading_(uint16_t mohm) {
  Maintenance &m = this->maint_;
  if (m.glow_n == GLOW_READINGS) {
    for (size_t i = 1; i < GLOW_READINGS; i++)
      m.glow_readings[i - 1] = m.glow_readings[i];
    m.glow_n--;
  }
  m.glow_readings[m.glow_n++] = mohm;
  if (m.glow_starts < 0xFFFF)
    m.glow_starts++;
  this->log_(LogKind::GLOW_READING, Source::NONE, 0, mohm);
  if (m.glow_starts <= 3) {
    // The baseline: the average of the first 3 starts with this plug.
    uint32_t sum = 0;
    const size_t k = m.glow_starts < m.glow_n ? m.glow_starts : m.glow_n;
    for (size_t i = m.glow_n - k; i < m.glow_n; i++)
      sum += m.glow_readings[i];
    m.glow_baseline_mohm = uint16_t(sum / k);
  } else {
    const int drift = this->c_.tuning().extra(M_GLOW_DRIFT);
    bool all = drift > 0 && m.glow_n >= 3 && m.glow_baseline_mohm != 0xFFFF && m.glow_baseline_mohm != 0;
    for (size_t i = m.glow_n >= 3 ? m.glow_n - 3 : 0; all && i < m.glow_n; i++) {
      const float off = std::fabs(float(m.glow_readings[i]) - m.glow_baseline_mohm) / m.glow_baseline_mohm * 100.0f;
      all = off > float(drift);
    }
    if (all) {
      char text[110];
      std::snprintf(text, sizeof(text), "the glow plug reads %u mOhm, %u when new: it may be wearing out",
                    unsigned(mohm), unsigned(m.glow_baseline_mohm));
      this->set_alert_(alert::GLOW_DRIFT, true, text);
    }
  }
  this->save_maintenance_now_();
}

Outcome Automation::glow_new_plug() {
  // The heater forgets the old plug's type check too (a safety MCU that has one).
  if (Heater *h = this->c_.heater())
    h->glow_plug_fitted();
  this->set_alert_(alert::GLOW_PLUG_TYPE, false, "");
  this->maint_.glow_baseline_mohm = 0xFFFF;
  this->maint_.glow_starts = 0;
  this->maint_.glow_n = 0;
  this->set_alert_(alert::GLOW_DRIFT, false, "");
  this->save_maintenance_now_();
  this->c_.refresh();
  return Outcome::ok();
}

// ---------------------------------------------------------------- carbon burn-off

void Automation::step_maintenance_(uint32_t now, const Status &s, float dt) {
  if (!(dt > 0.0f) || dt > 60.0f)
    dt = 0.0f;
  const bool heating = s.answering && s.state == HeaterState::HEATING && std::isfinite(s.power_pct);
  if (heating && s.power_pct <= float(this->c_.tuning().extra(M_LOW_POWER)))
    this->maint_.low_power_s += uint32_t(std::lround(dt));
  // 20 minutes in a row at 90 % or more counts as a burn-off.
  this->high_power_s_ = (heating && s.power_pct >= 90.0f) ? this->high_power_s_ + dt : 0.0f;
  if (this->high_power_s_ >= 20 * 60 && this->maint_.low_power_s != 0) {
    this->maint_.low_power_s = 0;
    this->save_maintenance_now_();
  }
  const int hours = this->c_.tuning().extra(M_REMINDER);
  this->set_alert_(alert::BURNOFF_DUE, hours > 0 && this->maint_.low_power_s >= uint32_t(hours) * 3600,
                   "a carbon burn-off is due: run one from Maintenance");
  // The run's end.
  if (this->burnoff_active_ && !this->burnoff_done_ && int32_t(now - this->burnoff_end_ms_) >= 0 &&
      s.mode != Mode::OFF) {
    this->burnoff_done_ = true;
    this->stop_(Source::BOARD, "carbon burn-off finished");
  }
}

Outcome Automation::burnoff_start(uint8_t minutes, Source source) {
  if (minutes < 10 || minutes > 30)
    return Outcome::refuse(Result::OUT_OF_RANGE, "10 to 30 minutes");
  const Status &s = this->c_.status();
  if (s.mode != Mode::OFF || this->burnoff_active_)
    return Outcome::refuse(Result::NOT_OFF, "the heater must be off");
  this->saved_settings_ = Restore{true, s.heat_mode, s.target_c, s.power};
  this->c_.set_heat_mode(HeatMode::POWER, source);
  this->c_.set_power(100, source);
  Outcome o = this->c_.set_mode(Mode::HEAT, source);
  if (!o.accepted()) {
    this->restore_();
    return o;
  }
  this->burnoff_active_ = true;
  this->burnoff_done_ = false;
  this->burnoff_minutes_ = minutes;
  this->burnoff_end_ms_ = this->c_.now() + uint32_t(minutes) * 60 * 1000;
  this->log_(LogKind::BURNOFF, source, 0, minutes);
  this->c_.refresh();
  return o;
}

uint8_t Automation::burnoff_minutes_left(uint32_t now) const {
  if (!this->burnoff_active_ || this->burnoff_done_)
    return 0;
  const int32_t left = int32_t(this->burnoff_end_ms_ - now);
  if (left <= 0)
    return 0;
  return uint8_t((uint32_t(left) + 59999) / 60000);
}

// ---------------------------------------------------------------- CO

void Automation::step_co_(const Status &s) {
  const bool alarm = s.fault == Fault::CO || s.co_state == uint8_t(CoState::ALARM);
  const bool sensor = s.fault == Fault::CO_SENSOR || s.co_state == uint8_t(CoState::SENSOR_FAULT);
  this->set_alert_(alert::CO_ALARM, alarm,
                   "CO alarm: the heater was stopped. Air the space; reset the heater fault once the air is clean");
  this->set_alert_(alert::CO_SENSOR, sensor, "CO sensor fault: the heater will not start until it reads again");
  // The safety MCU stops the heater itself; this is the second layer.
  if ((alarm || sensor) && s.mode != Mode::OFF && !this->stop_asked_)
    this->stop_(Source::BOARD, alarm ? "CO alarm" : "CO sensor fault");
}

}  // namespace flamekeeper
}  // namespace esphome
