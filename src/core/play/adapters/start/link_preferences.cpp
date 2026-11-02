#include "src/core/play/adapters/start/link_preferences.hpp"

namespace libchiaking::play {

auto LinkPreferences::video(const Picture& picture)
    -> std::optional<VideoRequest> {
  auto codec = net::protocol::remote_play::VideoCodec::h264;
  switch (picture.codec()) {
    case media::VideoCodec::h264:
      codec = net::protocol::remote_play::VideoCodec::h264;
      break;
    case media::VideoCodec::h265:
      codec = net::protocol::remote_play::VideoCodec::h265;
      break;
    case media::VideoCodec::h265_hdr:
      codec = net::protocol::remote_play::VideoCodec::h265_hdr;
      break;
  }
  auto request = VideoRequest::create(picture.height(), picture.fps(),
                                      picture.bitrate_kbps(), codec);
  if (!request) return std::nullopt;
  return request.value();
}

auto LinkPreferences::tuning(const DeveloperTuning& developer,
                             SoundRequest sound) noexcept
    -> net::protocol::remote_play::SessionTuning {
  net::protocol::remote_play::SessionTuning tuning;
  tuning.disable_audio = developer.disables_audio();
  tuning.disable_video = developer.disables_video();
  tuning.takion_version_override = developer.protocol_version().value_or(0);
  tuning.packet_loss_max = developer.packet_loss_max();
  tuning.negotiate_takion_version = developer.negotiates_protocol_version();
  tuning.requested_audio_channels = sound.channels();
  tuning.max_audio_channels = sound.ceiling();
  tuning.controller_speaker = sound.wants_controller_speaker();
  return tuning;
}

auto LinkPreferences::feedback(FeedbackStyle style) noexcept
    -> net::protocol::remote_play::FeedbackMode {
  return style == FeedbackStyle::switch_compatibility
             ? net::protocol::remote_play::FeedbackMode::switch_compatibility
             : net::protocol::remote_play::FeedbackMode::standard;
}

auto LinkPreferences::on_console(const Preferences& preferences,
                                 consoles::Generation generation)
    -> std::optional<OnConsole> {
  const auto picture = video(preferences.picture.on_console(generation));
  if (!picture) return std::nullopt;
  OnConsole resolved;
  resolved.video = *picture;
  resolved.console.feedback = feedback(preferences.controls.feedback());
  switch (preferences.controls.controller()) {
    case ControllerChoice::console_default:
      resolved.console.controller =
          net::protocol::remote_play::ControllerPreference::console_default;
      break;
    case ControllerChoice::dualshock4:
      resolved.console.controller =
          net::protocol::remote_play::ControllerPreference::dualshock4;
      break;
    case ControllerChoice::dualsense:
      resolved.console.controller =
          net::protocol::remote_play::ControllerPreference::dualsense;
      break;
  }
  resolved.console.keyboard = preferences.controls.console_keyboard();
  resolved.console.idr_on_fec_failure =
      preferences.developer.keyframe_on_lost_frame().value_or(
          console_keyframe_on_lost_frame);
  resolved.console.tuning =
      tuning(preferences.developer, preferences.sound.on_console(generation));
  resolved.port_guessing =
      PortGuessing{preferences.internet.forces_port_guessing(),
                   preferences.internet.port_guesses()};
  resolved.start_timeout = preferences.internet.start_timeout();
  return resolved;
}

auto LinkPreferences::on_cloud(const Preferences& preferences,
                               catalog::ServiceGeneration service)
    -> std::optional<OnCloud> {
  const auto fitted = preferences.picture.on_cloud(service);
  const auto picture = video(fitted);
  if (!picture) return std::nullopt;
  OnCloud resolved;
  resolved.video = *picture;
  resolved.measure_bandwidth = fitted.measures_bandwidth();
  resolved.quality = fitted.cloud_quality();
  resolved.controller =
      preferences.controls.uses_dualsense_on(service)
          ? net::protocol::remote_play::ControllerPreference::dualsense
          : net::protocol::remote_play::ControllerPreference::dualshock4;
  resolved.feedback = feedback(preferences.controls.feedback());
  resolved.idr_on_fec_failure =
      preferences.developer.keyframe_on_lost_frame().value_or(
          cloud_keyframe_on_lost_frame);
  resolved.tuning =
      tuning(preferences.developer, preferences.sound.on_cloud(service));
  resolved.accessibility = preferences.accessibility;
  return resolved;
}

auto LinkPreferences::engine(const OutputRequest& output, SessionSettings base)
    -> SessionSettings {
  if (const auto side = output.largest_picture_side())
    base.video.dimension = *side;
  base.audio_failure_ends_run =
      base.audio_failure_ends_run || output.audio_failure_ends_session();
  return base;
}

}  // namespace libchiaking::play
