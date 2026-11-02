#include "src/core/net/protocol/remote_play/stream/public_media_event.hpp"

#include "src/core/net/protocol/remote_play/stream/stream_info.hpp"

#include <type_traits>
#include <utility>
#include <variant>

namespace libchiaking::net::protocol::remote_play {

auto PublicMediaEvent::from(takion::client_event::Rumble value)
    -> event::EncodedMedia {
  return event::EncodedMedia{
      EncodedMediaKind::rumble,
      {std::byte{value.player_index}, std::byte{value.left},
       std::byte{value.right}, std::byte{value.unknown}}};
}

auto PublicMediaEvent::from(takion::client_event::TriggerEffects value)
    -> event::EncodedMedia {
  std::vector<std::byte> bytes(25);
  bytes[0] = std::byte{value.player_index};
  bytes[1] = std::byte{value.type_left};
  bytes[2] = std::byte{value.type_right};
  for (std::size_t index = 0; index < 10; ++index) {
    bytes[5 + index] = std::byte{value.left[index]};
    bytes[15 + index] = std::byte{value.right[index]};
  }
  return event::EncodedMedia{EncodedMediaKind::trigger_effects,
                             std::move(bytes)};
}

auto PublicMediaEvent::from(feedback_event::MotionReset)
    -> event::EncodedMedia {
  return event::EncodedMedia{EncodedMediaKind::motion_reset, {}};
}

auto PublicMediaEvent::from(feedback_event::MotionSettings value)
    -> event::EncodedMedia {
  return event::EncodedMedia{
      EncodedMediaKind::motion_settings,
      {std::byte{value.values[0]}, std::byte{value.values[1]}}};
}

auto PublicMediaEvent::from(feedback_event::HapticIntensity value)
    -> event::EncodedMedia {
  return event::EncodedMedia{EncodedMediaKind::haptic_intensity,
                             {std::byte{value.value}}};
}

auto PublicMediaEvent::from(feedback_event::TriggerIntensity value)
    -> event::EncodedMedia {
  return event::EncodedMedia{EncodedMediaKind::trigger_intensity,
                             {std::byte{value.value}}};
}

auto PublicMediaEvent::from(feedback_event::LedColor value)
    -> event::EncodedMedia {
  return event::EncodedMedia{EncodedMediaKind::led_color,
                             {std::byte{value.rgb[0]}, std::byte{value.rgb[1]},
                              std::byte{value.rgb[2]}}};
}

auto PublicMediaEvent::from(feedback_event::PlayerIndex value)
    -> event::EncodedMedia {
  return event::EncodedMedia{EncodedMediaKind::player_index,
                             {std::byte{value.value}}};
}

auto PublicMediaEvent::from(feedback_event::PadConfirmed value)
    -> event::EncodedMedia {
  return event::EncodedMedia{
      EncodedMediaKind::pad_confirmed,
      {std::byte{value.index}, std::byte{value.led[0]}, std::byte{value.led[1]},
       std::byte{value.led[2]}}};
}

auto PublicMediaEvent::from(feedback_event::VideoFecFailure value)
    -> event::EncodedMedia {
  event::EncodedMedia media{
      EncodedMediaKind::video_fec_failure,
      {std::byte{value.idr_request_sent ? std::uint8_t{1} : std::uint8_t{0}}}};
  media.frame_index = value.frame_index;
  return media;
}

auto PublicMediaEvent::from_feedback(ActiveFeedbackEvent value)
    -> event::EncodedMedia {
  static_assert(std::variant_size_v<ActiveFeedbackEvent> == 10);
  return std::visit([](auto& packet) { return from(std::move(packet)); },
                    value);
}

auto PublicMediaEvent::from(ActiveMediaEvent event) -> event::EncodedMedia {
  static_assert(std::variant_size_v<ActiveMediaEvent> == 4);
  return std::visit(
      [](auto& packet) -> event::EncodedMedia {
        using Packet = std::decay_t<decltype(packet)>;
        event::EncodedMedia media{EncodedMediaKind::audio,
                                  std::move(packet.bytes)};
        if constexpr (std::is_same_v<Packet,
                                     takion::client_event::EncodedVideo>) {
          media.kind = EncodedMediaKind::video;
          media.frame_index = packet.frame_index;
          media.codec = packet.codec;
          media.adaptive_stream_index = packet.adaptive_stream_index;
          media.frames_lost = packet.frames_lost;
          media.frame_recovered = packet.frame_recovered;
          media.profile_header = packet.profile_header;
          media.dynamic_range_flag = packet.dynamic_range_flag;
          media.suppress_presentation = packet.suppress_presentation;
        } else if constexpr (std::is_same_v<Packet,
                                            takion::client_event::Haptics>) {
          media.kind = EncodedMediaKind::haptics;
          media.pad_index = packet.pad_index;
        } else if constexpr (std::is_same_v<
                                 Packet,
                                 takion::client_event::ControllerSpeaker>) {
          media.kind = EncodedMediaKind::controller_speaker;
          media.pad_index = packet.pad_index;
        } else if (packet.profile_header) {
          media.kind = packet.voice ? EncodedMediaKind::voice_profile
                                    : EncodedMediaKind::audio_profile;
          if (const auto header =
                  StreamInfoCodec::parse_audio_header(media.bytes))
            media.audio = StreamInfoCodec::to_public_audio_format(*header);
          media.audio_raw_pcm = packet.raw_pcm;
        } else if (packet.voice) {
          media.kind = EncodedMediaKind::voice_audio;
        }
        return media;
      },
      event);
}

}  // namespace libchiaking::net::protocol::remote_play
