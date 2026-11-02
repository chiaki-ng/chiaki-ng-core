#include "src/core/net/nat/stun/message.hpp"

#include "src/core/utils/bytes/byte_io.hpp"
#include "src/core/utils/bytes/crc32.hpp"
#include "src/core/utils/memory/constant_time.hpp"

#include "libchiaking/detail/secret_array.hpp"

#include <algorithm>
#include <functional>

namespace libchiaking::net::nat::stun {

constexpr std::uint8_t family_ipv4 = 0x01;
constexpr std::uint8_t family_ipv6 = 0x02;
constexpr std::size_t address_value_header_size = 4;
constexpr std::size_t ipv4_address_value_size = 8;
constexpr std::size_t ipv6_address_value_size = 20;
constexpr std::size_t priority_value_size = 4;
constexpr std::size_t fingerprint_value_size = 4;
constexpr std::size_t error_code_header_size = 4;
constexpr std::size_t integrity_attribute_size =
    attribute_header_size + integrity_size;
constexpr std::size_t fingerprint_attribute_size =
    attribute_header_size + fingerprint_value_size;
constexpr std::size_t max_tracked_attribute_types = 16;
constexpr std::size_t encode_reserve_size = 256;
constexpr std::uint8_t first_byte_max = 3;
constexpr std::uint8_t leading_bits_mask = 0xc0;
constexpr std::uint16_t error_class_mask = 0x07;
constexpr std::uint16_t error_min_class = 3;
constexpr std::uint16_t error_max_class = 6;
constexpr std::uint16_t error_max_number = 99;
constexpr std::uint16_t error_class_multiplier = 100;
constexpr std::uint16_t class_request_bits = 0x0000;
constexpr std::uint16_t class_indication_bits = 0x0010;
constexpr std::uint16_t class_success_bits = 0x0100;
constexpr std::uint16_t class_error_bits = 0x0110;
constexpr std::uint16_t method_low_mask = 0x000f;
constexpr std::uint16_t method_middle_mask = 0x0070;
constexpr std::uint16_t method_high_mask = 0x0f80;
constexpr unsigned method_middle_shift = 1;
constexpr unsigned method_high_shift = 2;
constexpr unsigned class_low_shift = 4;
constexpr unsigned class_high_shift = 7;
constexpr std::uint16_t class_low_mask = 0x1;
constexpr std::uint16_t class_high_mask = 0x2;

auto Header::binding_type(MessageClass kind) noexcept -> std::uint16_t {
  std::uint16_t class_bits = class_request_bits;
  switch (kind) {
    case MessageClass::request:
      class_bits = class_request_bits;
      break;
    case MessageClass::indication:
      class_bits = class_indication_bits;
      break;
    case MessageClass::success:
      class_bits = class_success_bits;
      break;
    case MessageClass::error:
      class_bits = class_error_bits;
      break;
  }
  return static_cast<std::uint16_t>(
      (binding_method & method_low_mask) |
      ((binding_method & method_middle_mask) << method_middle_shift) |
      ((binding_method & method_high_mask) << method_high_shift) | class_bits);
}

auto Header::read(std::span<const std::byte> datagram) noexcept
    -> std::optional<Header> {
  if (datagram.size() < header_size) return std::nullopt;
  Header header;
  bytes::Reader reader{datagram};
  header.type = reader.u16_be().value();
  header.body_size = reader.u16_be().value();
  header.cookie = reader.u32_be().value();
  std::copy_n(datagram.begin() + transaction_id_offset, transaction_id_size,
              header.transaction.begin());
  return header;
}

void Header::write_body_size(std::span<std::byte> message,
                             std::size_t body) noexcept {
  bytes::store_be(message.subspan(2), static_cast<std::uint16_t>(body));
}

auto Header::message_class() const noexcept -> MessageClass {
  const auto bits = ((type >> class_low_shift) & class_low_mask) |
                    ((type >> class_high_shift) & class_high_mask);
  switch (bits) {
    case 0:
      return MessageClass::request;
    case 1:
      return MessageClass::indication;
    case 2:
      return MessageClass::success;
    default:
      return MessageClass::error;
  }
}

auto Header::method() const noexcept -> std::uint16_t {
  return static_cast<std::uint16_t>(
      (type & method_low_mask) |
      ((type >> method_middle_shift) & method_middle_mask) |
      ((type >> method_high_shift) & method_high_mask));
}

auto Header::encode() const noexcept -> std::array<std::byte, header_size> {
  std::array<std::byte, header_size> encoded{};
  const auto view = std::span{encoded};
  bytes::store_be(view, type);
  bytes::store_be(view.subspan(2), body_size);
  bytes::store_be(view.subspan(4), cookie);
  std::ranges::copy(transaction, view.subspan(transaction_id_offset).begin());
  return encoded;
}

auto Attribute::masked(std::span<const std::byte> address,
                       const TransactionId& transaction) noexcept
    -> std::array<std::byte, 16> {
  std::array<std::byte, 16> mask{};
  std::ranges::copy(transaction,
                    std::ranges::copy(magic_cookie_bytes, mask.begin()).out);
  std::array<std::byte, 16> result{};
  std::ranges::transform(address, mask, result.begin(), std::bit_xor<>{});
  return result;
}

auto Attribute::endpoint(const TransactionId& transaction) const noexcept
    -> std::optional<platform::SocketEndpoint> {
  if (value.size() < address_value_header_size) return std::nullopt;
  const auto family = std::to_integer<std::uint8_t>(value[1]);
  if (!(family == family_ipv4 && value.size() == ipv4_address_value_size) &&
      !(family == family_ipv6 && value.size() == ipv6_address_value_size))
    return std::nullopt;
  const bool xored = type == attribute::xor_mapped_address;
  const auto octets = value.subspan(address_value_header_size);
  auto port = bytes::load_be<std::uint16_t>(value.subspan(2));
  std::array<std::byte, 16> plain{};
  if (xored) {
    port = static_cast<std::uint16_t>(port ^ (magic_cookie >> 16U));
    plain = masked(octets, transaction);
  } else {
    std::ranges::copy(octets, plain.begin());
  }
  const auto address =
      platform::IpAddress::from_bytes(std::span{plain}.first(octets.size()));
  return platform::SocketEndpoint{*address, port};
}

auto Attribute::text() const -> std::string {
  std::string text(value.size(), '\0');
  std::ranges::transform(value, text.begin(), [](std::byte byte) {
    return static_cast<char>(std::to_integer<unsigned char>(byte));
  });
  return text;
}

AttributeReader::AttributeReader(std::span<const std::byte> datagram) noexcept
    : datagram_(datagram) {}

auto AttributeReader::exhausted() const noexcept -> bool {
  return offset_ >= datagram_.size();
}

auto AttributeReader::next() noexcept -> std::optional<Attribute> {
  if (exhausted() || datagram_.size() - offset_ < attribute_header_size)
    return std::nullopt;
  const auto type = bytes::Reader{datagram_.subspan(offset_)}.u16_be().value();
  const auto size = std::size_t{
      bytes::Reader{datagram_.subspan(offset_ + 2)}.u16_be().value()};
  if (padded(size) > datagram_.size() - offset_ - attribute_header_size)
    return std::nullopt;
  const Attribute item{
      type, datagram_.subspan(offset_ + attribute_header_size, size), offset_};
  offset_ += attribute_header_size + padded(size);
  return item;
}

auto ErrorResponse::reason_phrase(std::uint16_t code) noexcept
    -> std::string_view {
  switch (code) {
    case error_code::bad_request:
      return "Bad Request";
    case error_code::unauthorized:
      return "Unauthorized";
    case error_code::unknown_attribute:
      return "Unknown Attribute";
    case error_code::role_conflict:
      return "Role Conflict";
    default:
      return "Error";
  }
}

auto ErrorResponse::decode(std::span<const std::byte> value)
    -> std::optional<ErrorResponse> {
  if (value.size() < error_code_header_size ||
      value.size() - error_code_header_size > max_reason_size)
    return std::nullopt;
  const auto code_class =
      std::to_integer<std::uint16_t>(value[2]) & error_class_mask;
  const auto number = std::to_integer<std::uint16_t>(value[3]);
  if (code_class < error_min_class || code_class > error_max_class ||
      number > error_max_number)
    return std::nullopt;
  return ErrorResponse{
      static_cast<std::uint16_t>(code_class * error_class_multiplier + number),
      Attribute{attribute::error_code, value.subspan(error_code_header_size), 0}
          .text()};
}

class Message::Writer : public bytes::Appender {
 public:
  explicit Writer(std::vector<std::byte>& output) noexcept
      : bytes::Appender(output) {}

