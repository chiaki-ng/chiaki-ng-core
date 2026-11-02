/*
 * micro-ecc configuration for chiaki-ng
 *
 * This file configures micro-ecc for use in chiaki-ng's libnx crypto backend.
 */

#ifndef UECC_CONFIG_H
#define UECC_CONFIG_H

/* Only secp256k1 for PSN. Guarded so a translation unit can select a different
   curve set before including this - uECC_p521.c does, to keep its larger
   uECC_MAX_WORDS out of the secp256k1 build. */
#ifndef uECC_SUPPORTS_secp160r1
#define uECC_SUPPORTS_secp160r1 0
#endif
#ifndef uECC_SUPPORTS_secp192r1
#define uECC_SUPPORTS_secp192r1 0
#endif
#ifndef uECC_SUPPORTS_secp224r1
#define uECC_SUPPORTS_secp224r1 0
#endif
#ifndef uECC_SUPPORTS_secp256r1
#define uECC_SUPPORTS_secp256r1 0
#endif
#ifndef uECC_SUPPORTS_secp256k1
#define uECC_SUPPORTS_secp256k1 1
#endif

#ifndef uECC_SUPPORT_COMPRESSED_POINT
#define uECC_SUPPORT_COMPRESSED_POINT 0
#endif

#ifndef uECC_OPTIMIZATION_LEVEL
#define uECC_OPTIMIZATION_LEVEL 2
#endif

#endif /* UECC_CONFIG_H */
