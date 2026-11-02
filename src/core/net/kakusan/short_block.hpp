/// @file
/// @brief Declares Kakusan's short invitation and reply blocks, the copyable
/// text
/// host and guest exchange through a messenger, and their codec.
#pragma once

#include "libchiaking/detail/secret_string.hpp"
#include "libchiaking/platform/crypto/provider.hpp"
#include "libchiaking/platform/net/types.hpp"
#include "src/core/net/kakusan/digest.hpp"
#include "src/core/net/kakusan/identifiers.hpp"
#include "src/core/net/kakusan/result.hpp"
#include "src/core/net/nat/ice/parameters.hpp"
#include "src/core/utils/bytes/byte_io.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace libchiaking::net::kakusan {

/// @brief The text every Kakusan block starts with.
inline constexpr std::string_view block_prefix = "RCP1:";
/// @brief The only key policy: guests receive the source session's incoming key
/// until that session ends.
inline constexpr std::string_view key_policy =
    "shared-source-incoming-until-session-end-v1";
/// @brief Most seats a session may have.
inline constexpr std::size_t max_seat_capacity = 255;
/// @brief How long an unused invitation lasts: 600 seconds.
inline constexpr std::uint64_t invitation_lifetime_seconds = 600;
/// @brief Most ICE candidates a block carries.
using nat::ice::max_candidates;

/// @brief A reply's nonce, derived from its seed.
using ReplyNonce = std::array<std::byte, 16>;
/// @brief An Ed25519 private seed: 32 bytes.
using IdentitySeed = std::array<std::byte, platform::ed25519_seed_size>;

static_assert(std::tuple_size_v<IdentityKey> ==
              platform::ed25519_public_key_size);
static_assert(std::tuple_size_v<Signature> ==
              platform::ed25519_signature_size);

/// @brief An ICE candidate.
using nat::ice::Candidate;
/// @brief Host or server reflexive.
using nat::ice::CandidateType;
/// @brief A peer's ICE credentials and candidates.
using IceParameters = nat::ice::Parameters;
/// @brief A peer's ICE ufrag and password.
using IceCredentials = nat::ice::Credentials;

/// @brief The host's seat counts at one moment.
struct SeatSnapshot {
  /// @brief Seats in all, 1 to 255.
  std::uint8_t capacity{};
  /// @brief Seats held by local players, including player 1.
  std::uint8_t local_seats{};
  /// @brief Seats held by admitted guests.
  std::uint8_t remote_seats{};
  /// @brief Seats reserved by guests being admitted.
  std::uint8_t reserved_seats{};
};

/// @brief The random seed a block's ICE credentials and ids derive from.
using BlockSeed = std::array<std::byte, 16>;
/// @brief The first 8 bytes of an invitation's SHA-256, naming it in a reply.
using InviteTag = std::array<std::byte, 8>;

/// @brief The first byte of a block.
enum class BlockKind : std::uint8_t {
  /// @brief An invitation, written by the host.
  invitation = 0x51,
  /// @brief A reply, written by a guest.
  reply = 0x52,
};

/// @brief The host's invitation as carried in its short block.
struct ShortInvitation {
  /// @brief The random seed.
  BlockSeed seed{};
  /// @brief When the invitation expires, Unix seconds; must fit 32 bits.
  std::uint64_t invite_expires_at{};
  /// @brief The Takion version of the source, 9 to 20.
  std::uint8_t source_protocol{};
  /// @brief Seats in the session, at least 1.
  std::uint8_t capacity{};
  /// @brief SHA-256 of the host's DTLS certificate.
  Digest256 dtls_sha256{};
  /// @brief The host's identity hint.
  IdentityHint identity_hint{};
  /// @brief The host's ICE parameters; credentials derived from the seed.
  IceParameters ice;
  /// @brief Derived from the seed; filled in when parsed.
  InvitationId invitation_id{};

  /// @brief Copies the invitation, secrets into new storage.
  ///
  /// @return The copy.
  auto clone() const -> ShortInvitation;
};

/// @brief A guest's reply as carried in its short block.
struct ShortReply {
  /// @brief The random seed.
  BlockSeed seed{};
  /// @brief Names the invitation answered.
  InviteTag invite_tag{};
  /// @brief SHA-256 of the guest's DTLS certificate.
  Digest256 dtls_sha256{};
  /// @brief The guest's identity hint.
  IdentityHint identity_hint{};
  /// @brief The guest's ICE parameters; credentials derived from the seed.
  IceParameters ice;
  /// @brief Derived from the seed; filled in when parsed.
  ReplyNonce reply_nonce{};

