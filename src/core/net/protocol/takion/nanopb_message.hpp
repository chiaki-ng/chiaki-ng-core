/// @file
/// @brief Declares NanopbMessage, the owner of one generated nanopb Takion
/// message, and the failures its encode and decode report.
#pragma once

#include "src/core/net/protocol/takion/generated/takion.pb.h"
#include "src/core/net/protocol/takion/result.hpp"
#include "src/core/utils/memory/c_handle.hpp"

#include <libchiaking/detail/secure_zero.hpp>

#include <pb_decode.h>
#include <pb_encode.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace libchiaking::net::protocol::takion {

/// @brief Why a nanopb message could not be decoded or encoded.
enum class NanopbFailure : std::uint8_t {
  /// @brief The bytes are not a well-formed message of this type.
  malformed,
  /// @brief A required field is absent.
  missing_required_field,
  /// @brief The encoded message would exceed the caller's limit.
  too_large,
};

/// @brief Owns one generated nanopb message on the heap, together with every
/// buffer its pointer fields hold.
///
/// The message starts zeroed and is released through `pb_release` when the
/// owner goes away. A Takion message's secret fields, the BIG and BANG session
/// keys, the BIG launch specification and the CLIENTINFO session key, are
/// zeroed first. Movable, not copyable, so no unwiped duplicate exists.
///
/// @tparam Message A generated nanopb message struct.
/// @tparam Fields The generated descriptor of `Message`.
template <class Message, const pb_msgdesc_t& Fields>
class NanopbMessage {
 public:
  /// @brief Holds a zeroed message.
  NanopbMessage() : message_(new Message{}) {}

  /// @brief Reaches the message.
  ///
  /// @return The message.
  auto operator*() noexcept -> Message& { return *message_; }
  /// @brief Reaches the message read-only.
  ///
  /// @return The message.
  auto operator*() const noexcept -> const Message& { return *message_; }
  /// @brief Reaches the message's fields.
  ///
  /// @return The message.
  auto operator->() noexcept -> Message* { return message_.get(); }
  /// @brief Reaches the message's fields read-only.
  ///
  /// @return The message.
  auto operator->() const noexcept -> const Message* { return message_.get(); }

  /// @brief Replaces the message with the one the bytes encode.
  ///
  /// @param bytes The encoded message.
  /// @return Nothing once decoded.
  /// @retval NanopbFailure::missing_required_field A required field is
  ///     absent.
  /// @retval NanopbFailure::malformed Anything else is wrong.
  [[nodiscard]] auto decode(std::span<const std::byte> bytes)
      -> Expected<void, NanopbFailure> {
    pb_release(&Fields, message_.get());
    *message_ = Message{};
    auto stream = pb_istream_from_buffer(
        reinterpret_cast<const pb_byte_t*>(bytes.data()), bytes.size());
    if (pb_decode(&stream, &Fields, message_.get())) return {};
    return unexpected(failure_of(stream.errmsg));
  }

  /// @brief Counts the bytes the message encodes to.
  ///
  /// @return The size.
  /// @retval NanopbFailure::missing_required_field A required field is
  ///     unset.
  /// @retval NanopbFailure::malformed It cannot be encoded.
  [[nodiscard]] auto encoded_size() const
      -> Expected<std::size_t, NanopbFailure> {
    pb_ostream_t sizing = PB_OSTREAM_SIZING;
    if (pb_encode(&sizing, &Fields, message_.get()))
      return sizing.bytes_written;
    return unexpected(failure_of(sizing.errmsg));
  }

  /// @brief Encodes the message into a new buffer.
  ///
  /// @tparam Buffer A byte buffer with `zeroed(size)`, `data()` and `size()`,
  ///     or a `std::vector<std::byte>`.
  /// @param limit Largest size accepted.
  /// @return The encoded bytes.
  /// @retval NanopbFailure::too_large The message is larger than `limit`.
  /// @retval NanopbFailure::missing_required_field A required field is
  ///     unset.
  /// @retval NanopbFailure::malformed It cannot be encoded.
  template <class Buffer>
  [[nodiscard]] auto encode(std::size_t limit) const
      -> Expected<Buffer, NanopbFailure> {
    const auto size = encoded_size();
    if (!size) return unexpected(size.error());
    if (*size > limit) return unexpected(NanopbFailure::too_large);
    auto bytes = sized<Buffer>(*size);
    auto stream = pb_ostream_from_buffer(
        reinterpret_cast<pb_byte_t*>(bytes.data()), bytes.size());
    if (!pb_encode(&stream, &Fields, message_.get()))
      return unexpected(failure_of(stream.errmsg));
    return bytes;
  }

  /// @brief Replaces a bytes field with a copy of the bytes, wiping what it
  /// held.
  ///
  /// @param field The field, owned by a message.
  /// @param bytes The new contents.
  /// @throws std::bad_alloc No memory.
  /// @throws std::length_error More bytes than nanopb counts.
  static void assign(pb_bytes_array_t*& field,
                     std::span<const std::byte> bytes) {
    if (bytes.size() > std::numeric_limits<pb_size_t>::max())
      throw std::length_error{"nanopb bytes field too long"};
    auto* const replacement = static_cast<pb_bytes_array_t*>(
        std::malloc(PB_BYTES_ARRAY_T_ALLOCSIZE(bytes.size())));
    if (replacement == nullptr) throw std::bad_alloc{};
    replacement->size = static_cast<pb_size_t>(bytes.size());
    if (!bytes.empty())
      std::memcpy(replacement->bytes, bytes.data(), bytes.size());
    wipe(field);
    std::free(field);
    field = replacement;
  }

