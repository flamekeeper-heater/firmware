// The Wi-Fi sign-in and frames (PROTOCOL.md), board side against an app-side
// client written from the protocol text: key sign-in, web password sign-in over EC J-PAKE, the
// lockout, encrypted frames, and the attacks the design is meant to stop.
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

#include "../crypto.h"
#include "../wifi_session.h"
#include "fake_heater.h"
#include "mbedtls/ecjpake.h"
#include "mbedtls/sha256.h"

namespace w = fk::wire;
namespace cr = fk::crypto;
using Bytes = std::vector<uint8_t>;

static uint32_t g_x = 0x12345678;
static void test_rng(uint8_t *out, size_t len) {
  for (size_t i = 0; i < len; i++) {
    g_x = g_x * 1664525u + 1013904223u;
    out[i] = uint8_t(g_x >> 24);
  }
}
static int mbed_rng(void *, unsigned char *out, size_t len) {
  test_rng(out, len);
  return 0;
}

// The app's side, as an app would implement it.
struct Client {
  Bytes board_nonce;
  Bytes app_nonce = Bytes(16);
  uint8_t k_out[16]{}, k_in[16]{}, s_out[4]{}, s_in[4]{};
  uint64_t c_out = 0, c_in = 0;
  // J-PAKE
  mbedtls_ecjpake_context jp;
  mbedtls_sha256_context th;
  uint8_t k[32]{};
  Bytes key_id, key_secret;

