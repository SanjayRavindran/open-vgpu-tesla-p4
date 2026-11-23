#ifndef GRIDD_CRYPTO_H
#define GRIDD_CRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GRIDD_AES_KEY_LEN 32
#define GRIDD_GCM_IV_LEN 12
#define GRIDD_GCM_TAG_LEN 16

bool gridd_base64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_sz, bool url_safe);
bool gridd_base64_decode(const char *in, uint8_t *out, size_t out_sz, size_t *out_len);

bool gridd_sha256(const uint8_t *data, size_t len, uint8_t out_hash[32]);
bool gridd_random_bytes(uint8_t *out, size_t len);
bool gridd_pkce_generate(char *verifier_out, size_t verifier_sz, char *challenge_out, size_t challenge_sz);

bool gridd_hkdf_sha256(const uint8_t *ikm, size_t ikm_len,
                       const uint8_t *salt, size_t salt_len,
                       const uint8_t *info, size_t info_len,
                       uint8_t *okm, size_t okm_len);

bool gridd_hmac_sha256(const uint8_t *key, size_t key_len,
                       const uint8_t *data, size_t data_len,
                       uint8_t out_mac[32]);

bool gridd_aes256_gcm_encrypt(const uint8_t key[GRIDD_AES_KEY_LEN],
                              const uint8_t iv[GRIDD_GCM_IV_LEN],
                              const uint8_t *plaintext, size_t pt_len,
                              uint8_t *ciphertext_out,
                              uint8_t tag_out[GRIDD_GCM_TAG_LEN]);

bool gridd_aes256_gcm_decrypt(const uint8_t key[GRIDD_AES_KEY_LEN],
                              const uint8_t iv[GRIDD_GCM_IV_LEN],
                              const uint8_t *ciphertext, size_t ct_len,
                              const uint8_t tag[GRIDD_GCM_TAG_LEN],
                              uint8_t *plaintext_out);

bool gridd_jwt_split(const char *jwt,
                     char *header_b64, size_t header_sz,
                     char *payload_b64, size_t payload_sz,
                     char *sig_b64, size_t sig_sz);

bool gridd_verify_pem_signature(const char *pem_pubkey,
                                const uint8_t *message, size_t msg_len,
                                const uint8_t *sig, size_t sig_len);

bool gridd_verify_pkcs7_pem(const char *pkcs7_pem,
                            const char *ca_cert_pem,
                            char *verified_content_out,
                            size_t out_sz);

#ifdef __cplusplus
}
#endif

#endif /* GRIDD_CRYPTO_H */
