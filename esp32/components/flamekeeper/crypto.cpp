#include "crypto.h"

#include <cstring>

#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"

namespace esphome {
namespace flamekeeper {
namespace crypto {

bool hmac_sha256(const uint8_t *key, size_t key_len, std::initializer_list<Bytes> parts, uint8_t out[SHA256_SIZE]) {
  const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  bool ok = md != nullptr && mbedtls_md_setup(&ctx, md, 1) == 0 && mbedtls_md_hmac_starts(&ctx, key, key_len) == 0;
  for (const Bytes &b : parts)
    ok = ok && (b.len == 0 || mbedtls_md_hmac_update(&ctx, b.data, b.len) == 0);
  ok = ok && mbedtls_md_hmac_finish(&ctx, out) == 0;
  mbedtls_md_free(&ctx);
  return ok;
}

bool hkdf_sha256(const uint8_t *salt, size_t salt_len, const uint8_t *ikm, size_t ikm_len, const char *info,
                 uint8_t *out, size_t out_len) {
  if (out_len > 255 * SHA256_SIZE)
    return false;
  static const uint8_t ZERO[SHA256_SIZE] = {};
  uint8_t prk[SHA256_SIZE];
  if (!hmac_sha256(salt_len != 0 ? salt : ZERO, salt_len != 0 ? salt_len : SHA256_SIZE, {{ikm, ikm_len}}, prk))
    return false;
  uint8_t t[SHA256_SIZE];
  size_t t_len = 0, done = 0;
  const Bytes info_bytes{reinterpret_cast<const uint8_t *>(info), strlen(info)};
  for (uint8_t counter = 1; done < out_len; counter++) {
    if (!hmac_sha256(prk, sizeof(prk), {{t, t_len}, info_bytes, {&counter, 1}}, t)) {
      wipe(prk, sizeof(prk));
      return false;
    }
    t_len = SHA256_SIZE;
    size_t take = out_len - done < SHA256_SIZE ? out_len - done : SHA256_SIZE;
    memcpy(out + done, t, take);
    done += take;
  }
  wipe(prk, sizeof(prk));
  wipe(t, sizeof(t));
  return true;
}

static mbedtls_sha256_context *sha_ctx(void *p) { return static_cast<mbedtls_sha256_context *>(p); }

Sha256::Sha256() : ctx_(new mbedtls_sha256_context) { mbedtls_sha256_init(sha_ctx(this->ctx_)); }
Sha256::~Sha256() {
  mbedtls_sha256_free(sha_ctx(this->ctx_));
  delete sha_ctx(this->ctx_);
}
void Sha256::start() {
  mbedtls_sha256_free(sha_ctx(this->ctx_));
  mbedtls_sha256_init(sha_ctx(this->ctx_));
  mbedtls_sha256_starts(sha_ctx(this->ctx_), 0);
}
void Sha256::add(const uint8_t *data, size_t len) {
  if (len != 0)
    mbedtls_sha256_update(sha_ctx(this->ctx_), data, len);
}
void Sha256::finish(uint8_t out[SHA256_SIZE]) { mbedtls_sha256_finish(sha_ctx(this->ctx_), out); }

bool sha256(std::initializer_list<Bytes> parts, uint8_t out[SHA256_SIZE]) {
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  bool ok = mbedtls_sha256_starts(&ctx, 0) == 0;
  for (const Bytes &b : parts)
    ok = ok && (b.len == 0 || mbedtls_sha256_update(&ctx, b.data, b.len) == 0);
  ok = ok && mbedtls_sha256_finish(&ctx, out) == 0;
  mbedtls_sha256_free(&ctx);
  return ok;
}

bool gcm_seal(const uint8_t key[16], const uint8_t nonce[GCM_NONCE_SIZE], const uint8_t *aad, size_t aad_len,
              const uint8_t *in, size_t len, uint8_t *out) {
  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);
  bool ok = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128) == 0 &&
            mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, len, nonce, GCM_NONCE_SIZE, aad, aad_len, in, out,
                                      GCM_TAG_SIZE, out + len) == 0;
  mbedtls_gcm_free(&ctx);
  return ok;
}

bool gcm_open(const uint8_t key[16], const uint8_t nonce[GCM_NONCE_SIZE], const uint8_t *aad, size_t aad_len,
              const uint8_t *in, size_t len, uint8_t *out) {
  if (len < GCM_TAG_SIZE)
    return false;
  size_t n = len - GCM_TAG_SIZE;
  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);
  bool ok = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128) == 0 &&
            mbedtls_gcm_auth_decrypt(&ctx, n, nonce, GCM_NONCE_SIZE, aad, aad_len, in + n, GCM_TAG_SIZE, in, out) == 0;
  mbedtls_gcm_free(&ctx);
  return ok;
}

void wipe(void *p, size_t len) {
  volatile uint8_t *v = static_cast<volatile uint8_t *>(p);
  while (len-- != 0)
    *v++ = 0;
}

}  // namespace crypto
}  // namespace flamekeeper
}  // namespace esphome
