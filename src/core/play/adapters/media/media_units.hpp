/// @file
/// @brief Declares MediaUnits, the translation between the Remote Play
/// stream's media units and the typed values the public MediaSink receives.
#pragma once

#include "libchiaking/detail/media/media_sink.hpp"
#include "src/core/net/protocol/remote_play/stream/stream.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace libchiaking::play {

/// @brief Converts stream descriptions, audio units and controller feedback
/// between their wire form and the public media types.
///
/// Feedback payloads have fixed sizes; a unit of the wrong size is not
/// feedback. Stateless; every member may be called from any thread.
class MediaUnits {
 public:
  /// @brief Describes the stream for the sink.
  ///
  /// @param info The link's stream description.
  /// @return The audio format, every video profile with its header, the
  ///     granted picture when the launch reported one, and the audio header.
  static auto description(
      const net::protocol::remote_play::event::StreamInfoReady& info)
      -> media::StreamDescription;
  /// @brief Converts an audio format.
  ///
  /// @param format The wire format.
  /// @return The public format with the same channels, rate, frame size,
  ///     streams, mapping and positions.
  static auto format(
      const net::protocol::remote_play::event::AudioFormat& format) noexcept
      -> media::AudioFormat;
  /// @brief Converts an audio unit, taking its bytes.
  ///
  /// @param unit An audio or audio profile unit.
  /// @return A format unit for an audio profile, otherwise a samples unit.
  static auto audio(net::protocol::remote_play::event::EncodedMedia&& unit)
      -> media::AudioUnit;
  /// @brief Reads controller feedback from a unit.
  ///
  /// @param unit The unit.
  /// @return The feedback for haptics, rumble, trigger effects, motion reset
  ///     and settings, haptic and trigger intensity, lightbar, player LEDs or a
  ///     confirmed seat; empty for audio or video units, a payload of the wrong
  ///     size or haptics for an invalid seat.
  static auto feedback(
      const net::protocol::remote_play::event::EncodedMedia& unit)
      -> std::optional<controllers::ControllerFeedback>;
  /// @brief Writes controller feedback back into its wire unit, as a scripted
  /// link reports it.
  ///
  /// @param feedback The feedback.
  /// @return The unit with its kind, seat and payload.
  static auto wire(const controllers::ControllerFeedback& feedback)
      -> net::protocol::remote_play::event::EncodedMedia;
  /// @brief Converts a wire video codec.
  ///
  /// @param codec The wire codec.
  /// @return The public codec; H.264 for an unknown value.
  static auto codec(net::protocol::remote_play::VideoCodec codec) noexcept
      -> media::VideoCodec;

 private:
  /// @brief Bytes in a rumble payload: seat, left motor, right motor, spare.
  static constexpr std::size_t rumble_size = 4;
  /// @brief Bytes in a trigger effects payload.
  static constexpr std::size_t trigger_size = 25;
  /// @brief Offset of the left trigger's parameters in that payload.
  static constexpr std::size_t trigger_left_parameters = 5;
  /// @brief Offset of the right trigger's parameters in that payload.
  static constexpr std::size_t trigger_right_parameters = 15;
  /// @brief Bytes in a haptic or trigger intensity payload.
  static constexpr std::size_t intensity_size = 1;
  /// @brief Bytes in a colour: red, green, blue.
  static constexpr std::size_t colour_size = 3;
  /// @brief Bytes in a player LED payload.
  static constexpr std::size_t player_size = 1;
  /// @brief Bytes in a motion settings payload.
  static constexpr std::size_t motion_settings_size = 2;
  /// @brief Bytes in a seat confirmation payload: seat and colour.
  static constexpr std::size_t confirmed_size = 4;
  /// @brief The bits of an intensity byte that carry the level.
  static constexpr std::uint8_t intensity_mask = 3;

  /// @brief Reads a byte as an unsigned value.
  ///
  /// @param value The byte.
  /// @return Its value, 0 to 255.
  static auto octet(std::byte value) noexcept -> std::uint8_t {
    return std::to_integer<std::uint8_t>(value);
  }
  /// @brief Reads a colour.
  ///
  /// @param rgb At least three bytes: red, green, blue.
  /// @return The colour.
  static auto colour(std::span<const std::byte> rgb) noexcept
      -> controllers::SeatLight;
  /// @brief Reads an intensity level.
  ///
  /// @param value The intensity byte.
  /// @return Its low two bits as a level.
  static auto level(std::byte value) noexcept -> controllers::FeedbackIntensity;
  /// @brief Reads one trigger's effect from a trigger effects payload.
  ///
  /// @param bytes The payload.
  /// @param mode Offset of the trigger's mode byte.
  /// @param parameters Offset of the trigger's parameters.
  /// @return The effect.
  static auto effect(std::span<const std::byte> bytes, std::size_t mode,
                     std::size_t parameters) noexcept
      -> controllers::TriggerEffect;
  /// @brief Gives the wire kind of a feedback kind.
  ///
  /// @param typed The feedback kind.
  /// @return The matching unit kind.
  static auto kind(controllers::ControllerFeedback::Kind typed) noexcept
      -> net::protocol::remote_play::EncodedMediaKind;
};

}  // namespace libchiaking::play
