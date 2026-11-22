// The few primitives the Wi-Fi sign-in and frames need (PROTOCOL.md), on mbedTLS:
// ESP-IDF's on the board, the same mbedTLS sources in the host tests.
#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace esphome {
namespace flamekeeper {
namespace crypto {

struct Bytes {
  const uint8_t *data;
  size_t len;
};

static const size_t SHA256_SIZE = 32;
static const size_t GCM_TAG_SIZE = 16;
static const size_t GCM_NONCE_SIZE = 12;

// HMAC-SHA256 over the concatenation of parts.
bool hmac_sha256(const uint8_t *key, size_t key_len, std::initializer_list<Bytes> parts, uint8_t out[SHA256_SIZE]);
// HKDF-SHA256 (RFC 5869), out_len <= 255 * 32.
bool hkdf_sha256(const uint8_t *salt, size_t salt_len, const uint8_t *ikm, size_t ikm_len, const char *info,
                 uint8_t *out, size_t out_len);
bool sha256(std::initializer_list<Bytes> parts, uint8_t out[SHA256_SIZE]);
// SHA-256 over data that arrives in pieces.
class Sha256 {
 public:
  Sha256();
  ~Sha256();
  Sha256(const Sha256 &) = delete;
  Sha256 &operator=(const Sha256 &) = delete;
  void start();
  void add(const uint8_t *data, size_t len);
  void finish(uint8_t out[SHA256_SIZE]);

 private:
  void *ctx_;  // mbedtls_sha256_context
};

// AES-128-GCM. seal writes len bytes of ciphertext then the tag; open takes the same and fails
// unless the tag verifies (out holds len - GCM_TAG_SIZE bytes).
bool gcm_seal(const uint8_t key[16], const uint8_t nonce[GCM_NONCE_SIZE], const uint8_t *aad, size_t aad_len,
              const uint8_t *in, size_t len, uint8_t *out);
bool gcm_open(const uint8_t key[16], const uint8_t nonce[GCM_NONCE_SIZE], const uint8_t *aad, size_t aad_len,
              const uint8_t *in, size_t len, uint8_t *out);
// Overwrites a secret (not optimised away).
void wipe(void *p, size_t len);

}  // namespace crypto
}  // namespace flamekeeper
}  // namespace esphome
