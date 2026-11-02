/// @file
/// @brief Declares how a Takion session authenticates and encrypts its
/// packets: the key material, the provider that turns it into cipher
/// directions, and the context that tracks key positions per packet.
///
/// Each direction of a session has its own AES-128 key and IV (GKCrypt). Every
/// protected packet carries a 4-byte GMAC tag and the low 32 bits of its key
/// position, a byte offset into the direction's CTR keystream. The sender
/// advances its position by each packet's size; the receiver reconstructs the
/// full 64-bit position from the low half. Where the tag and position sit
/// depends on the packet family and on whether the version adds the 8-byte
/// extended header (see CryptoContext::packet_offsets()).
#pragma once

#include "src/core/net/protocol/takion/detail/packet_types.hpp"
#include "src/core/net/protocol/takion/profile.hpp"
#include "src/core/net/protocol/takion/result.hpp"
#include "src/core/net/protocol/takion/source_tap.hpp"

#include <libchiaking/detail/secret_string.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace libchiaking::net::protocol::takion {

/// @brief Bytes of the truncated GMAC tag a packet carries.
inline constexpr std::size_t takion_mac_size = 4;
/// @brief The AES block size, in bytes; also the extra key advance added for
/// families whose payload is encrypted at position + 16.
inline constexpr std::size_t takion_crypto_block_size = 16;
/// @brief Key positions after which the GMAC key is re-derived: every 45000
/// bytes of keystream a new GMAC key is used.
inline constexpr std::uint64_t gmac_key_refresh_position = 45000;
/// @brief How far, in blocks, the IV is offset per GMAC key rotation when the
/// rotated key is derived.
inline constexpr std::uint64_t gmac_key_refresh_iv_offset = 44910;
/// @brief A packet's 4-byte GMAC tag.
using Mac = std::array<std::byte, takion_mac_size>;

/// @brief Whether a session protects its packets.
enum class CryptoPolicy : std::uint8_t {
  /// @brief No authentication or encryption; Senkusha and the plaintext phase
  /// before keys exist.
  disabled,
  /// @brief Every packet except the handshake, length-prefixed and
  /// client-information families is authenticated, and payloads are
  /// encrypted.
  required,
};

/// @brief Why a packet could not be protected or opened.
enum class CryptoError : std::uint8_t {
  /// @brief No cipher direction is installed for that direction yet.
  key_material_missing,
  /// @brief The key material has the wrong sizes or no GKCrypt index.
  invalid_key_material,
  /// @brief The family carries no tag (length-prefixed, client information,
  /// trace), or generic data before version 15.
  unsupported_packet_family,
  /// @brief The packet is too short to hold the tag and key position.
  packet_too_short,
  /// @brief The authenticated or encrypted range lies outside the packet.
  invalid_coverage,
  /// @brief The platform's AES, GMAC, SHA-256 or HMAC failed, or memory ran
  /// out.
  provider_failure,
  /// @brief The received tag does not match.
  authentication_failed,
  /// @brief A key position would pass 2^64.
  counter_overflow,
  /// @brief A local packet is protected while the previous one's reservation is
  /// still open.
  reservation_in_progress,
  /// @brief A reservation was committed after a newer one superseded it.
  stale_reservation,
};

/// @brief The library's wiping byte buffer, used for every key and protected
/// packet copy.
using libchiaking::SecretBytes;

/// @brief The secrets one cipher direction is derived from.
///
/// For a console session it holds the 16-byte handshake key from the session
/// start, the ECDH shared secret (32 bytes for secp256k1, 66 for secp521r1)
/// and the GKCrypt index: 2 for the client's direction, 3 for the console's.
/// Move-only; the buffers wipe themselves.
class CryptoMaterial {
 public:
  /// @brief Holds only an ECDH secret; such material has no GKCrypt index and
  /// the platform provider refuses it.
  ///
  /// @param ecdh_secret The shared secret.
  explicit CryptoMaterial(SecretBytes ecdh_secret) noexcept
      : ecdh_secret_(std::move(ecdh_secret)) {}
  /// @brief Holds the full material of one direction.
  ///
  /// @param handshake_key The 16-byte handshake key.
  /// @param ecdh_secret The ECDH shared secret.
  /// @param gkcrypt_index The direction's index.
  CryptoMaterial(SecretBytes handshake_key, SecretBytes ecdh_secret,
                 std::uint8_t gkcrypt_index) noexcept
      : handshake_key_(std::move(handshake_key)),
        ecdh_secret_(std::move(ecdh_secret)),
        gkcrypt_index_(gkcrypt_index) {}

