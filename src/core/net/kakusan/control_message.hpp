/// @file
/// @brief Declares the Kakusan control messages carried reliably inside DTLS,
/// and
/// their JSON codec.
#pragma once

#include "libchiaking/detail/kakusan/protocol_reason.hpp"
#include "libchiaking/detail/secret_array.hpp"
#include "libchiaking/detail/secret_string.hpp"
#include "src/core/net/kakusan/controller_frame.hpp"
#include "src/core/net/kakusan/identifiers.hpp"
#include "src/core/net/kakusan/result.hpp"
#include "src/core/net/kakusan/source_setup.hpp"
#include "src/core/utils/json/strict.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace libchiaking::net::kakusan::control {

/// @brief Why a visit ends, under the protocol's name.
using Reason = libchiaking::kakusan::ProtocolReason;

/// @brief The admission timeout ADMISSION_PENDING announces, in milliseconds:
/// 60 seconds.
inline constexpr std::uint64_t admission_timeout_ms = 60000;
/// @brief The controller-state rate SEAT_GRANTED announces: 60 Hz.
inline constexpr std::uint64_t input_rate_hz = 60;
/// @brief Bytes of a SPAKE2 share: an uncompressed P-256 point.
inline constexpr std::size_t share_size = 65;

/// @brief A SPAKE2 share.
using Share = std::array<std::byte, share_size>;
/// @brief An unpredictable PING nonce.
using Nonce = std::array<std::byte, 8>;

/// @brief HELLO: the first message each side sends, carrying its transcript.
struct Hello {
  /// @brief The message type.
  static constexpr std::string_view wire_name = "HELLO";
  /// @brief The JSON member carrying the one field, base64url.
  static constexpr std::string_view wire_field = "transcript";
  /// @brief The sender's transcript.
  Digest256 transcript{};
};
/// @brief IDENTITY: the sender's Ed25519 key and its signature over the
/// transcript (see IdentityProof); the host adds its session and seat counts.
struct Identity {
  /// @brief The message type.
  static constexpr std::string_view wire_name = "IDENTITY";
  /// @brief The host's session id and seat counts, signed with its identity.
  struct Hosting {
    /// @brief The host session's id.
    SessionId session_id{};
    /// @brief Seats held by local players.
    std::uint8_t local_seats{};
    /// @brief Seats held by admitted guests.
    std::uint8_t remote_seats{};
    /// @brief Seats reserved by guests being admitted.
    std::uint8_t reserved_seats{};
  };

  /// @brief `identity_key`: the sender's public key.
  IdentityKey key{};
  /// @brief `sig`: the signature.
  Signature signature{};
  /// @brief The host's details; present exactly when the host sends.
  std::optional<Hosting> hosting;
};
/// @brief PAKE_A: the host's SPAKE2 share.
struct PakeA {
  /// @brief The message type.
  static constexpr std::string_view wire_name = "PAKE_A";
  /// @brief The JSON member carrying the share.
  static constexpr std::string_view wire_field = "point";
  /// @brief The share.
  Share point{};
};
/// @brief PAKE_B: the guest's SPAKE2 share.
struct PakeB {
  /// @brief The message type.
  static constexpr std::string_view wire_name = "PAKE_B";
  /// @brief The JSON member carrying the share.
  static constexpr std::string_view wire_field = "point";
  /// @brief The share.
  Share point{};
};
/// @brief CONFIRM_A: the host's key confirmation MAC.
struct ConfirmA {
  /// @brief The message type.
  static constexpr std::string_view wire_name = "CONFIRM_A";
  /// @brief The JSON member carrying the MAC.
  static constexpr std::string_view wire_field = "mac";
  /// @brief The 64-byte MAC.
  Digest512 mac{};
};
/// @brief CONFIRM_B: the guest's key confirmation MAC.
struct ConfirmB {
  /// @brief The message type.
  static constexpr std::string_view wire_name = "CONFIRM_B";
  /// @brief The JSON member carrying the MAC.
  static constexpr std::string_view wire_field = "mac";
  /// @brief The 64-byte MAC.
  Digest512 mac{};
};
/// @brief AUTHENTICATED: pairing succeeded on the sender's side.
struct Authenticated {
  /// @brief The message type.
  static constexpr std::string_view wire_name = "AUTHENTICATED";
  /// @brief The JSON member carrying the transcript.
  static constexpr std::string_view wire_field = "transcript";
  /// @brief The sender's transcript.
  Digest256 transcript{};
};
/// @brief ADMISSION_PENDING: the host reserved a seat and asked the source to
/// join; carries `timeout_ms` 60000.
struct AdmissionPending {
  /// @brief The message type.
  static constexpr std::string_view wire_name = "ADMISSION_PENDING";
};

