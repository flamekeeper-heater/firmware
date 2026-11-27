// The internet relay's board side, without any transport (PROTOCOL.md): the board's
// P-256 identity and id, the per-phone relay token and push key, and the channel-0 messages. The
// ESPHome glue (flamekeeper_net) moves the bytes over a TLS WebSocket.
//
// Plain C++ on mbedTLS (host-tested in tests/test_relay.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "board.h"

namespace esphome {
namespace flamekeeper {
namespace relay {

static const size_t PRIVATE_KEY_SIZE = 32;
static const size_t PUBLIC_KEY_SIZE = 65;  // uncompressed: 04 || X || Y
static const size_t SIGNATURE_SIZE = 64;   // r || s
static const size_t ID_SIZE = 16;
static const size_t ID_TEXT_SIZE = 26;     // base32, no padding
static const size_t CHALLENGE_SIZE = 32;
static const size_t TOKEN_SIZE = 32;       // T
static const size_t PUSH_KEY_SIZE = 16;    // P
static const size_t HOST_MAX = RELAY_HOST_MAX;
static const size_t CHANNELS = 3;          // app channels 1..3
static const size_t SLOTS = 8;             // phone key slots
static const size_t MESSAGE_MAX = 2048;    // what the relay passes

static const char *const DEFAULT_HOST = "relay.flamekeeper.app";
static const char *const SUBPROTOCOL = "flamekeeper-relay.v1";

namespace op {
static const uint8_t CHALLENGE = 0x01;
static const uint8_t AUTH = 0x02;
static const uint8_t ROSTER = 0x03;
static const uint8_t OPEN = 0x04;
static const uint8_t CLOSE = 0x05;
static const uint8_t PUSH = 0x06;
static const uint8_t LIVE = 0x07;
}  // namespace op

// Close codes the board sends on a channel.
static const uint16_t CLOSE_NORMAL = 1000;
static const uint16_t CLOSE_PROTOCOL = 1008;
static const uint16_t CLOSE_KEY_REMOVED = 4100;
// ...and the relay's that change what the board does next.
static const uint16_t CLOSE_REPLACED = 4001;
static const uint16_t CLOSE_AUTH_FAILED = 4002;

using Random = std::function<void(uint8_t *, size_t)>;

// The board's relay key pair.
class Identity {
 public:
  // A new key pair from `random`. False only if mbedTLS fails.
  bool generate(const Random &random);
  // A stored private key; false if it is not a valid P-256 scalar. `random` blinds the maths.
  bool load(const uint8_t priv[PRIVATE_KEY_SIZE], const Random &random);
  bool valid() const { return this->valid_; }
  void clear();
  const uint8_t *private_key() const { return this->priv_; }
  const uint8_t *public_key() const { return this->pub_; }
  const uint8_t *id() const { return this->id_; }
  // ECDSA P-256 over SHA-256 of msg, r || s.
  bool sign(const uint8_t *msg, size_t len, const Random &random, uint8_t out[SIGNATURE_SIZE]) const;

 protected:
  bool derive_(const Random &random);
  bool valid_{false};
  uint8_t priv_[PRIVATE_KEY_SIZE]{};
  uint8_t pub_[PUBLIC_KEY_SIZE]{};
  uint8_t id_[ID_SIZE]{};
};

// The id of a public key: SHA-256(key)[0..16].
bool id_of(const uint8_t pub[PUBLIC_KEY_SIZE], uint8_t out[ID_SIZE]);
// Lowercase RFC 4648 base32, no padding; writes ID_TEXT_SIZE characters and a NUL.
void id_text(const uint8_t id[ID_SIZE], char out[ID_TEXT_SIZE + 1]);

// Per phone key, from its secret.
bool relay_token(const uint8_t *secret, size_t len, uint8_t out[TOKEN_SIZE]);
bool push_key(const uint8_t *secret, size_t len, uint8_t out[PUSH_KEY_SIZE]);

// "fk1 relay board" || challenge || u8 n || host: what AUTH signs. Returns its length.
size_t auth_signed_bytes(const uint8_t challenge[CHALLENGE_SIZE], const char *host, uint8_t *out, size_t cap);
// The whole AUTH message (channel 0): 00 02 || public key || signature, 131 bytes, or 0.
size_t encode_auth(const Identity &who, const uint8_t challenge[CHALLENGE_SIZE], const char *host,
                   const Random &random, uint8_t *out, size_t cap);

struct RosterEntry {
  uint8_t slot;
  uint8_t hash[32];  // SHA-256(T)
};
size_t encode_roster(const RosterEntry *entries, size_t n, uint8_t *out, size_t cap);
size_t encode_close(uint8_t channel, uint16_t code, uint8_t out[5]);

// A channel-0 message from the relay.
struct Control {
  uint8_t op{0};
  const uint8_t *challenge{nullptr};  // CHALLENGE
  uint8_t channel{0};                 // OPEN, CLOSE
  uint8_t slot{0};                    // OPEN
  uint16_t code{0};                   // CLOSE
};
// False for anything malformed (including a channel or slot out of range).
bool parse_control(const uint8_t *in, size_t len, Control &out);


}  // namespace relay
}  // namespace flamekeeper
}  // namespace esphome