  /// @brief Not copyable; secrets are never duplicated implicitly.
  CryptoMaterial(const CryptoMaterial&) = delete;
  /// @brief Not copy-assignable.
  CryptoMaterial& operator=(const CryptoMaterial&) = delete;
  /// @brief Moves the secrets.
  CryptoMaterial(CryptoMaterial&&) noexcept = default;
  /// @brief Move-assigns the secrets.
  ///
  /// @return This object.
  CryptoMaterial& operator=(CryptoMaterial&&) noexcept = default;

  /// @brief Reads the handshake key.
  ///
  /// @return The key; empty when not held.
  std::span<const std::byte> handshake_key() const noexcept {
    return handshake_key_.bytes();
  }
  /// @brief Reads the ECDH secret.
  ///
  /// @return The secret.
  std::span<const std::byte> ecdh_secret() const noexcept {
    return ecdh_secret_.bytes();
  }
  /// @brief Reads the GKCrypt index.
  ///
  /// @return The index; empty when not held.
  std::optional<std::uint8_t> gkcrypt_index() const noexcept {
    return gkcrypt_index_;
  }
  /// @brief Tells whether the material can derive a GKCrypt direction.
  ///
  /// @return Whether an index is held.
  bool has_gkcrypt_material() const noexcept {
    return gkcrypt_index_.has_value();
  }

 private:
  /// @brief The handshake key.
  SecretBytes handshake_key_;
  /// @brief The ECDH shared secret.
  SecretBytes ecdh_secret_;
  /// @brief The GKCrypt index.
  std::optional<std::uint8_t> gkcrypt_index_;
};

/// @brief One direction's cipher: a keystream and a MAC keyed to positions in
/// it.
///
/// Implementations may be called from the session's pumping thread only, but
/// must tolerate the context's const reads from snapshot threads.
class CryptoDirection {
 public:
  /// @brief Makes an empty direction.
  CryptoDirection() = default;
  /// @brief Wipes the direction's keys.
  virtual ~CryptoDirection() = default;
  /// @brief Not copyable.
  CryptoDirection(const CryptoDirection&) = delete;
  /// @brief Not copy-assignable.
  CryptoDirection& operator=(const CryptoDirection&) = delete;
  /// @brief Movable.
  CryptoDirection(CryptoDirection&&) noexcept = default;
  /// @brief Move-assignable.
  ///
  /// @return This object.
  CryptoDirection& operator=(CryptoDirection&&) noexcept = default;

  /// @brief Computes the 4-byte GMAC tag of bytes at a key position.
  ///
  /// @param key_position The packet's key position.
  /// @param authenticated_bytes The bytes covered, with the tag field zeroed.
  /// @return The tag.
  /// @retval CryptoError::provider_failure The platform failed.
  [[nodiscard]] virtual auto authenticate(
      std::uint64_t key_position,
      std::span<const std::byte> authenticated_bytes)
      -> Expected<Mac, CryptoError> = 0;
  /// @brief Writes input XORed with the keystream starting at a key position
  /// into output, which encrypts and decrypts alike.
  ///
  /// Passing the same bytes as input and output transforms them in place; the
  /// two must otherwise not overlap.
  ///
  /// @param key_position Byte offset into the keystream.
  /// @param input The bytes to transform.
  /// @param output Where the result goes; exactly as long as `input`.
  /// @return Nothing.
  /// @retval CryptoError::counter_overflow The range overflows.
  /// @retval CryptoError::provider_failure The platform failed.
  [[nodiscard]] virtual auto apply_keystream(std::uint64_t key_position,
                                             std::span<const std::byte> input,
                                             std::span<std::byte> output)
      -> Expected<void, CryptoError> = 0;
};