/// @brief SOURCE_READY: the guest accepted SOURCE_SETUP; carries `epoch` 1.
struct SourceReady {
  /// @brief The message type.
  static constexpr std::string_view wire_name = "SOURCE_READY";
  /// @brief SHA-256 of the canonical SOURCE_SETUP it accepted.
  Digest256 setup_hash{};
};
/// @brief SOURCE_IDR: the guest asks for a keyframe; carries `epoch` 1.
struct SourceIdr {
  /// @brief The message type.
  static constexpr std::string_view wire_name = "SOURCE_IDR";
};

/// @brief SEAT_GRANTED: the host gives the guest a seat; carries `input_hz` 60.
struct SeatGranted {
  /// @brief The message type.
  static constexpr std::string_view wire_name = "SEAT_GRANTED";
  /// @brief The seat and generation granted.
  SeatBinding binding;
  /// @brief Seats in the session.
  std::uint8_t capacity{};
  /// @brief Seats held by local players.
  std::uint8_t local_seats{};
  /// @brief The Button bits the guest may use; must include required_buttons.
  std::uint32_t allowed_buttons{};
};
/// @brief READY: the guest's decoder and controller are ready.
struct Ready {
  /// @brief The message type.
  static constexpr std::string_view wire_name = "READY";
  /// @brief The seat granted.
  SeatBinding binding;
};
/// @brief ACTIVE: play starts; the guest begins sending input.
struct Active {
  /// @brief The message type.
  static constexpr std::string_view wire_name = "ACTIVE";
  /// @brief The seat.
  SeatBinding binding;
};
/// @brief SEAT_REVOKED: the host takes the seat back.
struct SeatRevoked {
  /// @brief The message type.
  static constexpr std::string_view wire_name = "SEAT_REVOKED";
  /// @brief The seat.
  SeatBinding binding;
  /// @brief Why.
  Reason reason{Reason::user};
};
/// @brief LEAVE: the guest leaves.
struct Leave {
  /// @brief The message type.
  static constexpr std::string_view wire_name = "LEAVE";
  /// @brief Why.
  Reason reason{Reason::user};
};
/// @brief END: the host ends the guest's session.
struct End {
  /// @brief The message type.
  static constexpr std::string_view wire_name = "END";
  /// @brief Why.
  Reason reason{Reason::user};
};
/// @brief PING: a liveness probe, every 2 seconds from admission on.
struct Ping {
  /// @brief The message type.
  static constexpr std::string_view wire_name = "PING";
  /// @brief The JSON member carrying the nonce.
  static constexpr std::string_view wire_field = "nonce";
  /// @brief The nonce.
  Nonce nonce{};
};
/// @brief PONG: the answer to a PING.
struct Pong {
  /// @brief The message type.
  static constexpr std::string_view wire_name = "PONG";
  /// @brief The JSON member carrying the nonce.
  static constexpr std::string_view wire_field = "nonce";
  /// @brief The PING's nonce.
  Nonce nonce{};
};
/// @brief ERROR: a fatal failure; never answered.
struct Failure {
  /// @brief The message type.
  static constexpr std::string_view wire_name = "ERROR";

  /// @brief The ERROR to send for a local failure, hiding local-only causes.
  ///
  /// @param error The failure.
  /// @return The message; internal and crypto_unavailable are sent as
  ///     protocol.
  static constexpr auto reporting(Error error) noexcept -> Failure {
    return Failure{error == Error::internal ||
                           error == Error::crypto_unavailable
                       ? Error::protocol
                       : error};
  }

  /// @brief `code`: one of the 15 registry errors.
  Error code{Error::protocol};
};

/// @brief Any control message.
using Message = std::variant<Hello, Identity, PakeA, PakeB, ConfirmA, ConfirmB,
                             Authenticated, AdmissionPending, SourceSetup,
                             SourceReady, SourceIdr, SeatGranted, Ready, Active,
                             SeatRevoked, Leave, End, Ping, Pong, Failure>;

