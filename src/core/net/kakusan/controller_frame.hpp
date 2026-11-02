/// @file
/// @brief Declares the guest controller-state and host rumble records Kakusan
/// sends unreliably over DTLS, and their codec.
#pragma once

#include "src/core/net/kakusan/result.hpp"
#include "src/core/utils/bytes/byte_io.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace libchiaking::net::kakusan {

/// @brief Prefix of a guest's controller-state record.
inline constexpr std::string_view input_prefix = "RCI1";
/// @brief Prefix of the host's rumble record.
inline constexpr std::string_view feedback_prefix = "RCF1";
/// @brief Bytes of a record prefix.
inline constexpr std::size_t frame_prefix_size = 4;
/// @brief Bytes of a controller-state payload.
inline constexpr std::size_t input_payload_size = 40;
/// @brief Bytes of a rumble payload.
inline constexpr std::size_t feedback_payload_size = 20;
/// @brief Bytes of a whole RCI1 record: 44.
inline constexpr std::size_t input_frame_size =
    frame_prefix_size + input_payload_size;
/// @brief Bytes of a whole RCF1 record: 24.
inline constexpr std::size_t feedback_frame_size =
    frame_prefix_size + feedback_payload_size;
/// @brief The 18 defined button bits.
inline constexpr std::uint32_t button_mask = 0x3ffffU;
/// @brief Largest stick magnitude; -32768 is invalid.
inline constexpr std::int16_t stick_limit = 32767;
/// @brief Half the 32-bit sequence space, the freshness window.
inline constexpr std::uint32_t sequence_half_range = 0x80000000U;

/// @brief The button bits of a controller state.
enum class Button : std::uint32_t {
  /// @brief South face button (Cross).
  south = 1U << 0U,
  /// @brief East face button (Circle).
  east = 1U << 1U,
  /// @brief West face button (Square).
  west = 1U << 2U,
  /// @brief North face button (Triangle).
  north = 1U << 3U,
  /// @brief D-pad up.
  dpad_up = 1U << 4U,
  /// @brief D-pad down.
  dpad_down = 1U << 5U,
  /// @brief D-pad left.
  dpad_left = 1U << 6U,
  /// @brief D-pad right.
  dpad_right = 1U << 7U,
  /// @brief L1.
  l1 = 1U << 8U,
  /// @brief R1.
  r1 = 1U << 9U,
  /// @brief L3, the left stick click.
  l3 = 1U << 10U,
  /// @brief R3, the right stick click.
  r3 = 1U << 11U,
  /// @brief Options.
  options = 1U << 12U,
  /// @brief Share or Create; the host may withhold it.
  share = 1U << 13U,
  /// @brief Home (PS); the host may withhold it.
  home = 1U << 14U,
  /// @brief Touchpad click.
  touchpad = 1U << 15U,
  /// @brief L2 pressed, independent of the analogue trigger.
  l2 = 1U << 16U,
  /// @brief R2 pressed, independent of the analogue trigger.
  r2 = 1U << 17U,
};

/// @brief Buttons the host may withhold from a guest: Share and Home.
inline constexpr std::uint32_t optional_buttons =
    static_cast<std::uint32_t>(Button::share) |
    static_cast<std::uint32_t>(Button::home);
/// @brief Buttons every grant must allow.
inline constexpr std::uint32_t required_buttons =
    button_mask & ~optional_buttons;

/// @brief The seat and generation a guest's traffic is bound to.
struct SeatBinding {
  /// @brief The seat, 1 to the capacity; seat 1 is local player 1.
  std::uint8_t seat{};
  /// @brief Incremented on every assignment of the seat; never 0, never
  /// reused.
  std::uint32_t generation{};

  /// @brief Compares seat and generation.
  ///
  /// @return True when both match.
  constexpr bool operator==(const SeatBinding&) const noexcept = default;
};

/// @brief The one touchpad contact a state carries.
struct TouchContact {
  /// @brief Whether a finger is down.
  bool active{};
  /// @brief The contact id, 0 to 127, new for each gesture; 0 when inactive.
  std::uint8_t id{};
  /// @brief X, normalised 0 to 65535 from the left; 0 when inactive.
  std::uint16_t x{};
  /// @brief Y, normalised 0 to 65535 from the top; 0 when inactive.
  std::uint16_t y{};

  /// @brief Compares every field.
  ///
  /// @return True when equal.
  constexpr bool operator==(const TouchContact&) const noexcept = default;
};

/// @brief A guest controller's complete state.
struct InputState {
  /// @brief The Button bits held.
  std::uint32_t buttons{};
  /// @brief Left stick X, -32767 to 32767, right positive.
  std::int16_t left_x{};
  /// @brief Left stick Y, -32767 to 32767, down positive.
  std::int16_t left_y{};
  /// @brief Right stick X.
  std::int16_t right_x{};
  /// @brief Right stick Y.
  std::int16_t right_y{};
  /// @brief L2 analogue, 0 to 65535.
  std::uint16_t left_trigger{};
  /// @brief R2 analogue, 0 to 65535.
  std::uint16_t right_trigger{};
  /// @brief The touchpad contact.
  TouchContact touch;

  /// @brief Compares every field.
  ///
  /// @return True when equal.
  constexpr bool operator==(const InputState&) const noexcept = default;
};

/// @brief One RCI1 controller-state record.
struct InputFrame {
  /// @brief The seat it is for.
  SeatBinding binding;
  /// @brief The guest's sequence, from 0, wrapping.
  std::uint32_t sequence{};
  /// @brief Milliseconds since the guest went ACTIVE, wrapping; diagnostic
  /// only.
  std::uint32_t elapsed_ms{};
  /// @brief The state.
  InputState state;
};