/// @brief Turns key material into cipher directions.
///
/// Not copyable; one provider serves many sessions and must be safe to call
/// from any of their threads.
class CryptoProvider {
 public:
  /// @brief Makes a provider.
  CryptoProvider() = default;
  /// @brief Releases the provider.
  virtual ~CryptoProvider() = default;
  /// @brief Not copyable.
  CryptoProvider(const CryptoProvider&) = delete;
  /// @brief Not copy-assignable.
  CryptoProvider& operator=(const CryptoProvider&) = delete;

  /// @brief Creates the direction this side sends with.
  ///
  /// @param material The material of the local direction.
  /// @return The direction.
  /// @retval CryptoError::invalid_key_material The material is unusable.
  /// @retval CryptoError::provider_failure The platform failed.
  [[nodiscard]] virtual auto create_local(CryptoMaterial material)
      -> Expected<std::unique_ptr<CryptoDirection>, CryptoError> = 0;
  /// @brief Creates the direction the far end sends with.
  ///
  /// @param material The material of the remote direction.
  /// @return The direction.
  /// @retval CryptoError::invalid_key_material The material is unusable.
  /// @retval CryptoError::provider_failure The platform failed.
  [[nodiscard]] virtual auto create_remote(CryptoMaterial material)
      -> Expected<std::unique_ptr<CryptoDirection>, CryptoError> = 0;

  /// @brief Derives the raw key and IV of a direction, for session sharing.
  ///
  /// @param material The material of the direction.
  /// @return The keys; the default refuses.
  /// @retval CryptoError::provider_failure Exporting is not supported.
  [[nodiscard]] virtual auto export_receive_keys(const CryptoMaterial& material)
      -> Expected<SourceReceiveKeys, CryptoError> {
    (void)material;
    return unexpected(CryptoError::provider_failure);
  }
  /// @brief Creates a direction from exported keys, for a viewer that only
  /// opens forwarded packets.
  ///
  /// @param keys The exported keys.
  /// @return The direction; the default refuses.
  /// @retval CryptoError::provider_failure Not supported.
  [[nodiscard]] virtual auto create_receive_only(const SourceReceiveKeys& keys)
      -> Expected<std::unique_ptr<CryptoDirection>, CryptoError> {
    (void)keys;
    return unexpected(CryptoError::provider_failure);
  }
};

/// @brief Where a family keeps its tag and key position, relative to the start
/// of the packet, and how its payload is keyed.
struct PacketCryptoOffsets {
  /// @brief Offset of the 4-byte tag.
  std::size_t mac{};
  /// @brief Offset of the 4-byte big-endian low half of the key position.
  std::size_t key_position_low{};
  /// @brief Whether the key position is zeroed, as the tag is, before the tag
  /// is computed (control and congestion packets).
  bool zero_key_position_for_authentication{};
  /// @brief Added to the key position to find where the payload's keystream
  /// starts: 16 for AV, feedback, extended-message and generic-data packets,
  /// 0 otherwise.
  std::uint64_t cipher_key_position_addition{};
};

/// @brief Which bytes of a packet are authenticated and which are encrypted.
struct PacketCryptoCoverage {
  /// @brief Start of the authenticated range.
  std::size_t authenticated_offset{};
  /// @brief Bytes in the authenticated range.
  std::size_t authenticated_size{};
  /// @brief Start of the encrypted range; inside the authenticated one.
  std::size_t payload_offset{};
  /// @brief Bytes in the encrypted range; zero to authenticate only.
  std::size_t payload_size{};
  /// @brief Replaces the family's cipher key position addition, for a
  /// payload keyed apart from its family, such as an encrypted direct
  /// message in a control packet.
  std::optional<std::uint64_t> cipher_key_position_addition{};
};

