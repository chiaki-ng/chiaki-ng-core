/// @file
/// @brief Declares the STUN message codec (RFC 8489) used for public address
/// discovery and ICE connectivity checks: header, attributes, integrity and
/// fingerprint.
///
/// Only the Binding method is spoken. Attributes are type (2 bytes), length (2
/// bytes) and value padded to 4 bytes, all big-endian.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/secret_string.hpp"
#include "libchiaking/platform/crypto/provider.hpp"
#include "libchiaking/platform/net/types.hpp"
#include "src/core/utils/bytes/endian.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::net::nat::stun {

/// @brief The STUN magic cookie at header offset 4 (RFC 8489 section 5).
inline constexpr std::uint32_t magic_cookie = 0x2112a442;
/// @brief The magic cookie as its four big-endian wire bytes.
inline constexpr auto magic_cookie_bytes = [] {
  std::array<std::byte, sizeof(magic_cookie)> encoded{};
  bytes::store_be(std::span{encoded}, magic_cookie);
  return encoded;
}();
/// @brief The value XORed into the CRC-32 of a FINGERPRINT attribute: ASCII
/// `STUN`.
inline constexpr std::uint32_t fingerprint_xor = 0x5354554e;
/// @brief Bytes in a STUN header.
inline constexpr std::size_t header_size = 20;
/// @brief Bytes in a transaction id.
inline constexpr std::size_t transaction_id_size = 12;
/// @brief Header offset of the transaction id.
inline constexpr std::size_t transaction_id_offset = 8;
/// @brief Largest message encoded or accepted, in bytes: the IPv6 minimum MTU.
inline constexpr std::size_t max_message_size = 1280;
/// @brief Bytes of a MESSAGE-INTEGRITY value: an HMAC-SHA1 digest, 20.
inline constexpr std::size_t integrity_size = platform::hmac_sha1_digest_size;
/// @brief Longest USERNAME value accepted or sent, in bytes.
inline constexpr std::size_t max_username_size = 513;
/// @brief Longest error reason phrase or SOFTWARE value accepted or sent, in
/// bytes.
inline constexpr std::size_t max_reason_size = 763;
/// @brief Most attribute types listed in, or collected for, an
/// UNKNOWN-ATTRIBUTES answer.
inline constexpr std::size_t max_unknown_attributes = 16;
/// @brief Bytes of an attribute's type and length.
inline constexpr std::size_t attribute_header_size = 4;
/// @brief Attribute values are padded to a multiple of this many bytes.
inline constexpr std::size_t attribute_alignment = 4;
/// @brief Bytes of an ICE-CONTROLLING or ICE-CONTROLLED tie-breaker.
inline constexpr std::size_t tie_breaker_size = 8;
/// @brief Lowest attribute type a receiver may ignore when it does not know it;
/// types below are comprehension-required.
inline constexpr std::uint16_t comprehension_optional_min = 0x8000;

/// @brief STUN attribute type codes (RFC 8489, RFC 8445).
namespace attribute {
/// @brief MAPPED-ADDRESS: the reflexive address, unobfuscated (old servers).
inline constexpr std::uint16_t mapped_address = 0x0001;
/// @brief USERNAME: the ICE `remote:local` ufrag pair.
inline constexpr std::uint16_t username = 0x0006;
/// @brief MESSAGE-INTEGRITY: HMAC-SHA1 over the message up to this attribute.
inline constexpr std::uint16_t message_integrity = 0x0008;
/// @brief ERROR-CODE: class, number and reason phrase.
inline constexpr std::uint16_t error_code = 0x0009;
/// @brief UNKNOWN-ATTRIBUTES: the required types a 420 error did not
/// understand.
inline constexpr std::uint16_t unknown_attributes = 0x000a;
/// @brief XOR-MAPPED-ADDRESS: the reflexive address XORed with the cookie and
/// transaction id.
inline constexpr std::uint16_t xor_mapped_address = 0x0020;
/// @brief PRIORITY: the priority a peer-reflexive candidate would get.
inline constexpr std::uint16_t priority = 0x0024;
/// @brief USE-CANDIDATE: the controlling agent nominates this pair.
inline constexpr std::uint16_t use_candidate = 0x0025;
/// @brief SOFTWARE: a description of the sender, ignored.
inline constexpr std::uint16_t software = 0x8022;
/// @brief FINGERPRINT: CRC-32 of the message XOR `STUN`, always last.
inline constexpr std::uint16_t fingerprint = 0x8028;
/// @brief ICE-CONTROLLED: the sender is the controlled agent; carries its
/// tie-breaker.
inline constexpr std::uint16_t ice_controlled = 0x8029;
/// @brief ICE-CONTROLLING: the sender is the controlling agent; carries its
/// tie-breaker.
inline constexpr std::uint16_t ice_controlling = 0x802a;
}  // namespace attribute