  /// @brief Copies the reply, secrets into new storage.
  ///
  /// @return The copy.
  auto clone() const -> ShortReply;
};

/// @brief A block with its text and hash, ready to share.
///
/// @tparam Body ShortInvitation or ShortReply.
template <class Body>
struct Sealed {
  /// @brief The decoded fields.
  Body body;
  /// @brief SHA-256 of the block's bytes, including its CRC.
  Digest256 hash{};
  /// @brief The `RCP1:` text to copy, secret because its seed derives the ICE
  /// password.
  SecretString text;

  /// @brief Copies the block.
  ///
  /// @return The copy.
  auto clone() const -> Sealed {
    return Sealed{body.clone(), hash, text.clone()};
  }
};

/// @brief A shareable invitation.
using SealedInvitation = Sealed<ShortInvitation>;
/// @brief A shareable reply.
using SealedReply = Sealed<ShortReply>;

/// @brief Writes and reads Kakusan's short invitation and reply blocks (draft
/// 05): `RCP1:` and unpadded base64url of a binary body with a CRC-32.
///
/// Invitation body: kind 0x51 (1), seed (16), expiry in Unix seconds (u32),
/// source Takion version (1), capacity (1), DTLS certificate SHA-256 (32),
/// identity hint (8), candidates. Reply body: kind 0x52 (1), seed (16), invite
/// tag (8), DTLS certificate SHA-256 (32), identity hint (8), candidates.
/// Candidates: port-guess count (1), candidate count (1, 1 to 32), then each
/// candidate as a flags byte (0x80 server reflexive, 0x40 IPv6, 0x20 reserved
/// zero, low 5 bits the base host's index), the 4- or 16-byte address and the
/// port (u16). Host candidates come first and in order. The body ends with its
/// CRC-32 (u32); the whole is at most 677 bytes. All integers big-endian. ICE
/// credentials, the invitation id and reply nonce are derived from the seed by
/// HKDF-SHA256 with salt `RCP/1/draft-05/seed` and info `label 0x00 kind`.
/// Stateless.
class ShortBlockCodec {
 public:
  /// @brief Largest block body, CRC included, in bytes.
  static constexpr std::size_t max_block_bytes = 677;
  /// @brief Largest pasted text accepted, in characters.
  static constexpr std::size_t max_text_size = 131072;

