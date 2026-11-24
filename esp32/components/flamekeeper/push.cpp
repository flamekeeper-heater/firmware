#include "push.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "crypto.h"
#include "event_log.h"
#include "relay_client.h"
#include "bytes.h"
#include <cstdlib>

namespace esphome {
namespace flamekeeper {

namespace push {
uint8_t alert_class(uint8_t bit) {
  switch (bit) {
    case 10: return 3;                                           // CO alarm
    case 11: case 7: case 1: case 2: case 3: case 6: return 2;   // stopped, cannot start, frost blind
    case 0: case 5: return 1;                                    // fuel low, no clock
    default: return 0;                                           // reminders (8, 9, 12) and bit 4
  }
}
}  // namespace push

namespace {


}  // namespace

PushEngine::PushEngine(Controller &c, Automation &a, Access &access, RelayClient &relay, relay::Random random)
    : c_(c), a_(a), access_(access), relay_(relay), random_(std::move(random)) {
  this->saved_.check = SAVED_CHECK;
  // The latest text of each alert (event 08), for the payload.
  this->c_.on_event([this](Event e, Result, uint16_t id, const char *text) {
    if (e != Event::ALERT || id >= 16 || text == nullptr)
      return;
    std::strncpy(this->texts_[id], text, push::TEXT_MAX);
    this->texts_[id][push::TEXT_MAX] = '\0';
  });
}

void PushEngine::load(const Saved &in) {
  if (in.check == SAVED_CHECK)
    this->saved_ = in;
}

void PushEngine::save_() {
  if (this->save_cb_)
    this->save_cb_(this->saved_);
}

PushPrefs PushEngine::prefs_(int slot) const {
  PushPrefs p;
  const WifiKey *k = this->access_.app_key(size_t(slot));
  const Saved::Slot &s = this->saved_.slots[slot];
  // Preferences belong to the key they were set for: a new key starts from the defaults.
  if (k != nullptr && s.used && std::memcmp(s.key_id, k->id, KEY_ID_SIZE) == 0) {
    p.alerts = s.alerts;
    p.events = s.events;
    p.flags = s.flags;
  }
  return p;
}

bool PushEngine::prefs_get(int slot, PushPrefs &out) const {
  if (slot < 0 || slot >= int(relay::SLOTS) || this->access_.app_key(size_t(slot)) == nullptr)
    return false;
  out = this->prefs_(slot);
  out.alerts |= uint16_t(1u << push::CO_BIT);  // the CO alarm is always sent: never shown off
  return true;
}

Outcome PushEngine::prefs_set(int slot, const PushPrefs &p) {
  const WifiKey *k = slot >= 0 && slot < int(relay::SLOTS) ? this->access_.app_key(size_t(slot)) : nullptr;
  if (k == nullptr)
    return Outcome::refuse(Result::KEY_UNKNOWN, "this phone has no Wi-Fi key yet: ask for one first");
  if (p.flags & ~push::FLAGS_KNOWN)
    return Outcome::refuse(Result::OUT_OF_RANGE, "flags bits 4-7 are 0");
  Saved::Slot &s = this->saved_.slots[slot];
  std::memcpy(s.key_id, k->id, KEY_ID_SIZE);
  s.alerts = p.alerts;
  s.events = p.events;
  s.flags = p.flags;
  s.used = 1;
  this->save_();
  return Outcome::ok();
}

// ---------------------------------------------------------------- what to push

void PushEngine::add_(const Item &it, uint32_t now) {
  uint8_t cls = 0;
  if (it.what == 0) {
    cls = push::alert_class(it.code);
    // Hold-off against flapping (never for the CO alarm or a CO sensor fault).
    if (it.code != 10 && it.code != 11 && this->pushed_once_[it.code]) {
      const uint32_t hold = it.code == 4 ? HOLD_BIT4_MS : HOLD_MS;
      if (uint32_t(now - this->last_push_ms_[it.code]) < hold)
        return;
    }
  }
  if (this->pending_.empty())
    this->window_start_ms_ = now;
  this->pending_.push_back({it, cls});
}

void PushEngine::read_log_(uint32_t now) {
  const EventLog &log = this->a_.log();
  if (log.newest() <= this->last_seq_)
    return;
  LogRecord recs[8];
  size_t n = log.read(this->last_seq_ + 1, recs, 8);
  for (size_t i = 0; i < n; i++) {
    const LogRecord &r = recs[i];
    this->last_seq_ = r.seq;
    Item it{1, 0, r.source, r.seq, r.utc};
    const Source src = Source(r.source);
    switch (LogKind(r.kind)) {
      case LogKind::ALERT:
        if (r.a >= 16)
          continue;
        it.what = 0;
        it.code = uint8_t(r.a);
        it.source = 0;
        break;
      case LogKind::RUN_STARTED:
        it.code = src == Source::FROST   ? push::EV_FROST_STARTED
                  : src == Source::TIMER ? push::EV_TIMER_STARTED
                                         : push::EV_STARTED_BY_OTHER;
        break;
      case LogKind::STOP_ASKED:
        if (src == Source::NONE)
          continue;  // the board's own stop (low fuel, low battery): an alert says why
        it.code = src == Source::FROST   ? push::EV_FROST_ENDED
                  : src == Source::TIMER ? push::EV_TIMER_ENDED
                                         : push::EV_STOPPED_BY_OTHER;
        break;
      case LogKind::BOARD_STARTED:
        it.code = push::EV_BOARD_RESTARTED;
        break;
      default:
        continue;
    }
    this->add_(it, now);
  }
}

bool PushEngine::wants_(int slot, const std::vector<Pending> &items) const {
  const PushPrefs p = this->prefs_(slot);
  for (const Pending &pi : items) {
    const Item &it = pi.item;
    if (it.what == 0) {
      if (it.code == push::CO_BIT)
        return true;  // the CO alarm reaches every phone, always
      if ((p.flags & push::FLAG_ON) && (p.alerts & (1u << it.code)))
        return true;
    } else if ((p.flags & push::FLAG_ON) && (p.events & (1u << it.code))) {
      return true;
    }
  }
  return false;
}

size_t PushEngine::encode_payload(const Item &lead, uint16_t other_alerts, uint16_t other_events, uint8_t flags,
                                  const Status &s, const char *text, uint8_t *out, size_t cap) {
  const size_t tl = text != nullptr ? std::min(std::strlen(text), push::TEXT_MAX) : 0;
  if (cap < push::PAYLOAD_FIXED + tl)
    return 0;
  out[0] = 1;  // version
  put_u32(out + 1, lead.seq);
  put_u32(out + 5, lead.utc);
  out[9] = lead.what == 0 ? push::alert_class(lead.code) : 0;
  out[10] = lead.what;
  out[11] = lead.code;
  out[12] = lead.source;
  put_u16(out + 13, s.alerts);
  put_u16(out + 15, other_alerts);
  put_u16(out + 17, other_events);
  out[19] = flags;
  out[20] = s.fuel_left;
  put_fixed_u16(out + 21, s.supply_v, 100.0f);
  out[23] = uint8_t(s.fault);
  out[24] = s.panel_fault;
  put_fixed_u16(out + 25, s.co_ppm, 1.0f);
  put_fixed_i16(out + 27, s.room_c, 10.0f);
  put_fixed_i16(out + 29, s.target_c, 10.0f);
  put_u32(out + 31, s.run_end_utc);
  out[35] = uint8_t(tl);
  std::memcpy(out + 36, text, tl);
  return push::PAYLOAD_FIXED + tl;
}

size_t PushEngine::seal_blob(const uint8_t key[relay::PUSH_KEY_SIZE], const uint8_t nonce[push::NONCE_SIZE],
                             const uint8_t board_id[relay::ID_SIZE], uint8_t cls, const uint8_t *payload, size_t len,
                             uint8_t *out, size_t cap) {
  const size_t total = push::NONCE_SIZE + len + push::TAG_SIZE;
  if (cap < total)
    return 0;
  uint8_t ad[relay::ID_SIZE + 1];
  std::memcpy(ad, board_id, relay::ID_SIZE);
  ad[relay::ID_SIZE] = cls;
  std::memcpy(out, nonce, push::NONCE_SIZE);
  // crypto::gcm_seal takes a 16-byte key: P is 16 bytes.
  if (!crypto::gcm_seal(key, nonce, ad, sizeof(ad), payload, len, out + push::NONCE_SIZE))
    return 0;
  return total;
}

bool PushEngine::send_(const std::vector<Pending> &items, uint8_t flags, uint32_t now) {
  if (items.empty())
    return true;
  // The lead: the most urgent, the first of equals.
  size_t lead = 0;
  for (size_t i = 1; i < items.size(); i++)
    if (items[i].cls > items[lead].cls)
      lead = i;
  uint16_t other_alerts = 0, other_events = 0;
  for (size_t i = 0; i < items.size(); i++) {
    if (i == lead)
      continue;
    if (items[i].item.what == 0)
      other_alerts |= uint16_t(1u << items[i].item.code);
    else
      other_events |= uint16_t(1u << items[i].item.code);
  }
  const Item &li = items[lead].item;
  const uint8_t cls = items[lead].cls;
  uint8_t payload[push::PAYLOAD_FIXED + push::TEXT_MAX];
  const char *text = li.what == 0 ? this->texts_[li.code] : nullptr;
  const size_t plen = encode_payload(li, other_alerts, other_events, flags, this->c_.status(), text, payload, sizeof(payload));
  if (plen == 0)
    return false;

  std::vector<uint8_t> m = {0, relay::op::PUSH, cls, 0};
  uint8_t n = 0;
  for (int slot = 0; slot < int(relay::SLOTS); slot++) {
    const WifiKey *k = this->access_.app_key(size_t(slot));
    if (k == nullptr || !(this->relay_.push_mask() & (1u << slot)) || !this->wants_(slot, items))
      continue;
    uint8_t key[relay::PUSH_KEY_SIZE];
    uint8_t nonce[push::NONCE_SIZE];
    uint8_t blob[push::NONCE_SIZE + sizeof(payload) + push::TAG_SIZE];
    this->random_(nonce, sizeof(nonce));
    size_t bl = 0;
    if (relay::push_key(k->secret, KEY_SECRET_SIZE, key))
      bl = seal_blob(key, nonce, this->relay_.identity().id(), cls, payload, plen, blob, sizeof(blob));
    crypto::wipe(key, sizeof(key));
    if (bl == 0 || bl > 255)
      continue;
    m.push_back(uint8_t(slot));
    m.push_back(uint8_t(bl));
    m.insert(m.end(), blob, blob + bl);
    n++;
  }
  // Delivered as far as the board goes: hold-off and CO timing count from now.
  for (const Pending &pi : items) {
    if (pi.item.what != 0)
      continue;
    this->pushed_once_[pi.item.code] = true;
    this->last_push_ms_[pi.item.code] = now;
  }
  if (n == 0)
    return true;  // nobody wants it: nothing to send
  m[3] = n;
  if (!this->relay_.send_control(m.data(), m.size()))
    return false;
  this->sent_++;
  return true;
}

void PushEngine::loop(uint32_t now) {
  if (!this->begun_) {
    this->begun_ = true;
    this->last_seq_ = this->a_.log().newest();
  }
  this->read_log_(now);

  // The relay is ready once signed in and its WANT has come (or a few seconds went by without it).
  const bool up = this->relay_.authed();
  if (up && !this->relay_was_up_)
    this->relay_up_ms_ = now;
  this->relay_was_up_ = up;
  const bool ready = up && (this->relay_.push_mask() != 0 || uint32_t(now - this->relay_up_ms_) >= WANT_WAIT_MS);

  this->live_loop_(now, ready);

  // A coalescing window has closed: send, or hold the alerts until the relay is back.
  if (!this->pending_.empty() && uint32_t(now - this->window_start_ms_) >= COALESCE_MS) {
    std::vector<Pending> items;
    items.swap(this->pending_);
    for (const Pending &pi : items) {
      if (pi.item.what == 0 && pi.item.code == push::CO_BIT && !this->co_active_) {
        this->co_active_ = true;
        this->co_first_ms_ = now;
        this->co_repeats_ = 0;
        this->co_item_ = pi.item;
        this->app_seen_ = false;
      }
    }
    if (!(ready && this->send_(items, 0, now))) {
      for (const Pending &pi : items) {
        if (pi.item.what != 0)
          continue;  // events are not kept
        auto it = std::find_if(this->held_.begin(), this->held_.end(),
                               [&](const Pending &h) { return h.item.code == pi.item.code; });
        if (it != this->held_.end())
          *it = pi;
        else
          this->held_.push_back(pi);
      }
    }
  }

  // Held alerts that are still on, once the relay is back: flagged delayed, with their own time.
  if (ready && !this->held_.empty()) {
    std::vector<Pending> still;
    for (const Pending &h : this->held_)
      if (this->c_.status().alerts & (1u << h.item.code))
        still.push_back(h);
    if (still.empty() || this->send_(still, 0x01, now))
      this->held_.clear();
  }

  // The CO alarm again after 5 and 10 minutes while it lasts, unless someone signed in meanwhile.
  if (this->co_active_) {
    if (!(this->c_.status().alerts & (1u << push::CO_BIT)) || this->app_seen_) {
      this->co_active_ = false;
    } else if (this->co_repeats_ < CO_REPEATS &&
               uint32_t(now - this->co_first_ms_) >= uint32_t(this->co_repeats_ + 1) * CO_REPEAT_MS) {
      if (ready) {
        std::vector<Pending> again = {{this->co_item_, 3}};
        this->co_repeats_++;
        this->send_(again, uint8_t(this->co_repeats_ << 1), now);
      }
    }
  }
}

// ---------------------------------------------------------------- the live card

size_t PushEngine::encode_live_record(uint32_t seq, uint32_t utc, const LiveContent &c,
                                      uint8_t out[push::LIVE_RECORD_SIZE]) {
  out[0] = 1;  // version
  put_u32(out + 1, seq);
  put_u32(out + 5, utc);
  out[9] = c.state;
  out[10] = c.kind;
  out[11] = c.source;
  out[12] = c.fault;
  put_u16(out + 13, c.alerts);
  put_u16(out + 15, uint16_t(c.room_c10));
  put_u16(out + 17, uint16_t(c.target_c10));
  out[19] = c.power;
  put_u32(out + 20, c.run_end_utc);
  put_u32(out + 24, c.burnoff_end_utc);
  put_u32(out + 28, c.started_utc);
  out[32] = c.fuel_left;
  return push::LIVE_RECORD_SIZE;
}

size_t PushEngine::encode_live_header(uint8_t event, uint8_t flags, uint16_t stale_s, uint16_t dismissal_s,
                                      uint8_t starter, uint32_t run_id, uint8_t slots,
                                      uint8_t out[LIVE_HEADER_SIZE]) {
  out[0] = 0;
  out[1] = relay::op::LIVE;
  out[2] = event;
  out[3] = flags;
  put_u16(out + 4, stale_s);
  put_u16(out + 6, dismissal_s);
  out[8] = starter;
  put_u32(out + 9, run_id);
  out[13] = slots;
  return LIVE_HEADER_SIZE;
}

namespace {
int16_t c10(float v) {
  if (!std::isfinite(v))
    return INT16_MIN;
  const long x = std::lround(v * 10.0f);
  return int16_t(x < -32767 ? -32767 : (x > 32767 ? 32767 : x));
}
bool card_state(HeaterState s) {
  return s == HeaterState::STARTING || s == HeaterState::HEATING || s == HeaterState::FAN_ONLY;
}
}  // namespace

PushEngine::LiveContent PushEngine::live_content_(uint32_t now, const Status &s) {
  LiveContent c;
  const uint32_t utc = this->a_.utc(now);
  c.state = uint8_t(s.state);
  if (s.burnoff_left_min > 0)
    c.kind = 3;
  else if (s.mode == Mode::FAN)
    c.kind = 2;
  else if (s.mode == Mode::HEAT)
    c.kind = s.heat_mode == HeatMode::TEMPERATURE ? 0 : 1;
  else
    c.kind = this->live_last_.kind;  // stopping: what it was
  c.source = uint8_t(s.run_source != Source::NONE ? s.run_source : s.ending_source);
  if (c.source == 0)
    c.source = this->live_last_.source;  // off: who started it
  c.fault = uint8_t(s.fault);
  c.alerts = s.alerts;
  c.room_c10 = c10(s.room_c);
  c.target_c10 = c10(s.target_c);
  c.power = s.power;
  c.run_end_utc = s.run_end_utc;
  // Burn-off end and run start: fixed once known (recomputing them would move by a second).
  if (s.burnoff_left_min == 0)
    this->live_burnoff_end_ = 0;
  else if (this->live_burnoff_end_ == 0 && utc != 0)
    this->live_burnoff_end_ = utc + uint32_t(s.burnoff_left_min) * 60;
  c.burnoff_end_utc = this->live_burnoff_end_;
  c.started_utc = this->live_last_.started_utc != 0 ? this->live_last_.started_utc
                                                    : (utc != 0 && utc > s.run_s ? utc - s.run_s : 0);
  c.fuel_left = s.fuel_left;
  return c;
}

bool PushEngine::live_wants_(int slot, uint8_t source) const {
  const PushPrefs p = this->prefs_(slot);
  if (!(p.flags & push::FLAG_LIVE))
    return false;
  if (slot == int(this->live_starter_))
    return true;  // the phone that started the run
  const Source src = Source(source);
  return (p.flags & (is_automatic(src) ? push::FLAG_LIVE_BOARD : push::FLAG_LIVE_OTHERS)) != 0;
}

uint32_t PushEngine::live_next_seq_() {
  if (!this->live_seq_loaded_) {
    this->live_seq_loaded_ = true;
    this->live_seq_ = this->saved_.live_floor > 0 ? this->saved_.live_floor : 1;
  }
  if (this->live_seq_ >= this->saved_.live_floor) {
    this->saved_.live_floor = this->live_seq_ + 256;  // saved once per 256 frames
    this->save_();
  }
  return this->live_seq_++;
}

bool PushEngine::live_send_(uint8_t event, uint8_t flags, uint16_t dismissal_s, uint32_t run_id, const LiveContent &c,
                            uint32_t now) {
  // One plain record for every phone it is for (the relay reads it to drive the cards).
  const uint8_t masks = uint8_t(this->relay_.live_mask() | this->relay_.startable_mask());
  uint8_t slots = 0;
  for (int slot = 0; slot < int(relay::SLOTS); slot++)
    if ((masks & (1u << slot)) && this->access_.app_key(size_t(slot)) != nullptr && this->live_wants_(slot, c.source))
      slots |= uint8_t(1u << slot);
  if (slots != 0) {
    uint8_t m[LIVE_HEADER_SIZE + push::LIVE_RECORD_SIZE];
    encode_live_header(event, flags, push::LIVE_STALE_S, dismissal_s, this->live_starter_, run_id, slots, m);
    encode_live_record(this->live_next_seq_(), this->a_.utc(now), c, m + LIVE_HEADER_SIZE);
    if (!this->relay_.send_control(m, sizeof(m)))
      return false;
    this->live_sent_++;
  }
  this->live_sent_ms_ = now == 0 ? 1 : now;
  this->live_last_ = c;
  this->live_dirty_ = this->live_important_ = this->live_prompt_ = false;
  return true;
}

void PushEngine::live_loop_(uint32_t now, bool ready) {
  const Status &s = this->c_.status();
  const bool reconnected = ready && !this->live_was_ready_;
  this->live_was_ready_ = ready;

  // An end owed from while the relay was down.
  if (this->live_end_owed_ && ready && this->live_run_ == 0) {
    if (this->live_send_(push::LIVE_END, this->live_end_flags_, this->live_end_dismiss_, this->live_end_run_,
                         this->live_end_, now))
      this->live_end_owed_ = false;
  }

  if (this->live_run_ != 0 && (s.run_id != this->live_run_ || s.state == HeaterState::OFF)) {
    // The run is over: the card's end, with its final content.
    const LiveContent c = this->live_content_(now, s);
    const bool bad = s.fault != Fault::NONE || (s.alerts & push::LIVE_FAULT_ALERTS);
    const uint8_t flags = uint8_t(push::LIVE_IMPORTANT | (bad ? push::LIVE_FAULT_END : 0));
    const uint16_t dismiss = bad ? push::LIVE_DISMISS_FAULT_S : push::LIVE_DISMISS_S;
    if (this->live_started_ && !(ready && this->live_send_(push::LIVE_END, flags, dismiss, this->live_run_, c, now))) {
      this->live_end_owed_ = true;
      this->live_end_run_ = this->live_run_;
      this->live_end_ = c;
      this->live_end_flags_ = flags;
      this->live_end_dismiss_ = dismiss;
    }
    this->live_run_ = 0;
    this->live_started_ = false;
    this->live_last_ = LiveContent{};
    this->live_burnoff_end_ = 0;
  }

  if (this->live_run_ == 0) {
    // A run the card follows begins when the heater leaves off for starting, heating or fan only.
    if (s.run_id == 0 || !card_state(s.state))
      return;
    this->live_run_ = s.run_id;
    this->live_starter_ = s.starter_slot;
    this->live_started_ = false;
    this->live_end_owed_ = false;  // a new card replaces an old card's owed end
    this->live_last_ = LiveContent{};
    this->live_last_ = this->live_content_(now, s);
    this->live_dirty_ = this->live_important_ = true;
  } else {
    const LiveContent c = this->live_content_(now, s);
    const LiveContent &l = this->live_last_;
    const bool room_moved = (c.room_c10 == INT16_MIN) != (l.room_c10 == INT16_MIN) ||
                            (c.room_c10 != INT16_MIN && std::abs(int(c.room_c10) - int(l.room_c10)) >= LIVE_ROOM_C10);
    if (c.state != l.state || (c.alerts & ~l.alerts) || (c.fault != l.fault && c.fault != 0) || reconnected)
      this->live_important_ = true;  // a reconnect: the relay may have lost the card meanwhile
    // What the user just asked for shows soon, without taking a push's priority.
    if (this->live_important_ || c.kind != l.kind || c.target_c10 != l.target_c10 || c.power != l.power ||
        c.run_end_utc != l.run_end_utc)
      this->live_prompt_ = true;
    if (this->live_prompt_ || c.source != l.source || c.fault != l.fault || c.alerts != l.alerts || room_moved)
      this->live_dirty_ = true;
  }

  if (!ready)
    return;
  const uint32_t gap = uint32_t(now - this->live_sent_ms_);
  const bool prompt = !this->live_started_ || this->live_prompt_;
  const bool may = this->live_sent_ms_ == 0 || gap >= (prompt ? LIVE_PROMPT_GAP_MS : LIVE_GAP_MS);
  const LiveContent c = this->live_content_(now, s);
  if (!this->live_started_) {
    if (may && this->live_send_(push::LIVE_START, push::LIVE_IMPORTANT, 0, this->live_run_, c, now))
      this->live_started_ = true;
  } else if (may && (this->live_dirty_ || gap >= LIVE_HEARTBEAT_MS)) {
    this->live_send_(push::LIVE_UPDATE, this->live_important_ ? push::LIVE_IMPORTANT : 0, 0, this->live_run_, c, now);
  }
}

}  // namespace flamekeeper
}  // namespace esphome
