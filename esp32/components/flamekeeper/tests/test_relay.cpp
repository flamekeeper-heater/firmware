// The relay's board side (PROTOCOL.md): ids, tokens, AUTH, the control messages.
// The same vectors are checked by the relay and the app.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../crypto.h"
#include "../relay.h"
#include "../relay_client.h"
#include "../access.h"
#include "mbedtls/bignum.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"

namespace fk = esphome::flamekeeper;
namespace rl = fk::relay;

static int g_checks = 0, g_failures = 0;
#define CHECK(c)                                                    \
  do {                                                              \
    g_checks++;                                                     \
    if (!(c)) {                                                     \
      g_failures++;                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);      \
    }                                                               \
  } while (0)

static std::string hex(const uint8_t *p, size_t n) {
  static const char *d = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; i++) {
    s += d[p[i] >> 4];
    s += d[p[i] & 15];
  }
  return s;
}
static std::vector<uint8_t> unhex(const char *s) {
  std::vector<uint8_t> v;
  for (; s[0] && s[1]; s += 2) {
    auto h = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    v.push_back(uint8_t(h(s[0]) << 4 | h(s[1])));
  }
  return v;
}

static uint32_t g_x = 1;
static const rl::Random RNG = [](uint8_t *out, size_t n) {
  for (size_t i = 0; i < n; i++) {
    g_x = g_x * 1103515245u + 12345u;
    out[i] = uint8_t(g_x >> 16);
  }
};

static bool verify(const uint8_t pub[65], const uint8_t *msg, size_t len, const uint8_t sig[64]) {
  uint8_t h[32];
  fk::crypto::sha256({{msg, len}}, h);
  mbedtls_ecp_group grp;
  mbedtls_ecp_point q;
  mbedtls_mpi r, s;
  mbedtls_ecp_group_init(&grp);
  mbedtls_ecp_point_init(&q);
  mbedtls_mpi_init(&r);
  mbedtls_mpi_init(&s);
  bool ok = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
            mbedtls_ecp_point_read_binary(&grp, &q, pub, 65) == 0 && mbedtls_mpi_read_binary(&r, sig, 32) == 0 &&
            mbedtls_mpi_read_binary(&s, sig + 32, 32) == 0 && mbedtls_ecdsa_verify(&grp, h, 32, &q, &r, &s) == 0;
  mbedtls_mpi_free(&s);
  mbedtls_mpi_free(&r);
  mbedtls_ecp_point_free(&q);
  mbedtls_ecp_group_free(&grp);
  return ok;
}

static void test_ids() {
  // The generator point G, uncompressed.
  std::vector<uint8_t> g = unhex(
      "046b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c2964fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315e"
      "cecbb6406837bf51f5");
  CHECK(g.size() == 65);
  uint8_t id[rl::ID_SIZE];
  CHECK(rl::id_of(g.data(), id));
  CHECK(hex(id, 16) == "698bea63dc44a344663ff1429aea1084");
  char text[rl::ID_TEXT_SIZE + 1];
  rl::id_text(id, text);
  CHECK(std::string(text) == "ngf6uy64isruizr76fbjv2qqqq");
  // The private key 1 is G itself.
  uint8_t one[32] = {};
  one[31] = 1;
  rl::Identity who;
  CHECK(who.load(one, RNG));
  CHECK(std::memcmp(who.public_key(), g.data(), 65) == 0 && std::memcmp(who.id(), id, 16) == 0);
  uint8_t zero[32] = {};
  CHECK(!who.load(zero, RNG) && !who.valid());
}

