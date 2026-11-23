#define _GNU_SOURCE
#include "gridd/crypto.h"
#include "gridd/log.h"

#include <openssl/bio.h>
#include <openssl/core_names.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/pem.h>
#include <openssl/pkcs7.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void log_ossl_err(const char *msg) {
    unsigned long err = ERR_get_error();
    char buf[256] = {0};
    ERR_error_string_n(err, buf, sizeof(buf));
    LOG_E("%s (%s)", msg, buf);
}

bool gridd_base64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_sz, bool url_safe) {
    if (!in || !out || out_sz == 0) return false;
    size_t needed = ((in_len + 2U) / 3U) * 4U + 1U;
    if (out_sz < needed) return false;

    int written = EVP_EncodeBlock((unsigned char *)out, in, (int)in_len);
    if (written < 0) return false;
    out[written] = '\0';

    if (url_safe) {
        size_t w = 0;
        for (int i = 0; i < written; ++i) {
            char c = out[i];
            if (c == '+') c = '-';
            else if (c == '/') c = '_';
            else if (c == '=') continue;
            out[w++] = c;
        }
        out[w] = '\0';
    }
    return true;
}

bool gridd_base64_decode(const char *in, uint8_t *out, size_t out_sz, size_t *out_len) {
    if (!in || !out || out_sz == 0) return false;
    size_t in_len = strlen(in);
    while (in_len > 0 && (in[in_len - 1] == '\n' || in[in_len - 1] == '\r' ||
                          in[in_len - 1] == ' ' || in[in_len - 1] == '\t')) {
        in_len--;
    }
    size_t pad = (4U - (in_len % 4U)) % 4U;
    if (pad == 3U) return false;

    char *norm = (char *)malloc(in_len + pad + 1U);
    if (!norm) return false;

    for (size_t i = 0; i < in_len; ++i) {
        char c = in[i];
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
        norm[i] = c;
    }
    for (size_t i = 0; i < pad; ++i) {
        norm[in_len + i] = '=';
    }
    norm[in_len + pad] = '\0';

    size_t total_len = in_len + pad;
    size_t max_decoded = (total_len / 4U) * 3U;
    if (out_sz < max_decoded) {
        free(norm);
        return false;
    }

    int dec = EVP_DecodeBlock(out, (const unsigned char *)norm, (int)total_len);
    size_t eq_count = 0;
    if (total_len >= 1U && norm[total_len - 1U] == '=') eq_count++;
    if (total_len >= 2U && norm[total_len - 2U] == '=') eq_count++;
    free(norm);
    if (dec < 0) return false;

    size_t actual = (size_t)dec;
    if (actual >= eq_count) {
        actual -= eq_count;
    }
    if (actual < out_sz) {
        out[actual] = 0;
    }
    if (out_len) *out_len = actual;
    return true;
}

bool gridd_sha256(const uint8_t *data, size_t len, uint8_t out_hash[32]) {
    if (!data || !out_hash) return false;
    unsigned int md_len = 0;
    return EVP_Digest(data, len, out_hash, &md_len, EVP_sha256(), NULL) == 1 && md_len == 32;
}

bool gridd_random_bytes(uint8_t *out, size_t len) {
    if (!out || len == 0) return false;
    return RAND_bytes(out, (int)len) == 1;
}

bool gridd_pkce_generate(char *verifier_out, size_t verifier_sz, char *challenge_out, size_t challenge_sz) {
    for (int attempt = 0; attempt < 64; ++attempt) {
        uint8_t raw[32];
        if (!gridd_random_bytes(raw, sizeof(raw))) return false;
        if (!gridd_base64_encode(raw, sizeof(raw), verifier_out, verifier_sz, true)) return false;

        uint8_t digest[32];
        if (!gridd_sha256((const uint8_t *)verifier_out, strlen(verifier_out), digest)) return false;
        if (!gridd_base64_encode(digest, sizeof(digest), challenge_out, challenge_sz, true)) return false;

        if (strchr(challenge_out, '-') == NULL && strchr(challenge_out, '_') == NULL) {
            return true;
        }
    }
    return true;
}

bool gridd_hkdf_sha256(const uint8_t *ikm, size_t ikm_len,
                       const uint8_t *salt, size_t salt_len,
                       const uint8_t *info, size_t info_len,
                       uint8_t *okm, size_t okm_len) {
    EVP_KDF *kdf = EVP_KDF_fetch(NULL, "HKDF", NULL);
    if (!kdf) return false;
    EVP_KDF_CTX *kctx = EVP_KDF_CTX_new(kdf);
    EVP_KDF_free(kdf);
    if (!kctx) return false;

    char digest_name[] = "SHA256";
    OSSL_PARAM params[5];
    size_t idx = 0;
    params[idx++] = OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, digest_name, 0);
    params[idx++] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY, (void *)ikm, ikm_len);
    if (salt && salt_len > 0) {
        params[idx++] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, (void *)salt, salt_len);
    }
    if (info && info_len > 0) {
        params[idx++] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO, (void *)info, info_len);
    }
    params[idx] = OSSL_PARAM_construct_end();

    bool ok = (EVP_KDF_derive(kctx, okm, okm_len, params) == 1);
    EVP_KDF_CTX_free(kctx);
    return ok;
}

