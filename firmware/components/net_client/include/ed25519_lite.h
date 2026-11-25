#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Computes SHA-256 digest of `data[0..len]` into `out[32]`.
void ez_sha256(const uint8_t* data, size_t len, uint8_t out[32]);

/// Computes SHA-512 digest of `data[0..len]` into `out[64]`.
void ez_sha512(const uint8_t* data, size_t len, uint8_t out[64]);

/// Derives RFC 8032 Ed25519 32-byte public key from 32-byte private `seed`.
void ez_ed25519_public_key(const uint8_t seed[32], uint8_t out_pub[32]);

/// Signs `msg[0..msg_len]` using RFC 8032 Ed25519 (SHA-512) into 64-byte `out_sig`.
void ez_ed25519_sign(const uint8_t seed[32],
                     const uint8_t pub[32],
                     const uint8_t* msg,
                     size_t msg_len,
                     uint8_t out_sig[64]);

/// Encodes `in[0..in_len]` as unpadded base64url (URL_SAFE_NO_PAD) + NUL terminator.
/// Returns number of characters written (excluding NUL), or 0 if `out_cap` is too small.
size_t ez_base64url_encode(const uint8_t* in, size_t in_len, char* out, size_t out_cap);

/// Formats `SHA256(pub)[0..4]` as `"XXXX-XXXX"` uppercase fingerprint into `out[10]`.
void ez_public_key_fingerprint(const uint8_t pub[32], char out[10]);

#ifdef __cplusplus
}
#endif