  void attribute_header(std::uint16_t type, std::size_t size) {
    u16_be(type);
    u16_be(static_cast<std::uint16_t>(size));
  }

  void padding(std::size_t size) {
    zeros(AttributeReader::padded(size) - size);
  }

  void text_attribute(std::uint16_t type, std::string_view value) {
    attribute_header(type, value.size());
    text(value);
    padding(value.size());
  }

  void xor_address(const platform::SocketEndpoint& endpoint,
                   const TransactionId& id) {
    const auto& address = endpoint.address;
    const auto octets = address.bytes();
    attribute_header(
        attribute::xor_mapped_address,
        address.is_ipv4() ? ipv4_address_value_size : ipv6_address_value_size);
    u8(0);
    u8(address.is_ipv4() ? family_ipv4 : family_ipv6);
    u16_be(static_cast<std::uint16_t>(endpoint.port ^ (magic_cookie >> 16U)));
    const auto masked = Attribute::masked(octets, id);
    bytes(std::span{masked}.first(octets.size()));
  }
};

auto Message::recognises(std::span<const std::byte> datagram) noexcept -> bool {
  if (datagram.size() < header_size || datagram.size() > max_message_size)
    return false;
  if (std::to_integer<std::uint8_t>(datagram[0]) > first_byte_max) return false;
  const auto length = bytes::Reader{datagram.subspan(2)}.u16_be().value();
  return bytes::Reader{datagram.subspan(4)}.u32_be().value() == magic_cookie &&
         length % attribute_alignment == 0 &&
         std::size_t{length} + header_size == datagram.size();
}

auto Message::encode_attributes() const
    -> Expected<std::vector<std::byte>, MessageError> {
  if (username && username->size() > max_username_size)
    return unexpected(MessageError::unencodable);
  if (error && (error->code < error_code::min_code ||
                error->code > error_code::max_code ||
                error->reason.size() > max_reason_size))
    return unexpected(MessageError::unencodable);
  if (unknown_attributes.size() > max_unknown_attributes)
    return unexpected(MessageError::unencodable);

  std::vector<std::byte> output;
  output.reserve(encode_reserve_size);
  Writer writer{output};
  writer.bytes(Header{Header::binding_type(kind), 0, magic_cookie, transaction}
                   .encode());

  if (xor_mapped) writer.xor_address(*xor_mapped, transaction);
  if (username) writer.text_attribute(attribute::username, username->view());
  if (priority) {
    writer.attribute_header(attribute::priority, priority_value_size);
    writer.u32_be(*priority);
  }
  if (ice_controlling) {
    writer.attribute_header(attribute::ice_controlling, tie_breaker_size);
    writer.u64_be(*ice_controlling);
  }
  if (ice_controlled) {
    writer.attribute_header(attribute::ice_controlled, tie_breaker_size);
    writer.u64_be(*ice_controlled);
  }
  if (use_candidate) writer.attribute_header(attribute::use_candidate, 0);
  if (error) {
    writer.attribute_header(attribute::error_code,
                            error_code_header_size + error->reason.size());
    writer.u16_be(0);
    writer.u8(static_cast<std::uint8_t>(error->code / error_class_multiplier));
    writer.u8(static_cast<std::uint8_t>(error->code % error_class_multiplier));
    writer.text(error->reason);
    writer.padding(error_code_header_size + error->reason.size());
  }
  if (!unknown_attributes.empty()) {
    writer.attribute_header(attribute::unknown_attributes,
                            unknown_attributes.size() * 2);
    for (const auto type : unknown_attributes) writer.u16_be(type);
    writer.padding(unknown_attributes.size() * 2);
  }

  Header::write_body_size(output, output.size() - header_size);
  return output;
}

auto Message::finish(std::vector<std::byte> output, Fingerprint fingerprint)
    -> Expected<std::vector<std::byte>, MessageError> {
  if (fingerprint == Fingerprint::present) append_fingerprint(output);
  if (output.size() > max_message_size)
    return unexpected(MessageError::unencodable);
  return output;
}

auto Message::encode(platform::CryptoProvider& crypto,
                     std::span<const std::byte> integrity_key,
                     Fingerprint fingerprint) const
    -> Expected<std::vector<std::byte>, MessageError> {
  auto output = encode_attributes();
  if (!output) return unexpected(output.error());
  if (!integrity_key.empty()) {
    const auto sealed = append_integrity(crypto, output.value(), integrity_key);
    if (!sealed) return unexpected(sealed.error());
  }
  return finish(std::move(output.value()), fingerprint);
}

auto Message::append_integrity(platform::CryptoProvider& crypto,
                               std::vector<std::byte>& message,
                               std::span<const std::byte> key)
    -> Expected<void, MessageError> {
  if (message.size() < header_size ||
      message.size() % attribute_alignment != 0 || key.empty())
    return unexpected(MessageError::unsealable);
  Header::write_body_size(
      message, message.size() + integrity_attribute_size - header_size);
  SecretArray<integrity_size> digest;
  const auto authenticated = crypto.hmac_sha1(
      key, message, std::span<std::byte>{digest.data(), digest.size()});
  if (!authenticated)
    return unexpected(authenticated.error() ==
                              platform::CryptoError::unsupported_capability
                          ? MessageError::crypto_unavailable
                          : MessageError::crypto_failed);
  Writer writer{message};
  writer.attribute_header(attribute::message_integrity, integrity_size);
  writer.bytes(std::span<const std::byte>{digest.data(), digest.size()});
  return {};
}

void Message::append_fingerprint(std::vector<std::byte>& message) {
  Header::write_body_size(
      message, message.size() + fingerprint_attribute_size - header_size);
  const auto crc = bytes::crc32(message) ^ fingerprint_xor;
  Writer writer{message};
  writer.attribute_header(attribute::fingerprint, fingerprint_value_size);
  writer.u32_be(crc);
}

class ReceivedMessage::SeenTypes {
 public:
  auto first(std::uint16_t type) noexcept -> bool {
    const auto seen = std::span<const std::uint16_t>{types_}.first(count_);
    if (std::ranges::find(seen, type) != seen.end()) return false;
    if (count_ < types_.size()) types_[count_++] = type;
    return true;
  }

