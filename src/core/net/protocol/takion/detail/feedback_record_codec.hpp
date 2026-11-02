/// @file
/// @brief Declares FeedbackRecordCodec, which encodes the controller event
/// records of a Takion feedback history packet for a version.
///
/// Every record starts with a header byte: kind << 5, a flag at 0x10, and the
/// controller slot in the low 2 bits. Kinds: 0 and 1 are records whose meaning
/// is not known (from versions 14 and 14 or 17), 4 is a button record, 6 a
/// touch record.
#pragma once

#include "src/core/net/protocol/takion/detail/packet_codec.hpp"
#include "src/core/net/protocol/takion/detail/packet_types.hpp"
#include "src/core/net/protocol/takion/profile.hpp"
#include "src/core/net/protocol/takion/result.hpp"
#include "src/core/utils/bytes/byte_io.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace libchiaking::net::protocol::takion {

/// @brief Encodes one controller event as a feedback history record.
/// Stateless; safe from any thread.
class FeedbackRecordCodec {
 public:
  /// @brief Encodes one event.
  ///
  /// Digital, analogue and presence events become one-entry button records;
  /// touch events become touch records (header with the down flag, 7-bit
  /// contact id, then x and y as 12 bits each in 3 bytes); the record types
  /// pass through their own encoders.
  ///
  /// @param event The event.
  /// @param controller_id Controller slot, 0 to 3.
  /// @param profile The version's layout.
  /// @return The record bytes.
  /// @retval ParseFailureReason::invalid_field The slot exceeds 3, or the
  /// record
  ///     is malformed (no entries, a repeated id, a one-bit value above 1, an
  ///     out-of-range count or a wrongly sized field).
  /// @retval ParseFailureReason::unsupported_layout The record kind, an id, the
  ///     flag or the trailing byte is not in this version.
  [[nodiscard]] static auto encode(const FeedbackHistoryEvent& event,
                                   std::uint8_t controller_id,
                                   const WireProfile& profile)
      -> Expected<std::vector<std::byte>, ParseFailure>;