/// @brief Encodes and decodes Kakusan control messages: canonical JSON objects
/// of at most 8192 bytes with a `type` member and exactly the members of that
/// type (RCP WIRE section 7). Byte fields are unpadded base64url. Unknown types
/// or members, wrong types and out-of-range values are refused. Stateless.
class MessageCodec {
 public:
  /// @brief The wire type of a message.
  ///
  /// @param message The message.
  /// @return Its `type` value, such as `PING`.
  static auto type_name(const Message& message) noexcept -> std::string_view;
  /// @brief Encodes a message that carries no secret.
  ///
  /// @param message The message.
  /// @return The canonical JSON.
  /// @retval Error::protocol A field is invalid or the text exceeds 8192
  ///     bytes.
  /// @retval Error::internal The message carries a secret, which only
  ///     encode_secret() writes out.
  static auto encode(const Message& message) -> Result<std::string>;
  /// @brief Encodes a message into wiped storage, for SOURCE_SETUP's keys.
  ///
  /// @param message The message.
  /// @return The canonical JSON.
  /// @retval Error::protocol A field is invalid or the text exceeds 8192
  ///     bytes.
  static auto encode_secret(const Message& message) -> Result<SecretString>;
  /// @brief Decodes a message from text.
  ///
  /// @param text The JSON, 1 to 8192 bytes.
  /// @return The message.
  /// @retval Error::protocol The text or a member is invalid.
  /// @retval Error::codec SOURCE_SETUP names an unsupported codec or invalid
  ///     profiles.
  static auto decode(std::string_view text) -> Result<Message>;
  /// @brief Decodes a message from a parsed document.
  ///
  /// @param document The parsed JSON.
  /// @return The message.
  /// @retval Error::protocol A member is invalid.
  /// @retval Error::codec SOURCE_SETUP names an unsupported codec or invalid
  ///     profiles.
  static auto decode(const json::strict::Value& document) -> Result<Message>;
  /// @brief The hash SOURCE_READY echoes: SHA-256 of the canonical
  /// SOURCE_SETUP.
  ///
  /// @param crypto Hashes.
  /// @param setup The setup.
  /// @return The digest.
  /// @retval Error Encoding or hashing failed.
  static auto setup_hash(platform::CryptoProvider& crypto,
                         const SourceSetup& setup) -> Result<Digest256>;

 private:
  /// @brief A JSON member.
  using Member = json::strict::Member;
  /// @brief Reads members, refusing unread ones.
  using ObjectReader = json::strict::ObjectReader;
  /// @brief A JSON value.
  using Value = json::strict::Value;
  /// @brief Reads one message type's members.
  using Reader = auto (*)(ObjectReader& reader) -> Result<Message>;
  /// @brief One message type's name and reader.
  struct Entry {
    /// @brief The `type` value.
    std::string_view name;
    /// @brief Its reader.
    Reader read;
  };
  /// @brief The `reason` values, in Reason order.
  static constexpr std::array<std::string_view, 6> reason_names{
      "user", "capacity", "source", "timeout", "network", "protocol"};
  /// @brief The errors an ERROR may carry.
  static constexpr std::array<Error, 15> wire_errors{Error::invalid_block,
                                                     Error::expired,
                                                     Error::version,
                                                     Error::identity_changed,
                                                     Error::transcript,
                                                     Error::pairing_failed,
                                                     Error::no_path,
                                                     Error::no_capacity,
                                                     Error::source_join_failed,
                                                     Error::codec,
                                                     Error::timeout,
                                                     Error::protocol,
                                                     Error::source_format,
                                                     Error::congestion,
                                                     Error::source_ended};
  /// @brief Largest seat generation.
  static constexpr std::uint64_t max_generation =
      std::numeric_limits<std::uint32_t>::max();
  /// @brief Every message type, in Message order.
  static const std::array<Entry, std::variant_size_v<Message>> entries;

