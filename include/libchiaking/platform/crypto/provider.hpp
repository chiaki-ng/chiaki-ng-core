/// @file
/// @brief Declares CryptoProvider, the cryptographic primitives the library
/// asks a host to supply, and the sizes they work with.
#pragma once

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/platform/crypto/error.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace libchiaking::platform {

/// @brief Size in bytes of a SHA-256 digest.
inline constexpr std::size_t sha256_digest_size = 32;
/// @brief Size in bytes of an HMAC-SHA-256 tag.
inline constexpr std::size_t hmac_sha256_digest_size = 32;
/// @brief Size in bytes of an AES-128 key.
inline constexpr std::size_t aes128_key_size = 16;
/// @brief Size in bytes of one AES block.
inline constexpr std::size_t aes128_block_size = 16;
/// @brief Size in bytes of a full-length AES-128-GCM tag.
inline constexpr std::size_t aes128_gcm_tag_size = 16;
/// @brief Shortest nonce, in bytes, CryptoProvider::aes128_gmac must accept.
inline constexpr std::size_t aes128_gcm_iv_size_min = 12;
/// @brief Longest nonce, in bytes, CryptoProvider::aes128_gmac must accept.
inline constexpr std::size_t aes128_gcm_iv_size_max = 16;
/// @brief Shortest truncated tag, in bytes, CryptoProvider::aes128_gmac must
/// produce.
inline constexpr std::size_t aes128_gcm_tag_size_min = 4;
/// @brief Longest tag, in bytes, CryptoProvider::aes128_gmac must produce.
inline constexpr std::size_t aes128_gcm_tag_size_max = aes128_gcm_tag_size;
/// @brief Size in bytes of a SHA-512 digest.
inline constexpr std::size_t sha512_digest_size = 64;
/// @brief Size in bytes of an HMAC-SHA-512 tag.
inline constexpr std::size_t hmac_sha512_digest_size = 64;
/// @brief Size in bytes of an HMAC-SHA-1 tag.
inline constexpr std::size_t hmac_sha1_digest_size = 20;
/// @brief Size in bytes of an Ed25519 private seed.
inline constexpr std::size_t ed25519_seed_size = 32;
/// @brief Size in bytes of an Ed25519 public key.
inline constexpr std::size_t ed25519_public_key_size = 32;
/// @brief Size in bytes of an Ed25519 signature.
inline constexpr std::size_t ed25519_signature_size = 64;
/// @brief Size in bytes of a P-256 scalar, encoded big-endian.
inline constexpr std::size_t p256_scalar_size = 32;
/// @brief Size in bytes of a P-256 point in uncompressed SEC1 form: the tag
/// byte, then X, then Y.
inline constexpr std::size_t p256_point_size = 65;
/// @brief First byte of an uncompressed SEC1 point.
inline constexpr std::uint8_t sec1_uncompressed_tag = 0x04;
/// @brief Number of tag bytes that precede the coordinates of an uncompressed
/// SEC1 point.
inline constexpr std::size_t sec1_uncompressed_point_prefix = 1;
/// @brief Size in bytes of a secp256k1 public key as raw X then Y coordinates.
inline constexpr std::size_t secp256k1_public_coordinates_size = 64;
/// @brief Size in bytes of a secp521r1 public key as raw X then Y coordinates.
inline constexpr std::size_t secp521r1_public_coordinates_size = 132;
/// @brief Size in bytes of a secp256r1 (P-256) public key as raw X then Y
/// coordinates.
inline constexpr std::size_t secp256r1_public_coordinates_size = 64;
/// @brief Size in bytes of a secp256k1 ECDH shared secret.
inline constexpr std::size_t secp256k1_shared_secret_size = 32;
/// @brief Size in bytes of a secp521r1 ECDH shared secret.
inline constexpr std::size_t secp521r1_shared_secret_size = 66;
/// @brief Size in bytes of a secp256r1 (P-256) ECDH shared secret.
inline constexpr std::size_t secp256r1_shared_secret_size = 32;

/// @brief Work factors for CryptoProvider::scrypt.
struct ScryptParameters {
  /// @brief CPU and memory cost N. The stock provider requires a power of two
  /// of at least 2.
  std::uint64_t cost{};
  /// @brief Block size r. Must not be zero.
  std::uint32_t block_size{};
  /// @brief Parallelisation factor p. Must not be zero.
  std::uint32_t parallelization{};
};

/// @brief Hash function an HKDF derivation is built on.
enum class HkdfDigest : std::uint8_t {
  /// @brief HKDF-SHA-256; a block is 32 bytes and the output may be up to
  ///     8160 bytes.
  sha256,
  /// @brief HKDF-SHA-512; a block is 64 bytes and the output may be up to
  ///     16320 bytes.
  sha512,
};

/// @brief Size of one block of an HKDF derivation, which is the digest size of
/// its hash.
///
/// @param digest Hash the derivation uses.
/// @return The size in bytes, or 0 for a value that names no hash.
constexpr auto hkdf_block_size(HkdfDigest digest) noexcept -> std::size_t {
  switch (digest) {
    case HkdfDigest::sha256:
      return hmac_sha256_digest_size;
    case HkdfDigest::sha512:
      return hmac_sha512_digest_size;
  }
  return 0;
}

/// @brief Most blocks one HKDF derivation can produce, as RFC 5869 limits it.
inline constexpr std::size_t hkdf_max_blocks = 255;

/// @brief Elliptic curve an ECDH key pair is generated on.
enum class EcdhCurve : std::uint8_t {
  /// @brief The secp256k1 curve; 64-byte coordinates and a 32-byte secret.
  secp256k1,
  /// @brief The secp521r1 curve; 132-byte coordinates and a 66-byte secret.
  secp521r1,
  /// @brief The secp256r1 (NIST P-256) curve; 64-byte coordinates and a
  /// 32-byte secret.
  secp256r1,
};

/// @brief Size of a public key on a curve as raw fixed-width X then Y
/// coordinates.
///
/// @param curve Curve to size for.
/// @return The size in bytes, or 0 for a value that names no curve.
constexpr auto ecdh_public_coordinates_size(EcdhCurve curve) noexcept
    -> std::size_t {
  switch (curve) {
    case EcdhCurve::secp256k1:
      return secp256k1_public_coordinates_size;
    case EcdhCurve::secp521r1:
      return secp521r1_public_coordinates_size;
    case EcdhCurve::secp256r1:
      return secp256r1_public_coordinates_size;
  }
  return 0;
}

/// @brief Size of the ECDH shared secret a curve produces.
///
/// @param curve Curve to size for.
/// @return The size in bytes, or 0 for a value that names no curve.
constexpr auto ecdh_shared_secret_size(EcdhCurve curve) noexcept
    -> std::size_t {
  switch (curve) {
    case EcdhCurve::secp256k1:
      return secp256k1_shared_secret_size;
    case EcdhCurve::secp521r1:
      return secp521r1_shared_secret_size;
    case EcdhCurve::secp256r1:
      return secp256r1_shared_secret_size;
  }
  return 0;
}

/// @brief Largest public coordinate size of any supported curve, for sizing
/// fixed buffers.
inline constexpr std::size_t max_ecdh_public_coordinates_size =
    ecdh_public_coordinates_size(EcdhCurve::secp521r1);
/// @brief Largest shared secret size of any supported curve, for sizing fixed
/// buffers.
inline constexpr std::size_t max_ecdh_shared_secret_size =
    ecdh_shared_secret_size(EcdhCurve::secp521r1);

/// @brief An ephemeral ECDH key pair held by the provider that generated it.
///
/// Returned by CryptoProvider::generate_ecdh. The private key never leaves the
/// object; the library owns the object and destroys it when the key exchange is
/// over. The library adds the SEC1 tag byte and authenticates the public key
/// itself, so implementations deal only in raw coordinates.
class EcdhKeyPair {
 public:
  /// @brief Destroys the key pair. Implementations should erase the private
  /// key.
  virtual ~EcdhKeyPair() = default;
  /// @brief Constructs the base of a provider's key pair type.
  EcdhKeyPair() = default;
  /// @brief Not copyable; a private key has one owner.
  EcdhKeyPair(const EcdhKeyPair&) = delete;
  /// @brief Not copy-assignable; a private key has one owner.
  EcdhKeyPair& operator=(const EcdhKeyPair&) = delete;

  /// @brief Writes this pair's public key as raw fixed-width X then Y
  /// coordinates.
  ///
  /// Blocks until the result is written. The library calls one pair
  /// from one thread at a time.
  ///
  /// @param output Destination of exactly ecdh_public_coordinates_size()
  ///     bytes for the pair's curve. Left untouched when the size is wrong.
  /// @return Nothing on success.
  /// @retval CryptoError::invalid_argument @p output has the wrong size.
  /// @retval CryptoError::backend_failure The key could not be exported.
  [[nodiscard]] virtual auto public_coordinates(
      std::span<std::byte> output) const -> Expected<void, CryptoError> = 0;
  /// @brief Computes the ECDH shared secret with a peer's public key.
  ///
  /// Blocks until the result is written. The library calls one pair
  /// from one thread at a time.
  ///
  /// @param peer_coordinates The peer's public key as raw X then Y
  ///     coordinates, exactly ecdh_public_coordinates_size() bytes. It must
  ///     encode a valid point on the pair's curve.
  /// @param secret Destination of exactly ecdh_shared_secret_size() bytes.
  ///     Left unchanged on failure.
  /// @return Nothing on success.
  /// @retval CryptoError::invalid_argument A span has the wrong size or the
  ///     peer point is not on the curve.
  /// @retval CryptoError::backend_failure The derivation failed.
  [[nodiscard]] virtual auto derive_secret(
      std::span<const std::byte> peer_coordinates,
      std::span<std::byte> secret) const -> Expected<void, CryptoError> = 0;
};

/// @brief One AES-128 key prepared for repeated use.
///
/// Made by CryptoProvider::aes128_cipher. The key schedule and the GHASH key
/// are computed once, when it is made, so per-packet encryption and
/// authentication repeat none of that work. The key stays inside the object
/// and is wiped when it is destroyed. Both operations are const and may be
/// called from several threads at once.
class Aes128Cipher {
 public:
  virtual ~Aes128Cipher() = default;

  /// @brief Encrypts whole blocks in ECB form with the prepared key.
  ///
  /// @param blocks A multiple of aes128_block_size bytes.
  /// @param output Destination of the same size; it may not overlap blocks
  ///     unless it is the same span.
  /// @return Nothing on success.
  /// @retval CryptoError::invalid_argument The sizes do not match or are not
  ///     whole blocks.
  /// @retval CryptoError::backend_failure The backend failed.
  [[nodiscard]] virtual auto encrypt_blocks(std::span<const std::byte> blocks,
                                            std::span<std::byte> output) const
      -> Expected<void, CryptoError> = 0;

  /// @brief Computes an AES-GCM tag over additional data only, with the
  /// prepared key, as CryptoProvider::aes128_gmac does.
  ///
  /// @param iv Nonce of aes128_gcm_iv_size_min to aes128_gcm_iv_size_max
  ///     bytes.
  /// @param aad Data to authenticate. May be empty.
  /// @param tag Destination of aes128_gcm_tag_size_min to
  ///     aes128_gcm_tag_size_max bytes; a shorter destination receives the
  ///     leading bytes of the full tag.
  /// @return Nothing on success.
  /// @retval CryptoError::invalid_argument The nonce or tag size is out of
  ///     range.
  /// @retval CryptoError::backend_failure The backend failed.
  [[nodiscard]] virtual auto gmac(std::span<const std::byte> iv,
                                  std::span<const std::byte> aad,
                                  std::span<std::byte> tag) const
      -> Expected<void, CryptoError> = 0;
};

/// @brief The cryptographic primitives the library needs from the host.
///
/// Implemented by the host frontend, or by one of the stock providers,
/// openssl::OpenSslCryptoProvider and boringssl::BoringSslCryptoProvider. The
/// library reaches it through PlatformFactory::crypto and borrows it for as
/// long as anything built from that factory exists.
///
/// One instance serves every component built from the same factory, including
/// components that run their own threads, so an implementation should be safe
/// to call from several threads at once. Calls are synchronous and write their
/// result into caller-owned spans; nothing passed in may be retained after the
/// call returns.
///
/// sha256(), hmac_sha256(), aes128_encrypt_block() and aes128_gmac() are
/// mandatory and are what crypto_conformance::run() checks, together with
/// hkdf(), whose default is built on the HMACs. Every implementation must
/// reject spans of the wrong size with CryptoError::invalid_argument. The
/// remaining primitives default to CryptoError::unsupported_capability; a
/// provider that leaves them out loses the features that need them, such as
/// Kakusan.
/// @ingroup api_platform
class CryptoProvider {
 public:
  /// @brief Destroys the provider. The library never deletes a provider it
  /// was given.
  virtual ~CryptoProvider() = default;

  /// @brief Computes the SHA-256 digest of a message.
  ///
  /// Blocks until the result is written. Implementations must accept
  /// calls from several threads at once.
  ///
  /// @param input Message to hash. May be empty.
  /// @param digest Destination of exactly sha256_digest_size bytes.
  /// @return Nothing on success.
  /// @retval CryptoError::invalid_argument @p digest has the wrong size.
  /// @retval CryptoError::backend_failure The backend failed.
  [[nodiscard]] virtual auto sha256(std::span<const std::byte> input,
                                    std::span<std::byte> digest)
      -> Expected<void, CryptoError> = 0;

  /// @brief Computes the HMAC-SHA-256 tag of a message.
  ///
  /// Blocks until the result is written. Implementations must accept
  /// calls from several threads at once.
  ///
  /// @param key Secret key of any length.
  /// @param input Message to authenticate.
  /// @param digest Destination of exactly hmac_sha256_digest_size bytes.
  /// @return Nothing on success.
  /// @retval CryptoError::invalid_argument @p digest has the wrong size.
  /// @retval CryptoError::backend_failure The backend failed.
  [[nodiscard]] virtual auto hmac_sha256(std::span<const std::byte> key,
                                         std::span<const std::byte> input,
                                         std::span<std::byte> digest)
      -> Expected<void, CryptoError> = 0;

  /// @brief Encrypts one block with the raw AES-128 block cipher, with no
  /// chaining mode or padding.
  ///
  /// Blocks until the result is written. Implementations must accept
  /// calls from several threads at once.
  ///
  /// @param key Exactly aes128_key_size bytes.
  /// @param block Exactly aes128_block_size bytes of plaintext.
  /// @param output Destination of exactly aes128_block_size bytes.
  /// @return Nothing on success.
  /// @retval CryptoError::invalid_argument A span has the wrong size.
  /// @retval CryptoError::backend_failure The backend failed.
  [[nodiscard]] virtual auto aes128_encrypt_block(
      std::span<const std::byte> key, std::span<const std::byte> block,
      std::span<std::byte> output) -> Expected<void, CryptoError> = 0;

  /// @brief Encrypts several independent blocks with the raw AES-128 block
  /// cipher.
  ///
  /// Each block is encrypted on its own, exactly as aes128_encrypt_block()
  /// would. The default implementation calls aes128_encrypt_block() once per
  /// block; a provider overrides it to schedule the key once.
  ///
  /// Blocks until the result is written. Implementations must accept
  /// calls from several threads at once.
  ///
  /// @param key Exactly aes128_key_size bytes.
  /// @param blocks Plaintext whose length is a multiple of aes128_block_size.
  ///     May be empty.
  /// @param output Destination of the same length as @p blocks.
  /// @return Nothing on success.
  /// @retval CryptoError::invalid_argument @p blocks is not a whole number of
  ///     blocks, @p output differs in length, or the key has the wrong size.
  /// @retval CryptoError::backend_failure The backend failed.
  [[nodiscard]] virtual auto aes128_encrypt_blocks(
      std::span<const std::byte> key, std::span<const std::byte> blocks,
      std::span<std::byte> output) -> Expected<void, CryptoError> {
    constexpr std::size_t block_size = aes128_block_size;
    if (blocks.size() % block_size != 0 || output.size() != blocks.size())
      return libchiaking::unexpected(CryptoError::invalid_argument);
    for (std::size_t offset = 0; offset < blocks.size(); offset += block_size) {
      auto encrypted =
          aes128_encrypt_block(key, blocks.subspan(offset, block_size),
                               output.subspan(offset, block_size));
      if (!encrypted) return encrypted;
    }
    return {};
  }

  /// @brief Computes an AES-128-GCM authentication tag over data that is
  /// authenticated but not encrypted.
  ///
  /// Blocks until the result is written. Implementations must accept
  /// calls from several threads at once.
  ///
  /// @param key Exactly aes128_key_size bytes.
  /// @param iv Nonce of aes128_gcm_iv_size_min to aes128_gcm_iv_size_max
  ///     bytes.
  /// @param aad Data to authenticate. May be empty.
  /// @param tag Destination of aes128_gcm_tag_size_min to
  ///     aes128_gcm_tag_size_max bytes. A shorter destination receives the
  ///     leading bytes of the full tag.
  /// @return Nothing on success.
  /// @retval CryptoError::invalid_argument The key, nonce or tag size is out
  ///     of range.
  /// @retval CryptoError::backend_failure The backend failed.
  [[nodiscard]] virtual auto aes128_gmac(std::span<const std::byte> key,
                                         std::span<const std::byte> iv,
                                         std::span<const std::byte> aad,
                                         std::span<std::byte> tag)
      -> Expected<void, CryptoError> = 0;

  /// @brief Generates a fresh ECDH key pair on the given curve.
  ///
  /// The library asks for a pair before it starts a key exchange with a
  /// console, so a provider without ECDH must report that here and not return
  /// a partial implementation.
  ///
  /// Blocks until the result is written. Implementations must accept
  /// calls from several threads at once.
  ///
  /// @param curve Curve to generate on.
  /// @return The key pair, owned by the caller.
  /// @retval CryptoError::unsupported_capability The provider has no ECDH.
  ///     This is the default.
  /// @retval CryptoError::invalid_argument @p curve names no supported curve.
  /// @retval CryptoError::backend_failure Key generation failed.
  [[nodiscard]] virtual auto generate_ecdh(EcdhCurve curve)
      -> Expected<std::unique_ptr<EcdhKeyPair>, CryptoError> {
    (void)curve;
    return libchiaking::unexpected(CryptoError::unsupported_capability);
  }

  /// @brief Computes the SHA-512 digest of a message.
  ///
  /// Blocks until the result is written. Implementations must accept
  /// calls from several threads at once.
  ///
  /// @param input Message to hash.
  /// @param digest Destination of exactly sha512_digest_size bytes.
  /// @return Nothing on success.
  /// @retval CryptoError::unsupported_capability Not implemented. This is the
  ///     default.
  /// @retval CryptoError::invalid_argument @p digest has the wrong size.
  /// @retval CryptoError::backend_failure The backend failed.
  [[nodiscard]] virtual auto sha512(std::span<const std::byte> input,
                                    std::span<std::byte> digest)
      -> Expected<void, CryptoError> {
    (void)input;
    (void)digest;
    return libchiaking::unexpected(CryptoError::unsupported_capability);
  }

  /// @brief Computes the HMAC-SHA-512 tag of a message.
  ///
  /// Blocks until the result is written. Implementations must accept
  /// calls from several threads at once.
  ///
  /// @param key Secret key of any length.
  /// @param input Message to authenticate.
  /// @param digest Destination of exactly hmac_sha512_digest_size bytes.
  /// @return Nothing on success.
  /// @retval CryptoError::unsupported_capability Not implemented. This is the
  ///     default.
  /// @retval CryptoError::invalid_argument @p digest has the wrong size.
  /// @retval CryptoError::backend_failure The backend failed.
  [[nodiscard]] virtual auto hmac_sha512(std::span<const std::byte> key,
                                         std::span<const std::byte> input,
                                         std::span<std::byte> digest)
      -> Expected<void, CryptoError> {
    (void)key;
    (void)input;
    (void)digest;
    return libchiaking::unexpected(CryptoError::unsupported_capability);
  }

  /// @brief Computes the HMAC-SHA-1 tag of a message, as STUN message
  /// integrity requires.
  ///
  /// Blocks until the result is written. Implementations must accept
  /// calls from several threads at once.
  ///
  /// @param key Secret key of any length.
  /// @param input Message to authenticate.
  /// @param digest Destination of exactly hmac_sha1_digest_size bytes.
  /// @return Nothing on success.
  /// @retval CryptoError::unsupported_capability Not implemented. This is the
  ///     default.
  /// @retval CryptoError::invalid_argument @p digest has the wrong size.
  /// @retval CryptoError::backend_failure The backend failed.
  [[nodiscard]] virtual auto hmac_sha1(std::span<const std::byte> key,
                                       std::span<const std::byte> input,
                                       std::span<std::byte> digest)
      -> Expected<void, CryptoError> {
    (void)key;
    (void)input;
    (void)digest;
    return libchiaking::unexpected(CryptoError::unsupported_capability);
  }

  /// @brief Derives key material with HKDF, extract then expand, as RFC 5869
  /// defines it.
  ///
  /// The default implementation builds HKDF from hmac_sha256() or
  /// hmac_sha512(), so a provider that has the HMAC it needs gets HKDF
  /// without overriding this. A provider overrides it to use its backend's
  /// own HKDF. The call blocks for the time the HMACs take and keeps no
  /// state, so it is as thread-safe as the HMAC it uses.
  ///
  /// @param digest Hash to build HKDF on.
  /// @param salt Extract salt of any length. An empty salt means a block of
  ///     zero bytes, as RFC 5869 says.
  /// @param input Input keying material. Treat it as secret.
  /// @param info Context bound into the expanded output. May be empty.
  /// @param output Destination for the derived bytes; its length selects how
  ///     many are produced. Left in an unspecified state on failure.
  /// @return Nothing on success.
  /// @retval CryptoError::invalid_argument @p output is empty, longer than
  ///     hkdf_max_blocks blocks of @p digest, or @p digest names no hash.
  /// @retval CryptoError::unsupported_capability The default implementation
  ///     needs hmac_sha512() for HkdfDigest::sha512 and the provider has
  ///     none.
  /// @retval CryptoError::backend_failure The backend failed, or the
  ///     default implementation could not allocate its working buffer.
  [[nodiscard]] virtual auto hkdf(HkdfDigest digest,
                                  std::span<const std::byte> salt,
                                  std::span<const std::byte> input,
                                  std::span<const std::byte> info,
                                  std::span<std::byte> output)
      -> Expected<void, CryptoError>;

  /// @brief Derives key material from a password with scrypt.
  ///
  /// Blocks until the result is written. Implementations must accept
  /// calls from several threads at once.
  ///
  /// @param password Secret to stretch.
  /// @param salt Salt for the derivation.
  /// @param parameters Work factors.
  /// @param output Destination for the derived bytes; its length selects how
  ///     many are produced. Must not be empty.
  /// @return Nothing on success.
  /// @retval CryptoError::unsupported_capability Not implemented. This is the
  ///     default.
  /// @retval CryptoError::invalid_argument @p output is empty or a work
  ///     factor is not acceptable.
  /// @retval CryptoError::backend_failure The derivation failed, for example
  ///     because it needs more memory than the backend allows.
  [[nodiscard]] virtual auto scrypt(std::span<const std::byte> password,
                                    std::span<const std::byte> salt,
                                    ScryptParameters parameters,
                                    std::span<std::byte> output)
      -> Expected<void, CryptoError> {
    (void)password;
    (void)salt;
    (void)parameters;
    (void)output;
    return libchiaking::unexpected(CryptoError::unsupported_capability);
  }

  /// @brief Derives the Ed25519 public key that belongs to a private seed.
  ///
  /// Blocks until the result is written. Implementations must accept
  /// calls from several threads at once.
  ///
  /// @param seed Exactly ed25519_seed_size bytes.
  /// @param public_key Destination of exactly ed25519_public_key_size bytes.
  /// @return Nothing on success.
  /// @retval CryptoError::unsupported_capability Not implemented. This is the
  ///     default.
  /// @retval CryptoError::invalid_argument A span has the wrong size.
  /// @retval CryptoError::backend_failure The backend failed.
  [[nodiscard]] virtual auto ed25519_public_key(std::span<const std::byte> seed,
                                                std::span<std::byte> public_key)
      -> Expected<void, CryptoError> {
    (void)seed;
    (void)public_key;
    return libchiaking::unexpected(CryptoError::unsupported_capability);
  }

  /// @brief Signs a message with Ed25519.
  ///
  /// Blocks until the result is written. Implementations must accept
  /// calls from several threads at once.
  ///
  /// @param seed Private seed of exactly ed25519_seed_size bytes.
  /// @param message Message to sign, of any length.
  /// @param signature Destination of exactly ed25519_signature_size bytes.
  /// @return Nothing on success.
  /// @retval CryptoError::unsupported_capability Not implemented. This is the
  ///     default.
  /// @retval CryptoError::invalid_argument A span has the wrong size.
  /// @retval CryptoError::backend_failure The backend failed.
  [[nodiscard]] virtual auto ed25519_sign(std::span<const std::byte> seed,
                                          std::span<const std::byte> message,
                                          std::span<std::byte> signature)
      -> Expected<void, CryptoError> {
    (void)seed;
    (void)message;
    (void)signature;
    return libchiaking::unexpected(CryptoError::unsupported_capability);
  }

  /// @brief Checks an Ed25519 signature.
  ///
  /// Blocks until the result is written. Implementations must accept
  /// calls from several threads at once.
  ///
  /// @param public_key Exactly ed25519_public_key_size bytes.
  /// @param message Message the signature should cover.
  /// @param signature Exactly ed25519_signature_size bytes.
  /// @return True when the signature is valid for the message and key. False
  ///     when it is not, which is an answer and not an error.
  /// @retval CryptoError::unsupported_capability Not implemented. This is the
  ///     default.
  /// @retval CryptoError::invalid_argument A span has the wrong size.
  /// @retval CryptoError::backend_failure The backend could not perform the
  ///     check.
  [[nodiscard]] virtual auto ed25519_verify(
      std::span<const std::byte> public_key, std::span<const std::byte> message,
      std::span<const std::byte> signature) -> Expected<bool, CryptoError> {
    (void)public_key;
    (void)message;
    (void)signature;
    return libchiaking::unexpected(CryptoError::unsupported_capability);
  }

  /// @brief Multiplies the P-256 generator by a scalar.
  ///
  /// Blocks until the result is written. Implementations must accept
  /// calls from several threads at once.
  ///
  /// @param scalar Big-endian scalar of exactly p256_scalar_size bytes. Treat
  ///     it as secret.
  /// @param point Destination of exactly p256_point_size bytes for the
  ///     product in uncompressed SEC1 form.
  /// @return Nothing on success.
  /// @retval CryptoError::unsupported_capability Not implemented. This is the
  ///     default.
  /// @retval CryptoError::invalid_argument A span has the wrong size, or the
  ///     product is the point at infinity.
  /// @retval CryptoError::backend_failure The backend failed.
  [[nodiscard]] virtual auto p256_base_multiply(
      std::span<const std::byte> scalar, std::span<std::byte> point)
      -> Expected<void, CryptoError> {
    (void)scalar;
    (void)point;
    return libchiaking::unexpected(CryptoError::unsupported_capability);
  }

  /// @brief Multiplies a P-256 point by a scalar.
  ///
  /// Blocks until the result is written. Implementations must accept
  /// calls from several threads at once.
  ///
  /// @param scalar Big-endian scalar of exactly p256_scalar_size bytes. Treat
  ///     it as secret.
  /// @param point Point in uncompressed SEC1 form, exactly p256_point_size
  ///     bytes. It must lie on the curve.
  /// @param product Destination of exactly p256_point_size bytes.
  /// @return Nothing on success.
  /// @retval CryptoError::unsupported_capability Not implemented. This is the
  ///     default.
  /// @retval CryptoError::invalid_argument A span has the wrong size, @p
  ///     point is not a valid curve point, or the product is the point at
  ///     infinity.
  /// @retval CryptoError::backend_failure The backend failed.
  [[nodiscard]] virtual auto p256_multiply(std::span<const std::byte> scalar,
                                           std::span<const std::byte> point,
                                           std::span<std::byte> product)
      -> Expected<void, CryptoError> {
    (void)scalar;
    (void)point;
    (void)product;
    return libchiaking::unexpected(CryptoError::unsupported_capability);
  }

  /// @brief Adds two P-256 points.
  ///
  /// Blocks until the result is written. Implementations must accept
  /// calls from several threads at once.
  ///
  /// @param left First point in uncompressed SEC1 form, exactly
  ///     p256_point_size bytes.
  /// @param right Second point in the same form.
  /// @param sum Destination of exactly p256_point_size bytes.
  /// @return Nothing on success.
  /// @retval CryptoError::unsupported_capability Not implemented. This is the
  ///     default.
  /// @retval CryptoError::invalid_argument A span has the wrong size, an
  ///     operand is not a valid curve point, or the sum is the point at
  ///     infinity.
  /// @retval CryptoError::backend_failure The backend failed.
  [[nodiscard]] virtual auto p256_add(std::span<const std::byte> left,
                                      std::span<const std::byte> right,
                                      std::span<std::byte> sum)
      -> Expected<void, CryptoError> {
    (void)left;
    (void)right;
    (void)sum;
    return libchiaking::unexpected(CryptoError::unsupported_capability);
  }

  /// @brief Prepares an AES-128 key for repeated encryption and GMAC.
  ///
  /// The default keeps a wiped copy of the key and calls
  /// aes128_encrypt_blocks() and aes128_gmac() with it, so every provider
  /// supports it; a provider whose backend can hold a key schedule overrides
  /// it to avoid repeating that work per call. The cipher borrows this
  /// provider and must not outlive it.
  ///
  /// @param key Exactly aes128_key_size bytes; copied.
  /// @return The cipher.
  /// @retval CryptoError::invalid_argument The key has the wrong size.
  /// @retval CryptoError::backend_failure The cipher could not be made.
  [[nodiscard]] virtual auto aes128_cipher(std::span<const std::byte> key)
      -> Expected<std::unique_ptr<Aes128Cipher>, CryptoError>;

 private:
  class KeyedAes128Cipher;
};

}  // namespace libchiaking::platform
