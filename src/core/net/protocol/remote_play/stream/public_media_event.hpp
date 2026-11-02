/// @file
/// @brief Declares PublicMediaEvent, which turns the active session's media
/// and controller feedback into the event::EncodedMedia the caller receives.
#pragma once

#include "src/core/net/protocol/remote_play/stream/active_session.hpp"
#include "src/core/net/protocol/remote_play/stream/stream.hpp"

namespace libchiaking::net::protocol::remote_play {

/// @brief Converts session media and feedback into event::EncodedMedia.
///
/// Stateless; every member is static. The payload layouts are fixed per kind;
/// see each overload.
class PublicMediaEvent {
 public:
  /// @brief A rumble: four bytes of player index, left motor, right motor and
  /// a fourth byte the console sends.
  ///
  /// @param value The rumble.
  /// @return EncodedMediaKind::rumble.
  static auto from(takion::client_event::Rumble value) -> event::EncodedMedia;

  /// @brief Adaptive trigger effects: 25 bytes of player index, left type,
  /// right type, two zero bytes, ten left parameters and ten right parameters.
  ///
  /// @param value The effects.
  /// @return EncodedMediaKind::trigger_effects.
  static auto from(takion::client_event::TriggerEffects value)
      -> event::EncodedMedia;

  /// @brief A motion reset; empty payload.
  ///
  /// @return EncodedMediaKind::motion_reset.
  static auto from(feedback_event::MotionReset) -> event::EncodedMedia;

  /// @brief Motion settings: two bytes.
  ///
  /// @param value The settings.
  /// @return EncodedMediaKind::motion_settings.
  static auto from(feedback_event::MotionSettings value) -> event::EncodedMedia;

  /// @brief Haptic intensity: one byte.
  ///
  /// @param value The intensity.
  /// @return EncodedMediaKind::haptic_intensity.
  static auto from(feedback_event::HapticIntensity value)
      -> event::EncodedMedia;

  /// @brief Trigger intensity: one byte.
  ///
  /// @param value The intensity.
  /// @return EncodedMediaKind::trigger_intensity.
  static auto from(feedback_event::TriggerIntensity value)
      -> event::EncodedMedia;

  /// @brief Light bar colour: three bytes, red, green, blue.
  ///
  /// @param value The colour.
  /// @return EncodedMediaKind::led_color.
  static auto from(feedback_event::LedColor value) -> event::EncodedMedia;

  /// @brief Player indicator: one byte.
  ///
  /// @param value The index.
  /// @return EncodedMediaKind::player_index.
  static auto from(feedback_event::PlayerIndex value) -> event::EncodedMedia;

  /// @brief A confirmed pad: index, then its three light bar bytes.
  ///
  /// @param value The confirmation.
  /// @return EncodedMediaKind::pad_confirmed.
  static auto from(feedback_event::PadConfirmed value) -> event::EncodedMedia;

  /// @brief A video frame lost to FEC: one byte, 1 when a keyframe was
  /// requested; the frame index is set.
  ///
  /// @param value The failure.
  /// @return EncodedMediaKind::video_fec_failure.
  static auto from(feedback_event::VideoFecFailure value)
      -> event::EncodedMedia;

  /// @brief Converts any controller feedback event.
  ///
  /// @param value The event.
  /// @return The matching media.
  static auto from_feedback(ActiveFeedbackEvent value) -> event::EncodedMedia;

  /// @brief Converts a media unit: video with its frame metadata, haptics with
  /// its pad, or audio. Audio carrying a profile header becomes
  /// EncodedMediaKind::audio_profile with its parsed format.
  ///
  /// @param event The unit; its bytes are moved.
  /// @return The media.
  static auto from(ActiveMediaEvent event) -> event::EncodedMedia;
};

}  // namespace libchiaking::net::protocol::remote_play