  /// @brief Builds the table of message types.
  ///
  /// @tparam Index The Message alternatives' indices.
  /// @return One entry per alternative.
  template <std::size_t... Index>
  static constexpr auto entries_of(std::index_sequence<Index...>) noexcept
      -> std::array<Entry, sizeof...(Index)>;
  /// @brief A JSON integer.
  ///
  /// @param value The value.
  /// @return The value.
  static auto integer(std::uint64_t value) -> Value;
  /// @brief A base64url JSON string.
  ///
  /// @tparam Size The array's size.
  /// @param value The bytes.
  /// @return The value.
  template <std::size_t Size>
  static auto bytes(const std::array<std::byte, Size>& value) -> Value;
  /// @brief A base64url JSON string held in wiped storage.
  ///
  /// @param value The key.
  /// @return The value.
  static auto secret(const SourceKey& value) -> Value;
  /// @brief The wire text of a reason.
  ///
  /// @param reason The reason.
  /// @return Its name.
  static auto reason_name(Reason reason) noexcept -> std::string_view;
  /// @brief Appends `seat` and `generation`.
  ///
  /// @param members The object being written.
  /// @param binding The binding.
  static void append_binding(Value::Object& members,
                             const SeatBinding& binding);
  /// @brief Whether a binding may be sent.
  ///
  /// @param binding The binding.
  /// @return True when seat and generation are at least 1.
  static constexpr auto valid_binding(const SeatBinding& binding) noexcept
      -> bool {
    return binding.seat >= 1 && binding.generation >= 1;
  }
  /// @brief Whether a grant is consistent.
  ///
  /// @param grant The grant.
  /// @return True when the seat is within the capacity, local seats are 1 to
  ///     the capacity and the buttons are defined and include the required
  ///     ones.
  static constexpr auto valid_grant(const SeatGranted& grant) noexcept -> bool {
    return valid_binding(grant.binding) && grant.capacity >= 1 &&
           grant.binding.seat <= grant.capacity && grant.local_seats >= 1 &&
           grant.local_seats <= grant.capacity &&
           (grant.allowed_buttons & ~button_mask) == 0 &&
           (grant.allowed_buttons & required_buttons) == required_buttons;
  }
  /// @brief Builds a message's JSON object.
  ///
  /// @param message The message.
  /// @return The object.
  /// @retval Error::protocol A field is invalid.
  static auto object(const Message& message) -> Result<Value>;
  /// @brief Refuses text over the size limit.
  ///
  /// @tparam Text std::string or SecretString.
  /// @param text The text.
  /// @return The text.
  /// @retval Error::protocol It exceeds 8192 bytes.
  template <class Text>
  static auto bounded(Text text) -> Result<Text>;
  /// @brief Appends a message's own members.
  ///
  /// @tparam Kind The message type.
  /// @param members The object being written.
  /// @param message The message.
  /// @return Nothing on success.
  /// @retval Error::protocol A field is invalid.
  template <class Kind>
  static auto write(Value::Object& members, const Kind& message)
      -> Result<void>;
  /// @brief Reads one message type's members.
  ///
  /// @tparam Kind The message type.
  /// @param reader The object's reader.
  /// @return The message.
  /// @retval Error::protocol A member is missing, invalid or extra.
  template <class Kind>
  static auto read(ObjectReader& reader) -> Result<Message>;
  /// @brief Reads an exact-length base64url member.
  ///
  /// @tparam Bytes A std::array of bytes.
  /// @param reader The reader.
  /// @param key The member.
  /// @return The bytes; nothing when missing or not exactly that long.
  template <class Bytes>
  [[nodiscard]] static auto read_bytes(ObjectReader& reader,
                                       std::string_view key)
      -> std::optional<Bytes>;
  /// @brief Reads a 16-byte key member into wiped storage.
  ///
  /// @param reader The reader.
  /// @param key The member.
  /// @return The key; nothing when missing or invalid.
  [[nodiscard]] static auto read_key(ObjectReader& reader, std::string_view key)
      -> std::optional<SourceKey>;
  /// @brief Reads an integer member that must have one value.
  ///
  /// @param reader The reader.
  /// @param key The member.
  /// @param expected The value.
  /// @return True when present and equal.
  static auto read_fixed(ObjectReader& reader, std::string_view key,
                         std::uint64_t expected) -> bool;
  /// @brief Reads `seat` and `generation`.
  ///
  /// @param reader The reader.
  /// @return The binding; nothing when out of range.
  [[nodiscard]] static auto read_binding(ObjectReader& reader)
      -> std::optional<SeatBinding>;
  /// @brief Reads `reason`.
  ///
  /// @param reader The reader.
  /// @return The reason; nothing when unknown.
  [[nodiscard]] static auto read_reason(ObjectReader& reader)
      -> std::optional<Reason>;
  /// @brief Reads one video profile object.
  ///
  /// @param value The object.
  /// @return The profile, sizes clamped to 16 bits; nothing when malformed.
  [[nodiscard]] static auto read_profile(const Value& value)
      -> std::optional<VideoProfile>;
};

}  // namespace libchiaking::net::kakusan::control