/// @brief STUN error codes the library sends or reads, and the valid range.
namespace error_code {
/// @brief 400 Bad Request.
inline constexpr std::uint16_t bad_request = 400;
/// @brief 401 Unauthorized: missing or wrong integrity.
inline constexpr std::uint16_t unauthorized = 401;
/// @brief 420 Unknown Attribute: a required attribute was not understood.
inline constexpr std::uint16_t unknown_attribute = 420;
/// @brief 487 Role Conflict: both ICE agents claimed the same role.
inline constexpr std::uint16_t role_conflict = 487;
/// @brief Lowest valid error code.
inline constexpr std::uint16_t min_code = 300;
/// @brief Highest valid error code.
inline constexpr std::uint16_t max_code = 699;
}  // namespace error_code

/// @brief The 96-bit id that ties a STUN response to its request.
using TransactionId = std::array<std::byte, transaction_id_size>;

/// @brief The class bits of a STUN message type.
enum class MessageClass : std::uint8_t {
  /// @brief A request, which expects a response.
  request,
  /// @brief An indication, which expects none.
  indication,
  /// @brief A success response.
  success,
  /// @brief An error response.
  error,
};

/// @brief Why a STUN message could not be decoded or encoded.
enum class MessageError : std::uint8_t {
  /// @brief The datagram is not a well-formed Binding message: wrong size,
  /// leading bits, cookie or method, a bad attribute, a bad fingerprint, or
  /// anything after the fingerprint.
  malformed,
  /// @brief A field is out of range or the message would exceed 1280 bytes.
  unencodable,
  /// @brief MESSAGE-INTEGRITY cannot be appended: the message is unaligned or
  /// the key empty.
  unsealable,
  /// @brief The crypto provider cannot compute HMAC-SHA1.
  crypto_unavailable,
  /// @brief The crypto provider failed computing HMAC-SHA1.
  crypto_failed,
};

/// @brief Whether an encoded message ends with a FINGERPRINT attribute.
enum class Fingerprint : bool {
  /// @brief No fingerprint.
  absent,
  /// @brief Append a fingerprint, as ICE requires.
  present,
};

/// @brief The 20-byte STUN header, all fields big-endian:
///
/// | Offset | Size | Field |
/// | ------ | ---- | ----- |
/// | 0 | 2 | message type: two zero bits, method and class bits interleaved |
/// | 2 | 2 | body length in bytes, a multiple of 4, header excluded |
/// | 4 | 4 | magic cookie 0x2112A442 |
/// | 8 | 12 | transaction id |
struct Header {
  /// @brief The Binding method, the only one this library speaks.
  static constexpr std::uint16_t binding_method = 0x0001;

  /// @brief The message type as on the wire.
  std::uint16_t type{};
  /// @brief Bytes after the header.
  std::uint16_t body_size{};
  /// @brief The magic cookie as read; must be magic_cookie.
  std::uint32_t cookie{magic_cookie};
  /// @brief The transaction id.
  TransactionId transaction{};

  /// @brief The message type of a Binding message of a class.
  ///
  /// @param kind The class.
  /// @return 0x0001, 0x0011, 0x0101 or 0x0111.
  static auto binding_type(MessageClass kind) noexcept -> std::uint16_t;
  /// @brief Reads the header fields without checking them.
  ///
  /// @param datagram The datagram.
  /// @return The header; nothing when the datagram is shorter than 20 bytes.
  [[nodiscard]] static auto read(std::span<const std::byte> datagram) noexcept
      -> std::optional<Header>;
  /// @brief Rewrites the body length field of an encoded message.
  ///
  /// @param message The message, at least 4 bytes.
  /// @param body The body length in bytes; truncated to 16 bits.
  static void write_body_size(std::span<std::byte> message,
                              std::size_t body) noexcept;

  /// @brief The class encoded in the type.
  ///
  /// @return The class.
  auto message_class() const noexcept -> MessageClass;
  /// @brief The method encoded in the type.
  ///
  /// @return The 12-bit method.
  auto method() const noexcept -> std::uint16_t;
  /// @brief Writes the header.
  ///
  /// @return The 20 bytes.
  auto encode() const noexcept -> std::array<std::byte, header_size>;
};

/// @brief One attribute as read: type, value and position. The value views
/// the datagram, which must outlive it.
struct Attribute {
  /// @brief The attribute type.
  std::uint16_t type{};
  /// @brief The value without padding.
  std::span<const std::byte> value;
  /// @brief Offset of the attribute's header in the datagram.
  std::size_t offset{};

