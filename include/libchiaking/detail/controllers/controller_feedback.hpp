/// @file
/// @brief Declares the feedback a console sends for a controller: rumble,
/// haptics, trigger effects and indicator lights.
#pragma once

#include "libchiaking/detail/controllers/seat_number.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>

namespace libchiaking::controllers {

/// @brief The colour the console assigned to a seat's controller light.
struct SeatLight {
  /// @brief Red component, 0 to 255.
  std::uint8_t red{};
  /// @brief Green component, 0 to 255.
  std::uint8_t green{};
  /// @brief Blue component, 0 to 255.
  std::uint8_t blue{};

  /// @brief Compares all three components.
  friend constexpr auto operator==(const SeatLight&, const SeatLight&) noexcept
      -> bool = default;
};

/// @brief A strength setting the player chose on the console for haptics or
/// trigger effects.
enum class FeedbackIntensity : std::uint8_t {
  /// @brief The effect is turned off.
  off = 0,
  /// @brief Full strength.
  strong = 1,
  /// @brief Reduced strength.
  medium = 2,
  /// @brief Lowest strength.
  weak = 3,
};

/// @brief Rumble motor strengths for one controller.
struct Rumble {
  /// @brief Strength of the left motor, 0 off to 255 full.
  std::uint8_t left{};
  /// @brief Strength of the right motor, 0 off to 255 full.
  std::uint8_t right{};

  /// @brief Compares both strengths.
  friend constexpr auto operator==(const Rumble&, const Rumble&) noexcept
      -> bool = default;
};

/// @brief The adaptive trigger effect for one trigger, as the console sent it.
///
/// The mode and parameters are the values a DualSense takes for its trigger
/// effect and are passed on unchanged.
struct TriggerEffect {
  /// @brief Number of parameter bytes in an effect.
  static constexpr std::size_t parameter_count = 10;

  /// @brief The effect mode byte.
  std::uint8_t mode{};
  /// @brief The effect's parameter bytes.
  std::array<std::uint8_t, parameter_count> parameters{};

  /// @brief Compares mode and parameters.
  friend constexpr auto operator==(const TriggerEffect&,
                                   const TriggerEffect&) noexcept
      -> bool = default;
};

/// @brief The adaptive trigger effects for both triggers of one controller.
struct TriggerEffects {
  /// @brief Size in bytes of the packed form.
  static constexpr std::size_t packed_size =
      2 * (1 + TriggerEffect::parameter_count);

  /// @brief Effect for the left trigger.
  TriggerEffect left{};
  /// @brief Effect for the right trigger.
  TriggerEffect right{};

  /// @brief Lays both effects out as one byte block.
  ///
  /// @return The left mode, the left parameters, the right mode, then the
  ///     right parameters.
  constexpr auto packed() const noexcept
      -> std::array<std::uint8_t, packed_size> {
    std::array<std::uint8_t, packed_size> bytes{};
    bytes[0] = left.mode;
    std::ranges::copy(left.parameters, bytes.begin() + 1);
    bytes[1 + TriggerEffect::parameter_count] = right.mode;
    std::ranges::copy(right.parameters,
                      bytes.begin() + 2 + TriggerEffect::parameter_count);
    return bytes;
  }

  /// @brief Compares both effects.
  friend constexpr auto operator==(const TriggerEffects&,
                                   const TriggerEffects&) noexcept
      -> bool = default;
};

/// @brief A burst of haptic audio for one controller.
///
/// The samples are interleaved stereo, signed 16-bit little-endian, at
/// sample_rate. The struct does not own them; in MediaSink::on_feedback they
/// are valid only during the call.
///
/// @see FeedbackInbox
struct HapticAudio {
  /// @brief Number of interleaved channels.
  static constexpr std::size_t channels = 2;
  /// @brief Size in bytes of one frame, one sample for each channel.
  static constexpr std::size_t frame_bytes = channels * sizeof(std::int16_t);
  /// @brief Largest burst, in bytes, that counts as playable.
  static constexpr std::size_t max_bytes = 4096;
  /// @brief Sample rate of the audio, in hertz.
  static constexpr std::uint32_t sample_rate = 3000;

  /// @brief The samples; not owned.
  std::span<const std::byte> pcm{};

