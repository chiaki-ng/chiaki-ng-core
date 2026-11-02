/* micro-ecc secp521r1 instantiation, vendored from the chiaki-ng Switch fork's
 * libnx crypto backend (lib/src/crypto/libnx/microecc). Built on micro-ecc,
 * Copyright 2014, Kenneth MacKay. Licensed under the BSD 2-clause license. */

/* Public API of the secp521r1-only micro-ecc instantiation in uECC_p521.c.
 * Kept separate from uECC.h so the two instantiations never share sizing. */

#ifndef _UECC_P521_H_
#define _UECC_P521_H_

#include <stdint.h>

struct uECC_Curve_t;

#ifdef __cplusplus
extern "C" {
#endif

const struct uECC_Curve_t *uECC_p521_secp521r1(void);

void uECC_p521_set_rng(int (*rng_function)(uint8_t *dest, unsigned size));
int uECC_p521_curve_private_key_size(const struct uECC_Curve_t *curve);
int uECC_p521_curve_public_key_size(const struct uECC_Curve_t *curve);
int uECC_p521_make_key(uint8_t *public_key, uint8_t *private_key, const struct uECC_Curve_t *curve);
int uECC_p521_shared_secret(const uint8_t *public_key, const uint8_t *private_key, uint8_t *secret, const struct uECC_Curve_t *curve);
int uECC_p521_compute_public_key(const uint8_t *private_key, uint8_t *public_key, const struct uECC_Curve_t *curve);
int uECC_p521_valid_public_key(const uint8_t *public_key, const struct uECC_Curve_t *curve);

#ifdef __cplusplus
}
#endif

#endif /* _UECC_P521_H_ */