static void test_identity_and_auth() {
  rl::Identity a, b;
  CHECK(a.generate(RNG));
  CHECK(b.load(a.private_key(), RNG));
  CHECK(std::memcmp(a.public_key(), b.public_key(), 65) == 0 && std::memcmp(a.id(), b.id(), 16) == 0);
  rl::Identity c;
  CHECK(c.generate(RNG) && std::memcmp(a.id(), c.id(), 16) != 0);

  // The signed bytes: the spec's vector.
  uint8_t ch[32];
  std::memset(ch, 0x01, 32);
  uint8_t sb[160];
  size_t n = rl::auth_signed_bytes(ch, "relay.flamekeeper.app", sb, sizeof(sb));
  CHECK(n == 69);
  CHECK(hex(sb, n) ==
        "666b312072656c617920626f61726401010101010101010101010101010101010101010101010101010101010101011572656c61792e"
        "666c616d656b65657065722e617070");

  // AUTH: key and a signature that verifies over those bytes, nothing after it.
  uint8_t m[200];
  size_t k = rl::encode_auth(a, ch, "relay.flamekeeper.app", RNG, m, sizeof(m));
  CHECK(k == 131);
  CHECK(m[0] == 0 && m[1] == rl::op::AUTH && std::memcmp(m + 2, a.public_key(), 65) == 0);
  CHECK(verify(m + 2, sb, n, m + 67));
  // Over another host: does not verify.
  uint8_t sb2[160];
  size_t n2 = rl::auth_signed_bytes(ch, "relay.example.com", sb2, sizeof(sb2));
  CHECK(!verify(m + 2, sb2, n2, m + 67));
  // Too long a host, or no identity: nothing.
  std::string long_host(64, 'a');
  CHECK(rl::encode_auth(a, ch, long_host.c_str(), RNG, m, sizeof(m)) == 0);
  CHECK(rl::encode_auth(rl::Identity{}, ch, "h", RNG, m, sizeof(m)) == 0);
}

static void test_tokens() {
  uint8_t secret[32];
  std::memset(secret, 0x09, 32);
  uint8_t t[32], th[32], p[16];
  CHECK(rl::relay_token(secret, 32, t));
  CHECK(hex(t, 32) == "16c1df6a87aaeae336a68c3cb93442b2246922a2edaee096e2439ee205578c0a");
  fk::crypto::sha256({{t, 32}}, th);
  CHECK(hex(th, 32) == "d91429cfba5c46b38cfeb129eb4f263b616323ad4d7c20aeb3430d641258e834");
  CHECK(rl::push_key(secret, 32, p));
  uint8_t t16[16];
  std::memcpy(t16, t, 16);
  CHECK(std::memcmp(p, t16, 16) != 0);  // a different label: a different key
  std::printf("  push key for 32 x 09: %s\n", hex(p, 16).c_str());
}

static void test_messages() {
  rl::RosterEntry e[2] = {{2, {}}, {5, {}}};
  std::memset(e[0].hash, 0xAA, 32);
  std::memset(e[1].hash, 0xBB, 32);
  uint8_t m[300];
  CHECK(rl::encode_roster(e, 2, m, sizeof(m)) == 3 + 66);
  CHECK(m[0] == 0 && m[1] == 3 && m[2] == 1 && m[3] == 2 && m[4] == 0xAA && m[36] == 5 && m[37] == 0xBB);
  CHECK(rl::encode_roster(e, 0, m, sizeof(m)) == 3);
  uint8_t c[5];
  CHECK(rl::encode_close(2, 4001, c) == 5 && c[1] == 5 && c[2] == 2 && c[3] == 0xA1 && c[4] == 0x0F);

  rl::Control k;
  uint8_t chal[34] = {0, 1};
  CHECK(rl::parse_control(chal, 34, k) && k.op == rl::op::CHALLENGE && k.challenge == chal + 2);
  CHECK(!rl::parse_control(chal, 33, k));
  const uint8_t open[] = {0, 4, 1, 2};
  CHECK(rl::parse_control(open, 4, k) && k.channel == 1 && k.slot == 2);
  const uint8_t open_bad[] = {0, 4, 4, 2}, open_slot[] = {0, 4, 1, 8}, open_zero[] = {0, 4, 0, 1};
  CHECK(!rl::parse_control(open_bad, 4, k) && !rl::parse_control(open_slot, 4, k) && !rl::parse_control(open_zero, 4, k));
  CHECK(rl::parse_control(c, 5, k) && k.op == rl::op::CLOSE && k.channel == 2 && k.code == 4001);
  const uint8_t want[] = {0, 8, 0x05, 0x01, 0x03};
  CHECK(!rl::parse_control(want, 5, k));  // op 8 (WANT, no longer sent): ignored
  const uint8_t not_control[] = {1, 4, 1, 2}, unknown[] = {0, 0x7F};
  CHECK(!rl::parse_control(not_control, 4, k) && !rl::parse_control(unknown, 2, k));

  auto host = [](const char *h) { return fk::relay_host_ok((const uint8_t *) h, std::strlen(h)); };
  CHECK(host("relay.flamekeeper.app") && host("192.168.1.20:8787") && host("my-relay.example.org"));
  CHECK(!host("") && !host("relay.example.org/path") && !host("bad host") && !host(".x") && !host("h\x01"));
  CHECK(!host(std::string(64, 'a').c_str()));
}