  Client() {
    mbedtls_ecjpake_init(&jp);
    mbedtls_sha256_init(&th);
    mbedtls_sha256_starts(&th, 0);
  }
  ~Client() {
    mbedtls_ecjpake_free(&jp);
    mbedtls_sha256_free(&th);
  }
  void hello(const Bytes &f) {
    CHECK(f.size() == 17 && f[0] == w::HELLO);
    board_nonce.assign(f.begin() + 1, f.end());
    mbedtls_sha256_update(&th, f.data(), f.size());
  }
  Bytes sign_in_key(const Bytes &id, const Bytes &secret) {
    test_rng(app_nonce.data(), 16);
    Bytes f = {w::SIGN_IN, 0};
    f.insert(f.end(), id.begin(), id.end());
    f.insert(f.end(), app_nonce.begin(), app_nonce.end());
    uint8_t mac[32];
    const uint8_t label[] = {'f', 'k', '1', ' ', 'a', 'p', 'p'};
    cr::hmac_sha256(secret.data(), 32, {{label, 7}, {board_nonce.data(), 16}, {app_nonce.data(), 16}}, mac);
    f.insert(f.end(), mac, mac + 32);
    key_id = id;
    key_secret = secret;
    return f;
  }
  bool signed_in(const Bytes &f) {
    if (f.size() != 34 || f[0] != w::SIGNED_IN || f[1] != 0)
      return false;
    uint8_t mac[32];
    const uint8_t label[] = {'f', 'k', '1', ' ', 'b', 'o', 'a', 'r', 'd'};
    cr::hmac_sha256(key_secret.data(), 32, {{label, 9}, {board_nonce.data(), 16}, {app_nonce.data(), 16}}, mac);
    if (std::memcmp(mac, f.data() + 2, 32) != 0)
      return false;  // not the board that issued the key
    Bytes salt = board_nonce;
    salt.insert(salt.end(), app_nonce.begin(), app_nonce.end());
    uint8_t okm[40];
    cr::hkdf_sha256(salt.data(), 32, key_secret.data(), 32, "fk1 session", okm, 40);
    std::memcpy(k_out, okm, 16);
    std::memcpy(k_in, okm + 16, 16);
    std::memcpy(s_out, okm + 32, 4);
    std::memcpy(s_in, okm + 36, 4);
    c_out = c_in = 0;
    return true;
  }
  Bytes sign_in_password(const char *password, bool remember) {
    mbedtls_ecjpake_setup(&jp, MBEDTLS_ECJPAKE_CLIENT, MBEDTLS_MD_SHA256, MBEDTLS_ECP_DP_SECP256R1,
                          (const unsigned char *) password, strlen(password));
    Bytes f = {w::SIGN_IN, 1, uint8_t(remember ? 1 : 0)};
    uint8_t r1[512];
    size_t n = 0;
    CHECK(mbedtls_ecjpake_write_round_one(&jp, r1, sizeof(r1), &n, mbed_rng, nullptr) == 0);
    f.insert(f.end(), r1, r1 + n);
    mbedtls_sha256_update(&th, f.data(), f.size());
    return f;
  }
  Bytes round_two(const Bytes &f) {
    CHECK(f.size() > 3 && f[0] == w::JPAKE_BOARD);
    mbedtls_sha256_update(&th, f.data(), f.size());
    size_t r1 = f[1] | (f[2] << 8);
    CHECK(mbedtls_ecjpake_read_round_one(&jp, f.data() + 3, r1) == 0);
    CHECK(mbedtls_ecjpake_read_round_two(&jp, f.data() + 3 + r1, f.size() - 3 - r1) == 0);
    uint8_t r2[512];
    size_t n = 0;
    CHECK(mbedtls_ecjpake_write_round_two(&jp, r2, sizeof(r2), &n, mbed_rng, nullptr) == 0);
    Bytes out = {w::JPAKE_APP};
    out.insert(out.end(), r2, r2 + n);
    mbedtls_sha256_update(&th, out.data(), out.size());
    uint8_t pms[32];
    size_t olen = 0;
    CHECK(mbedtls_ecjpake_derive_secret(&jp, pms, 32, &olen, mbed_rng, nullptr) == 0);
    cr::hkdf_sha256(board_nonce.data(), 16, pms, 32, "fk1 password", k, 32);
    return out;
  }
  void hash(uint8_t h[32]) {
    mbedtls_sha256_context copy;
    mbedtls_sha256_init(&copy);
    mbedtls_sha256_clone(&copy, &th);
    mbedtls_sha256_finish(&copy, h);
    mbedtls_sha256_free(&copy);
  }
  Bytes confirm() {
    uint8_t h[32], mac[32];
    hash(h);
    const uint8_t label[] = "fk1 app confirm";
    cr::hmac_sha256(k, 32, {{label, 15}, {h, 32}}, mac);
    Bytes f = {w::CONFIRM};
    f.insert(f.end(), mac, mac + 32);
    return f;
  }
  // The board's answer to the confirmation: the new key, if the board proved it knows K.
  bool take_key(const Bytes &f) {
    if (f.size() != 2 + 32 + 48 + 16 || f[0] != w::SIGNED_IN || f[1] != 0)
      return false;
    uint8_t h[32], mac[32];
    hash(h);
    const uint8_t label[] = "fk1 board confirm";
    cr::hmac_sha256(k, 32, {{label, 17}, {h, 32}}, mac);
    if (std::memcmp(mac, f.data() + 2, 32) != 0)
      return false;
    uint8_t plain[48];
    const uint8_t zero[12] = {};
    if (!cr::gcm_open(k, zero, nullptr, 0, f.data() + 34, 64, plain))
      return false;
    key_id.assign(plain, plain + 16);
    key_secret.assign(plain + 16, plain + 48);
    return true;
  }
  static void nonce(const uint8_t salt[4], uint64_t c, uint8_t out[12]) {
    std::memcpy(out, salt, 4);
    for (int i = 0; i < 8; i++)
      out[4 + i] = uint8_t(c >> (8 * i));
  }
  Bytes command(const Bytes &record, uint8_t type = w::COMMAND) {
    Bytes f(1 + record.size() + 16);
    f[0] = type;
    uint8_t n[12];
    nonce(s_out, c_out++, n);
    cr::gcm_seal(k_out, n, f.data(), 1, record.data(), record.size(), f.data() + 1);
    return f;
  }
  bool open(const Bytes &f, Bytes &record) {
    uint8_t n[12];
    nonce(s_in, c_in++, n);
    record.resize(f.size() - 17);
    return cr::gcm_open(k_in, n, f.data(), 1, f.data() + 1, f.size() - 1, record.data());
  }
};

struct Rig {
  fk::Access access{test_rng};
  fk::WifiSession board{access};
  std::deque<Bytes> to_app;
  fk::WifiSession::Send send = [this](const uint8_t *p, size_t n) { to_app.emplace_back(p, p + n); };
  uint8_t cmd[w::FRAME_MAX];
  size_t cmd_len = 0;
  uint32_t now = 1000;

  Rig() { board.start(send); }
  explicit Rig(bool) {}  // the caller starts the session
  fk::WifiSession::Step feed(const Bytes &f) {
    return board.receive(f.data(), f.size(), now, send, cmd, &cmd_len, sizeof(cmd));
  }
  Bytes next() {
    if (to_app.empty())
      return {};
    Bytes f = to_app.front();
    to_app.pop_front();
    return f;
  }
};

