#include "relay.h"

#include <cstring>

#include "crypto.h"
#include "mbedtls/bignum.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"

namespace esphome {
namespace flamekeeper {
namespace relay {

namespace {

int rng(void *ctx, unsigned char *out, size_t len) {
  (*static_cast<const Random *>(ctx))(out, len);
  return 0;
}

}  // namespace

bool id_of(const uint8_t pub[PUBLIC_KEY_SIZE], uint8_t out[ID_SIZE]) {
  uint8_t h[crypto::SHA256_SIZE];
  if (!crypto::sha256({{pub, PUBLIC_KEY_SIZE}}, h))
    return false;
  std::memcpy(out, h, ID_SIZE);
  return true;
}

void id_text(const uint8_t id[ID_SIZE], char out[ID_TEXT_SIZE + 1]) {
  static const char *const A = "abcdefghijklmnopqrstuvwxyz234567";
  uint32_t buf = 0;
  int bits = 0;
  size_t n = 0;
  for (size_t i = 0; i < ID_SIZE; i++) {
    buf = (buf << 8) | id[i];
    bits += 8;
    while (bits >= 5) {
      out[n++] = A[(buf >> (bits - 5)) & 31];
      bits -= 5;
    }
  }
  if (bits > 0)
    out[n++] = A[(buf << (5 - bits)) & 31];
  out[n] = '\0';
}

void Identity::clear() {
  crypto::wipe(this->priv_, sizeof(this->priv_));
  std::memset(this->pub_, 0, sizeof(this->pub_));
  std::memset(this->id_, 0, sizeof(this->id_));
  this->valid_ = false;
}

bool Identity::derive_(const Random &random) {
  mbedtls_ecp_keypair kp;
  mbedtls_ecp_keypair_init(&kp);
  size_t olen = 0;
  bool ok = mbedtls_ecp_read_key(MBEDTLS_ECP_DP_SECP256R1, &kp, this->priv_, PRIVATE_KEY_SIZE) == 0 &&
            mbedtls_ecp_keypair_calc_public(&kp, rng, const_cast<Random *>(&random)) == 0 &&
            mbedtls_ecp_write_public_key(&kp, MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, this->pub_, PUBLIC_KEY_SIZE) == 0 &&
            olen == PUBLIC_KEY_SIZE;
  mbedtls_ecp_keypair_free(&kp);
  this->valid_ = ok && id_of(this->pub_, this->id_);
  if (!this->valid_)
    this->clear();
  return this->valid_;
}

bool Identity::generate(const Random &random) {
  mbedtls_ecp_keypair kp;
  mbedtls_ecp_keypair_init(&kp);
  size_t olen = 0;
  bool ok = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, &kp, rng, const_cast<Random *>(&random)) == 0 &&
            mbedtls_ecp_write_key_ext(&kp, &olen, this->priv_, PRIVATE_KEY_SIZE) == 0 && olen == PRIVATE_KEY_SIZE;
  mbedtls_ecp_keypair_free(&kp);
  if (!ok) {
    this->clear();
    return false;
  }
  return this->derive_(random);
}

bool Identity::load(const uint8_t priv[PRIVATE_KEY_SIZE], const Random &random) {
  std::memcpy(this->priv_, priv, PRIVATE_KEY_SIZE);
  return this->derive_(random);
}

bool Identity::sign(const uint8_t *msg, size_t len, const Random &random, uint8_t out[SIGNATURE_SIZE]) const {
  if (!this->valid_)
    return false;
  uint8_t h[crypto::SHA256_SIZE];
  if (!crypto::sha256({{msg, len}}, h))
    return false;
  mbedtls_ecp_group grp;
  mbedtls_mpi d, r, s;
  mbedtls_ecp_group_init(&grp);
  mbedtls_mpi_init(&d);
  mbedtls_mpi_init(&r);
  mbedtls_mpi_init(&s);
  bool ok = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
            mbedtls_mpi_read_binary(&d, this->priv_, PRIVATE_KEY_SIZE) == 0 &&
            mbedtls_ecdsa_sign(&grp, &r, &s, &d, h, sizeof(h), rng, const_cast<Random *>(&random)) == 0 &&
            mbedtls_mpi_write_binary(&r, out, 32) == 0 && mbedtls_mpi_write_binary(&s, out + 32, 32) == 0;
  mbedtls_mpi_free(&s);
  mbedtls_mpi_free(&r);
  mbedtls_mpi_free(&d);
  mbedtls_ecp_group_free(&grp);
  return ok;
}

bool relay_token(const uint8_t *secret, size_t len, uint8_t out[TOKEN_SIZE]) {
  return crypto::hkdf_sha256(nullptr, 0, secret, len, "fk1 relay", out, TOKEN_SIZE);
}

bool push_key(const uint8_t *secret, size_t len, uint8_t out[PUSH_KEY_SIZE]) {
  return crypto::hkdf_sha256(nullptr, 0, secret, len, "fk1 push", out, PUSH_KEY_SIZE);
}

size_t auth_signed_bytes(const uint8_t challenge[CHALLENGE_SIZE], const char *host, uint8_t *out, size_t cap) {
  static const char LABEL[] = "fk1 relay board";
  const size_t hl = std::strlen(host);
  const size_t n = sizeof(LABEL) - 1 + CHALLENGE_SIZE + 1 + hl;
  if (hl > HOST_MAX || n > cap)
    return 0;
  size_t k = 0;
  std::memcpy(out + k, LABEL, sizeof(LABEL) - 1);
  k += sizeof(LABEL) - 1;
  std::memcpy(out + k, challenge, CHALLENGE_SIZE);
  k += CHALLENGE_SIZE;
  out[k++] = uint8_t(hl);
  std::memcpy(out + k, host, hl);
  return k + hl;
}

size_t encode_auth(const Identity &who, const uint8_t challenge[CHALLENGE_SIZE], const char *host,
                   const Random &random, uint8_t *out, size_t cap) {
  uint8_t signed_bytes[15 + CHALLENGE_SIZE + 1 + HOST_MAX];
  const size_t n = auth_signed_bytes(challenge, host, signed_bytes, sizeof(signed_bytes));
  const size_t total = 2 + PUBLIC_KEY_SIZE + SIGNATURE_SIZE;
  if (n == 0 || total > cap || !who.valid())
    return 0;
  out[0] = 0;
  out[1] = op::AUTH;
  std::memcpy(out + 2, who.public_key(), PUBLIC_KEY_SIZE);
  if (!who.sign(signed_bytes, n, random, out + 2 + PUBLIC_KEY_SIZE))
    return 0;
  return total;
}

size_t encode_roster(const RosterEntry *entries, size_t n, uint8_t *out, size_t cap) {
  const size_t total = 2 + n * 33;
  if (n > SLOTS || total > cap)
    return 0;
  out[0] = 0;
  out[1] = op::ROSTER;
  for (size_t i = 0; i < n; i++) {
    out[2 + i * 33] = entries[i].slot;
    std::memcpy(out + 3 + i * 33, entries[i].hash, 32);
  }
  return total;
}

size_t encode_close(uint8_t channel, uint16_t code, uint8_t out[5]) {
  out[0] = 0;
  out[1] = op::CLOSE;
  out[2] = channel;
  out[3] = uint8_t(code);
  out[4] = uint8_t(code >> 8);
  return 5;
}

bool parse_control(const uint8_t *in, size_t len, Control &out) {
  out = Control{};
  if (len < 2 || in[0] != 0)
    return false;
  out.op = in[1];
  switch (in[1]) {
    case op::CHALLENGE:
      if (len != 2 + CHALLENGE_SIZE)
        return false;
      out.challenge = in + 2;
      return true;
    case op::OPEN:
      if (len != 4 || in[2] < 1 || in[2] > CHANNELS || in[3] >= SLOTS)
        return false;
      out.channel = in[2];
      out.slot = in[3];
      return true;
    case op::CLOSE:
      if (len != 5 || in[2] < 1 || in[2] > CHANNELS)
        return false;
      out.channel = in[2];
      out.code = uint16_t(in[3] | (in[4] << 8));
      return true;
    default:
      return false;
  }
}

}  // namespace relay
}  // namespace flamekeeper
}  // namespace esphome
