#include "src/core/net/protocol/takion/detail/feedback_record_codec.hpp"

#include "src/core/net/protocol/takion/detail/packet_codec.hpp"
#include "src/core/net/protocol/takion/detail/packet_types.hpp"
#include "src/core/utils/bytes/byte_io.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <variant>
#include <vector>

namespace libchiaking::net::protocol::takion {

auto FeedbackRecordCodec::invalid_record() noexcept -> ParseFailure {
  return ParseFailure::of(history_type, ParseFailureReason::invalid_field);
}

auto FeedbackRecordCodec::unsupported_record() noexcept -> ParseFailure {
  return ParseFailure::of(history_type, ParseFailureReason::unsupported_layout);
}

constexpr auto FeedbackRecordCodec::header_byte(std::uint8_t kind,
                                                std::uint8_t controller_id,
                                                bool flag) noexcept
    -> std::uint8_t {
  return static_cast<std::uint8_t>(
      static_cast<std::uint8_t>(kind << record_kind_shift) | controller_id |
      (flag ? record_flag_bit : 0U));
}

constexpr auto FeedbackRecordCodec::highest_encodable_button_id(
    std::uint32_t version) noexcept -> std::uint8_t {
  if (version >= feedback_button_id_21_version) return button_id_21;
  if (version >= feedback_button_id_20_version) return button_id_20;
  return button_highest_base_id;
}

constexpr bool FeedbackRecordCodec::is_digital_button_id(
    std::uint8_t id) noexcept {
  return id >= button_first_digital_id;
}

constexpr bool FeedbackRecordCodec::precedes_in_button_record(
    const FeedbackButtonEntry& left,
    const FeedbackButtonEntry& right) noexcept {
  if (is_digital_button_id(left.id) != is_digital_button_id(right.id))
    return is_digital_button_id(left.id);
  return left.id < right.id;
}

[[nodiscard]] constexpr auto FeedbackRecordCodec::digital_button_wire_id(
    FeedbackDigitalButton button) noexcept -> std::optional<std::uint8_t> {
  switch (button) {
    case FeedbackDigitalButton::dpad_up:
      return 0;
    case FeedbackDigitalButton::dpad_down:
      return 1;
    case FeedbackDigitalButton::dpad_left:
      return 2;
    case FeedbackDigitalButton::dpad_right:
      return 3;
    case FeedbackDigitalButton::l1:
      return 4;
    case FeedbackDigitalButton::r1:
      return 5;
    case FeedbackDigitalButton::cross:
      return 8;
    case FeedbackDigitalButton::moon:
      return 9;
    case FeedbackDigitalButton::box:
      return 10;
    case FeedbackDigitalButton::pyramid:
      return 11;
    case FeedbackDigitalButton::options:
      return 12;
    case FeedbackDigitalButton::share:
      return 13;
    case FeedbackDigitalButton::ps:
      return 14;
    case FeedbackDigitalButton::l3:
      return 15;
    case FeedbackDigitalButton::r3:
      return 16;
    case FeedbackDigitalButton::touchpad:
      return 17;
    case FeedbackDigitalButton::digital_18:
      return 18;
    case FeedbackDigitalButton::digital_19:
      return 19;
    case FeedbackDigitalButton::digital_20:
      return button_id_20;
    case FeedbackDigitalButton::digital_21:
      return button_id_21;
  }
  return std::nullopt;
}

[[nodiscard]] constexpr auto FeedbackRecordCodec::analog_button_wire_id(
    FeedbackAnalogButton button) noexcept -> std::optional<std::uint8_t> {
  switch (button) {
    case FeedbackAnalogButton::l2:
      return 6;
    case FeedbackAnalogButton::r2:
      return 7;
  }
  return std::nullopt;
}

[[nodiscard]] auto FeedbackRecordCodec::button_record_for(
    const FeedbackButtonRecord& record) -> std::optional<FeedbackButtonRecord> {
  return record;
}

[[nodiscard]] auto FeedbackRecordCodec::button_record_for(
    const FeedbackDigitalButtonEvent& event)
    -> std::optional<FeedbackButtonRecord> {
  const auto id = digital_button_wire_id(event.button);
  if (!id) return std::nullopt;
  const std::uint8_t value =
      is_digital_button_id(*id)
          ? static_cast<std::uint8_t>(event.pressed ? 1U : 0U)
          : (event.pressed ? analog_button_pressed : analog_button_released);
  return FeedbackButtonRecord{.entries = {FeedbackButtonEntry{*id, value}}};
}

[[nodiscard]] auto FeedbackRecordCodec::button_record_for(
    const FeedbackAnalogButtonEvent& event)
    -> std::optional<FeedbackButtonRecord> {
  const auto id = analog_button_wire_id(event.button);
  if (!id) return std::nullopt;
  return FeedbackButtonRecord{
      .entries = {FeedbackButtonEntry{*id, event.value}}};
}

[[nodiscard]] auto FeedbackRecordCodec::button_record_for(
    const FeedbackPresenceEvent& event) -> std::optional<FeedbackButtonRecord> {
  return FeedbackButtonRecord{.presence = event.present};
}

void FeedbackRecordCodec::append_values(bytes::Appender& output,
                                        std::span<const std::uint8_t> values) {
  std::ranges::for_each(values,
                        [&output](std::uint8_t value) { output.u8(value); });
}

[[nodiscard]] auto FeedbackRecordCodec::encode_buttons(
    FeedbackButtonRecord record, std::uint8_t controller_id,
    std::uint32_t version) -> Expected<std::vector<std::byte>, ParseFailure> {
  if (record.flag && version < feedback_button_flag_version)
    return unexpected(unsupported_record());
  if (record.trailing != 0 && version < feedback_button_trailing_byte_version)
    return unexpected(unsupported_record());
  if (record.entries.empty() && !record.presence)
    return unexpected(invalid_record());
  std::ranges::sort(record.entries, precedes_in_button_record);
  const auto highest = highest_encodable_button_id(version);
  for (std::size_t index = 0; index < record.entries.size(); ++index) {
    const auto& entry = record.entries[index];
    if (entry.id > highest) return unexpected(unsupported_record());
    if (is_digital_button_id(entry.id) && entry.value > 1)
      return unexpected(invalid_record());
    if (index > 0 && record.entries[index - 1].id == entry.id)
      return unexpected(invalid_record());
  }

  std::vector<std::byte> wire;
  bytes::Appender output{wire};
  output.u8(header_byte(button_record, controller_id, record.flag));
  std::size_t last_tag = 0;
  if (record.presence) {
    last_tag = wire.size();
    output.u8(static_cast<std::uint8_t>(
        button_presence_id |
        (*record.presence ? 1U << button_digital_value_shift : 0U)));
  }
  for (const auto& entry : record.entries) {
    last_tag = wire.size();
    if (is_digital_button_id(entry.id)) {
      output.u8(static_cast<std::uint8_t>(
          entry.id | (entry.value << button_digital_value_shift)));
    } else {
      output.u8(entry.id);
      output.u8(entry.value);
    }
  }
  wire[last_tag] |= std::byte{button_last_entry_bit};
  if (version >= feedback_button_trailing_byte_version)
    output.u8(record.trailing);
  return wire;
}

auto FeedbackRecordCodec::encode_touch(const FeedbackTouchEvent& touch,
                                       std::uint8_t controller_id)
    -> std::vector<std::byte> {
  const auto x = std::min<std::uint16_t>(touch.x, touch_coordinate_max);
  const auto y = std::min<std::uint16_t>(touch.y, touch_coordinate_max);
  std::vector<std::byte> wire;
  bytes::Appender output{wire};
  output.u8(header_byte(touch_record, controller_id, touch.down));
  output.u8(static_cast<std::uint8_t>(touch.pointer_id & touch_pointer_mask));
  output.u8(static_cast<std::uint8_t>(x >> 4U));
  output.u8(static_cast<std::uint8_t>(((x & 0x0fU) << 4U) | (y >> 8U)));
  output.u8(static_cast<std::uint8_t>(y & 0xffU));
  return wire;
}

[[nodiscard]] auto FeedbackRecordCodec::encode_kind0(
    const FeedbackKind0Record& record, std::uint8_t controller_id,
    std::uint32_t version) -> Expected<std::vector<std::byte>, ParseFailure> {
  if (version < feedback_kind0_version) return unexpected(unsupported_record());
  if (record.upper_3 > kind0_upper_max ||
      record.entries.size() > feedback_kind0_max_entries)
    return unexpected(invalid_record());
  std::vector<std::byte> wire;
  bytes::Appender output{wire};
  output.u8(header_byte(kind0_record, controller_id, false));
  output.u8(record.raw_1);
  output.u8(record.raw_2);
  output.u8(static_cast<std::uint8_t>(
      static_cast<std::size_t>(record.upper_3 << record_kind_shift) |
      record.entries.size()));
  for (const auto& entry : record.entries) append_values(output, entry);
  return wire;
}

[[nodiscard]] auto FeedbackRecordCodec::encode_kind1_legacy(
    const FeedbackKind1LegacyRecord& record, std::uint8_t controller_id,
    std::uint32_t version) -> Expected<std::vector<std::byte>, ParseFailure> {
  if (version < feedback_kind1_legacy_version ||
      version >= feedback_kind1_mode_version)
    return unexpected(unsupported_record());
  std::vector<std::byte> wire;
  bytes::Appender output{wire};
  output.u8(header_byte(kind1_record, controller_id, record.flag));
  append_values(output, record.payload);
  return wire;
}

[[nodiscard]] auto FeedbackRecordCodec::encode_kind1(
    const FeedbackKind1Record& record, std::uint8_t controller_id,
    std::uint32_t version) -> Expected<std::vector<std::byte>, ParseFailure> {
  if (version < feedback_kind1_mode_version)
    return unexpected(unsupported_record());
  const bool has_mode_2_fields = std::ranges::any_of(
      record.mode_2_fields, [](const auto& field) { return !field.empty(); });
  const bool has_mode_1_tail = std::ranges::any_of(
      record.mode_1_tail, [](std::uint8_t value) { return value != 0; });
  if (record.mode == kind1_mode_1
          ? has_mode_2_fields
          : record.mode != kind1_mode_2 || has_mode_1_tail)
    return unexpected(invalid_record());

  std::uint16_t mask = 0;
  for (std::size_t bit = 0; bit < record.mode_2_fields.size(); ++bit) {
    const auto& field = record.mode_2_fields[bit];
    if (field.empty()) continue;
    if (field.size() != PacketCodec::feedback_kind1_field_size(bit))
      return unexpected(invalid_record());
    mask = static_cast<std::uint16_t>(mask | (1U << bit));
  }

  std::vector<std::byte> wire;
  bytes::Appender output{wire};
  output.u8(header_byte(kind1_record, controller_id, record.flag));
  output.u8(record.raw_1);
  output.u8(record.raw_2);
  output.u8(record.mode);
  append_values(output, record.common);
  if (record.mode == kind1_mode_1) {
    append_values(output, record.mode_1_tail);
    return wire;
  }
  output.u16_le(mask);
  for (const auto& field : record.mode_2_fields) append_values(output, field);
  return wire;
}

auto FeedbackRecordCodec::encode(const FeedbackHistoryEvent& event,
                                 std::uint8_t controller_id,
                                 const WireProfile& profile)
    -> Expected<std::vector<std::byte>, ParseFailure> {
  if (controller_id > record_controller_mask)
    return unexpected(invalid_record());
  const auto version = profile.version();
  return std::visit(
      [controller_id, version](
          const auto& value) -> Expected<std::vector<std::byte>, ParseFailure> {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, FeedbackTouchEvent>) {
          return encode_touch(value, controller_id);
        } else if constexpr (std::is_same_v<T, FeedbackKind0Record>) {
          return encode_kind0(value, controller_id, version);
        } else if constexpr (std::is_same_v<T, FeedbackKind1LegacyRecord>) {
          return encode_kind1_legacy(value, controller_id, version);
        } else if constexpr (std::is_same_v<T, FeedbackKind1Record>) {
          return encode_kind1(value, controller_id, version);
        } else {
          auto record = button_record_for(value);
          if (!record) return unexpected(invalid_record());
          return encode_buttons(std::move(*record), controller_id, version);
        }
      },
      event);
}

}  // namespace libchiaking::net::protocol::takion