static void set_password(fk::Access &a, const char *pw) {
  CHECK(a.change_web_password(nullptr, 0, (const uint8_t *) pw, strlen(pw)).accepted());
}

// The sign-in deadline: 30 s from the open. An open stamped a few ms after the loop pass's clock
// (a relay channel opened inside that pass) has just opened: it once underflowed and closed every
// relay phone at once.
static void test_sign_in_deadline() {
  CHECK(!w::sign_in_expired(1000, 1000));
  CHECK(!w::sign_in_expired(1000, 1005));  // stamped 5 ms "later"
  CHECK(!w::sign_in_expired(0xFFFFFFF0u, 0xFFFFFFF5u));
  CHECK(!w::sign_in_expired(1000 + 30000, 1000));
  CHECK(w::sign_in_expired(1000 + 30001, 1000));
  CHECK(w::sign_in_expired(20000, 0xFFFF0000u));   // 85.5 s, across the wrap
  CHECK(!w::sign_in_expired(20000, 0xFFFFFF00u));  // 20.3 s, across the wrap
}

// A relay channel: Hello first, only the phone key the relay matched, never the web password.
static void test_relay_channel() {
  const uint8_t phone_a[6] = {1, 1, 1, 1, 1, 1}, phone_b[6] = {2, 2, 2, 2, 2, 2};
  {
    Rig r(true);
    const fk::WifiKey *a = r.access.issue_app_key(phone_a);
    const fk::WifiKey *b = r.access.issue_app_key(phone_b);
    CHECK(r.access.app_slot(a->id) == 0 && r.access.app_slot(b->id) == 1);
    CHECK(r.access.app_slot_of_owner(phone_b) == 1 && r.access.app_key(1) == b && r.access.app_key(2) == nullptr);
    Bytes ida(a->id, a->id + 16), sa(a->secret, a->secret + 32);
    r.board.start_relay(a->id, r.send);
    CHECK(r.board.relay());
    Client c;
    c.hello(r.next());  // Hello comes first: no Info in the clear
    CHECK(r.feed(c.sign_in_key(ida, sa)) == fk::WifiSession::Step::CONTINUE);
    CHECK(c.signed_in(r.next()) && r.board.signed_in());
  }
  {
    // Another phone's valid key on this slot's channel: refused like an unknown key.
    Rig r(true);
    const fk::WifiKey *a = r.access.issue_app_key(phone_a);
    const fk::WifiKey *b = r.access.issue_app_key(phone_b);
    Bytes idb(b->id, b->id + 16), sb(b->secret, b->secret + 32);
    r.board.start_relay(a->id, r.send);
    Client c;
    c.hello(r.next());
    CHECK(r.feed(c.sign_in_key(idb, sb)) == fk::WifiSession::Step::CLOSE);
    Bytes no = r.next();
    CHECK(no.size() == 2 && no[0] == w::SIGNED_IN && no[1] == uint8_t(fk::Result::KEY_UNKNOWN));
  }
  {
    // A browser key, or the web password: never over the relay.
    Rig r(true);
    set_password(r.access, "correct horse");
    const fk::WifiKey *br = r.access.issue_browser_key(true);
    Bytes idr(br->id, br->id + 16), sr(br->secret, br->secret + 32);
    r.board.start_relay(br->id, r.send);
    Client c;
    c.hello(r.next());
    CHECK(r.feed(c.sign_in_key(idr, sr)) == fk::WifiSession::Step::CLOSE);
    Rig r2(true);
    r2.board.start_relay(idr.data(), r2.send);
    r2.next();
    Bytes jpake = {w::SIGN_IN, 1, 0};
    CHECK(r2.feed(jpake) == fk::WifiSession::Step::CLOSE);
  }
}

