#include "wifi_session.h"

#include <cstring>

#include "crypto.h"
#include "mbedtls/ecjpake.h"
#include "mbedtls/sha256.h"

namespace esphome {
namespace flamekeeper {

using crypto::Bytes;

namespace {

int access_rng(void *p, unsigned char *out, size_t len) {
  static_cast<Access *>(p)->random(out, len);
  return 0;
}

const uint8_t APP_LABEL[] = {'f', 'k', '1', ' ', 'a', 'p', 'p'};
const uint8_t BOARD_LABEL[] = {'f', 'k', '1', ' ', 'b', 'o', 'a', 'r', 'd'};
const uint8_t APP_CONFIRM[] = {'f', 'k', '1', ' ', 'a', 'p', 'p', ' ', 'c', 'o', 'n', 'f', 'i', 'r', 'm'};
const uint8_t BOARD_CONFIRM[] = {'f', 'k', '1', ' ', 'b', 'o', 'a', 'r', 'd', ' ',
                                 'c', 'o', 'n', 'f', 'i', 'r', 'm'};

}  // namespace

// The J-PAKE exchange and the hash of every sign-in frame, alive only during a password sign-in.
struct WifiSession::Jpake {
  mbedtls_ecjpake_context ctx;
  mbedtls_sha256_context transcript;
  uint8_t k[32]{};
  bool remember{true};