  /// @brief XORs an address with the cookie followed by the transaction id,
  /// as
  /// XOR-MAPPED-ADDRESS does.
  ///
  /// @param address 4 or 16 address bytes.
  /// @param transaction The message's transaction id.
  /// @return The masked bytes in the first `address.size()` bytes; the rest
  ///     zero.
  static auto masked(std::span<const std::byte> address,
                     const TransactionId& transaction) noexcept
      -> std::array<std::byte, 16>;

  /// @brief Reads a MAPPED-ADDRESS or XOR-MAPPED-ADDRESS value.
  ///
  /// The value is a reserved byte, the family (1 IPv4, 2 IPv6), the 16-bit
  /// port
  /// and the address; for XOR-MAPPED-ADDRESS the port is XORed with the top
  /// 16
  /// bits of the cookie and the address with the cookie and transaction id.
  ///
  /// @param transaction The message's transaction id.
  /// @return The endpoint; nothing when the family is unknown or the value is
  ///     not 8 bytes for IPv4 or 20 for IPv6.
  [[nodiscard]] auto endpoint(const TransactionId& transaction) const noexcept
      -> std::optional<platform::SocketEndpoint>;
  /// @brief The value as text, byte for byte.
  ///
  /// @return The text.
  auto text() const -> std::string;
};

/// @brief Walks the attributes of a STUN message after its header.
///
/// Holds a view of the datagram, which must outlive it.
class AttributeReader {
 public:
  /// @brief Starts after the header.
  ///
  /// @param datagram The whole message.
  explicit AttributeReader(std::span<const std::byte> datagram) noexcept;

  /// @brief Whether every attribute has been read.
  ///
  /// @return True at the end of the datagram.
  auto exhausted() const noexcept -> bool;
  /// @brief Reads the next attribute and moves past its padding.
  ///
  /// @return The attribute; nothing at the end, or when the header or padded
  ///     value runs past the datagram.
  [[nodiscard]] auto next() noexcept -> std::optional<Attribute>;

  /// @brief Rounds a value size up to the attribute alignment.
  ///
  /// @param size The size in bytes.
  /// @return The next multiple of 4.
  static constexpr auto padded(std::size_t size) noexcept -> std::size_t {
    return (size + attribute_alignment - 1) & ~(attribute_alignment - 1);
  }

 private:
  /// @brief The message.
  std::span<const std::byte> datagram_;
  /// @brief Offset of the next attribute.
  std::size_t offset_{header_size};
};

/// @brief An ERROR-CODE attribute's code and reason.
struct ErrorResponse {
  /// @brief The code, 300 to 699.
  std::uint16_t code{};
  /// @brief The reason phrase, at most 763 bytes.
  std::string reason;

  /// @brief The standard reason phrase of a code this library sends.
  ///
  /// @param code The code.
  /// @return The phrase, such as `Role Conflict`; `Error` for other codes.
  static auto reason_phrase(std::uint16_t code) noexcept -> std::string_view;
  /// @brief Reads an ERROR-CODE value: two reserved bytes, the class (low 3
  /// bits), the number, then the reason.
  ///
  /// @param value The attribute value.
  /// @return The error; nothing when shorter than 4 bytes, the reason is too
  ///     long, the class is outside 3 to 6 or the number above 99.
  [[nodiscard]] static auto decode(std::span<const std::byte> value)
      -> std::optional<ErrorResponse>;
};

/// @brief A STUN Binding message: the fields this library sends and reads.
///
/// Encoded attributes appear in the order the fields are declared; integrity
/// and fingerprint are appended after, when asked for. Move-only, because
/// the USERNAME is held in wiping memory.
struct Message {
  /// @brief The class.
  MessageClass kind{MessageClass::request};
  /// @brief The transaction id.
  TransactionId transaction{};
  /// @brief XOR-MAPPED-ADDRESS, the reflexive address in a success response.
  std::optional<platform::SocketEndpoint> xor_mapped;
  /// @brief MAPPED-ADDRESS; read but never sent.
  std::optional<platform::SocketEndpoint> mapped;
  /// @brief USERNAME, the ICE ufrag pair, held in wiping memory.
  std::optional<SecretString> username;
  /// @brief PRIORITY.
  std::optional<std::uint32_t> priority;
  /// @brief Whether USE-CANDIDATE is present.
  bool use_candidate{};
  /// @brief ICE-CONTROLLING's tie-breaker.
  std::optional<std::uint64_t> ice_controlling;
  /// @brief ICE-CONTROLLED's tie-breaker.
  std::optional<std::uint64_t> ice_controlled;
  /// @brief ERROR-CODE.
  std::optional<ErrorResponse> error;
  /// @brief UNKNOWN-ATTRIBUTES; empty when absent.
  std::vector<std::uint16_t> unknown_attributes;

  /// @brief Whether a datagram looks like STUN, to tell it from other traffic
  /// on a shared socket.
  ///
  /// @param datagram The datagram.
  /// @return True when it is 20 to 1280 bytes, its first byte is 0 to 3, it
  ///     carries the cookie, and its length field is a multiple of 4 matching
  ///     the datagram.
  static auto recognises(std::span<const std::byte> datagram) noexcept -> bool;