static void test_key_sign_in() {
  Rig r;
  const uint8_t phone[6] = {1, 2, 3, 4, 5, 6};
  const fk::WifiKey *k = r.access.issue_app_key(phone);
  Bytes id(k->id, k->id + 16), secret(k->secret, k->secret + 32);

  Client c;
  c.hello(r.next());
  CHECK(r.feed(c.sign_in_key(id, secret)) == fk::WifiSession::Step::CONTINUE);
  CHECK(c.signed_in(r.next()));
  CHECK(r.board.signed_in());
  CHECK(r.board.key_kind() == fk::KeyKind::APP);
  CHECK(std::memcmp(r.board.key_id(), id.data(), 16) == 0);

  // Commands in, records out, both encrypted.
  Bytes rec = {7, 0x05, 1};
  CHECK(r.feed(c.command(rec)) == fk::WifiSession::Step::COMMAND);
  CHECK_EQ(r.cmd_len, 3);
  CHECK(std::memcmp(r.cmd, rec.data(), 3) == 0);
  uint8_t state[17] = {1, 2, 3}, out[64];
  size_t n = r.board.seal(w::STATE, state, 17, out, sizeof(out));
  CHECK_EQ(n, 1 + 17 + 16);
  Bytes got;
  CHECK(c.open(Bytes(out, out + n), got));
  CHECK(got.size() == 17 && got[2] == 3);
  // The ciphertext does not carry the record in the clear.
  CHECK(std::memcmp(out + 1, state, 17) != 0);
  // Second frame: the counters move on.
  n = r.board.seal(w::RESULT, state, 5, out, sizeof(out));
  CHECK(c.open(Bytes(out, out + n), got));
  CHECK(r.feed(c.command(rec)) == fk::WifiSession::Step::COMMAND);
  // Board firmware data (frame 06): decrypted on the same counter, handed over as update data.
  Bytes data = {0x00, 0x10, 0, 0, 0xAA, 0xBB};
  CHECK(r.feed(c.command(data, w::UPDATE)) == fk::WifiSession::Step::UPDATE_DATA);
  CHECK(r.cmd_len == 6 && r.cmd[4] == 0xAA);
  CHECK(r.feed(c.command(rec)) == fk::WifiSession::Step::COMMAND);
}

static void test_key_sign_in_refused() {
  // An unknown key.
  {
    Rig r;
    Client c;
    c.hello(r.next());
    CHECK(r.feed(c.sign_in_key(Bytes(16, 9), Bytes(32, 9))) == fk::WifiSession::Step::CLOSE);
    Bytes f = r.next();
    CHECK(f.size() == 2 && f[1] == uint8_t(fk::Result::KEY_UNKNOWN));
  }
  // A known id with the wrong secret: same answer.
  {
    Rig r;
    const uint8_t phone[6] = {1};
    const fk::WifiKey *k = r.access.issue_app_key(phone);
    Client c;
    c.hello(r.next());
    CHECK(r.feed(c.sign_in_key(Bytes(k->id, k->id + 16), Bytes(32, 0))) == fk::WifiSession::Step::CLOSE);
    CHECK(r.next()[1] == uint8_t(fk::Result::KEY_UNKNOWN));
    CHECK(!r.board.signed_in());
  }
  // A command before signing in.
  {
    Rig r;
    r.next();
    CHECK(r.feed(Bytes{w::COMMAND, 1, 2, 3}) == fk::WifiSession::Step::CLOSE);
  }
}

static void test_tampering() {
  Rig r;
  const uint8_t phone[6] = {1};
  const fk::WifiKey *k = r.access.issue_app_key(phone);
  Client c;
  c.hello(r.next());
  r.feed(c.sign_in_key(Bytes(k->id, k->id + 16), Bytes(k->secret, k->secret + 32)));
  CHECK(c.signed_in(r.next()));
  // A flipped bit is refused and closes the connection.
  Bytes f = c.command({1, 0x01, 0});
  f[2] ^= 0x01;
  CHECK(r.feed(f) == fk::WifiSession::Step::CLOSE);

  // A replayed frame (counter already used) is refused.
  Rig r2;
  const fk::WifiKey *k2 = r2.access.issue_app_key(phone);
  Client c2;
  c2.hello(r2.next());
  r2.feed(c2.sign_in_key(Bytes(k2->id, k2->id + 16), Bytes(k2->secret, k2->secret + 32)));
  CHECK(c2.signed_in(r2.next()));
  Bytes one = c2.command({1, 0x01, 0});
  CHECK(r2.feed(one) == fk::WifiSession::Step::COMMAND);
  CHECK(r2.feed(one) == fk::WifiSession::Step::CLOSE);

  // A board that does not hold the key cannot answer the sign-in (the app refuses it).
  Client c3;
  Rig r3;
  c3.hello(r3.next());
  c3.sign_in_key(Bytes(16, 1), Bytes(32, 1));
  Bytes fake = {w::SIGNED_IN, 0};
  fake.resize(34, 0xAA);
  CHECK(!c3.signed_in(fake));
}

