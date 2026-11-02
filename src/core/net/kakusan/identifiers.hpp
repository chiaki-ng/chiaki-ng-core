/// @file
/// @brief Declares the fixed-size values the Kakusan protocol names: digests,
/// identity keys, signatures and ids, free of any provider.
#pragma once

#include <array>
#include <cstddef>

namespace libchiaking::net::kakusan {

/// @brief A SHA-256 digest: 32 bytes.
using Digest256 = std::array<std::byte, 32>;
/// @brief A SHA-512 digest: 64 bytes.
using Digest512 = std::array<std::byte, 64>;
/// @brief SHA-256 of `RCP/1/transcript`, a zero byte, the invitation hash and
/// the reply hash.
using Transcript = Digest256;
/// @brief A host session's random id.
using SessionId = std::array<std::byte, 16>;
/// @brief An invitation's id, derived from its seed.
using InvitationId = std::array<std::byte, 16>;
/// @brief An Ed25519 public identity key: 32 bytes.
using IdentityKey = std::array<std::byte, 32>;
/// @brief An Ed25519 signature: 64 bytes.
using Signature = std::array<std::byte, 64>;
/// @brief The first 8 bytes of a peer's identity key, promised in its block.
using IdentityHint = std::array<std::byte, 8>;

/// @brief The identity keys SPAKE2 names the two parties by.
struct Parties {
  /// @brief The host's identity key.
  IdentityKey host_identity{};
  /// @brief The guest's identity key.
  IdentityKey guest_identity{};
};

}  // namespace libchiaking::net::kakusan