bool gridd_hmac_sha256(const uint8_t *key, size_t key_len,
                       const uint8_t *data, size_t data_len,
                       uint8_t out_mac[32]) {
    EVP_MAC *mac = EVP_MAC_fetch(NULL, "HMAC", NULL);
    if (!mac) return false;
    EVP_MAC_CTX *ctx = EVP_MAC_CTX_new(mac);
    EVP_MAC_free(mac);
    if (!ctx) return false;

    char digest_name[] = "SHA256";
    OSSL_PARAM params[2];
    params[0] = OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, digest_name, 0);
    params[1] = OSSL_PARAM_construct_end();

    size_t out_len = 0;
    bool ok = (EVP_MAC_init(ctx, key, key_len, params) == 1 &&
               EVP_MAC_update(ctx, data, data_len) == 1 &&
               EVP_MAC_final(ctx, out_mac, &out_len, 32) == 1 &&
               out_len == 32);
    EVP_MAC_CTX_free(ctx);
    return ok;
}

bool gridd_aes256_gcm_encrypt(const uint8_t key[GRIDD_AES_KEY_LEN],
                              const uint8_t iv[GRIDD_GCM_IV_LEN],
                              const uint8_t *plaintext, size_t pt_len,
                              uint8_t *ciphertext_out,
                              uint8_t tag_out[GRIDD_GCM_TAG_LEN]) {
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        log_ossl_err("Failed to initialize context");
        return false;
    }

    int len = 0;
    int total = 0;
    bool ok = false;

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) {
        log_ossl_err("Failed to initialize encryption operation");
        goto cleanup;
    }
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, GRIDD_GCM_IV_LEN, NULL) != 1) {
        log_ossl_err("Failed to set initialization vector length");
        goto cleanup;
    }
    if (EVP_EncryptInit_ex(ctx, NULL, NULL, key, iv) != 1) {
        log_ossl_err("Failed to initialize key and initialization vector");
        goto cleanup;
    }
    if (EVP_EncryptUpdate(ctx, ciphertext_out, &len, plaintext, (int)pt_len) != 1) {
        log_ossl_err("Failed to update the message to be encrypted");
        goto cleanup;
    }
    total = len;
    if (EVP_EncryptFinal_ex(ctx, ciphertext_out + total, &len) != 1) {
        log_ossl_err("Failed to encrypt message");
        goto cleanup;
    }
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, GRIDD_GCM_TAG_LEN, tag_out) != 1) {
        log_ossl_err("Failed to get the tag");
        goto cleanup;
    }
    ok = true;

cleanup:
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

bool gridd_aes256_gcm_decrypt(const uint8_t key[GRIDD_AES_KEY_LEN],
                              const uint8_t iv[GRIDD_GCM_IV_LEN],
                              const uint8_t *ciphertext, size_t ct_len,
                              const uint8_t tag[GRIDD_GCM_TAG_LEN],
                              uint8_t *plaintext_out) {
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        log_ossl_err("Failed to initialize context");
        return false;
    }

    int len = 0;
    bool ok = false;

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) {
        log_ossl_err("Failed to initialize decryption operation");
        goto cleanup;
    }
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, GRIDD_GCM_IV_LEN, NULL) != 1) {
        log_ossl_err("Failed to set initialization vector length");
        goto cleanup;
    }
    if (EVP_DecryptInit_ex(ctx, NULL, NULL, key, iv) != 1) {
        log_ossl_err("Failed to initialize key and initialization vector");
        goto cleanup;
    }
    if (EVP_DecryptUpdate(ctx, plaintext_out, &len, ciphertext, (int)ct_len) != 1) {
        log_ossl_err("Failed to update the message to be decrypted");
        goto cleanup;
    }
    int total = len;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, GRIDD_GCM_TAG_LEN, (void *)tag) != 1) {
        log_ossl_err("Failed to set expected tag");
        goto cleanup;
    }
    if (EVP_DecryptFinal_ex(ctx, plaintext_out + total, &len) != 1) {
        log_ossl_err("Failed to decrypt the message");
        goto cleanup;
    }
    total += len;
    plaintext_out[total] = '\0';
    ok = true;

cleanup:
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