/// @brief The key positions and cipher directions of one Takion session.
///
/// The local position starts where the session start says and advances by
/// each sent packet's key advance once the packet has actually left the
/// socket: protect_local() hands out a LocalReservation, transmit commits it,
/// and a failed send rolls it back so the position is reused. Only one
/// reservation may be open at a time. The remote position follows the
/// latest authenticated packet. With CryptoPolicy::disabled every call
/// succeeds without touching the packet.
///
/// Owned by the Takion client and used under its lock; not movable, since
/// reservations point back at it.
class CryptoContext {
 public:
  /// @brief The claim on a range of the local keystream for one packet being
  /// sent.
  ///
  /// Committing moves the local position on; destroying or rolling back an
  /// uncommitted reservation releases it with the position unchanged. A
  /// default-constructed reservation (crypto disabled) commits as a no-op.
  /// Move-only.
  class LocalReservation {
   public:
    /// @brief Makes a no-op reservation.
    LocalReservation() = default;
    /// @brief Rolls back an uncommitted reservation.
    ~LocalReservation();
    /// @brief Not copyable.
    LocalReservation(const LocalReservation&) = delete;
    /// @brief Not copy-assignable.
    LocalReservation& operator=(const LocalReservation&) = delete;
    /// @brief Takes over another reservation, leaving it empty.
    ///
    /// @param other The reservation to take.
    LocalReservation(LocalReservation&& other) noexcept;
    /// @brief Rolls this reservation back and takes over another.
    ///
    /// @param other The reservation to take.
    /// @return This object.
    LocalReservation& operator=(LocalReservation&& other) noexcept;

    /// @brief Reads the key position the packet was protected at.
    ///
    /// @return The position.
    std::uint64_t key_position() const noexcept { return key_position_; }
    /// @brief Advances the local key position past the packet.
    ///
    /// @return Nothing, also for a no-op reservation.
    /// @retval CryptoError::stale_reservation Already committed or rolled back,
    /// or
    ///     superseded.
    [[nodiscard]] auto commit() noexcept -> Expected<void, CryptoError>;
    /// @brief Releases the claim without moving the position.
    void rollback() noexcept;

   private:
    /// @brief Lets the context create live reservations.
    friend class CryptoContext;
    /// @brief Makes a live reservation.
    ///
    /// @param owner The context.
    /// @param key_position The position the packet used.
    /// @param next_position The position after it.
    /// @param generation The reservation's number, to detect superseded ones.
    LocalReservation(CryptoContext* owner, std::uint64_t key_position,
                     std::uint64_t next_position,
                     std::uint64_t generation) noexcept;

    /// @brief The context; null once settled.
    CryptoContext* owner_{};
    /// @brief The position the packet used.
    std::uint64_t key_position_{};
    /// @brief The position to move to on commit.
    std::uint64_t next_position_{};
    /// @brief The reservation's number.
    std::uint64_t generation_{};
    /// @brief Whether this is a no-op reservation.
    bool no_op_{true};
  };

  /// @brief A tag computed apart from the packet, for trace packets that carry
  /// it in their body, and the reservation behind it.
  struct DetachedLocalAuthentication {
    /// @brief The tag.
    Mac mac{};
    /// @brief The reservation to commit once the packet is sent.
    LocalReservation reservation;
  };

  /// @brief Makes a context with no directions installed and both positions 0.
  ///
  /// @param policy Whether packets are protected.
  explicit CryptoContext(CryptoPolicy policy) noexcept;
  /// @brief Releases the directions.
  ~CryptoContext() = default;
  /// @brief Not copyable.
  CryptoContext(const CryptoContext&) = delete;
  /// @brief Not copy-assignable.
  CryptoContext& operator=(const CryptoContext&) = delete;
  /// @brief Not movable; reservations point at it.
  CryptoContext(CryptoContext&&) = delete;
  /// @brief Not move-assignable.
  CryptoContext& operator=(CryptoContext&&) = delete;

