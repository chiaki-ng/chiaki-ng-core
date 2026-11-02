/// @file
/// @brief Declares RegistrationCipher, Remote Play's AES-128-CFB stream cipher
/// keyed per message by a counter.
///
/// The same construction protects the registration payload, the encrypted
/// control request headers and the control channel: the IV for counter `n` is
/// the first 16 bytes of HMAC-SHA256(generation key, seed || big-endian
/// 64-bit n), and the data is run through AES-128 in CFB-128 mode from that IV.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/platform/crypto/provider.hpp"
#include "src/core/net/protocol/remote_play/registered_console.hpp"

namespace libchiaking::net::protocol::remote_play {

/// @brief Which way RegistrationCipher::transform runs.
enum class CipherDirection : std::uint8_t {
  /// @brief The input is plaintext; ciphertext feeds back into the next block.
  encrypt,
  /// @brief The input is ciphertext; it is what feeds back.
  decrypt,
};

/// @brief Why the cipher could not run.
enum class CipherError : std::uint8_t {
  /// @brief A size, direction or target is wrong.
  invalid_argument,
  /// @brief The crypto provider failed HMAC-SHA256 or AES.
  provider_failure,
};

/// @brief The Remote Play counter-keyed AES-128-CFB cipher.
///
/// The HMAC key that turns the seed into an IV is fixed per generation: one
/// for PS4 RP 8.0/9.0 (and an unknown PS4), one for PS4 RP 10.0, one for PS5.
/// Stateless; every member is static and may run on any thread the provider
/// allows. Intermediate buffers are wiped.
class RegistrationCipher {
 public:
  /// @brief Encrypts or decrypts one message.
  ///
  /// @param provider The provider that runs HMAC-SHA256 and AES-128.
  /// @param target The console variant; picks the HMAC key.
  /// @param key The 16-byte AES key.
  /// @param ambassador The 16-byte IV seed.
  /// @param counter The message's position; sealed into the IV big-endian.
  /// @param direction Whether `input` is plaintext or ciphertext.
  /// @param input The bytes to transform; any length, a partial last block is
  ///     allowed.
  /// @param output Where the result goes; must be as long as `input`. It is
  ///     written only after all of `input` has been read, so it may be the same
  ///     buffer.
  /// @return Nothing on success.
  /// @retval CipherError::invalid_argument A size is wrong, the direction is
  ///     out of range or the target is unknown.
  /// @retval CipherError::provider_failure A primitive failed; `output` is
  ///     unchanged.
  [[nodiscard]] static Expected<void, CipherError> transform(
      platform::CryptoProvider& provider, RegistrationTarget target,
      std::span<const std::byte> key, std::span<const std::byte> ambassador,
      std::uint64_t counter, CipherDirection direction,
      std::span<const std::byte> input, std::span<std::byte> output);

 private:
  /// @brief The HMAC key that derives the IV for a console variant.
  ///
  /// @param target The variant.
  /// @return The 16-byte key, or nothing for a value outside the enumeration.
  static auto target_key(RegistrationTarget target) noexcept
      -> std::optional<std::span<const std::byte, platform::aes128_key_size>>;
};

}  // namespace libchiaking::net::protocol::remote_play
