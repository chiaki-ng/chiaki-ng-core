/* micro-ecc secp521r1 instantiation, vendored from the chiaki-ng Switch fork's
 * libnx crypto backend (lib/src/crypto/libnx/microecc). Built on micro-ecc,
 * Copyright 2014, Kenneth MacKay. Licensed under the BSD 2-clause license. */

/* Second instantiation of micro-ecc, configured for secp521r1 only.
 *
 * uECC_MAX_WORDS is a single compile-time constant shared by every curve in a
 * translation unit: it sizes uECC_Curve_t's arrays and every scratch buffer in
 * the implementation. Enabling secp521r1 alongside secp256k1 therefore changes
 * the code generated for secp256k1, which broke key generation on aarch64.
 *
 * Building the two in separate translation units keeps each curve's sizing to
 * itself. uECC.c stays configured exactly as it was before secp521r1 existed,
 * so the secp256k1 path is unchanged; this unit carries secp521r1 on its own.
 * Only the public API is renamed - everything else in uECC.c is static.
 */

#define uECC_SUPPORTS_secp160r1 0
#define uECC_SUPPORTS_secp192r1 0
#define uECC_SUPPORTS_secp224r1 0
#define uECC_SUPPORTS_secp256r1 0
#define uECC_SUPPORTS_secp256k1 0
#define uECC_SUPPORTS_secp521r1 1

#define uECC_set_rng               uECC_p521_set_rng
#define uECC_get_rng               uECC_p521_get_rng
#define uECC_sign_with_k           uECC_p521_sign_with_k
#define uECC_curve_private_key_size uECC_p521_curve_private_key_size
#define uECC_curve_public_key_size uECC_p521_curve_public_key_size
#define uECC_make_key              uECC_p521_make_key
#define uECC_shared_secret         uECC_p521_shared_secret
#define uECC_compute_public_key    uECC_p521_compute_public_key
#define uECC_valid_public_key      uECC_p521_valid_public_key
#define uECC_sign                  uECC_p521_sign
#define uECC_sign_deterministic    uECC_p521_sign_deterministic
#define uECC_verify                uECC_p521_verify
#define uECC_compress              uECC_p521_compress
#define uECC_decompress            uECC_p521_decompress
#define uECC_secp521r1             uECC_p521_secp521r1

#define init_SHA256                uECC_p521_init_SHA256
#define update_SHA256              uECC_p521_update_SHA256
#define finish_SHA256              uECC_p521_finish_SHA256

#include "uECC.c"
