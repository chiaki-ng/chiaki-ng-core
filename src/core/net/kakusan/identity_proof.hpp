/// @file
/// @brief Declares IdentityProof, the signature that binds a peer's Ed25519
/// identity to the pairing transcript and DTLS association.
#pragma once

#include "libchiaking/platform/crypto/provider.hpp"
#include "src/core/net/kakusan/control_message.hpp"
#include "src/core/net/kakusan/pake.hpp"
#include "src/core/net/kakusan/result.hpp"
#include "src/core/net/kakusan/short_block.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace libchiaking::net::kakusan {

/// @brief Signs and checks the IDENTITY message, which proves each peer holds
/// the
/// Ed25519 identity key its short block promised.
///
/// The signature covers `RCP/1/identity`, a zero byte, the sender's role (1
/// host, 2 guest), the 32-byte transcript, the DTLS pairing exporter, the key
/// policy text and, for the host, its session id and seat counts (local,
/// remote, reserved, one byte each). Stateless.
class IdentityProof {
 public:
  /// @brief The host's session id and seat counts.
  using Hosting = control::Identity::Hosting;

  /// @brief Makes this side's IDENTITY.
  ///
  /// @param crypto Derives the public key and signs.
  /// @param sender This side's role.
  /// @param transcript The pairing transcript.
  /// @param exporter The DTLS exporter output bound into the proof.
  /// @param identity_seed The 32-byte Ed25519 seed.
  /// @param hosting The host's session details; must be present exactly when
  ///     `sender` is the host.
  /// @return The message.
  /// @retval Error::internal The seed has the wrong size or `hosting` does
  ///     not
  ///     match the role.
  /// @retval Error Signing failed; see Hash::failure().
  [[nodiscard]] static auto sign(platform::CryptoProvider& crypto, Role sender,
                                 const Transcript& transcript,
                                 std::span<const std::byte> exporter,
                                 std::span<const std::byte> identity_seed,
                                 std::optional<Hosting> hosting)
      -> Result<control::Identity>;
  /// @brief Checks the peer's IDENTITY against the hint in its block.
  ///
  /// @param crypto Verifies the signature.
  /// @param sender The peer's role.
  /// @param transcript The pairing transcript.
  /// @param exporter The DTLS exporter output.
  /// @param identity The peer's message.
  /// @param promised The first 8 bytes of the key, from the peer's block.
  /// @return Nothing when it holds.
  /// @retval Error::protocol Hosting details are present for a guest or
  ///     absent
  ///     for a host.
  /// @retval Error::identity_changed The key does not match the hint, or the
  ///     signature is invalid.
  /// @retval Error Verification could not run; see Hash::failure().
  [[nodiscard]] static auto verify(platform::CryptoProvider& crypto,
                                   Role sender, const Transcript& transcript,
                                   std::span<const std::byte> exporter,
                                   const control::Identity& identity,
                                   const IdentityHint& promised)
      -> Result<void>;

 private:
  /// @brief The domain label that starts the signed bytes.
  static constexpr std::string_view label = "RCP/1/identity";
  /// @brief The role byte of the host.
  static constexpr std::uint8_t host_role = 1;
  /// @brief The role byte of a guest.
  static constexpr std::uint8_t guest_role = 2;

  /// @brief Builds the signed bytes.
  ///
  /// @param sender The signer's role.
  /// @param transcript The transcript.
  /// @param exporter The DTLS exporter output.
  /// @param hosting The host's session details, if the signer hosts.
  /// @return The bytes.
  static auto signed_bytes(Role sender, const Transcript& transcript,
                           std::span<const std::byte> exporter,
                           const std::optional<Hosting>& hosting)
      -> std::vector<std::byte>;
};

}  // namespace libchiaking::net::kakusan