  /// @brief Installs the direction this side sends with.
  ///
  /// @param provider Creates the direction.
  /// @param material The local material.
  /// @return Nothing.
  /// @retval CryptoError::reservation_in_progress A local packet is being sent.
  /// @retval CryptoError The provider's failure.
  [[nodiscard]] auto install_local(CryptoProvider& provider,
                                   CryptoMaterial material)
      -> Expected<void, CryptoError>;
  /// @brief Installs the direction the far end sends with.
  ///
  /// @param provider Creates the direction.
  /// @param material The remote material.
  /// @return Nothing.
  /// @retval CryptoError The provider's failure.
  [[nodiscard]] auto install_remote(CryptoProvider& provider,
                                    CryptoMaterial material)
      -> Expected<void, CryptoError>;

  /// @brief Writes the key position and tag into an outgoing packet and
  /// encrypts its payload, claiming the keystream it uses.
  ///
  /// @param wire The whole packet, without envelope; changed in place only on
  ///     success.
  /// @param family Its family.
  /// @param profile The session's wire profile, which decides the header shift.
  /// @param coverage The authenticated and encrypted ranges.
  /// @param key_advance_input_size The packet's size for key advance; the
  ///     position moves by this plus its remainder modulo 16.
  /// @return A reservation to commit once the packet is sent; a no-op one when
  ///     crypto is disabled.
  /// @retval CryptoError::reservation_in_progress Another reservation is open.
  /// @retval CryptoError::key_material_missing No local direction.
  /// @retval CryptoError::unsupported_packet_family The family is not
  ///     protected.
  /// @retval CryptoError::invalid_coverage The ranges do not fit.
  /// @retval CryptoError::packet_too_short The tag or position does not fit.
  /// @retval CryptoError::counter_overflow The position would overflow.
  /// @retval CryptoError::provider_failure The platform failed.
  [[nodiscard]] auto protect_local(std::span<std::byte> wire,
                                   PacketFamily family,
                                   const WireProfile& profile,
                                   PacketCryptoCoverage coverage,
                                   std::uint64_t key_advance_input_size)
      -> Expected<LocalReservation, CryptoError>;

  /// @brief Checks an incoming packet's tag and decrypts its covered payload.
  ///
  /// The full key position is reconstructed from the packet's low 32 bits and
  /// the last accepted position, allowing for wrap in either direction; it
  /// becomes the new remote position when the tag checks.
  ///
  /// @param wire The whole packet, without envelope; decrypted in place.
  /// @param family Its family.
  /// @param profile The session's wire profile.
  /// @param coverage The authenticated and encrypted ranges.
  /// @return The packet's key position; 0 when crypto is disabled.
  /// @retval CryptoError::key_material_missing No remote direction.
  /// @retval CryptoError::unsupported_packet_family The family is not
  ///     protected.
  /// @retval CryptoError::invalid_coverage The ranges do not fit.
  /// @retval CryptoError::packet_too_short The tag or position does not fit.
  /// @retval CryptoError::counter_overflow The position would overflow.
  /// @retval CryptoError::authentication_failed The tag does not match.
  /// @retval CryptoError::provider_failure The platform failed.
  [[nodiscard]] auto authenticate_and_decrypt_remote(
      std::span<std::byte> wire, PacketFamily family,
      const WireProfile& profile, PacketCryptoCoverage coverage)
      -> Expected<std::uint64_t, CryptoError>;

  /// @brief Computes the tag of bytes for a packet that carries its tag
  /// elsewhere (a trace packet), claiming keystream as protect_local() does.
  ///
  /// @param authenticated The bytes to authenticate.
  /// @param key_advance_input_size The size to advance the position by.
  /// @return The tag and its reservation; a zero tag and no-op reservation when
  ///     crypto is disabled.
  /// @retval CryptoError::reservation_in_progress Another reservation is open.
  /// @retval CryptoError::key_material_missing No local direction.
  /// @retval CryptoError::counter_overflow The position would overflow.
  /// @retval CryptoError::provider_failure The platform failed.
  [[nodiscard]] auto authenticate_local_detached(
      std::span<const std::byte> authenticated,
      std::uint64_t key_advance_input_size)
      -> Expected<DetachedLocalAuthentication, CryptoError>;

