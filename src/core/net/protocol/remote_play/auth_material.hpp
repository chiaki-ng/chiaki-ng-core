/// @file
/// @brief Declares AuthMaterial, the per-session key pair that seals the
/// encrypted headers of the control request and the control channel.
///
/// When a console accepts a session request it returns a 16-byte nonce. Mixed
/// with the registration's morning key (`RP-Key` from pairing), it yields the
/// two 16-byte values the Remote Play cipher needs: the AES key ("bright") and
/// the IV seed ("ambassador"). Everything encrypted on the control connection
/// afterwards, from `RP-Auth` to each control message, uses them.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/secret_array.hpp"
#include "libchiaking/detail/wire_sizes.hpp"
#include "libchiaking/platform/crypto/provider.hpp"
#include "src/core/net/protocol/remote_play/pairing/cipher.hpp"
#include "src/core/net/protocol/remote_play/registered_console.hpp"

namespace libchiaking::net::protocol::remote_play {

/// @brief Why AuthMaterial::derive refused its inputs.
enum class AuthMaterialError : std::uint8_t {
  /// @brief The target is RegistrationTarget::ps4_unknown or not a known
  /// variant, so no derivation rule applies.
  invalid_target,
  /// @brief The nonce is not exactly 16 bytes.
  invalid_nonce,
  /// @brief The morning key is not exactly 16 bytes.
  invalid_morning,
};

/// @brief The AES key and IV seed of one Remote Play session, derived from the
/// console's session nonce and the registration's morning key.
///
/// Derivation depends on the console variant: PS4 RP 8.0 and 9.0 use a fixed
/// pad (`echo_a`/`echo_b`), PS4 RP 10.0 and PS5 each use a pair of 32-row
/// tables indexed by the top five bits of nonce bytes 0 and 7. Both values are
/// secrets: they are wiped on destruction and the type is move-only.
///
/// Immutable once derived; transform() may be called from any thread as long
/// as the crypto provider allows it.
struct AuthMaterial {
  /// @brief The 16-byte AES-128 key of the session cipher.
  SecretArray<rp_key_size> bright;
  /// @brief The 16-byte seed hashed with each counter to form the cipher IV.
  SecretArray<rp_key_size> ambassador;

  /// @brief Makes empty (all-zero) material, to be overwritten by derive().
  AuthMaterial() noexcept = default;
  AuthMaterial(const AuthMaterial&) = delete;
  AuthMaterial& operator=(const AuthMaterial&) = delete;
  /// @brief Takes over another session's material; the source is left
  /// valid but unspecified.
  AuthMaterial(AuthMaterial&&) noexcept = default;
  /// @brief Takes over another session's material, wiping what this held.
  AuthMaterial& operator=(AuthMaterial&&) noexcept = default;

  /// @brief Derives the session material from the console's nonce.
  ///
  /// For PS4 RP 8.0/9.0, byte i of the IV seed is `(nonce[i] - i - 0x27) ^
  /// echo_a[i]` and of the key `(morning[i] - i + 0x34) ^ echo_b[i] ^
  /// nonce[i]`. For PS4 RP 10.0 and PS5 the same bytes are mixed with table
  /// rows chosen by `nonce[0] >> 3` and `nonce[7] >> 3`, with per-generation
  /// offsets.
  ///
  /// @param target The console variant the session runs against.
  /// @param nonce The 16-byte nonce the console sent in `RP-Nonce`, decoded.
  /// @param morning The 16-byte morning key stored at registration.
  /// @return The material, or why the inputs were refused.
  /// @retval AuthMaterialError::invalid_target The target has no RP version.
  /// @retval AuthMaterialError::invalid_nonce The nonce is not 16 bytes.
  /// @retval AuthMaterialError::invalid_morning The morning key is not 16
  ///     bytes.
  [[nodiscard]] static auto derive(RegistrationTarget target,
                                   std::span<const std::byte> nonce,
                                   std::span<const std::byte> morning) noexcept
      -> Expected<AuthMaterial, AuthMaterialError>;

  /// @brief Encrypts or decrypts with the session cipher at one counter.
  ///
  /// Delegates to RegistrationCipher::transform with this material's key and IV
  /// seed. Does not block beyond the provider's own work.
  ///
  /// @param crypto The provider that runs HMAC-SHA256 and AES.
  /// @param target The console variant; selects the HMAC key of the IV.
  /// @param counter The position in the cipher stream; each header and control
  ///     message uses its own value.
  /// @param direction Whether `input` is plaintext or ciphertext.
  /// @param input The bytes to transform.
  /// @param output Where the result is written; must be as long as `input`.
  /// @return Nothing on success.
  /// @retval CipherError::invalid_argument The sizes or target are wrong.
  /// @retval CipherError::provider_failure The provider failed a primitive.
  [[nodiscard]] auto transform(platform::CryptoProvider& crypto,
                               RegistrationTarget target, std::uint64_t counter,
                               CipherDirection direction,
                               std::span<const std::byte> input,
                               std::span<std::byte> output) const
      -> Expected<void, CipherError>;

 private:
  /// @brief PS4 RP 10.0 IV-seed table, indexed by `nonce[0] >> 3`.
  static const std::array<std::array<std::uint8_t, rp_key_size>, 32>
      ps4_auth_table_a;
  /// @brief PS4 RP 10.0 key table, indexed by `nonce[7] >> 3`.
  static const std::array<std::array<std::uint8_t, rp_key_size>, 32>
      ps4_auth_table_b;
  /// @brief PS5 IV-seed table, indexed by `nonce[0] >> 3`.
  static const std::array<std::array<std::uint8_t, rp_key_size>, 32>
      ps5_auth_table_a;
  /// @brief PS5 key table, indexed by `nonce[7] >> 3`.
  static const std::array<std::array<std::uint8_t, rp_key_size>, 32>
      ps5_auth_table_b;
  /// @brief The fixed pad XORed into the IV seed for PS4 RP 8.0 and 9.0.
  static const std::array<std::uint8_t, rp_key_size> echo_a;
};

}  // namespace libchiaking::net::protocol::remote_play
