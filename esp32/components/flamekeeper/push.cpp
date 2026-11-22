#include "push.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "crypto.h"
#include "event_log.h"
#include "relay_client.h"
#include "bytes.h"

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

Outcome PushEngine::local_alerts(int slot, bool on) {
  if (slot < 0 || slot >= int(relay::SLOTS) || this->access_.app_key(size_t(slot)) == nullptr)
    return Outcome::refuse(Result::KEY_UNKNOWN, "this phone has no Wi-Fi key yet: ask for one first");
  this->local_[slot] = on;
  return Outcome::ok();
}

void PushEngine::bluetooth_gone(int slot) {
  if (slot >= 0 && slot < int(relay::SLOTS))
    this->local_[slot] = false;
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
      if ((p.flags & push::FLAG_ON) && (p.alerts & (1u << it.code)) && !this->local_[slot])
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
  put_u32(out + 31, s.timer_end_utc);
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

}  // namespace flamekeeper
}  // namespace esphome