  /// @brief Reports whether the burst can be played: not empty, whole frames,
  /// and no larger than max_bytes.
  ///
  /// @return `true` when the burst can be played: not empty, whole frames,
  ///     and no larger than max_bytes.
  constexpr auto playable() const noexcept -> bool {
    return !pcm.empty() && pcm.size() % frame_bytes == 0 &&
           pcm.size() <= max_bytes;
  }
};

/// @brief The console asked that the controller's motion sensing be reset.
struct MotionReset {
  /// @brief Always true; the fact carries no data.
  friend constexpr auto operator==(const MotionReset&,
                                   const MotionReset&) noexcept
      -> bool = default;
};

/// @brief The console's settings for how the controller's motion is processed.
///
/// Each value is the byte the console sent; anything other than 0 turns the
/// setting on. Both are on until the console says otherwise.
struct MotionSettings {
  /// @brief Whether the orientation estimate is pulled towards the gravity
  /// the accelerometer measures.
  std::uint8_t tilt_correction{1};
  /// @brief Whether gyroscope axes turning slower than one degree per second
  /// count as still.
  std::uint8_t angular_velocity_deadband{1};

  /// @brief Compares the settings.
  friend constexpr auto operator==(const MotionSettings&,
                                   const MotionSettings&) noexcept
      -> bool = default;
};

/// @brief The console's haptics strength setting changed.
struct HapticIntensity {
  /// @brief The new setting.
  FeedbackIntensity level{FeedbackIntensity::strong};

  /// @brief Compares the settings.
  friend constexpr auto operator==(const HapticIntensity&,
                                   const HapticIntensity&) noexcept
      -> bool = default;
};

/// @brief The console's trigger effect strength setting changed.
struct TriggerIntensity {
  /// @brief The new setting.
  FeedbackIntensity level{FeedbackIntensity::strong};

  /// @brief Compares the settings.
  friend constexpr auto operator==(const TriggerIntensity&,
                                   const TriggerIntensity&) noexcept
      -> bool = default;
};

/// @brief The colour the console wants on the controller's light bar.
struct Lightbar {
  /// @brief The colour.
  SeatLight colour{};

  /// @brief Compares the colours.
  friend constexpr auto operator==(const Lightbar&, const Lightbar&) noexcept
      -> bool = default;
};

/// @brief The player number the console wants shown on the controller's player
/// indicator.
struct PlayerLeds {
  /// @brief The player number as the console sent it.
  std::uint8_t player{};

  /// @brief Compares the player numbers.
  friend constexpr auto operator==(const PlayerLeds&,
                                   const PlayerLeds&) noexcept
      -> bool = default;
};

/// @brief The console confirmed a seat's controller and assigned it a light
/// colour.
///
/// A PS5 console's acceptance of Seats::add_player arrives as this fact.
struct SeatConfirmed {
  /// @brief The colour assigned to the seat.
  SeatLight colour{};

  /// @brief Compares the colours.
  friend constexpr auto operator==(const SeatConfirmed&,
                                   const SeatConfirmed&) noexcept
      -> bool = default;
};

/// @brief One piece of feedback from the console, addressed to a seat's
/// controller.
///
/// Delivered through MediaSink::on_feedback and SharedAudience::feedback. Read
/// it with kind() and get(), or by visiting fact.
struct ControllerFeedback {
  /// @brief Names the alternatives of Fact, in the same order.
  enum class Kind : std::uint8_t {
    /// @brief A HapticAudio burst.
    haptics,
    /// @brief A Rumble change.
    rumble,
    /// @brief New TriggerEffects.
    trigger_effects,
    /// @brief A MotionReset request.
    motion_reset,
    /// @brief A HapticIntensity change.
    haptic_intensity,
    /// @brief A TriggerIntensity change.
    trigger_intensity,
    /// @brief A Lightbar colour.
    led_color,
    /// @brief A PlayerLeds number.
    player_index,
    /// @brief A SeatConfirmed notice.
    seat_confirmed,
    /// @brief New MotionSettings.
    motion_settings,
  };

  /// @brief What the feedback says; exactly one alternative is held.
  using Fact = std::variant<HapticAudio, Rumble, TriggerEffects, MotionReset,
                            HapticIntensity, TriggerIntensity, Lightbar,
                            PlayerLeds, SeatConfirmed, MotionSettings>;

  /// @brief The seat whose controller the feedback is for.
  SeatNumber seat{};
  /// @brief What the feedback says.
  Fact fact{Rumble{}};

  /// @brief Which alternative fact holds.
  ///
  /// @return Which alternative fact holds.
  constexpr auto kind() const noexcept -> Kind {
    return static_cast<Kind>(fact.index());
  }

  /// @brief Returns the fact as one particular alternative.
  ///
  /// @tparam Held One of the alternatives of Fact.
  /// @return A pointer into this object when fact holds a Held, otherwise
  ///     null.
  template <class Held>
  constexpr auto get() const noexcept -> const Held* {
    return std::get_if<Held>(&fact);
  }
};

}  // namespace libchiaking::controllers