  /// @brief Replaces a bytes field with a copy of the text.
  ///
  /// @param field The field, owned by a message.
  /// @param text The new contents.
  /// @throws std::bad_alloc No memory.
  static void assign(pb_bytes_array_t*& field, std::string_view text) {
    assign(field, std::as_bytes(std::span{text}));
  }

  /// @brief Gives a repeated field room for `count` zeroed entries,
  /// releasing what it held.
  ///
  /// @tparam Entry The entry type.
  /// @param entries The field's array, owned by a message.
  /// @param size The field's count.
  /// @param count The new number of entries.
  /// @return The entries.
  /// @throws std::bad_alloc No memory.
  /// @throws std::length_error More entries than nanopb counts.
  template <class Entry>
  static auto resize(Entry*& entries, pb_size_t& size, std::size_t count)
      -> std::span<Entry> {
    if (count > std::numeric_limits<pb_size_t>::max())
      throw std::length_error{"nanopb repeated field too long"};
    auto* const replacement =
        count == 0 ? nullptr
                   : static_cast<Entry*>(std::calloc(count, sizeof(Entry)));
    if (count != 0 && replacement == nullptr) throw std::bad_alloc{};
    std::free(entries);
    entries = replacement;
    size = static_cast<pb_size_t>(count);
    return {entries, count};
  }

  /// @brief Gives the wire number of a generated enum constant, as the
  /// `int32` its field holds.
  ///
  /// @tparam Enum A generated enum.
  /// @param value The constant.
  /// @return Its number.
  template <class Enum>
    requires std::is_enum_v<Enum>
  static constexpr auto number(Enum value) noexcept -> std::int32_t {
    return static_cast<std::int32_t>(value);
  }

  /// @brief Views a bytes field.
  ///
  /// @param field The field; may be null.
  /// @return Its bytes, empty when unset.
  static auto bytes_of(const pb_bytes_array_t* field) noexcept
      -> std::span<const std::byte> {
    if (field == nullptr) return {};
    return std::as_bytes(std::span{field->bytes, field->size});
  }

  /// @brief Views a bytes field as text.
  ///
  /// @param field The field; may be null.
  /// @return Its text, empty when unset.
  static auto text_of(const pb_bytes_array_t* field) noexcept
      -> std::string_view {
    const auto bytes = bytes_of(field);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
  }

 private:
  /// @brief Makes a buffer of the size the encoding fills.
  ///
  /// @tparam Buffer The buffer type.
  /// @param size Its size.
  /// @return The buffer.
  template <class Buffer>
  static auto sized(std::size_t size) -> Buffer {
    if constexpr (requires { Buffer::zeroed(size); })
      return Buffer::zeroed(size);
    else
      return Buffer(size);
  }

  /// @brief Names the failure nanopb's error message stands for.
  ///
  /// @param message nanopb's error message; may be null.
  /// @return The failure.
  static auto failure_of(const char* message) noexcept -> NanopbFailure {
    if (message != nullptr &&
        std::string_view{message} == "missing required field")
      return NanopbFailure::missing_required_field;
    return NanopbFailure::malformed;
  }

  /// @brief Zeroes a bytes field's contents in place.
  ///
  /// @param field The field; may be null.
  static void wipe(pb_bytes_array_t* field) noexcept {
    if (field == nullptr) return;
    memory::secure_zero(std::span{field->bytes, field->size});
  }

  /// @brief Zeroes the secret fields a Takion message can carry.
  ///
  /// @param message The message.
  static void scrub(Message& message) noexcept {
    if constexpr (std::is_same_v<Message, tkproto_TakionMessage>) {
      wipe(message.big_payload.session_key);
      wipe(message.big_payload.launch_spec);
      wipe(message.bang_payload.session_key);
      wipe(message.client_info_payload.session_key);
    }
  }

  /// @brief Wipes, releases and deletes a message.
  ///
  /// @param message The message.
  static void release(Message* message) noexcept {
    scrub(*message);
    pb_release(&Fields, message);
    delete message;
  }

  /// @brief The message.
  memory::CHandle<Message, &release> message_;
};

/// @brief Owns a generated top-level Takion message.
using TakionWire =
    NanopbMessage<tkproto_TakionMessage, tkproto_TakionMessage_msg>;
/// @brief Owns a generated EXT message.
using ExtWire = NanopbMessage<tkproto_ExtMessage, tkproto_ExtMessage_msg>;
/// @brief Owns a generated GENERICDATA payload.
using GenericDataWire =
    NanopbMessage<tkproto_GenericDataPayload, tkproto_GenericDataPayload_msg>;
/// @brief Owns a generated deep link payload.
using DeepLinkWire =
    NanopbMessage<tkproto_DeepLinkPayload, tkproto_DeepLinkPayload_msg>;
/// @brief Owns a generated microphone information payload.
using MicInfoWire =
    NanopbMessage<tkproto_MicInfoPayload, tkproto_MicInfoPayload_msg>;

}  // namespace libchiaking::net::protocol::takion