struct ClientRig {
  fk::Access access{RNG};
  fk::RelayClient rc{access, RNG};
  std::vector<std::string> connects;
  std::vector<std::vector<uint8_t>> sent;
  int disconnects = 0, saves = 0;
  std::vector<std::string> events;  // "open 1 0", "close 1 1000", "frame 1 n"
  uint32_t now = 1000;
  ClientRig() {
    rc.set_io({[this](const char *h, const char *p) { connects.push_back(std::string(h) + p); },
               [this]() { disconnects++; },
               [this](const uint8_t *d, size_t n) {
                 sent.emplace_back(d, d + n);
                 return true;
               }});
    rc.set_channels({[this](uint8_t ch, int slot) { events.push_back("open " + std::to_string(ch) + " " + std::to_string(slot)); },
                     [this](uint8_t ch, uint16_t code) { events.push_back("close " + std::to_string(ch) + " " + std::to_string(code)); },
                     [this](uint8_t ch, const uint8_t *, size_t n) { events.push_back("frame " + std::to_string(ch) + " " + std::to_string(n)); }});
    rc.on_settings([this]() { saves++; });
  }
  void step(uint32_t ms, bool wifi = true) {
    now += ms;
    rc.loop(now, wifi);
  }
  void msg(std::vector<uint8_t> m) { rc.message(m.data(), m.size(), now); }
  void challenge() {
    std::vector<uint8_t> c = {0, 1};
    c.resize(34, 0x01);
    msg(c);
  }
};

