/// @file
/// @brief Declares Kakusan pairing: the verification codes, the transcript and
/// the SPAKE2 exchange that turns two four-digit codes into mutual
/// authentication.
#pragma once

#include <libchiaking/detail/secret_string.hpp>

#include "libchiaking/detail/secret_array.hpp"
#include "libchiaking/platform/crypto/provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "src/core/net/kakusan/ack_token.hpp"
#include "src/core/net/kakusan/digest.hpp"
#include "src/core/net/kakusan/result.hpp"
#include "src/core/net/kakusan/short_block.hpp"
#include "src/core/net/kakusan/verification_code.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace libchiaking::net::kakusan {

/// @brief Domain label of the transcript hash.
inline constexpr std::string_view transcript_label = "RCP/1/transcript";
/// @brief Domain label of the host's SPAKE2 identity.
inline constexpr std::string_view host_label = "RCP/1/host";
/// @brief Domain label of the guest's SPAKE2 identity.
inline constexpr std::string_view guest_label = "RCP/1/guest";
/// @brief Domain label of the scrypt salt.
inline constexpr std::string_view password_label = "RCP/1/password";
/// @brief The DTLS exporter label whose output binds pairing to the
/// association.
inline constexpr std::string_view pairing_exporter_label =
    "EXPORTER-RCP-1-pairing";
/// @brief Bytes reduced modulo the P-256 order to make a scalar without bias.
inline constexpr std::size_t wide_scalar_size = 64;
/// @brief Bytes of the PAKE shared key: half a SHA-512.
inline constexpr std::size_t shared_key_size = platform::sha512_digest_size / 2;
/// @brief Offset of the y coordinate in an uncompressed SEC1 point.
inline constexpr std::size_t sec1_y_offset = 1 + platform::p256_scalar_size;

/// @brief A secret P-256 scalar, 32 bytes big-endian.
using Scalar = SecretArray<platform::p256_scalar_size>;
/// @brief An uncompressed SEC1 P-256 point: 65 bytes.
using Point = std::array<std::byte, platform::p256_point_size>;
/// @brief A 64-byte key confirmation MAC.
using Confirmation = SecretArray<platform::sha512_digest_size>;
/// @brief The 32-byte PAKE shared key.
using SharedKey = SecretArray<shared_key_size>;
/// @brief A view of an invitation id.
using InvitationIdBytes =
    std::span<const std::byte, std::tuple_size_v<InvitationId>>;

/// @brief A peer's side in a Kakusan session.
enum class Role : std::uint8_t {
  /// @brief The host, which keeps the console session; SPAKE2 party A.
  host,
  /// @brief A guest, which receives the stream; SPAKE2 party B.
  guest,
};


/// @brief What a completed exchange yields.
struct Confirmations {
  /// @brief The MAC the host sends as CONFIRM_A.
  Confirmation host;
  /// @brief The MAC the guest sends as CONFIRM_B.
  Confirmation guest;
  /// @brief The shared key; not used to replace DTLS.
  SharedKey shared_key;
};

/// @brief One side of the RFC 9382 SPAKE2 exchange over P-256 with SHA-512 that
/// authenticates the pairing transcript with both verification codes.
///
/// The password is scrypt (N 32768, r 8, p 1, 64 bytes) of the eight digits
/// `host_code || guest_code`, salted with `RCP/1/password`, a zero byte and the
/// invitation id, reduced modulo the group order. The host masks its share with
/// M, the guest with N. Completion builds the RFC transcript (each item
/// prefixed by its 64-bit little-endian length), takes the first half of its
/// SHA-512 as the shared key and expands the second half with HKDF-SHA512 (info
/// `ConfirmationKeys` followed by the associated data) into the two
/// confirmation keys. Secrets are wiped and the exchange can complete once.
/// Move-only; one thread at a time.
class Exchange {
 public:
  /// @brief Draws tried for a non-zero ephemeral scalar.
  static constexpr std::size_t ephemeral_draw_attempts = 8;