 private:
  std::array<std::uint16_t, max_tracked_attribute_types> types_{};
  std::size_t count_{};
};

auto ReceivedMessage::absorb(const Attribute& item) -> bool {
  const auto& value = item.value;
  switch (item.type) {
    case attribute::mapped_address:
      message.mapped = item.endpoint(message.transaction);
      return message.mapped.has_value();
    case attribute::xor_mapped_address:
      message.xor_mapped = item.endpoint(message.transaction);
      return message.xor_mapped.has_value();
    case attribute::username:
      if (value.size() > max_username_size) return false;
      message.username = SecretString{value};
      return true;
    case attribute::error_code:
      message.error = ErrorResponse::decode(value);
      return message.error.has_value();
    case attribute::unknown_attributes:
      if (value.size() % 2 != 0) return false;
      for (std::size_t offset = 0;
           offset < value.size() &&
           message.unknown_attributes.size() < max_unknown_attributes;
           offset += 2)
        message.unknown_attributes.push_back(
            bytes::Reader{value.subspan(offset)}.u16_be().value());
      return true;
    case attribute::priority:
      if (value.size() != priority_value_size) return false;
      message.priority = bytes::Reader{value}.u32_be().value();
      return true;
    case attribute::use_candidate:
      if (!value.empty()) return false;
      message.use_candidate = true;
      return true;
    case attribute::ice_controlled:
    case attribute::ice_controlling:
      if (value.size() != tie_breaker_size) return false;
      if (item.type == attribute::ice_controlled)
        message.ice_controlled = bytes::Reader{value}.u64_be().value();
      else
        message.ice_controlling = bytes::Reader{value}.u64_be().value();
      return true;
    case attribute::software:
      return value.size() <= max_reason_size;
    default:
      if (item.type < comprehension_optional_min &&
          unknown_required.size() < max_unknown_attributes)
        unknown_required.push_back(item.type);
      return true;
  }
}

auto ReceivedMessage::decode(std::span<const std::byte> datagram)
    -> Expected<ReceivedMessage, MessageError> {
  if (datagram.size() < header_size || datagram.size() > max_message_size ||
      (std::to_integer<std::uint8_t>(datagram[0]) & leading_bits_mask) != 0)
    return unexpected(MessageError::malformed);
  const auto header = Header::read(datagram);
  if (!header || header->cookie != magic_cookie ||
      header->body_size % attribute_alignment != 0 ||
      std::size_t{header->body_size} + header_size != datagram.size() ||
      header->method() != Header::binding_method)
    return unexpected(MessageError::malformed);

  ReceivedMessage received;
  received.message.kind = header->message_class();
  received.message.transaction = header->transaction;

  SeenTypes seen;
  AttributeReader reader{datagram};
  while (!reader.exhausted()) {
    if (received.fingerprinted) return unexpected(MessageError::malformed);
    const auto item = reader.next();
    if (!item) return unexpected(MessageError::malformed);

    if (item->type == attribute::fingerprint) {
      if (item->value.size() != fingerprint_value_size ||
          item->offset + fingerprint_attribute_size != datagram.size() ||
          (bytes::crc32(datagram.first(item->offset)) ^ fingerprint_xor) !=
              bytes::Reader{item->value}.u32_be().value())
        return unexpected(MessageError::malformed);
      received.fingerprinted = true;
    } else if (!received.integrity_offset) {
      if (item->type == attribute::message_integrity) {
        if (item->value.size() != integrity_size)
          return unexpected(MessageError::malformed);
        received.integrity_offset = item->offset;
      } else if (seen.first(item->type) && !received.absorb(*item)) {
        return unexpected(MessageError::malformed);
      }
    }
  }
  return received;
}

auto ReceivedMessage::verify_integrity(platform::CryptoProvider& crypto,
                                       std::span<const std::byte> datagram,
                                       std::span<const std::byte> key) const
    -> bool {
  if (!integrity_offset || key.empty()) return false;
  const auto offset = *integrity_offset;
  if (offset < header_size ||
      offset + integrity_attribute_size > datagram.size())
    return false;
  std::vector<std::byte> input(
      datagram.begin(), datagram.begin() + static_cast<std::ptrdiff_t>(offset));
  Header::write_body_size(input,
                          offset + integrity_attribute_size - header_size);
  SecretArray<integrity_size> digest;
  if (!crypto.hmac_sha1(key, input,
                        std::span<std::byte>{digest.data(), digest.size()}))
    return false;
  return memory::constant_time_equal(
      std::span<const std::byte>{digest.data(), digest.size()},
      datagram.subspan(offset + attribute_header_size, integrity_size));
}

}  // namespace libchiaking::net::nat::stun