static void test_password_sign_in() {
  Rig r;
  set_password(r.access, "correct horse");
  Client c;
  c.hello(r.next());
  CHECK(r.feed(c.sign_in_password("correct horse", true)) == fk::WifiSession::Step::CONTINUE);
  Bytes jp = r.next();
  CHECK(r.feed(c.round_two(jp)) == fk::WifiSession::Step::CONTINUE);
  CHECK(r.feed(c.confirm()) == fk::WifiSession::Step::CONTINUE);
  CHECK(c.take_key(r.next()));
  CHECK_EQ(r.access.count(fk::KeyKind::BROWSER), 1);
  // Then the key sign-in on the same connection.
  CHECK(r.feed(c.sign_in_key(c.key_id, c.key_secret)) == fk::WifiSession::Step::CONTINUE);
  CHECK(c.signed_in(r.next()));
  CHECK(r.board.signed_in());
  CHECK(r.board.key_kind() == fk::KeyKind::BROWSER);
  CHECK(r.board.key_remembered());
  CHECK(r.feed(c.command({3, 0x50})) == fk::WifiSession::Step::COMMAND);
  CHECK(r.board.tab_key_id() == nullptr);  // remembered: not a tab-only key

  // "This tab only": the key is known to the connection from the moment it is given, so closing before the key sign-in still removes it.
  {
    Rig t;
    set_password(t.access, "correct horse");
    Client tc;
    tc.hello(t.next());
    t.feed(tc.sign_in_password("correct horse", false));
    t.feed(tc.round_two(t.next()));
    t.feed(tc.confirm());
    CHECK(tc.take_key(t.next()));
    CHECK(t.board.tab_key_id() != nullptr);
    CHECK(std::memcmp(t.board.tab_key_id(), tc.key_id.data(), 16) == 0);
    CHECK(!t.board.signed_in());
  }

  // The key works on a new connection, without the password.
  Rig r2;
  uint8_t buf[fk::Access::SERIALIZED_MAX];
  size_t n = r.access.serialize(buf, sizeof(buf));
  CHECK(r2.access.deserialize(buf, n));
  Client c2;
  c2.hello(r2.next());
  r2.feed(c2.sign_in_key(c.key_id, c.key_secret));
  CHECK(c2.signed_in(r2.next()));
}

// Password sign-ins are paced board-wide, right or wrong.
static void test_password_pacing() {
  Rig base;
  set_password(base.access, "correct horse");
  for (int attempt = 0; attempt < 3; attempt++) {
    if (attempt == 2)
      base.now += fk::PasswordLockout::PACE_MS;
    fk::WifiSession s(base.access);
    std::deque<Bytes> q;
    fk::WifiSession::Send send = [&](const uint8_t *p, size_t n) { q.emplace_back(p, p + n); };
    uint8_t cmd[w::FRAME_MAX];
    size_t cl;
    s.start(send);
    Client c;
    c.hello(q.front());
    q.pop_front();
    Bytes f = c.sign_in_password("correct horse", false);
    auto step = s.receive(f.data(), f.size(), base.now, send, cmd, &cl, sizeof(cmd));
    if (attempt == 1) {
      // Right after another one: refused before any J-PAKE work, with the wait.
      CHECK(step == fk::WifiSession::Step::CLOSE);
      CHECK(q.front().size() == 4 && q.front()[1] == uint8_t(fk::Result::LOCKED_OUT));
      CHECK_EQ(q.front()[2] | (q.front()[3] << 8), 5);
    } else {
      CHECK(step == fk::WifiSession::Step::CONTINUE);
      CHECK(q.front()[0] == w::JPAKE_BOARD);
    }
  }
  CHECK_EQ(base.access.lockout().fails(), 0);  // pacing is not a wrong password
}