  /// @brief Checks a tag carried apart from the bytes it covers.
  ///
  /// @param authenticated The bytes covered.
  /// @param key_position_low The low 32 bits of the key position.
  /// @param received_mac The tag received.
  /// @return The full key position, which becomes the remote position; 0 when
  ///     crypto is disabled.
  /// @retval CryptoError::key_material_missing No remote direction.
  /// @retval CryptoError::counter_overflow The position would overflow.
  /// @retval CryptoError::authentication_failed The tag does not match.
  /// @retval CryptoError::provider_failure The platform failed.
  [[nodiscard]] auto verify_remote_detached(
      std::span<const std::byte> authenticated, std::uint32_t key_position_low,
      const Mac& received_mac) -> Expected<std::uint64_t, CryptoError>;

  /// @brief XORs the remote keystream at a key position into bytes, as an
  /// encrypted direct message's data is read.
  ///
  /// @param key_position The position in the remote keystream.
  /// @param bytes The bytes to decrypt in place.
  /// @return Nothing.
  /// @retval CryptoError::key_material_missing No remote direction.
  /// @retval CryptoError::provider_failure The platform failed.
  [[nodiscard]] auto apply_remote_keystream(std::uint64_t key_position,
                                            std::span<std::byte> bytes)
      -> Expected<void, CryptoError>;
  /// @brief Decrypts a payload whose packet was already authenticated, such as
  /// AV unit data once its header has been parsed.
  ///
  /// @param payload The bytes to decrypt.
  /// @param output Where the plaintext goes, as long as `payload`; the payload
  ///     itself to decrypt in place.
  /// @param family The packet's family.
  /// @param profile The session's wire profile.
  /// @param key_position The packet's key position, as authentication returned.
  /// @return Nothing.
  /// @retval CryptoError::key_material_missing No remote direction.
  /// @retval CryptoError::unsupported_packet_family The family is not
  ///     protected.
  /// @retval CryptoError::invalid_coverage `output` is not as long as
  ///     `payload`.
  /// @retval CryptoError::counter_overflow The position would overflow.
  /// @retval CryptoError::provider_failure The platform failed.
  [[nodiscard]] auto decrypt_remote_payload(std::span<const std::byte> payload,
                                            std::span<std::byte> output,
                                            PacketFamily family,
                                            const WireProfile& profile,
                                            std::uint64_t key_position)
      -> Expected<void, CryptoError>;

  /// @brief Locates a family's tag and key position for a version.
  ///
  /// Without the extended header the offsets are: control tag 5, position 9;
  /// video and audio tag 10, position 14; congestion tag 7, position 11;
  /// extended message and generic data tag 1, position 5; feedback tag 8,
  /// position 4. With the 8-byte extended header (version 20 on, every family
  /// but control, length-prefixed and client information) each moves 8 later.
  ///
  /// @param family The packet family.
  /// @param profile The session's wire profile.
  /// @return The offsets.
  /// @retval CryptoError::unsupported_packet_family Length-prefixed, client
  ///     information or trace, or generic data before version 15.
  [[nodiscard]] static auto packet_offsets(PacketFamily family,
                                           const WireProfile& profile)
      -> Expected<PacketCryptoOffsets, CryptoError>;

  /// @brief Reads whether packets are protected.
  ///
  /// @return The policy.
  CryptoPolicy policy() const noexcept { return policy_; }
  /// @brief Reads the next local key position.
  ///
  /// @return The position.
  std::uint64_t local_key_position() const noexcept { return local_position_; }
  /// @brief Reads the last accepted remote key position.
  ///
  /// @return The position.
  std::uint64_t remote_key_position() const noexcept {
    return remote_position_;
  }

  /// @brief Sets the next local key position, as the session start dictates.
  ///
  /// @param position The position.
  /// @return Nothing.
  /// @retval CryptoError::reservation_in_progress A reservation is open.
  [[nodiscard]] auto set_local_key_position(std::uint64_t position) noexcept
      -> Expected<void, CryptoError>;
  /// @brief Sets the remote key position reconstruction starts from.
  ///
  /// @param position The position.
  void set_remote_key_position(std::uint64_t position) noexcept {
    remote_position_ = position;
  }

