#include "link_client.h"
#include "bytes.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace esphome {
namespace flamekeeper {

namespace {
float fixed(int32_t raw, int32_t unknown, float scale) { return raw == unknown ? NAN : float(raw) / scale; }
const char *const QUEUE_FULL_TEXT = "too many changes waiting";
}  // namespace

LinkClient::LinkClient(LinkTransport *transport) : transport_(transport) {
  link_parser_init(&this->parser_, LINK_REPLY);
  link_status_unknown(&this->status_);
  link_detail_unknown(&this->detail_);
  this->status_.state = 0;
}

// ---------------------------------------------------------------- the link

void LinkClient::loop(uint32_t now) {
  this->now_ = now;
  if (!this->started_) {
    this->started_ = true;
    this->heard_ms_ = now;  // silence counts from the start
  }
  // The safety-MCU updater has the UART: the link waits, and starts afresh when it is back.
  if (this->mcu_ != nullptr) {
    this->mcu_->loop(now);
    if (this->mcu_->owns_uart()) {
      if (!this->lent_) {
        this->lent_ = true;
        this->down_("updating");
        this->outstanding_ = false;
      }
      return;
    }
    if (this->lent_) {
      this->lent_ = false;
      this->relink_();
    }
  }
  for (int c; (c = this->transport_->read()) >= 0;) {
    if (link_parser_feed(&this->parser_, uint8_t(c)))
      this->on_reply_(this->parser_.buf);
    if (this->mcu_ != nullptr && this->mcu_->owns_uart())
      return;  // UPDATE_PREPARE agreed: nothing more on the link
  }
  if (this->outstanding_ && now - this->out_ms_ >= REPLY_TIMEOUT_MS) {
    this->outstanding_ = false;
    if (this->up_ && ++this->misses_ >= MISSES_DOWN)
      this->down_("no reply");
  }
  if (!this->outstanding_)
    this->send_next_(now);
  if (this->mcu_ != nullptr)
    this->mcu_->on_link(this->up_, this->info_.version);
}

// After the updater: the MCU has been reset, so the link starts again with HELLO.
void LinkClient::relink_() {
  link_parser_init(&this->parser_, LINK_REPLY);
  this->down_("updated");
  this->outstanding_ = false;
  this->polled_once_ = false;
  this->heard_ms_ = this->now_;
}

void LinkClient::send_(uint8_t op, const uint8_t *payload, size_t len, uint16_t id, uint32_t now) {
  uint8_t frame[LINK_FRAME_MAX];
  if (++this->seq_ == 0)
    this->seq_ = 1;
  size_t n = link_build_request(this->seq_, op, payload, len, frame);
  if (n == 0)
    return;
  this->outstanding_ = true;
  this->out_seq_ = this->seq_;
  this->out_op_ = op;
  this->out_id_ = id;
  this->out_ms_ = now;
  this->transport_->send(frame, n);
}

void LinkClient::send_next_(uint32_t now) {
  if (!this->up_) {
    if (!this->polled_once_ || now - this->last_hello_ms_ >= HELLO_MS) {
      this->polled_once_ = true;
      this->last_hello_ms_ = now;
      const uint8_t ver[2] = {LINK_MAJOR, LINK_MINOR};
      this->send_(LINK_OP_HELLO, ver, 2, 0, now);
    }
    return;
  }
  // The poll keeps its 10 Hz: it is the dead-man's heartbeat.
  if (now - this->last_poll_ms_ >= POLL_MS) {
    this->last_poll_ms_ = now;
    link_desired want{};
    want.start = this->start_;
    want.mode = this->mode_;
    want.power = this->power_;
    want.fuel_trim = this->fuel_trim_;
    want.fan_trim = this->fan_trim_;
    want.flags = uint8_t(this->room_known_ ? LINK_WANT_ROOM_KNOWN : 0);
    uint8_t p[LINK_DESIRED_SIZE];
    link_encode_desired(&want, p);
    this->send_(LINK_OP_POLL, p, sizeof(p), 0, now);
    return;
  }
  if (!this->queue_cmds_.empty()) {
    Request r = this->queue_cmds_.front();
    this->queue_cmds_.pop_front();
    this->send_(r.op, r.payload, r.len, r.id, now);
    return;
  }
  // The fault log, whole, when it is not known or Status numbers a newer one: a page at a time.
  if (this->has_log_() && this->have_status_ &&
      (this->log_busy_ || !this->log_known_ || this->status_.log_seq != this->log_newest_())) {
    if (!this->log_busy_) {
      this->log_busy_ = true;
      this->log_reading_.clear();
    }
    uint8_t from = uint8_t(this->log_reading_.size());
    this->send_(LINK_OP_FAULT_LOG, &from, 1, 0, now);
    return;
  }
  // Describe the parameter table once.
  if (this->param_count_ != 0 && !this->params_ready_()) {
    for (size_t i = 0; i < this->param_count_; i++) {
      if (i >= this->params_.size() || !this->params_[i].described) {
        uint8_t index = uint8_t(i);
        this->send_(LINK_OP_PARAM_DESCRIBE, &index, 1, 0, now);
        return;
      }
    }
  }
  // Then each parameter's description, once (an MCU without PARAM_HELP answers NOT_SUPPORTED, and
  // the parameter keeps an empty one).
  if (this->params_ready_()) {
    for (const Param &p : this->params_) {
      if (!p.help_done) {
        uint8_t q[3];
        put_u16(q, p.desc.id);
        q[2] = uint8_t(p.help.size());
        this->send_(LINK_OP_PARAM_HELP, q, 3, p.desc.id, now);
        return;
      }
    }
  }
  // HELLO again now and then: the MCU's description can change (its firmware text, a firmware
  // update, a different parameter table).
  if (now - this->last_hello_ms_ >= INFO_REFRESH_MS) {
    this->last_hello_ms_ = now;
    const uint8_t ver[2] = {LINK_MAJOR, LINK_MINOR};
    this->send_(LINK_OP_HELLO, ver, 2, 0, now);
    return;
  }
  if (now - this->last_detail_ms_ >= DETAIL_MS) {
    this->last_detail_ms_ = now;
    this->send_(LINK_OP_DETAIL, nullptr, 0, 0, now);
    return;
  }
  // Keep the parameter values fresh, one at a time.
  if (this->params_ready_() && !this->params_.empty() && now - this->last_param_ms_ >= PARAM_REFRESH_MS) {
    this->last_param_ms_ = now;
    this->refresh_index_ = (this->refresh_index_ + 1) % this->params_.size();
    uint8_t p[2];
    put_u16(p, this->params_[this->refresh_index_].desc.id);
    this->send_(LINK_OP_PARAM_GET, p, 2, this->params_[this->refresh_index_].desc.id, now);
  }
}

void LinkClient::down_(const char *why) {
  (void) why;
  this->up_ = false;
  this->have_status_ = false;
  this->need_baseline_ = true;
  this->misses_ = 0;
  this->queue_cmds_.clear();
  this->log_known_ = false;  // read again once back: the MCU may have reset or changed
  this->log_busy_ = false;
}

void LinkClient::on_reply_(const uint8_t *f) {
  if (!this->outstanding_ || link_frame_seq(f) != this->out_seq_ || link_frame_op(f) != this->out_op_)
    return;  // late or foreign: dropped
  this->outstanding_ = false;
  this->misses_ = 0;
  this->heard_ms_ = this->now_;
  const uint8_t op = link_frame_op(f), result = link_reply_result(f);
  const uint8_t *p = link_reply_payload(f);
  const size_t n = link_reply_payload_len(f);
  switch (op) {
    case LINK_OP_HELLO: {
      link_info info;
      if (result != LINK_OK || !link_decode_info(p, n, &info))
        return;
      if (info.major != LINK_MAJOR) {
        // Never drive a heater we do not understand: down (polls stop, so the MCU's dead-man stops
        // a run), also when a HELLO refresh reports another major.
        this->incompatible_ = true;
        this->up_ = false;
        this->have_status_ = false;
        this->info_ = info;
        return;
      }
      this->incompatible_ = false;
      bool table_changed = info.param_count != this->param_count_ || info.kind != this->info_.kind;
      this->info_ = info;
      this->param_count_ = (info.capabilities & LINK_CAP_PARAMETERS) ? info.param_count : 0;
      if (table_changed)
        this->params_.clear();
      if (!this->up_) {
        this->up_ = true;
        this->need_baseline_ = true;
        this->last_poll_ms_ = this->now_ - POLL_MS;  // poll at once
      }
      break;
    }
    case LINK_OP_POLL: {
      link_status st;
      if (result == LINK_NOT_SUPPORTED) {
        // The MCU has no HELLO from us (it restarted): HELLO again at once.
        this->up_ = false;
        this->polled_once_ = false;
        return;
      }
      if (result != LINK_OK || !link_decode_status(p, n, &st))
        return;
      this->status_ = st;
      this->have_status_ = true;
      if (this->need_baseline_) {
        // Continue from the number the MCU last acted on, so our next start is a new one and an
        // unchanged one never starts anything (DESIGN.md).
        this->start_ = st.start_ack;
        this->need_baseline_ = false;
      }
      // Our start, as the MCU handled it. Refused, or run and back to off by itself (fault,
      // flame-out, dead-man, priming done): what we ask for follows, so the user sees it off.
      if (this->mode_ != LINK_MODE_OFF && st.start_ack == this->start_) {
        if (st.state != uint8_t(HeaterState::OFF))
          this->seen_running_ = true;
        if (st.start_result != LINK_OK || (this->seen_running_ && st.state == uint8_t(HeaterState::OFF)))
          this->mode_ = LINK_MODE_OFF;
      }
      break;
    }
    case LINK_OP_DETAIL:
      if (result == LINK_OK)
        link_decode_detail(p, n, &this->detail_);
      break;
    case LINK_OP_PARAM_DESCRIBE: {
      link_param_desc d;
      if (result != LINK_OK || !link_decode_param_desc(p, n, &d) || d.index >= this->param_count_)
        return;
      if (this->params_.size() < this->param_count_)
        this->params_.resize(this->param_count_);
      this->params_[d.index].desc = d;
      this->params_[d.index].described = true;
      break;
    }
    case LINK_OP_PARAM_HELP: {
      Param *pp = this->param_(this->out_id_);
      if (pp == nullptr || pp->help_done)
        return;
      if (result != LINK_OK || n < LINK_PARAM_HELP_FIXED || get_u16(p) != this->out_id_ || p[2] != pp->help.size() ||
          p[3] < p[2]) {
        pp->help.clear();
        pp->help_done = true;  // none, or a muddle: left without one
        return;
      }
      const size_t total = p[3] > LINK_PARAM_HELP_MAX ? LINK_PARAM_HELP_MAX : p[3];
      size_t chunk = n - LINK_PARAM_HELP_FIXED;
      if (pp->help.size() + chunk > total)
        chunk = total - pp->help.size();
      pp->help.append(reinterpret_cast<const char *>(p + LINK_PARAM_HELP_FIXED), chunk);
      // Done at the full length, or when a piece brings nothing (the MCU's text got shorter).
      if (pp->help.size() >= total || chunk == 0)
        pp->help_done = true;
      break;
    }
    case LINK_OP_PARAM_GET: {
      link_param_value v;
      if (result != LINK_OK || !link_decode_param_value(p, n, &v))
        return;
      Param *pp = this->param_(v.id);
      if (pp == nullptr)
        return;
      bool changed = pp->value.value != v.value || pp->value.flags != v.flags;
      pp->value = v;
      pp->staged_here = false;
      if (changed)
        this->emit_(Event::PARAM_CHANGED, Result::OK, v.id, "");
      break;
    }
    case LINK_OP_PARAM_SET:
      if (result != LINK_OK) {
        Param *pp = this->param_(this->out_id_);
        if (pp != nullptr)
          pp->staged_here = false;
        this->emit_(Event::PARAM_CHANGED, Result(result), this->out_id_, "the heater refused the value");
      }
      break;
    case LINK_OP_FAULT_LOG: {
      if (!this->log_busy_)
        return;
      if (result != LINK_OK || n < 1) {
        // Not answered as asked: what was read stands, and nothing more is asked until it changes.
        this->log_busy_ = false;
        this->log_known_ = true;
        return;
      }
      const size_t total = p[0] > HEATER_LOG_MAX ? HEATER_LOG_MAX : p[0];
      size_t got = 0;
      for (size_t k = 1; k + LINK_FAULT_ENTRY_SIZE <= n && this->log_reading_.size() < total; k += LINK_FAULT_ENTRY_SIZE) {
        link_fault_entry e;
        link_decode_fault_entry(p + k, LINK_FAULT_ENTRY_SIZE, &e);
        this->log_reading_.push_back(e);
        got++;
      }
      if (got == 0 || this->log_reading_.size() >= total) {
        // Done. Should an entry have come in meanwhile, Status's number differs and it is read again.
        this->log_ = this->log_reading_;
        this->log_busy_ = false;
        this->log_known_ = true;
      }
      break;
    }
    case LINK_OP_FAULT_RESET:
      this->emit_(Event::FAULT_RESET, Result(result), 0,
                  result == LINK_OK ? "fault cleared" : "the heater kept the fault (its cause may still be there)");
      break;
    case LINK_OP_UPDATE_PREPARE:
      if (this->mcu_ != nullptr)
        this->mcu_->prepared(result, p, n);
      break;
    case LINK_OP_PARAM_DEFAULTS:
      this->emit_(Event::PARAMS_DEFAULTS, Result(result), 0,
                  result == LINK_OK ? "the heater's parameters are back to their defaults"
                                    : "the heater did not reset its parameters");
      break;
    case LINK_OP_PARAM_APPLY:
      // The PARAM_GET sweep picks up the new values.
      if (this->apply_events_ || result != LINK_OK)
        this->emit_(this->out_id_ == 1 ? Event::PARAMS_SAVED : Event::PARAMS_APPLIED,
                  result == LINK_OK ? Result::OK : Result::REFUSED,
                  0, result == LINK_OK ? (this->out_id_ == 1 ? "saved in the heater" : "applied")
                                       : "the heater refused the changes");
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------- Heater

// ---------------------------------------------------------------- the fault log

size_t LinkClient::fault_log(size_t from, HeaterLogEntry *out, size_t max) const {
  size_t n = 0;
  for (size_t i = from; i < this->log_.size() && n < max; i++, n++) {
    const link_fault_entry &e = this->log_[i];
    HeaterLogEntry &o = out[n];
    o.kind = e.kind;
    o.code = e.code;
    o.why = e.why;
    o.step = e.step;
    o.seq = e.seq;
    o.board_c = e.board_c;
    o.hx_c10 = e.hx;
    o.supply_cv = e.supply;
    o.uptime_s = e.uptime_s;
  }
  return n;
}

Outcome LinkClient::clear_fault_log() {
  if (!this->up_)
    return Outcome::refuse(Result::NO_REPLY, "the heater does not answer");
  if (!this->has_log_())
    return Outcome::refuse(Result::NOT_SUPPORTED, "this heater has no fault log");
  const uint8_t cl[2] = {'C', 'L'};
  return this->queue_(LINK_OP_FAULT_LOG_CLEAR, cl, 2);
}

uint32_t LinkClient::capabilities() const {
  // The heater firmware update also works on an MCU that does not answer (an unfinished update).
  uint32_t c = this->mcu_available_() ? cap::FIRMWARE_UPDATE : 0;
  if (!this->up_)
    return c;
  const uint32_t l = this->info_.capabilities;
  if (l & LINK_CAP_FAN_ONLY)
    c |= cap::FAN_ONLY;
  if (l & LINK_CAP_PRIME)
    c |= cap::PRIME;
  if (l & LINK_CAP_PRIME_STOP)
    c |= cap::PRIME_STOP;
  if ((l & LINK_CAP_PARAMETERS) && this->params_ready_() && !this->params_.empty())
    c |= cap::PARAMETERS;
  if (l & LINK_CAP_FAULT_LOG)
    c |= cap::FAULT_LOG;
  return c;
}

void LinkClient::read_status(HeaterStatus &out) const {
  out = HeaterStatus();
  const link_status &s = this->status_;
  out.mode = Mode(this->mode_);
  out.heat_mode = HeatMode::POWER;
  out.power = this->power_;
  out.target_c = this->target_c_;
  out.answering = this->up_ && this->have_status_;
  out.state = out.answering && s.state <= uint8_t(HeaterState::UPDATING) ? HeaterState(s.state) : HeaterState::UNKNOWN;
  if (!out.answering)
    out.control = ControlStatus::NO_REPLY;
  else {
    switch (out.state) {
      case HeaterState::STARTING: out.control = ControlStatus::STARTING; break;
      case HeaterState::HEATING:
      case HeaterState::FAN_ONLY: out.control = ControlStatus::RUNNING; break;
      case HeaterState::PRIMING: out.control = ControlStatus::PRIMING; break;
      case HeaterState::STOPPING:
      case HeaterState::COOLING: out.control = ControlStatus::STOPPING; break;
      case HeaterState::UPDATING: out.control = ControlStatus::UPDATING; break;
      default:
        // A start sent but not reported yet: starting, not idle (the heater still says off).
        out.control = this->mode_ != LINK_MODE_OFF && !this->seen_running_ ? ControlStatus::STARTING
                                                                             : ControlStatus::IDLE;
        break;
    }
  }
  out.fault_log_seq = this->log_newest_();  // as last read, answering or not
  out.fault_log_count = uint8_t(this->log_.size());
  if (this->mcu_running_()) {
    out.update_running = true;
    out.update_progress = this->mcu_->progress();
    out.state = HeaterState::UPDATING;
    out.control = ControlStatus::UPDATING;
  }
  if (!out.answering)
    return;
  out.fault = Fault(s.fault);
  out.panel_fault = s.fault_detail;
  out.prime_left_s = s.prime_left_s;
  uint8_t staged = 0;
  for (const Param &p : this->params_)
    staged += (p.staged_here || (p.value.flags & LINK_PARAM_STAGED)) ? 1 : 0;
  out.params_pending = staged;
  out.plug_warning = this->have_status_ && (this->status_.flags & LINK_STATUS_PLUG_WARNING) != 0;
  out.heat_exchanger_c = fixed(s.heat_exchanger, LINK_UNKNOWN_I16, 10.0f);
  out.supply_v = fixed(s.supply, LINK_UNKNOWN_U16, 100.0f);
  out.fan_rpm = fixed(s.fan_rpm, LINK_UNKNOWN_U16, 1.0f);
  out.pump_hz = fixed(s.pump, LINK_UNKNOWN_U16, 100.0f);
  out.glow_w = fixed(s.glow_w, LINK_UNKNOWN_U16, 1.0f);
  out.power_pct = s.power == LINK_UNKNOWN_U8 ? NAN : float(s.power);
  out.run_time_s = s.run_time_s == LINK_UNKNOWN_U32 ? NAN : float(s.run_time_s);
  const link_detail &d = this->detail_;
  out.glow_v = fixed(d.glow_v, LINK_UNKNOWN_U16, 100.0f);
  out.glow_a = fixed(d.glow_a, LINK_UNKNOWN_U16, 100.0f);
  out.fan_v = fixed(d.fan_v, LINK_UNKNOWN_U16, 100.0f);
  out.co_state = d.co_state;
  out.co_ppm = fixed(d.co_ppm, LINK_UNKNOWN_U16, 1.0f);
}

Outcome LinkClient::set_mode(Mode mode) {
  if (mode == Mode::OFF) {
    this->mode_ = LINK_MODE_OFF;
    return Outcome::ok();
  }
  if (this->mcu_running_())
    return Outcome::refuse(Result::BUSY, McuUpdater::BUSY_TEXT);
  if (!this->up_ || !this->have_status_)
    return Outcome::refuse(Result::NO_REPLY, this->incompatible_ ? "the safety MCU speaks an unknown link version"
                                                                  : "the heater does not answer");
  if (this->status_.flags & LINK_STATUS_NO_STARTS)
    return Outcome::refuse(Result::BUSY, "the heater refuses starts (update or start-up check)");
  const uint8_t want = uint8_t(mode);
  if (this->mode_ != LINK_MODE_OFF) {
    // Running: heat <-> fan needs no new start where the MCU can switch; priming only from off.
    if (mode == Mode::PRIME || this->mode_ == LINK_MODE_PRIME)
      return Outcome::refuse(Result::NOT_OFF, "the heater must be off");
    if (want != this->mode_ && !(this->info_.capabilities & LINK_CAP_RUN_SWITCH))
      return Outcome::refuse(Result::NOT_OFF, "set the heater to off first, then start it in the other mode");
    this->mode_ = want;
    return Outcome::ok();
  }
  if (this->status_.state != uint8_t(HeaterState::OFF))
    return Outcome::refuse(Result::NOT_OFF, "the heater is not off yet (wait for the cooldown)");
  this->start_ = uint8_t(this->start_ + 1);  // a new start (DESIGN.md)
  this->mode_ = want;
  this->seen_running_ = false;
  this->last_poll_ms_ = this->now_ - POLL_MS;  // ask at once
  return Outcome::ok();
}

Outcome LinkClient::set_heat_mode(HeatMode mode) {
  if (mode == HeatMode::POWER)
    return Outcome::ok();
  return Outcome::refuse(Result::NOT_SUPPORTED, "temperature mode is not available yet");
}

Outcome LinkClient::set_power_demand(uint8_t percent) {
  this->power_ = percent > 100 ? 100 : percent;
  return Outcome::ok();
}

Outcome LinkClient::set_target(float celsius) {
  this->target_c_ = celsius;
  return Outcome::ok();
}

Outcome LinkClient::stop_prime() {
  if (this->mode_ == LINK_MODE_PRIME)
    this->mode_ = LINK_MODE_OFF;
  return Outcome::ok();
}

void LinkClient::set_trims(int fuel_pct, int fan_rpm) {
  this->fuel_trim_ = int8_t(fuel_pct < -30 ? -30 : (fuel_pct > 30 ? 30 : fuel_pct));
  this->fan_trim_ = int16_t(fan_rpm < -1000 ? -1000 : (fan_rpm > 1000 ? 1000 : fan_rpm));
}

Outcome LinkClient::queue_(uint8_t op, const uint8_t *payload, size_t len, uint16_t id) {
  if (!this->up_)
    return Outcome::refuse(Result::NO_REPLY, "the heater does not answer");
  if (this->queue_full_())
    return Outcome::refuse(Result::BUSY, QUEUE_FULL_TEXT);
  Request r{};
  r.op = op;
  r.len = uint8_t(len);
  r.id = id;
  if (len != 0)
    memcpy(r.payload, payload, len);
  this->queue_cmds_.push_back(r);
  return Outcome::ok();
}

// Answered later by event 06 with the heater's own result.
Outcome LinkClient::reset_fault() {
  if (!this->up_ || !this->have_status_)
    return Outcome::refuse(Result::NO_REPLY, "the heater does not answer");
  if (!(this->status_.flags & LINK_STATUS_FAULT_LATCHED))
    return Outcome::refuse(Result::REFUSED, "the heater has no latched fault to reset");
  return this->queue_(LINK_OP_FAULT_RESET, nullptr, 0);
}

Outcome LinkClient::glow_plug_fitted() {
  if (!this->up_)
    return Outcome::refuse(Result::NO_REPLY, "the heater does not answer");
  // An MCU without a plug check answers "not supported": nothing to forget, and nothing to show.
  return this->queue_(LINK_OP_GLOW_NEW_PLUG, nullptr, 0);
}

// ---------------------------------------------------------------- the safety-MCU updater

Outcome LinkClient::start_mcu_job(McuJob job) {
  if (this->mcu_ == nullptr || (job == McuJob::UPDATE && !this->mcu_->available()))
    return Outcome::refuse(Result::NOT_SUPPORTED, "this board cannot update the heater's firmware");
  if (this->mcu_running_())
    return Outcome::refuse(Result::BUSY, McuUpdater::BUSY_TEXT);
  if (this->mode_ != LINK_MODE_OFF)
    return Outcome::refuse(Result::NOT_OFF, "turn the heater off first");
  if (this->up_ && this->have_status_) {
    // The MCU decides: UPDATE_PREPARE only with the heater off and cold.
    if (this->status_.state != uint8_t(HeaterState::OFF))
      return Outcome::refuse(Result::NOT_OFF, "the heater is not off yet (wait for the cooldown)");
    if (this->queue_full_())
      return Outcome::refuse(Result::BUSY, QUEUE_FULL_TEXT);
    Outcome o = this->mcu_->begin(job, true, this->now_);
    if (o.accepted())
      this->queue_(LINK_OP_UPDATE_PREPARE, nullptr, 0);
    return o;
  }
  // Silent: only this board's own update, agreed to and left unfinished, is taken up again.
  if (this->mcu_->may_take(this->now_ - this->heard_ms_))
    return this->mcu_->begin(job, false, this->now_);
  return Outcome::refuse(Result::NO_REPLY, "the heater does not answer");
}

const char *LinkClient::firmware_update_text() const {
  if (!this->mcu_available_())
    return "Not available";
  if (this->mcu_->text()[0] != 0)
    return this->mcu_->text();
  const uint8_t *v = this->mcu_->image_version();
  if (!this->up_)
    snprintf(this->update_text_, sizeof(this->update_text_), "This build carries %u.%u.%u", v[0], v[1], v[2]);
  else if (std::memcmp(v, this->info_.version, 3) == 0)
    snprintf(this->update_text_, sizeof(this->update_text_), "Up to date (%u.%u.%u)", v[0], v[1], v[2]);
  else
    snprintf(this->update_text_, sizeof(this->update_text_), "%u.%u.%u available (the heater runs %u.%u.%u)", v[0],
             v[1], v[2], this->info_.version[0], this->info_.version[1], this->info_.version[2]);
  return this->update_text_;
}

Outcome LinkClient::reset_parameters() {
  const uint8_t df[2] = {'D', 'F'};
  return this->queue_(LINK_OP_PARAM_DEFAULTS, df, 2);
}

// ---------------------------------------------------------------- parameters

bool LinkClient::params_ready_() const {
  if (this->params_.size() != this->param_count_)
    return false;
  for (const Param &p : this->params_)
    if (!p.described)
      return false;
  return true;
}

LinkClient::Param *LinkClient::param_(uint16_t id) {
  for (Param &p : this->params_)
    if (p.described && p.desc.id == id)
      return &p;
  return nullptr;
}

const LinkClient::Param *LinkClient::param_(uint16_t id) const {
  for (const Param &p : this->params_)
    if (p.described && p.desc.id == id)
      return &p;
  return nullptr;
}

size_t LinkClient::param_count() const { return this->params_ready_() ? this->params_.size() : 0; }

bool LinkClient::param_info(size_t index, ParamInfo &out) const {
  if (!this->params_ready_() || index >= this->params_.size())
    return false;
  const link_param_desc &d = this->params_[index].desc;
  out.id = d.id;
  out.flags = d.flags;
  out.unit = Unit(d.unit);
  out.decimals = d.decimals;
  out.min = d.min;
  out.max = d.max;
  out.step = d.step;
  out.name = d.name;
  out.group = ParamGroup(d.group);
  out.level = d.level > uint8_t(ParamLevel::EXPERT) ? ParamLevel::EXPERT : ParamLevel(d.level);
  return true;
}

const char *LinkClient::param_help(uint16_t id) const {
  const Param *p = this->param_(id);
  return p != nullptr && p->help_done ? p->help.c_str() : "";
}

bool LinkClient::param_get(uint16_t id, ParamValue &out) const {
  const Param *p = this->param_(id);
  if (p == nullptr)
    return false;
  out.known = (p->value.flags & LINK_PARAM_KNOWN) != 0;
  out.value = p->value.value;
  out.staged = p->staged_here || (p->value.flags & LINK_PARAM_STAGED);
  out.staged_value = p->staged_here ? p->staged_value : p->value.staged;
  return true;
}

Outcome LinkClient::param_set(uint16_t id, int32_t value) {
  Param *p = this->param_(id);
  if (p == nullptr)
    return Outcome::refuse(Result::OUT_OF_RANGE, "no such parameter");
  if (p->desc.flags & 0x04)
    return Outcome::refuse(Result::REFUSED, "read-only");
  if (value < p->desc.min || value > p->desc.max)
    return Outcome::refuse(Result::OUT_OF_RANGE, "out of range");
  uint8_t pl[6];
  put_u16(pl, id);
  put_u32(pl + 2, uint32_t(value));
  Outcome o = this->queue_(LINK_OP_PARAM_SET, pl, 6, id);
  if (o.result == Result::OK) {
    p->staged_here = true;
    p->staged_value = value;
  }
  return o;
}

Outcome LinkClient::param_apply(bool save) {
  uint8_t s = save ? 1 : 0;
  return this->queue_(LINK_OP_PARAM_APPLY, &s, 1, s);
}

Outcome LinkClient::param_discard() {
  for (Param &p : this->params_)
    p.staged_here = false;
  return this->queue_(LINK_OP_PARAM_DISCARD, nullptr, 0);
}

const char *LinkClient::param_status_text() const {
  HeaterStatus s;
  this->read_status(s);
  if (!this->up_)
    snprintf(this->status_text_, sizeof(this->status_text_), "Heater not answering");
  else if (s.params_pending != 0)
    snprintf(this->status_text_, sizeof(this->status_text_), "%u change(s) not applied", unsigned(s.params_pending));
  else
    snprintf(this->status_text_, sizeof(this->status_text_), "In the heater");
  return this->status_text_;
}

}  // namespace flamekeeper
}  // namespace esphome