  Jpake() {
    mbedtls_ecjpake_init(&this->ctx);
    mbedtls_sha256_init(&this->transcript);
    mbedtls_sha256_starts(&this->transcript, 0);
  }
  ~Jpake() {
    mbedtls_ecjpake_free(&this->ctx);
    mbedtls_sha256_free(&this->transcript);
    crypto::wipe(this->k, sizeof(this->k));
  }
  bool hash(uint8_t out[32]) {
    mbedtls_sha256_context copy;
    mbedtls_sha256_init(&copy);
    mbedtls_sha256_clone(&copy, &this->transcript);
    bool ok = mbedtls_sha256_finish(&copy, out) == 0;
    mbedtls_sha256_free(&copy);
    return ok;
  }
};

WifiSession::WifiSession(Access &access) : access_(access) {}

WifiSession::~WifiSession() {
  crypto::wipe(this->key_in_, sizeof(this->key_in_));
  crypto::wipe(this->key_out_, sizeof(this->key_out_));
}

void WifiSession::start(const Send &send) {
  this->access_.random(this->board_nonce_, sizeof(this->board_nonce_));
  uint8_t hello[1 + wire::NONCE_SIZE];
  hello[0] = wire::HELLO;
  memcpy(hello + 1, this->board_nonce_, wire::NONCE_SIZE);
  this->state_ = State::WAIT_SIGN_IN;
  send(hello, sizeof(hello));
}

void WifiSession::start_relay(const uint8_t key_id[KEY_ID_SIZE], const Send &send) {
  this->relay_ = true;
  memcpy(this->relay_key_id_, key_id, KEY_ID_SIZE);
  this->start(send);
}

WifiSession::Step WifiSession::close_(const char *why) {
  this->state_ = State::CLOSED;
  this->close_reason_ = why;
  this->jpake_.reset();
  crypto::wipe(this->key_in_, sizeof(this->key_in_));
  crypto::wipe(this->key_out_, sizeof(this->key_out_));
  return Step::CLOSE;
}

void WifiSession::transcript_(const uint8_t *frame, size_t len) {
  if (this->jpake_)
    mbedtls_sha256_update(&this->jpake_->transcript, frame, len);
}

void WifiSession::nonce_(const uint8_t salt[4], uint64_t counter, uint8_t out[12]) {
  memcpy(out, salt, 4);
  for (int i = 0; i < 8; i++)
    out[4 + i] = uint8_t(counter >> (8 * i));
}

WifiSession::Step WifiSession::receive(const uint8_t *frame, size_t len, uint32_t now, const Send &send,
                                       uint8_t *command, size_t *command_len, size_t command_cap) {
  if (len == 0)
    return this->close_("empty frame");
  const uint8_t type = frame[0];
  switch (this->state_) {
    case State::WAIT_SIGN_IN:
      if (type != wire::SIGN_IN || len < 2)
        return this->close_("expected a sign-in");
      if (frame[1] == 0)
        return this->sign_in_key_(frame + 2, len - 2, send);
      if (this->relay_)
        return this->close_("only a key sign-in over the relay");
      if (frame[1] == 1 && !this->password_done_) {
        this->jpake_.reset(new Jpake());
        uint8_t hello[1 + wire::NONCE_SIZE];
        hello[0] = wire::HELLO;
        memcpy(hello + 1, this->board_nonce_, wire::NONCE_SIZE);
        this->transcript_(hello, sizeof(hello));
        this->transcript_(frame, len);
        return this->sign_in_password_(frame + 2, len - 2, now, send);
      }
      return this->close_("unknown sign-in method");
    case State::WAIT_ROUND_TWO:
      if (type != wire::JPAKE_APP)
        return this->close_("expected J-PAKE round two");
      this->transcript_(frame, len);
      return this->round_two_(frame + 1, len - 1);
    case State::WAIT_CONFIRM:
      if (type != wire::CONFIRM)
        return this->close_("expected the key confirmation");
      return this->confirm_(frame + 1, len - 1, now, send);
    case State::SIGNED_IN: {
      if (type != wire::COMMAND && type != wire::UPDATE)
        return this->close_("only commands and update data after signing in");
      if (len < 1 + crypto::GCM_TAG_SIZE || command_cap < len)
        return this->close_("command frame too short or too long");
      uint8_t nonce[crypto::GCM_NONCE_SIZE];
      nonce_(this->salt_in_, this->count_in_++, nonce);
      if (!crypto::gcm_open(this->key_in_, nonce, frame, 1, frame + 1, len - 1, command))
        return this->close_("a frame did not verify");
      *command_len = len - 1 - crypto::GCM_TAG_SIZE;
      return type == wire::UPDATE ? Step::UPDATE_DATA : Step::COMMAND;
    }
    default:
      return this->close_("frame out of order");
  }
}

WifiSession::Step WifiSession::sign_in_key_(const uint8_t *p, size_t n, const Send &send) {
  if (n != KEY_ID_SIZE + wire::NONCE_SIZE + crypto::SHA256_SIZE)
    return this->close_("sign-in with a key: wrong length");
  const uint8_t *id = p;
  const uint8_t *app_nonce = p + KEY_ID_SIZE;
  const uint8_t *proof = app_nonce + wire::NONCE_SIZE;
  const WifiKey *key = this->access_.find_key(id);
  // Over the relay: only the phone key the relay matched (its slot), never a browser's.
  if (this->relay_ && (key == nullptr || key->kind != KeyKind::APP || !equal_ct(id, KEY_ID_SIZE, this->relay_key_id_, KEY_ID_SIZE)))
    key = nullptr;
  uint8_t secret[KEY_SECRET_SIZE];
  uint8_t mac[crypto::SHA256_SIZE];
  bool good = false;
  if (key != nullptr) {
    memcpy(secret, key->secret, KEY_SECRET_SIZE);
    this->key_kind_ = key->kind;
    this->key_remember_ = key->remember;
    good = crypto::hmac_sha256(secret, KEY_SECRET_SIZE,
                               {{APP_LABEL, sizeof(APP_LABEL)},
                                {this->board_nonce_, wire::NONCE_SIZE},
                                {app_nonce, wire::NONCE_SIZE}},
                               mac) &&
           equal_ct(mac, sizeof(mac), proof, crypto::SHA256_SIZE);
  }
  if (!good) {
    const uint8_t no[2] = {wire::SIGNED_IN, uint8_t(Result::KEY_UNKNOWN)};
    send(no, sizeof(no));
    crypto::wipe(secret, sizeof(secret));
    return this->close_("unknown Wi-Fi key");
  }

  uint8_t reply[2 + crypto::SHA256_SIZE] = {wire::SIGNED_IN, uint8_t(Result::OK)};
  uint8_t salt[2 * wire::NONCE_SIZE];
  memcpy(salt, this->board_nonce_, wire::NONCE_SIZE);
  memcpy(salt + wire::NONCE_SIZE, app_nonce, wire::NONCE_SIZE);
  uint8_t okm[40];
  bool ok = crypto::hmac_sha256(secret, KEY_SECRET_SIZE,
                                {{BOARD_LABEL, sizeof(BOARD_LABEL)},
                                 {this->board_nonce_, wire::NONCE_SIZE},
                                 {app_nonce, wire::NONCE_SIZE}},
                                reply + 2) &&
            crypto::hkdf_sha256(salt, sizeof(salt), secret, KEY_SECRET_SIZE, "fk1 session", okm, sizeof(okm));
  crypto::wipe(secret, sizeof(secret));
  if (!ok)
    return this->close_("crypto failure");
  memcpy(this->key_in_, okm, 16);
  memcpy(this->key_out_, okm + 16, 16);
  memcpy(this->salt_in_, okm + 32, 4);
  memcpy(this->salt_out_, okm + 36, 4);
  crypto::wipe(okm, sizeof(okm));
  memcpy(this->key_id_, id, KEY_ID_SIZE);
  this->access_.mark_used(id);  // only now: a failed sign-in changes nothing
  this->count_in_ = this->count_out_ = 0;
  this->state_ = State::SIGNED_IN;
  send(reply, sizeof(reply));
  return Step::CONTINUE;
}

WifiSession::Step WifiSession::sign_in_password_(const uint8_t *p, size_t n, uint32_t now, const Send &send) {
  if (!this->access_.web_password_set()) {
    const uint8_t no[2] = {wire::SIGNED_IN, uint8_t(Result::NO_WEB_PASSWORD)};
    send(no, sizeof(no));
    return this->close_("no web password is set");
  }
  uint32_t wait = this->access_.lockout().seconds_left(now);
  if (wait == 0)
    wait = this->access_.lockout().pace_left_s(now);  // another password sign-in just started
  if (wait != 0) {
    uint16_t w = wait > 0xFFFF ? 0xFFFF : uint16_t(wait);
    const uint8_t no[4] = {wire::SIGNED_IN, uint8_t(Result::LOCKED_OUT), uint8_t(w), uint8_t(w >> 8)};
    send(no, sizeof(no));
    return this->close_("locked out after wrong web passwords");
  }
  // remember (1), round one (the rest)
  if (n < 2 || p[0] > 1)
    return this->close_("sign-in with the web password: bad layout");
  this->access_.lockout().started(now);
  this->jpake_->remember = p[0] == 1;
  const uint8_t *round_one = p + 1;
  size_t round_one_len = n - 1;

  mbedtls_ecjpake_context *ctx = &this->jpake_->ctx;
  if (mbedtls_ecjpake_setup(ctx, MBEDTLS_ECJPAKE_SERVER, MBEDTLS_MD_SHA256, MBEDTLS_ECP_DP_SECP256R1,
                            this->access_.web_password(), this->access_.web_password_len()) != 0 ||
      mbedtls_ecjpake_read_round_one(ctx, round_one, round_one_len) != 0)
    return this->close_("J-PAKE round one did not verify");

  uint8_t out[wire::FRAME_MAX];
  out[0] = wire::JPAKE_BOARD;
  size_t r1 = 0, r2 = 0;
  if (mbedtls_ecjpake_write_round_one(ctx, out + 3, sizeof(out) - 3, &r1, access_rng, &this->access_) != 0 ||
      mbedtls_ecjpake_write_round_two(ctx, out + 3 + r1, sizeof(out) - 3 - r1, &r2, access_rng, &this->access_) != 0)
    return this->close_("J-PAKE failed");
  out[1] = uint8_t(r1);
  out[2] = uint8_t(r1 >> 8);
  size_t len = 3 + r1 + r2;
  this->transcript_(out, len);
  this->state_ = State::WAIT_ROUND_TWO;
  send(out, len);
  return Step::CONTINUE;
}

WifiSession::Step WifiSession::round_two_(const uint8_t *p, size_t n) {
  uint8_t pms[32];
  size_t olen = 0;
  if (mbedtls_ecjpake_read_round_two(&this->jpake_->ctx, p, n) != 0 ||
      mbedtls_ecjpake_derive_secret(&this->jpake_->ctx, pms, sizeof(pms), &olen, access_rng, &this->access_) != 0 ||
      olen != sizeof(pms))
    return this->close_("J-PAKE round two did not verify");
  bool ok = crypto::hkdf_sha256(this->board_nonce_, wire::NONCE_SIZE, pms, sizeof(pms), "fk1 password",
                                this->jpake_->k, sizeof(this->jpake_->k));
  crypto::wipe(pms, sizeof(pms));
  if (!ok)
    return this->close_("crypto failure");
  this->state_ = State::WAIT_CONFIRM;
  return Step::CONTINUE;
}

WifiSession::Step WifiSession::confirm_(const uint8_t *p, size_t n, uint32_t now, const Send &send) {
  uint8_t h[crypto::SHA256_SIZE], mac[crypto::SHA256_SIZE];
  Jpake &j = *this->jpake_;
  if (n != crypto::SHA256_SIZE || !j.hash(h) ||
      !crypto::hmac_sha256(j.k, sizeof(j.k), {{APP_CONFIRM, sizeof(APP_CONFIRM)}, {h, sizeof(h)}}, mac))
    return this->close_("bad key confirmation");
  if (!equal_ct(mac, sizeof(mac), p, n)) {
    // Both sides ran J-PAKE with different passwords: a wrong guess.
    this->access_.lockout().failed(now);
    const uint8_t no[2] = {wire::SIGNED_IN, uint8_t(Result::WRONG_PASSWORD)};
    send(no, sizeof(no));
    return this->close_("wrong web password");
  }
  this->access_.lockout().succeeded();

  const WifiKey *key = this->access_.issue_browser_key(j.remember);
  if (!j.remember) {
    // A key for this tab only goes when the connection does, signed in with it or not.
    memcpy(this->tab_key_id_, key->id, KEY_ID_SIZE);
    this->tab_key_ = true;
  }
  uint8_t plain[KEY_ID_SIZE + KEY_SECRET_SIZE];
  memcpy(plain, key->id, KEY_ID_SIZE);
  memcpy(plain + KEY_ID_SIZE, key->secret, KEY_SECRET_SIZE);
  uint8_t reply[2 + crypto::SHA256_SIZE + sizeof(plain) + crypto::GCM_TAG_SIZE] = {wire::SIGNED_IN,
                                                                                   uint8_t(Result::OK)};
  static const uint8_t ZERO_NONCE[crypto::GCM_NONCE_SIZE] = {};  // K encrypts this one message only
  bool ok = crypto::hmac_sha256(j.k, sizeof(j.k), {{BOARD_CONFIRM, sizeof(BOARD_CONFIRM)}, {h, sizeof(h)}},
                                reply + 2) &&
            crypto::gcm_seal(j.k, ZERO_NONCE, nullptr, 0, plain, sizeof(plain), reply + 2 + crypto::SHA256_SIZE);
  crypto::wipe(plain, sizeof(plain));
  this->jpake_.reset();
  if (!ok)
    return this->close_("crypto failure");
  this->password_done_ = true;
  this->state_ = State::WAIT_SIGN_IN;
  send(reply, sizeof(reply));
  return Step::CONTINUE;
}

size_t WifiSession::seal(uint8_t type, const uint8_t *record, size_t len, uint8_t *out, size_t out_cap) {
  if (this->state_ != State::SIGNED_IN || out_cap < len + wire::SEAL_OVERHEAD)
    return 0;
  out[0] = type;
  uint8_t nonce[crypto::GCM_NONCE_SIZE];
  nonce_(this->salt_out_, this->count_out_++, nonce);
  if (!crypto::gcm_seal(this->key_out_, nonce, out, 1, record, len, out + 1))
    return 0;
  return len + wire::SEAL_OVERHEAD;
}

}  // namespace flamekeeper
}  // namespace esphome