 private:
  /// @brief Lets reservations commit and roll back.
  friend class LocalReservation;
  /// @brief Releases the open reservation if it is the given one.
  ///
  /// @param generation The reservation's number.
  void rollback(std::uint64_t generation) noexcept;
  /// @brief Moves the local position on for the open reservation.
  ///
  /// @param generation The reservation's number.
  /// @param next_position The new position.
  /// @return Nothing.
  /// @retval CryptoError::stale_reservation It is not the open reservation.
  [[nodiscard]] auto commit(std::uint64_t generation,
                            std::uint64_t next_position) noexcept
      -> Expected<void, CryptoError>;

  /// @brief Whether packets are protected.
  CryptoPolicy policy_;
  /// @brief The direction this side sends with; null until installed.
  std::unique_ptr<CryptoDirection> local_;
  /// @brief The direction the far end sends with; null until installed.
  std::unique_ptr<CryptoDirection> remote_;
  /// @brief Next local key position.
  std::uint64_t local_position_{};
  /// @brief Last accepted remote key position.
  std::uint64_t remote_position_{};
  /// @brief Number of the latest reservation.
  std::uint64_t reservation_generation_{};
  /// @brief Whether a local reservation is open.
  bool local_reservation_active_{};

 private:
  /// @brief Tells whether a range lies inside a total.
  ///
  /// @param offset Start of the range.
  /// @param size Its length.
  /// @param total The containing length.
  /// @return Whether `offset + size <= total` without overflow.
  static bool range_fits(std::size_t offset, std::size_t size,
                         std::size_t total) noexcept;
  /// @brief Computes the next key position: `position + input_size +
  /// input_size % 16`.
  ///
  /// @param position The current position.
  /// @param input_size The packet's size for key advance.
  /// @return The next position.
  /// @retval CryptoError::counter_overflow It would overflow.
  [[nodiscard]] static auto checked_advance(std::uint64_t position,
                                            std::uint64_t input_size)
      -> Expected<std::uint64_t, CryptoError>;
  /// @brief Adds to a key position.
  ///
  /// @param position The position.
  /// @param addition The amount.
  /// @return The sum.
  /// @retval CryptoError::counter_overflow It would overflow.
  [[nodiscard]] static auto checked_addition(std::uint64_t position,
                                             std::uint64_t addition)
      -> Expected<std::uint64_t, CryptoError>;
  /// @brief Checks the ranges against the packet and the family's offsets.
  ///
  /// @param wire The packet.
  /// @param coverage The ranges.
  /// @param offsets The family's offsets.
  /// @return Nothing.
  /// @retval CryptoError::invalid_coverage A range does not fit.
  /// @retval CryptoError::packet_too_short The tag or position does not fit.
  [[nodiscard]] static auto validate_coverage(std::span<const std::byte> wire,
                                              PacketCryptoCoverage coverage,
                                              PacketCryptoOffsets offsets)
      -> Expected<void, CryptoError>;
  /// @brief Copies the authenticated range with the tag (and, where the family
  /// asks, the key position) zeroed.
  ///
  /// @param wire The packet.
  /// @param coverage The ranges.
  /// @param offsets The family's offsets.
  /// @return The bytes to feed the GMAC.
  [[nodiscard]] static auto authenticated_copy(std::span<const std::byte> wire,
                                               PacketCryptoCoverage coverage,
                                               PacketCryptoOffsets offsets)
      -> std::vector<std::byte>;
  /// @brief Rebuilds a 64-bit key position from its low 32 bits, choosing the
  /// high half that puts it within 2^31 of the previous position.
  ///
  /// @param previous The last accepted position.
  /// @param low The low 32 bits received.
  /// @return The full position.
  /// @retval CryptoError::counter_overflow The high half would wrap.
  [[nodiscard]] static auto reconstruct_position(std::uint64_t previous,
                                                 std::uint32_t low)
      -> Expected<std::uint64_t, CryptoError>;
};

}  // namespace libchiaking::net::protocol::takion
