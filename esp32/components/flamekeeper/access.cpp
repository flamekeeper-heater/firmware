#include "access.h"
#include "bytes.h"

#include <cstring>

namespace esphome {
namespace flamekeeper {

bool equal_ct(const uint8_t *a, size_t a_len, const uint8_t *b, size_t b_len) {
  if (a_len != b_len)
    return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < a_len; i++)
    diff |= uint8_t(a[i] ^ b[i]);
  return diff == 0;
}

uint32_t PasswordLockout::seconds_left(uint32_t now) const {
  if (!this->locked_)
    return 0;
  uint32_t elapsed = now - this->since_ms_;
  if (elapsed >= this->wait_ms_)
    return 0;
  return (this->wait_ms_ - elapsed + 999) / 1000;
}

void PasswordLockout::failed(uint32_t now) {
  if (this->fails_ < 255)
    this->fails_++;
  if (this->fails_ < FREE_TRIES)
    return;
  // 5th wrong one: 60 s; every further one doubles, up to an hour.
  uint32_t wait_s = FIRST_WAIT_S;
  for (uint8_t i = FREE_TRIES; i < this->fails_ && wait_s < MAX_WAIT_S; i++)
    wait_s *= 2;
  if (wait_s > MAX_WAIT_S)
    wait_s = MAX_WAIT_S;
  this->locked_ = true;
  this->since_ms_ = now;
  this->wait_ms_ = wait_s * 1000;
}

WifiKey *Access::fresh_(WifiKey *slot, KeyKind kind) {
  *slot = WifiKey{};
  slot->kind = kind;
  this->random_(slot->id, KEY_ID_SIZE);
  this->random_(slot->secret, KEY_SECRET_SIZE);
  slot->last_used = ++this->sequence_;
  return slot;
}

const WifiKey *Access::issue_app_key(const uint8_t owner[6]) {
  WifiKey *slot = nullptr;
  for (size_t i = 0; i < APP_KEYS; i++) {
    WifiKey &k = this->keys_[i];
    if (k.kind == KeyKind::APP && memcmp(k.owner, owner, 6) == 0) {
      slot = &k;  // this phone's previous key: replaced
      break;
    }
    if (k.kind == KeyKind::FREE && slot == nullptr)
      slot = &k;
  }
  if (slot == nullptr)
    return nullptr;
  this->fresh_(slot, KeyKind::APP);
  memcpy(slot->owner, owner, 6);
  this->dirty_ = true;
  return slot;
}

const WifiKey *Access::issue_browser_key(bool remember) {
  WifiKey *slot = nullptr;
  for (size_t i = APP_KEYS; i < SLOTS; i++) {
    WifiKey &k = this->keys_[i];
    if (k.kind == KeyKind::FREE) {
      slot = &k;
      break;
    }
    if (slot == nullptr || k.last_used < slot->last_used)
      slot = &k;  // least recently used so far
  }
  this->fresh_(slot, KeyKind::BROWSER);
  slot->remember = remember;
  this->dirty_ = true;
  return slot;
}

const WifiKey *Access::find_key(const uint8_t id[KEY_ID_SIZE]) const {
  for (const WifiKey &k : this->keys_)
    if (k.kind != KeyKind::FREE && equal_ct(k.id, KEY_ID_SIZE, id, KEY_ID_SIZE))
      return &k;
  return nullptr;
}

void Access::mark_used(const uint8_t id[KEY_ID_SIZE]) {
  for (WifiKey &k : this->keys_) {
    if (k.kind != KeyKind::FREE && equal_ct(k.id, KEY_ID_SIZE, id, KEY_ID_SIZE)) {
      k.last_used = ++this->sequence_;
      if (k.kind == KeyKind::BROWSER && k.remember)
        this->dirty_ = true;  // keep the order over a restart
      return;
    }
  }
}

bool Access::revoke(const uint8_t id[KEY_ID_SIZE]) {
  for (WifiKey &k : this->keys_) {
    if (k.kind != KeyKind::FREE && memcmp(k.id, id, KEY_ID_SIZE) == 0) {
      k = WifiKey{};
      this->dirty_ = true;
      return true;
    }
  }
  return false;
}

size_t Access::revoke_owner(const uint8_t owner[6]) {
  size_t n = 0;
  for (WifiKey &k : this->keys_) {
    if (k.kind == KeyKind::APP && memcmp(k.owner, owner, 6) == 0) {
      k = WifiKey{};
      n++;
    }
  }
  if (n != 0)
    this->dirty_ = true;
  return n;
}

size_t Access::revoke_browsers() {
  size_t n = 0;
  for (WifiKey &k : this->keys_) {
    if (k.kind == KeyKind::BROWSER) {
      k = WifiKey{};
      n++;
    }
  }
  if (n != 0)
    this->dirty_ = true;
  return n;
}

void Access::clear() {
  for (WifiKey &k : this->keys_)
    k = WifiKey{};
  memset(this->web_password_, 0, sizeof(this->web_password_));
  this->web_password_len_ = 0;
  this->lockout_.succeeded();
  this->dirty_ = true;
  this->web_password_dirty_ = true;
}

int Access::app_slot(const uint8_t id[KEY_ID_SIZE]) const {
  for (size_t i = 0; i < APP_KEYS; i++)
    if (this->keys_[i].kind == KeyKind::APP && memcmp(this->keys_[i].id, id, KEY_ID_SIZE) == 0)
      return int(i);
  return -1;
}

int Access::app_slot_of_owner(const uint8_t owner[6]) const {
  for (size_t i = 0; i < APP_KEYS; i++)
    if (this->keys_[i].kind == KeyKind::APP && memcmp(this->keys_[i].owner, owner, 6) == 0)
      return int(i);
  return -1;
}

uint8_t Access::count(KeyKind kind) const {
  uint8_t n = 0;
  for (const WifiKey &k : this->keys_)
    n += k.kind == kind ? 1 : 0;
  return n;
}

Outcome Access::change_web_password(const uint8_t *current, size_t current_len, const uint8_t *password,
                                    size_t len) {
  if (this->web_password_set() &&
      !equal_ct(current, current_len, this->web_password_, this->web_password_len_))
    return Outcome::refuse(Result::WRONG_PASSWORD, "the current web password is wrong");
  if (len != 0 && (len < WEB_PASSWORD_MIN || len > WEB_PASSWORD_MAX))
    return Outcome::refuse(Result::OUT_OF_RANGE, "the web password has 8 to 64 bytes");
  this->load_web_password(password, len);
  this->revoke_browsers();
  this->lockout_.succeeded();
  this->web_password_dirty_ = true;
  return Outcome::ok();
}

void Access::load_web_password(const uint8_t *password, size_t len) {
  memset(this->web_password_, 0, sizeof(this->web_password_));
  if (len > WEB_PASSWORD_MAX)
    len = 0;
  if (len != 0)
    memcpy(this->web_password_, password, len);
  this->web_password_len_ = len;
}

// Format 2: u8 version, u8 count, then per key: u8 kind (bits 0-3) and slot (bits 4-7), id, secret,
// owner, u32 last_used (LE). The slot is kept because it is the phone's place in the relay roster and
// what its push preferences are bound to. Format 1 (no slot: keys packed from the first slot) still
// loads, so keys saved by older builds survive the update.
size_t Access::serialize(uint8_t *out, size_t cap) const {
  if (cap < SERIALIZED_MAX)
    return 0;
  size_t n = 2;
  uint8_t count = 0;
  for (size_t i = 0; i < SLOTS; i++) {
    const WifiKey &k = this->keys_[i];
    if (k.kind == KeyKind::FREE || !k.remember)
      continue;
    out[n++] = uint8_t(uint8_t(k.kind) | (i << 4));
    memcpy(out + n, k.id, KEY_ID_SIZE);
    n += KEY_ID_SIZE;
    memcpy(out + n, k.secret, KEY_SECRET_SIZE);
    n += KEY_SECRET_SIZE;
    memcpy(out + n, k.owner, 6);
    n += 6;
    put_u32(out + n, k.last_used);
    n += 4;
    count++;
  }
  out[0] = 2;
  out[1] = count;
  return n;
}

bool Access::deserialize(const uint8_t *in, size_t len) {
  const uint8_t version = len >= 2 ? in[0] : 0;
  if ((version != 1 && version != 2) || in[1] > SLOTS || len < 2 + in[1] * ENTRY_SIZE)
    return false;
  for (WifiKey &k : this->keys_)
    k = WifiKey{};
  size_t next_app = 0, next_browser = APP_KEYS;
  const uint8_t *p = in + 2;
  for (uint8_t i = 0; i < in[1]; i++, p += ENTRY_SIZE) {
    const KeyKind kind = KeyKind(p[0] & 0x0F);
    const bool app = kind == KeyKind::APP, browser = kind == KeyKind::BROWSER;
    size_t at;
    if (version == 2)
      at = p[0] >> 4;
    else if (app)
      at = next_app++;
    else
      at = next_browser++;
    // Each kind stays in its own part of the table; a damaged or repeated slot is skipped.
    const bool fits = (app && at < APP_KEYS) || (browser && at >= APP_KEYS && at < SLOTS);
    if (!fits || this->keys_[at].kind != KeyKind::FREE)
      continue;
    WifiKey *slot = &this->keys_[at];
    slot->kind = kind;
    slot->remember = true;
    memcpy(slot->id, p + 1, KEY_ID_SIZE);
    memcpy(slot->secret, p + 1 + KEY_ID_SIZE, KEY_SECRET_SIZE);
    memcpy(slot->owner, p + 1 + KEY_ID_SIZE + KEY_SECRET_SIZE, 6);
    const uint8_t *u = p + 1 + KEY_ID_SIZE + KEY_SECRET_SIZE + 6;
    slot->last_used = get_u32(u);
    if (slot->last_used > this->sequence_)
      this->sequence_ = slot->last_used;
  }
  this->dirty_ = false;
  return true;
}

}  // namespace flamekeeper
}  // namespace esphome