  /// @brief Computes the pairing transcript.
  ///
  /// @param crypto Hashes.
  /// @param invitation_hash The invitation's hash.
  /// @param reply_hash The reply's hash.
  /// @return The transcript.
  /// @retval Error Hashing failed.
  static auto transcript(platform::CryptoProvider& crypto,
                         const Digest256& invitation_hash,
                         const Digest256& reply_hash) -> Result<Transcript>;
  /// @brief Reduces 64 big-endian bytes modulo the P-256 order in constant
  /// time.
  ///
  /// @param wide The bytes.
  /// @return The scalar.
  static auto reduce_wide(
      std::span<const std::byte, wide_scalar_size> wide) noexcept -> Scalar;
  /// @brief Derives the SPAKE2 password scalar w.
  ///
  /// @param crypto Runs scrypt.
  /// @param host_code The host's code.
  /// @param guest_code The guest's code.
  /// @param invitation_id The invitation id, the salt's tail.
  /// @return The scalar.
  /// @retval Error::pairing_failed It reduced to zero.
  /// @retval Error scrypt failed; see Hash::failure().
  static auto password_scalar(platform::CryptoProvider& crypto,
                              const Code& host_code, const Code& guest_code,
                              InvitationIdBytes invitation_id)
      -> Result<Scalar>;
  /// @brief Draws an ephemeral scalar uniformly.
  ///
  /// @param random The random source.
  /// @return The scalar; nothing when the source failed or 8 draws reduced to
  ///     zero.
  [[nodiscard]] static auto ephemeral(platform::SecureRandomProvider& random)
      -> std::optional<Scalar>;
  /// @brief Computes this side's share and keeps the secrets for completion.
  ///
  /// @param crypto Does the point arithmetic.
  /// @param role This side.
  /// @param password The password scalar; consumed.
  /// @param ephemeral The ephemeral scalar; consumed.
  /// @param parties Both identity keys.
  /// @param associated_data Bound into the confirmation keys: the transcript
  ///     followed by the DTLS pairing exporter output.
  /// @return The exchange.
  /// @retval Error::pairing_failed A scalar is zero or the share is the point
  ///     at infinity.
  /// @retval Error The arithmetic failed.
  static auto begin(platform::CryptoProvider& crypto, Role role,
                    Scalar password, Scalar ephemeral, const Parties& parties,
                    std::span<const std::byte> associated_data)
      -> Result<Exchange>;

  /// @brief Wipes any secrets left.
  ~Exchange() = default;
  /// @brief Not copyable: it holds secrets.
  Exchange(const Exchange&) = delete;
  /// @brief Not copyable.
  ///
  /// @return Never returns.
  Exchange& operator=(const Exchange&) = delete;
  /// @brief Takes over another exchange's secrets.
  ///
  /// @param other The source; left unable to complete.
  Exchange(Exchange&& other) noexcept;
  /// @brief Takes over another exchange's secrets.
  ///
  /// @param other The source; left unable to complete.
  /// @return This exchange.
  Exchange& operator=(Exchange&& other) noexcept;

  /// @brief This side's share, sent as PAKE_A or PAKE_B.
  ///
  /// @return The 65-byte point.
  auto share() const noexcept -> const Point& { return share_; }

  /// @brief Finishes the exchange with the peer's share and wipes the
  /// secrets.
  ///
  /// @param crypto Does the arithmetic, hashing and HKDF.
  /// @param peer_share The peer's 65-byte point.
  /// @return Both confirmation MACs and the shared key.
  /// @retval Error::pairing_failed Already completed, wrong size, or an
  ///     invalid point.
  /// @retval Error::internal A MAC or the transcript could not be made.
  [[nodiscard]] auto complete(platform::CryptoProvider& crypto,
                              std::span<const std::byte> peer_share)
      -> Result<Confirmations>;

 private:
  /// @brief What completion needs, wiped with the exchange.
  struct Secrets {
    /// @brief The password scalar w.
    Scalar password;
    /// @brief The ephemeral scalar.
    Scalar ephemeral;
    /// @brief The data bound into the confirmation keys.
    SecretBytes associated_data;
  };

  /// @brief Holds a begun exchange.
  ///
  /// @param role This side.
  /// @param share This side's share.
  /// @param parties Both identity keys.
  /// @param secrets The secrets.
  Exchange(Role role, const Point& share, const Parties& parties,
           Secrets secrets) noexcept;

  /// @brief This side.
  Role role_;
  /// @brief This side's share.
  Point share_;
  /// @brief Both identity keys.
  Parties parties_;
  /// @brief The secrets; empty once completed or moved from.
  std::optional<Secrets> secrets_;
};

}  // namespace libchiaking::net::kakusan
