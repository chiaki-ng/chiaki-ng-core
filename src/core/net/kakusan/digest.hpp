/// @file
/// @brief Declares the digest types and the hashing helpers Kakusan uses.
#pragma once

#include "libchiaking/platform/crypto/provider.hpp"
#include "src/core/net/kakusan/identifiers.hpp"
#include "src/core/net/kakusan/result.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <tuple>

namespace libchiaking::net::kakusan {

static_assert(std::tuple_size_v<Digest256> == platform::sha256_digest_size);
static_assert(std::tuple_size_v<Digest512> == platform::sha512_digest_size);

/// @brief The SHA-256 and HMAC-SHA256 the Kakusan code uses, over the
/// platform's crypto provider. Stateless.
class Hash {
 public:
  /// @brief Translates a crypto failure.
  ///
  /// @param error The provider's error.
  /// @return crypto_unavailable when the capability is missing, otherwise
  ///     internal.
  static auto failure(platform::CryptoError error) noexcept -> Error;
  /// @brief Views text as bytes.
  ///
  /// @param text The text.
  /// @return Its bytes, without a terminator.
  static auto bytes_of(std::string_view text) noexcept
      -> std::span<const std::byte>;
  /// @brief Hashes bytes with SHA-256.
  ///
  /// @param crypto The crypto provider.
  /// @param input The bytes.
  /// @return The digest.
  /// @retval Error The provider failed; see failure().
  static auto sha256(platform::CryptoProvider& crypto,
                     std::span<const std::byte> input) -> Result<Digest256>;
  /// @brief Authenticates bytes with HMAC-SHA256.
  ///
  /// @param crypto The crypto provider.
  /// @param key The key.
  /// @param input The bytes.
  /// @return The 32-byte tag.
  /// @retval Error The provider failed; see failure().
  static auto hmac_sha256(platform::CryptoProvider& crypto,
                          std::span<const std::byte> key,
                          std::span<const std::byte> input)
      -> Result<Digest256>;
};

}  // namespace libchiaking::net::kakusan