  /// @brief Encodes the message.
  ///
  /// @param crypto Computes the integrity HMAC.
  /// @param integrity_key The HMAC-SHA1 key (the ICE password); empty for no
  ///     MESSAGE-INTEGRITY.
  /// @param fingerprint Whether to append FINGERPRINT.
  /// @return The message.
  /// @retval MessageError::unencodable A field is out of range or the message
  ///     exceeds 1280 bytes.
  /// @retval MessageError::crypto_unavailable HMAC-SHA1 is not available.
  /// @retval MessageError::crypto_failed HMAC-SHA1 failed.
  [[nodiscard]] auto encode(platform::CryptoProvider& crypto,
                            std::span<const std::byte> integrity_key,
                            Fingerprint fingerprint) const
      -> Expected<std::vector<std::byte>, MessageError>;

  /// @brief Appends MESSAGE-INTEGRITY, first setting the length field to
  /// cover
  /// it.
  ///
  /// @param crypto Computes the HMAC.
  /// @param message The encoded message so far.
  /// @param key The HMAC-SHA1 key.
  /// @return Nothing on success.
  /// @retval MessageError::unsealable The message is shorter than a header or
  ///     unaligned, or the key is empty.
  /// @retval MessageError::crypto_unavailable HMAC-SHA1 is not available.
  /// @retval MessageError::crypto_failed HMAC-SHA1 failed.
  [[nodiscard]] static auto append_integrity(platform::CryptoProvider& crypto,
                                             std::vector<std::byte>& message,
                                             std::span<const std::byte> key)
      -> Expected<void, MessageError>;
  /// @brief Appends FINGERPRINT, first setting the length field to cover it.
  ///
  /// @param message The encoded message so far.
  static void append_fingerprint(std::vector<std::byte>& message);

 private:
  /// @brief Appends header, attributes and padding to an encoded message.
  class Writer;

  /// @brief Encodes the header and every attribute, without integrity or
  /// fingerprint.
  ///
  /// @return The message.
  /// @retval MessageError::unencodable The username, error or unknown list is
  ///     out of range.
  [[nodiscard]] auto encode_attributes() const
      -> Expected<std::vector<std::byte>, MessageError>;
  /// @brief Appends the fingerprint if asked and checks the size.
  ///
  /// @param output The encoded message.
  /// @param fingerprint Whether to append FINGERPRINT.
  /// @return The message.
  /// @retval MessageError::unencodable It exceeds 1280 bytes.
  [[nodiscard]] static auto finish(std::vector<std::byte> output,
                                   Fingerprint fingerprint)
      -> Expected<std::vector<std::byte>, MessageError>;
};

/// @brief A decoded Binding message and what is needed to check it.
///
/// Attributes after MESSAGE-INTEGRITY, other than FINGERPRINT, are ignored, and
/// only the first of each type is used. Plain data, safe to copy.
struct ReceivedMessage {
  /// @brief The fields read.
  Message message;
  /// @brief Offset of MESSAGE-INTEGRITY; nothing when absent.
  std::optional<std::size_t> integrity_offset;
  /// @brief Whether a valid FINGERPRINT ended the message.
  bool fingerprinted{};
  /// @brief Comprehension-required types not understood, at most 16.
  std::vector<std::uint16_t> unknown_required;

  /// @brief Decodes a Binding message. Integrity is not checked here; see
  /// verify_integrity().
  ///
  /// @param datagram The datagram.
  /// @return The message.
  /// @retval MessageError::malformed The datagram is not a well-formed
  ///     Binding
  ///     message.
  [[nodiscard]] static auto decode(std::span<const std::byte> datagram)
      -> Expected<ReceivedMessage, MessageError>;

  /// @brief Checks MESSAGE-INTEGRITY in constant time.
  ///
  /// @param crypto Computes the HMAC.
  /// @param datagram The datagram decoded.
  /// @param key The HMAC-SHA1 key (the local ICE password).
  /// @return True when the attribute is present and matches; false when
  ///     absent,
  ///     the key is empty, the HMAC fails, or it does not match.
  auto verify_integrity(platform::CryptoProvider& crypto,
                        std::span<const std::byte> datagram,
                        std::span<const std::byte> key) const -> bool;

 private:
  /// @brief Remembers which attribute types have been read, so repeats are
  /// skipped.
  class SeenTypes;

  /// @brief Reads one attribute into the message.
  ///
  /// @param item The attribute.
  /// @return False when a known attribute has an invalid value.
  [[nodiscard]] auto absorb(const Attribute& item) -> bool;
};

}  // namespace libchiaking::net::nat::stun