/// @brief One RCF1 rumble record.
struct FeedbackFrame {
  /// @brief The seat it is for.
  SeatBinding binding;
  /// @brief The host's sequence, from 0, wrapping.
  std::uint32_t sequence{};
  /// @brief Milliseconds since the host sent ACTIVE, wrapping.
  std::uint32_t elapsed_ms{};
  /// @brief Low-frequency motor amplitude.
  std::uint16_t low_frequency{};
  /// @brief High-frequency motor amplitude.
  std::uint16_t high_frequency{};
};

/// @brief An encoded RCI1 record.
using InputBytes = std::array<std::byte, input_frame_size>;
/// @brief An encoded RCF1 record.
using FeedbackBytes = std::array<std::byte, feedback_frame_size>;

/// @brief Encodes and decodes the controller-state and rumble records (RCP WIRE
/// section 8). All integers big-endian.
///
/// Shared header: prefix `RCI1` or `RCF1` (4), version 1 (1), kind 1 input or
/// 2 feedback (1), seat (1), reserved 0 (1), generation (u32), sequence (u32),
/// elapsed milliseconds (u32). Input then carries buttons (u32), four sticks
/// (i16), two triggers (u16), touch active (1), touch id (1), touch x and y
/// (u16) and a reserved zero u16; feedback carries the low- and high-frequency
/// amplitudes (u16). Stateless.
class ControllerFrameCodec {
 public:
  /// @brief Whether a state may be sent or applied.
  ///
  /// @param state The state.
  /// @return False for undefined buttons, a stick at -32768, an active touch
  ///     id
  ///     above 127, or an inactive touch with any non-zero field.
  static auto validate_input_state(const InputState& state) noexcept -> bool;
  /// @brief Encodes a controller-state record.
  ///
  /// @param frame The record.
  /// @return The 44 bytes.
  /// @retval Error::protocol The state is invalid or the seat or generation
  ///     is
  ///     0.
  static auto encode_input(const InputFrame& frame) -> Result<InputBytes>;
  /// @brief Decodes a controller-state record.
  ///
  /// @param record The record, prefix included.
  /// @return The record.
  /// @retval Error::protocol Wrong size, prefix, version, kind, reserved
  ///     bytes, zero seat or generation, or an invalid state.
  static auto decode_input(std::span<const std::byte> record)
      -> Result<InputFrame>;
  /// @brief Encodes a rumble record.
  ///
  /// @param frame The record.
  /// @return The 24 bytes.
  /// @retval Error::protocol The seat or generation is 0.
  static auto encode_feedback(const FeedbackFrame& frame)
      -> Result<FeedbackBytes>;
  /// @brief Decodes a rumble record.
  ///
  /// @param record The record, prefix included.
  /// @return The record.
  /// @retval Error::protocol Wrong size, prefix, version, kind, reserved
  ///     byte,
  ///     or zero seat or generation.
  static auto decode_feedback(std::span<const std::byte> record)
      -> Result<FeedbackFrame>;

  /// @brief The freshness rule: whether a sequence is newer than the last one
  /// accepted.
  ///
  /// @param sequence The received sequence.
  /// @param last The last accepted.
  /// @return True when 0 < (sequence - last) mod 2^32 < 2^31.
  static constexpr auto is_newer(std::uint32_t sequence,
                                 std::uint32_t last) noexcept -> bool {
    const auto distance = static_cast<std::uint32_t>(sequence - last);
    return distance != 0 && distance < sequence_half_range;
  }

 private:
  /// @brief The record version.
  static constexpr std::uint8_t frame_version = 1;
  /// @brief The kind byte of input.
  static constexpr std::uint8_t kind_input = 1;
  /// @brief The kind byte of feedback.
  static constexpr std::uint8_t kind_feedback = 2;
  /// @brief Largest touch contact id.
  static constexpr std::uint8_t max_touch_id = 127;
  /// @brief Whether a stick value is in range.
  ///
  /// @param value The value.
  /// @return False only for -32768.
  static constexpr auto valid_axis(std::int16_t value) noexcept -> bool {
    return value >= -stick_limit;
  }
  /// @brief Writes the 20-byte shared header.
  ///
  /// @param writer The writer.
  /// @param prefix The record prefix.
  /// @param kind The kind byte.
  /// @param binding The seat binding.
  /// @param sequence The sequence.
  /// @param elapsed_ms The elapsed milliseconds.
  static void write_header(bytes::Writer& writer, std::string_view prefix,
                           std::uint8_t kind, const SeatBinding& binding,
                           std::uint32_t sequence, std::uint32_t elapsed_ms);
  /// @brief The shared header as read.
  struct Header {
    /// @brief The seat binding.
    SeatBinding binding;
    /// @brief The sequence.
    std::uint32_t sequence{};
    /// @brief The elapsed milliseconds.
    std::uint32_t elapsed_ms{};
  };
  static auto
  /// @brief Reads and checks the shared header.
  ///
  /// @param reader The reader, holding at least 20 bytes.
  /// @param prefix The prefix expected.
  /// @param kind The kind expected.
  /// @return The header.
  /// @retval Error::protocol A field is wrong.
  read_header(bytes::Reader& reader, std::string_view prefix,
              std::uint8_t kind) noexcept -> Result<Header>;
};

}  // namespace libchiaking::net::kakusan