bool gridd_jwt_split(const char *jwt,
                     char *header_b64, size_t header_sz,
                     char *payload_b64, size_t payload_sz,
                     char *sig_b64, size_t sig_sz) {
    if (!jwt || !header_b64 || !payload_b64 || !sig_b64) return false;
    const char *dot1 = strchr(jwt, '.');
    if (!dot1) return false;
    const char *dot2 = strchr(dot1 + 1, '.');
    if (!dot2 || strchr(dot2 + 1, '.') != NULL) return false;

    size_t hlen = (size_t)(dot1 - jwt);
    size_t plen = (size_t)(dot2 - (dot1 + 1));
    size_t slen = strlen(dot2 + 1);
    while (slen > 0 && (dot2[1 + slen - 1] == '\n' || dot2[1 + slen - 1] == '\r' ||
                        dot2[1 + slen - 1] == ' ')) {
        slen--;
    }

    if (hlen == 0 || plen == 0 || slen == 0) return false;
    if (hlen >= header_sz || plen >= payload_sz || slen >= sig_sz) return false;

    memcpy(header_b64, jwt, hlen);
    header_b64[hlen] = '\0';
    memcpy(payload_b64, dot1 + 1, plen);
    payload_b64[plen] = '\0';
    memcpy(sig_b64, dot2 + 1, slen);
    sig_b64[slen] = '\0';
    return true;
}

bool gridd_verify_pem_signature(const char *pem_pubkey,
                                const uint8_t *message, size_t msg_len,
                                const uint8_t *sig, size_t sig_len) {
    if (!pem_pubkey || !message || !sig) return false;

    BIO *bio = BIO_new_mem_buf(pem_pubkey, -1);
    if (!bio) {
        log_ossl_err("Failed to create key BIO");
        return false;
    }

    EVP_PKEY *pkey = PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL);
    BIO_free(bio);
    if (!pkey) {
        log_ossl_err("Failed to read public key");
        return false;
    }

    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    if (!mdctx) {
        EVP_PKEY_free(pkey);
        log_ossl_err("Failed to allocate and initialize digest context");
        return false;
    }

    bool ok = false;
    if (EVP_DigestVerifyInit(mdctx, NULL, EVP_sha256(), NULL, pkey) != 1) {
        log_ossl_err("Failed to set up verification context");
    } else if (EVP_DigestVerifyUpdate(mdctx, message, msg_len) != 1) {
        log_ossl_err("Failed to update the verification context with message digest");
    } else if (EVP_DigestVerifyFinal(mdctx, sig, sig_len) != 1) {
        log_ossl_err("Failed to verify signature");
    } else {
        ok = true;
    }

    EVP_MD_CTX_free(mdctx);
    EVP_PKEY_free(pkey);
    return ok;
}

bool gridd_verify_pkcs7_pem(const char *pkcs7_pem,
                            const char *ca_cert_pem,
                            char *verified_content_out,
                            size_t out_sz) {
    if (!pkcs7_pem || !verified_content_out || out_sz == 0) return false;
    verified_content_out[0] = '\0';

    X509 *cert = NULL;
    X509_STORE *store = NULL;
    STACK_OF(X509) *certs = NULL;
    BIO *p7_bio = NULL;
    PKCS7 *p7 = NULL;
    BIO *out_bio = NULL;
    bool ok = false;

    if (ca_cert_pem && *ca_cert_pem) {
        BIO *cbio = BIO_new_mem_buf(ca_cert_pem, -1);
        if (!cbio) {
            log_ossl_err("Failed to create BIO");
            goto cleanup;
        }
        cert = PEM_read_bio_X509(cbio, NULL, NULL, NULL);
        BIO_free(cbio);
        if (!cert) {
            log_ossl_err("Failed to read certificate from BIO");
            goto cleanup;
        }
        store = X509_STORE_new();
        if (!store || X509_STORE_add_cert(store, cert) != 1) {
            log_ossl_err("Failed to add certificate to the certificate store");
            goto cleanup;
        }
        certs = sk_X509_new_null();
        if (!certs || sk_X509_push(certs, cert) == 0) {
            log_ossl_err("Failed to create cert stack object");
            goto cleanup;
        }
    }

    p7_bio = BIO_new_mem_buf(pkcs7_pem, -1);
    if (!p7_bio) goto cleanup;
    p7 = PEM_read_bio_PKCS7(p7_bio, NULL, NULL, NULL);
    if (!p7) {
        log_ossl_err("Failed to read signature from BIO");
        goto cleanup;
    }

    out_bio = BIO_new(BIO_s_mem());
    if (!out_bio) goto cleanup;

    int flags = ca_cert_pem ? 0 : PKCS7_NOVERIFY;
    if (PKCS7_verify(p7, certs, store, NULL, out_bio, flags) != 1) {
        log_ossl_err("Failed to verify instance identity signature");
        goto cleanup;
    }

    BUF_MEM *bptr = NULL;
    BIO_get_mem_ptr(out_bio, &bptr);
    if (bptr && bptr->length < out_sz) {
        memcpy(verified_content_out, bptr->data, bptr->length);
        verified_content_out[bptr->length] = '\0';
        ok = true;
    }

cleanup:
    if (out_bio) BIO_free(out_bio);
    if (p7) PKCS7_free(p7);
    if (p7_bio) BIO_free(p7_bio);
    if (certs) sk_X509_free(certs);
    if (store) X509_STORE_free(store);
    if (cert) X509_free(cert);
    return ok;
}