  /// @brief Derives a block's ICE credentials: 8 bytes as the ufrag and 24 as
  /// the password, both written as lower-case hex.
  ///
  /// @param crypto Runs HKDF.
  /// @param seed The block's seed.
  /// @param kind Which block.
  /// @return The credentials.
  /// @retval Error HKDF failed; see Hash::failure().
  [[nodiscard]] static auto credentials(platform::CryptoProvider& crypto,
                                        const BlockSeed& seed, BlockKind kind)
      -> Result<IceCredentials>;
  /// @brief Derives an invitation's id.
  ///
  /// @param crypto Runs HKDF.
  /// @param seed The invitation's seed.
  /// @return The 16-byte id.
  /// @retval Error HKDF failed.
  [[nodiscard]] static auto invitation_id(platform::CryptoProvider& crypto,
                                          const BlockSeed& seed)
      -> Result<InvitationId>;
  /// @brief Writes an invitation and reads it back.
  ///
  /// @param crypto Hashes and derives.
  /// @param invitation The fields; its ICE credentials are replaced by
  ///     derived
  ///     ones.
  /// @return The sealed invitation.
  /// @retval Error::invalid_block A field is out of range or the block too
  ///     large.
  [[nodiscard]] static auto seal(platform::CryptoProvider& crypto,
                                 const ShortInvitation& invitation)
      -> Result<SealedInvitation>;
  /// @brief Writes a reply and reads it back.
  ///
  /// @param crypto Hashes and derives.
  /// @param reply The fields; its ICE credentials are replaced by derived
  ///     ones.
  /// @return The sealed reply.
  /// @retval Error::invalid_block A field is out of range or the block too
  ///     large.
  [[nodiscard]] static auto seal(platform::CryptoProvider& crypto,
                                 const ShortReply& reply)
      -> Result<SealedReply>;
  /// @brief Reads a pasted invitation. White space anywhere is ignored.
  ///
  /// @param crypto Hashes and derives.
  /// @param text The pasted text.
  /// @return The sealed invitation.
  /// @retval Error::invalid_block The text is too long, lacks the prefix, is
  ///     not
  ///     base64url, fails its CRC, has a wrong kind, a source version outside
  ///     9
  ///     to 20, zero capacity, an expiry below 600, bad candidates or
  ///     trailing
  ///     bytes.
  [[nodiscard]] static auto parse_invitation(platform::CryptoProvider& crypto,
                                             std::string_view text)
      -> Result<SealedInvitation>;
  /// @brief Reads a pasted reply. White space anywhere is ignored.
  ///
  /// @param crypto Hashes and derives.
  /// @param text The pasted text.
  /// @return The sealed reply.
  /// @retval Error::invalid_block The text or its fields are invalid.
  [[nodiscard]] static auto parse_reply(platform::CryptoProvider& crypto,
                                        std::string_view text)
      -> Result<SealedReply>;
  /// @brief The tag a reply names an invitation by.
  ///
  /// @param invitation_hash The invitation's hash.
  /// @return Its first 8 bytes.
  static auto tag_of(const Digest256& invitation_hash) noexcept -> InviteTag;
  /// @brief The hint a block promises an identity key by.
  ///
  /// @param key The identity key.
  /// @return Its first 8 bytes.
  static auto hint_of(const IdentityKey& key) noexcept -> IdentityHint;
  /// @brief Whether a reply answers an invitation, compared in constant time.
  ///
  /// @param invitation The invitation.
  /// @param reply The reply.
  /// @return True when the reply's tag is the invitation's.
  static auto answers(const SealedInvitation& invitation,
                      const SealedReply& reply) noexcept -> bool;

 private:
  /// @brief The HKDF salt of every derivation.
  static constexpr std::string_view seed_salt = "RCP/1/draft-05/seed";
  /// @brief Candidate flag: server reflexive.
  static constexpr std::uint8_t srflx_flag = 0x80;
  /// @brief Candidate flag: the address is IPv6.
  static constexpr std::uint8_t ipv6_flag = 0x40;
  /// @brief Candidate flag that must be zero.
  static constexpr std::uint8_t reserved_flag = 0x20;
  /// @brief Bits of a candidate's flags holding its base host's index.
  static constexpr std::uint8_t base_mask = 0x1f;
  /// @brief Bytes of the trailing CRC-32.
  static constexpr std::size_t check_size = 4;
  /// @brief Derived bytes behind a ufrag.
  static constexpr std::size_t ufrag_bytes = 8;
  /// @brief Derived bytes behind a password.
  static constexpr std::size_t password_bytes = 24;
  /// @brief Lowest Takion version an invitation may name.
  static constexpr std::uint8_t lowest_source = 9;
  /// @brief Highest Takion version an invitation may name.
  static constexpr std::uint8_t highest_source = 20;
  /// @brief Type preference given to host candidates read from a block.
  static constexpr std::uint32_t host_preference = 126;
  /// @brief Type preference given to server-reflexive candidates read from a
  /// block.
  static constexpr std::uint32_t srflx_preference = 100;
  /// @brief Local preference of the first candidate; later ones get one less
  /// each.
  static constexpr std::uint32_t highest_local = 65535;
  /// @brief The low byte of every priority: 256 less component 1.
  static constexpr std::uint32_t component_remainder = 255;

  /// @brief Derives bytes from a seed with HKDF-SHA256.
  ///
  /// @param crypto Runs HKDF.
  /// @param seed The seed.
  /// @param label What is derived, such as `ice-ufrag`.
  /// @param kind Which block.
  /// @param output Receives the bytes.
  /// @return Nothing on success.
  /// @retval Error HKDF failed.
  [[nodiscard]] static auto derive(platform::CryptoProvider& crypto,
                                   const BlockSeed& seed,
                                   std::string_view label, BlockKind kind,
                                   std::span<std::byte> output) -> Result<void>;
  /// @brief The priority given to a candidate read from a block.
  ///
  /// @param preference The type preference.
  /// @param index The candidate's host index.
  /// @return (preference << 24) | ((65535 - index) << 8) | 255.
  static constexpr auto priority(std::uint32_t preference,
                                 std::size_t index) noexcept -> std::uint32_t {
    return (preference << 24U) |
           ((highest_local - static_cast<std::uint32_t>(index)) << 8U) |
           component_remainder;
  }
  /// @brief The candidate section of a block, before its credentials are
  /// derived.
  struct CandidateSection {
    /// @brief The candidates, hosts first.
    std::vector<Candidate> candidates;
    /// @brief How many port guesses the other side should try.
    std::size_t port_guesses{};
  };