static void test_client() {
  ClientRig r;
  r.step(10);
  CHECK(r.connects.empty() && r.rc.state() == fk::RelayState::OFF);
  // On: a key pair, saved; connects at once to the default host with the id in the path.
  CHECK(r.rc.set(true, nullptr, 0) && r.saves == 1 && r.rc.identity().valid());
  r.step(10, false);
  CHECK(r.connects.empty() && r.rc.state() == fk::RelayState::NO_INTERNET);
  r.step(10);
  char id[27];
  rl::id_text(r.rc.identity().id(), id);
  CHECK(r.connects.size() == 1 && r.connects[0] == std::string("relay.flamekeeper.app/v1/board/") + id);
  CHECK(r.rc.state() == fk::RelayState::CONNECTING);
  // Up, CHALLENGE: AUTH that verifies, then the roster (one phone key).
  const uint8_t phone[6] = {7, 7, 7, 7, 7, 7};
  const fk::WifiKey *k = r.access.issue_app_key(phone);
  r.rc.connected(r.now);
  r.challenge();
  CHECK(r.sent.size() == 2 && r.rc.state() == fk::RelayState::CONNECTED && r.rc.seconds_since_connected(r.now) == 0);
  const std::vector<uint8_t> &auth = r.sent[0];
  uint8_t ch[32];
  std::memset(ch, 1, 32);
  uint8_t sb[160];
  size_t n = rl::auth_signed_bytes(ch, "relay.flamekeeper.app", sb, sizeof(sb));
  CHECK(auth[1] == rl::op::AUTH && verify(auth.data() + 2, sb, n, auth.data() + 67));
  const std::vector<uint8_t> &roster = r.sent[1];
  uint8_t t[32], th[32];
  rl::relay_token(k->secret, 32, t);
  fk::crypto::sha256({{t, 32}}, th);
  CHECK(roster.size() == 3 + 33 && roster[1] == rl::op::ROSTER && roster[3] == 0 && std::memcmp(roster.data() + 4, th, 32) == 0);
  // A phone on channel 1; its frames both ways.
  r.msg({0, 4, 1, 0});
  CHECK(r.events.back() == "open 1 0" && r.rc.open_channels() == 1);
  r.msg({1, 0x11, 0x22});
  CHECK(r.events.back() == "frame 1 2");
  const uint8_t f[] = {0x02, 0xAA};
  CHECK(r.rc.send_frame(1, f, 2) && r.sent.back() == std::vector<uint8_t>({1, 0x02, 0xAA}));
  CHECK(!r.rc.send_frame(2, f, 2));  // no phone there
  r.msg({2, 0x11});                  // nor frames from there
  CHECK(r.events.back() == "frame 1 2");
  // A second phone key: a new roster on the next pass.
  size_t before = r.sent.size();
  const uint8_t phone2[6] = {8, 8, 8, 8, 8, 8};
  r.access.issue_app_key(phone2);
  r.step(10);
  CHECK(r.sent.size() == before + 1 && r.sent.back().size() == 3 + 66);
  r.step(10);
  CHECK(r.sent.size() == before + 1);  // unchanged: not sent again
  // The first phone forgotten: roster without it, its session closed (4100).
  r.access.revoke(k->id);
  r.step(10);
  CHECK(r.sent.back().size() == 3 + 33 && r.events.back() == "close 1 4100" && r.rc.open_channels() == 0);
  // The relay closes a channel.
  r.msg({0, 4, 2, 1});
  r.msg({0, 5, 2, 0xE8, 0x03});
  CHECK(r.events.back() == "close 2 1000");
  // Our side closes one: CLOSE to the relay.
  r.msg({0, 4, 3, 1});
  r.rc.close_channel(3, 1008);
  CHECK(r.sent.back() == std::vector<uint8_t>({0, 5, 3, 0xF0, 0x03}));
  // Refused (4002): state 3, then a retry after 2 s ±20 %.
  r.msg({0, 4, 1, 1});
  r.rc.closed(4002, r.now);
  CHECK(r.rc.state() == fk::RelayState::REFUSED && r.events.back() == "close 1 1000");
  size_t tries = r.connects.size();
  r.step(1500);
  CHECK(r.connects.size() == tries);
  r.step(1000);
  CHECK(r.connects.size() == tries + 1);
  // The host does not answer: no internet; the wait doubles (4 s ±20 %).
  r.rc.closed(0, r.now);
  CHECK(r.rc.state() == fk::RelayState::NO_INTERNET);
  r.step(3000);
  CHECK(r.connects.size() == tries + 1);
  r.step(2000);
  CHECK(r.connects.size() == tries + 2);
  // Up but no CHALLENGE in 10 s: dropped.
  r.rc.connected(r.now);
  int d = r.disconnects;
  r.step(10500);
  CHECK(r.disconnects == d + 1 && !r.rc.authed());
  // Replaced (4001): 60 s.
  r.step(20000);
  r.rc.connected(r.now);
  r.challenge();
  r.rc.closed(4001, r.now);
  tries = r.connects.size();
  r.step(45000);
  CHECK(r.connects.size() == tries);
  r.step(30000);
  CHECK(r.connects.size() == tries + 1);
  CHECK(r.rc.seconds_since_connected(r.now) == 75);
  // Off while up: disconnected, channels closed, nothing more.
  r.rc.connected(r.now);
  r.challenge();
  r.msg({0, 4, 1, 1});
  d = r.disconnects;
  CHECK(r.rc.set(false, nullptr, 0));
  CHECK(r.disconnects == d + 1 && r.events.back() == "close 1 1000" && r.rc.state() == fk::RelayState::OFF);
  tries = r.connects.size();
  r.step(400000);
  CHECK(r.connects.size() == tries);
  // A custom host; the key pair stays.
  uint8_t keep[32];
  std::memcpy(keep, r.rc.identity().private_key(), 32);
  const char *h = "192.168.1.20:8787";
  CHECK(r.rc.set(true, (const uint8_t *) h, std::strlen(h)));
  CHECK(std::memcmp(keep, r.rc.identity().private_key(), 32) == 0);
  r.step(10);
  CHECK(r.connects.back().rfind("192.168.1.20:8787/v1/board/", 0) == 0);
  // Hosts are kept as the relay sees them: lower case, no default port.
  const char *mixed = "Relay.Example.COM:443";
  CHECK(r.rc.set(true, (const uint8_t *) mixed, std::strlen(mixed)) && std::string(r.rc.host()) == "relay.example.com");
  const char *port = "relay.example.com:8443";
  CHECK(r.rc.set(true, (const uint8_t *) port, std::strlen(port)) && std::string(r.rc.host()) == "relay.example.com:8443");
  const char *dflt = "RELAY.flamekeeper.app:443";
  CHECK(r.rc.set(true, (const uint8_t *) dflt, std::strlen(dflt)) && std::string(r.rc.custom_host()).empty());
  CHECK(r.rc.set(true, (const uint8_t *) h, std::strlen(h)));
  // Saved settings come back.
  fk::RelayClient again{r.access, RNG};
  again.configure(true, "192.168.1.20:8787", keep);
  CHECK(again.on() && std::memcmp(again.identity().id(), r.rc.identity().id(), 16) == 0);
  CHECK(std::string(again.host()) == "192.168.1.20:8787");
}

int main() {
  test_ids();
  test_identity_and_auth();
  test_tokens();
  test_messages();
  test_client();
  std::printf("relay: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