 private:
  /// @brief Packet type the failures name.
  static constexpr auto history_type =
      PacketType{static_cast<std::uint8_t>(PacketFamily::feedback_history)};
  /// @brief Shift of the kind in the header byte.
  static constexpr std::uint8_t record_kind_shift = 5;
  /// @brief Mask of the controller slot in the header byte.
  static constexpr std::uint8_t record_controller_mask = 0x03;
  /// @brief The header byte's flag.
  static constexpr std::uint8_t record_flag_bit = 0x10;
  /// @brief Kind of a kind 0 record.
  static constexpr std::uint8_t kind0_record = 0;
  /// @brief Kind of a kind 1 record.
  static constexpr std::uint8_t kind1_record = 1;
  /// @brief Kind of a button record.
  static constexpr std::uint8_t button_record = 4;
  /// @brief Kind of a touch record.
  static constexpr std::uint8_t touch_record = 6;
  /// @brief Bit marking a button record's last entry.
  static constexpr std::uint8_t button_last_entry_bit = 0x80;
  /// @brief Shift of a one-bit entry's value.
  static constexpr std::uint8_t button_digital_value_shift = 5;
  /// @brief Wire id of the presence entry.
  static constexpr std::uint8_t button_presence_id = 31;
  /// @brief First id with a one-bit value; lower ids carry a value byte.
  static constexpr std::uint8_t button_first_digital_id = 12;
  /// @brief Highest id before version 12.
  static constexpr std::uint8_t button_highest_base_id = 19;
  /// @brief Wire id 20.
  static constexpr std::uint8_t button_id_20 = 20;
  /// @brief Wire id 21.
  static constexpr std::uint8_t button_id_21 = 21;
  /// @brief Value sent for a byte-valued control pressed.
  static constexpr std::uint8_t analog_button_pressed = 0xff;
  /// @brief Value sent for a byte-valued control released.
  static constexpr std::uint8_t analog_button_released = 0x00;
  /// @brief Largest kind 0 upper field.
  static constexpr std::uint8_t kind0_upper_max = 0x07;
  /// @brief Kind 1 mode with an 8-byte tail.
  static constexpr std::uint8_t kind1_mode_1 = 1;
  /// @brief Kind 1 mode with masked fields.
  static constexpr std::uint8_t kind1_mode_2 = 2;
  /// @brief Mask of the touch contact id.
  static constexpr std::uint8_t touch_pointer_mask = 0x7f;
  /// @brief Largest touch coordinate (12 bits).
  static constexpr std::uint16_t touch_coordinate_max = 0x0fff;
  /// @brief Builds the invalid-record failure.
  ///
  /// @return The failure.
  static auto invalid_record() noexcept -> ParseFailure;
  /// @brief Builds the unsupported-record failure.
  ///
  /// @return The failure.
  static auto unsupported_record() noexcept -> ParseFailure;
  /// @brief Builds a record header byte.
  ///
  /// @param kind The record kind.
  /// @param controller_id The slot.
  /// @param flag The flag bit.
  /// @return The byte.
  static constexpr auto header_byte(std::uint8_t kind,
                                    std::uint8_t controller_id,
                                    bool flag) noexcept -> std::uint8_t;
  /// @brief Appends bytes.
  ///
  /// @param output Where to append.
  /// @param values The bytes.
  static void append_values(bytes::Appender& output,
                            std::span<const std::uint8_t> values);
  /// @brief Tells the highest button id a version encodes.
  ///
  /// @param version The Takion version.
  /// @return 21 from version 16, 20 from version 12, 19 before.
  static constexpr auto highest_encodable_button_id(
      std::uint32_t version) noexcept -> std::uint8_t;
  /// @brief Tells whether an id has a one-bit value.
  ///
  /// @param id The wire id.
  /// @return Whether it is 12 or above.
  static constexpr bool is_digital_button_id(std::uint8_t id) noexcept;
  /// @brief Orders button entries: one-bit ones first, each group by id.
  ///
  /// @param left One entry.
  /// @param right The other.
  /// @return Whether `left` goes first.
  static constexpr bool precedes_in_button_record(
      const FeedbackButtonEntry& left,
      const FeedbackButtonEntry& right) noexcept;
  /// @brief Maps a digital control to its wire id.
  ///
  /// @param button The control.
  /// @return The id.
  [[nodiscard]] static constexpr auto digital_button_wire_id(
      FeedbackDigitalButton button) noexcept -> std::optional<std::uint8_t>;
  /// @brief Maps a trigger to its wire id.
  ///
  /// @param button The trigger.
  /// @return The id.
  [[nodiscard]] static constexpr auto analog_button_wire_id(
      FeedbackAnalogButton button) noexcept -> std::optional<std::uint8_t>;
  /// @brief Passes a button record through.
  ///
  /// @param record The record.
  /// @return The same record.
  [[nodiscard]] static auto button_record_for(
      const FeedbackButtonRecord& record)
      -> std::optional<FeedbackButtonRecord>;
  /// @brief Makes a one-entry button record for a digital control.
  ///
  /// @param event The event.
  /// @return The record.
  [[nodiscard]] static auto button_record_for(
      const FeedbackDigitalButtonEvent& event)
      -> std::optional<FeedbackButtonRecord>;
  /// @brief Makes a one-entry button record for a trigger.
  ///
  /// @param event The event.
  /// @return The record.
  [[nodiscard]] static auto button_record_for(
      const FeedbackAnalogButtonEvent& event)
      -> std::optional<FeedbackButtonRecord>;
  /// @brief Makes a presence-only button record.
  ///
  /// @param event The event.
  /// @return The record.
  [[nodiscard]] static auto button_record_for(
      const FeedbackPresenceEvent& event)
      -> std::optional<FeedbackButtonRecord>;
  /// @brief Encodes a button record.
  ///
  /// @param record The record; its entries are sorted here.
  /// @param controller_id The slot.
  /// @param version The Takion version.
  /// @return The bytes.
  /// @retval ParseFailureReason::unsupported_layout The flag before version 16,
  ///     a trailing byte before 19, or an id the version lacks.
  /// @retval ParseFailureReason::invalid_field No entries or presence, a
  /// repeated
  ///     id, or a one-bit value above 1.
  [[nodiscard]] static auto encode_buttons(FeedbackButtonRecord record,
                                           std::uint8_t controller_id,
                                           std::uint32_t version)
      -> Expected<std::vector<std::byte>, ParseFailure>;
  /// @brief Encodes a touch record.
  ///
  /// @param touch The event.
  /// @param controller_id The slot.
  /// @return The 5 bytes.
  static auto encode_touch(const FeedbackTouchEvent& touch,
                           std::uint8_t controller_id)
      -> std::vector<std::byte>;
  /// @brief Encodes a kind 0 record.
  ///
  /// @param record The record.
  /// @param controller_id The slot.
  /// @param version The Takion version.
  /// @return The bytes.
  /// @retval ParseFailureReason::unsupported_layout Before version 14.
  /// @retval ParseFailureReason::invalid_field The upper field exceeds 7 or
  /// more
  ///     than 16 entries.
  [[nodiscard]] static auto encode_kind0(const FeedbackKind0Record& record,
                                         std::uint8_t controller_id,
                                         std::uint32_t version)
      -> Expected<std::vector<std::byte>, ParseFailure>;
  /// @brief Encodes a legacy kind 1 record.
  ///
  /// @param record The record.
  /// @param controller_id The slot.
  /// @param version The Takion version.
  /// @return The 8 bytes.
  /// @retval ParseFailureReason::unsupported_layout Outside versions 14 to 16.
  [[nodiscard]] static auto encode_kind1_legacy(
      const FeedbackKind1LegacyRecord& record, std::uint8_t controller_id,
      std::uint32_t version) -> Expected<std::vector<std::byte>, ParseFailure>;
  /// @brief Encodes a moded kind 1 record.
  ///
  /// @param record The record.
  /// @param controller_id The slot.
  /// @param version The Takion version.
  /// @return The bytes.
  /// @retval ParseFailureReason::unsupported_layout Before version 17.
  /// @retval ParseFailureReason::invalid_field The mode is not 1 or 2, holds
  /// the
  ///     other mode's data, or a field has the wrong size.
  [[nodiscard]] static auto encode_kind1(const FeedbackKind1Record& record,
                                         std::uint8_t controller_id,
                                         std::uint32_t version)
      -> Expected<std::vector<std::byte>, ParseFailure>;
};

}  // namespace libchiaking::net::protocol::takion