  /// @brief Appends the candidate section.
  ///
  /// @param output The block so far.
  /// @param ice The parameters.
  /// @return Nothing on success.
  /// @retval Error::invalid_block A host after a reflexive one, or a reflexive
  ///     one without its base.
  [[nodiscard]] static auto write_candidates(std::vector<std::byte>& output,
                                             const IceParameters& ice)
      -> Result<void>;
  /// @brief Reads the candidate section, giving each candidate a priority and
  /// foundation (`h<index>` or `s<base>`).
  ///
  /// @param reader Positioned at the section.
  /// @return The candidates and port guesses.
  /// @retval Error::invalid_block The section is malformed.
  [[nodiscard]] static auto read_candidates(bytes::Reader& reader)
      -> Result<CandidateSection>;
  /// @brief Reads a fixed-size array.
  ///
  /// @tparam Bytes A std::array of bytes.
  /// @param reader The reader.
  /// @return The array; nothing when too few bytes remain.
  template <class Bytes>
  [[nodiscard]] static auto read_array(bytes::Reader& reader) noexcept
      -> std::optional<Bytes> {
    const auto piece = reader.take(std::tuple_size_v<Bytes>);
    if (!piece) return std::nullopt;
    Bytes output{};
    std::ranges::copy(piece.value(), output.begin());
    return output;
  }
  /// @brief Reads a raw address.
  ///
  /// @param reader The reader.
  /// @param ipv6 Whether it is 16 bytes rather than 4.
  /// @return The address.
  /// @retval Error::invalid_block Too few bytes remain.
  [[nodiscard]] static auto read_address(bytes::Reader& reader, bool ipv6)
      -> Result<platform::IpAddress>;
  /// @brief Strips white space and the prefix, decodes base64url and checks
  /// the CRC.
  ///
  /// @param text The pasted text.
  /// @return The block bytes, CRC included.
  /// @retval Error::invalid_block Any check failed.
  [[nodiscard]] static auto decode(std::string_view text)
      -> Result<std::vector<std::byte>>;
  /// @brief The block without its CRC.
  ///
  /// @param bytes The block, at least 4 bytes.
  /// @return All but the last 4 bytes.
  static auto body_of(std::span<const std::byte> bytes) noexcept
      -> std::span<const std::byte>;
  /// @brief Gives parsed parameters their derived credentials and validates
  /// them.
  ///
  /// @param crypto Runs HKDF.
  /// @param seed The block's seed.
  /// @param kind Which block.
  /// @param section The parsed candidate section.
  /// @return The completed parameters.
  /// @retval Error::invalid_block They fail ICE validation.
  [[nodiscard]] static auto adopt_credentials(platform::CryptoProvider& crypto,
                                              const BlockSeed& seed,
                                              BlockKind kind,
                                              CandidateSection section)
      -> Result<IceParameters>;
  /// @brief Hashes a block and pairs it with its text.
  ///
  /// @tparam Body ShortInvitation or ShortReply.
  /// @param crypto Hashes.
  /// @param body The fields.
  /// @param bytes The block bytes, CRC included.
  /// @return The sealed block.
  /// @retval Error Hashing failed.
  template <class Body>
  [[nodiscard]] static auto sealed(platform::CryptoProvider& crypto, Body body,
                                   std::vector<std::byte> bytes)
      -> Result<Sealed<Body>>;
  /// @brief Appends the CRC and encodes the text.
  ///
  /// @param bytes The block body.
  /// @return The `RCP1:` text.
  /// @retval Error::invalid_block The block would exceed 677 bytes.
  [[nodiscard]] static auto finish(std::vector<std::byte>& bytes)
      -> Result<SecretString>;
};

}  // namespace libchiaking::net::kakusan