static void test_password_wrong_and_lockout() {
  Rig base;
  set_password(base.access, "correct horse");
  for (int attempt = 1; attempt <= 6; attempt++) {
    if (attempt > 1 && attempt < 6)
      base.now += fk::PasswordLockout::PACE_MS;  // password sign-ins are paced
    fk::WifiSession s(base.access);
    std::deque<Bytes> q;
    fk::WifiSession::Send send = [&](const uint8_t *p, size_t n) { q.emplace_back(p, p + n); };
    uint8_t cmd[w::FRAME_MAX];
    size_t cl;
    s.start(send);
    Client c;
    c.hello(q.front());
    q.pop_front();
    Bytes f = c.sign_in_password("battery staple", false);
    auto step = s.receive(f.data(), f.size(), base.now, send, cmd, &cl, sizeof(cmd));
    if (attempt == 6) {
      // Five wrong ones: locked out for a minute, before any J-PAKE work.
      CHECK(step == fk::WifiSession::Step::CLOSE);
      CHECK(q.front().size() == 4 && q.front()[1] == uint8_t(fk::Result::LOCKED_OUT));
      CHECK_EQ(q.front()[2] | (q.front()[3] << 8), 60);
      break;
    }
    Bytes jp = q.front();
    q.pop_front();
    Bytes two = c.round_two(jp);
    s.receive(two.data(), two.size(), base.now, send, cmd, &cl, sizeof(cmd));
    Bytes conf = c.confirm();
    CHECK(s.receive(conf.data(), conf.size(), base.now, send, cmd, &cl, sizeof(cmd)) == fk::WifiSession::Step::CLOSE);
    CHECK(q.front().size() == 2 && q.front()[1] == uint8_t(fk::Result::WRONG_PASSWORD));
  }
  CHECK_EQ(base.access.count(fk::KeyKind::BROWSER), 0);
  // After the minute the right password works and resets the count.
  base.now += 60000;
  fk::WifiSession s(base.access);
  std::deque<Bytes> q;
  fk::WifiSession::Send send = [&](const uint8_t *p, size_t n) { q.emplace_back(p, p + n); };
  uint8_t cmd[w::FRAME_MAX];
  size_t cl;
  s.start(send);
  Client c;
  c.hello(q.front());
  q.pop_front();
  Bytes f = c.sign_in_password("correct horse", false);
  s.receive(f.data(), f.size(), base.now, send, cmd, &cl, sizeof(cmd));
  Bytes two = c.round_two(q.front());
  q.pop_front();
  s.receive(two.data(), two.size(), base.now, send, cmd, &cl, sizeof(cmd));
  Bytes conf = c.confirm();
  s.receive(conf.data(), conf.size(), base.now, send, cmd, &cl, sizeof(cmd));
  CHECK(c.take_key(q.front()));
  CHECK_EQ(base.access.lockout().fails(), 0);
  CHECK_EQ(base.access.count(fk::KeyKind::BROWSER), 1);
}

static void test_password_edge_cases() {
  // No web password set.
  {
    Rig r;
    Client c;
    c.hello(r.next());
    CHECK(r.feed(c.sign_in_password("anything at all", true)) == fk::WifiSession::Step::CLOSE);
    CHECK(r.next()[1] == uint8_t(fk::Result::NO_WEB_PASSWORD));
  }
  // A second password sign-in on the same connection is refused.
  {
    Rig r;
    set_password(r.access, "correct horse");
    Client c;
    c.hello(r.next());
    r.feed(c.sign_in_password("correct horse", true));
    r.feed(c.round_two(r.next()));
    r.feed(c.confirm());
    CHECK(c.take_key(r.next()));
    Client c2;
    c2.board_nonce = c.board_nonce;
    CHECK(r.feed(c2.sign_in_password("correct horse", true)) == fk::WifiSession::Step::CLOSE);
  }
  // Garbage instead of J-PAKE round one.
  {
    Rig r;
    set_password(r.access, "correct horse");
    r.next();
    CHECK(r.feed(Bytes{w::SIGN_IN, 1, 1, 1, 2, 3}) == fk::WifiSession::Step::CLOSE);
    CHECK_EQ(r.access.lockout().fails(), 0);  // not a password guess
  }
  // Only the remember flag before round one: anything else there is a bad layout.
  {
    Rig r;
    set_password(r.access, "correct horse");
    r.next();
    CHECK(r.feed(Bytes{w::SIGN_IN, 1, 2, 0x41, 0x04}) == fk::WifiSession::Step::CLOSE);
    CHECK_EQ(r.access.lockout().fails(), 0);
  }
}

int main() {
  test_sign_in_deadline();
  test_relay_channel();
  test_key_sign_in();
  test_key_sign_in_refused();
  test_tampering();
  test_password_sign_in();
  test_password_pacing();
  test_password_wrong_and_lockout();
  test_password_edge_cases();
  return report("wifi_session");
}
