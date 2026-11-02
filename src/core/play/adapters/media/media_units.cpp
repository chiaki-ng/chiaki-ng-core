#include "src/core/play/adapters/media/media_units.hpp"

#include <algorithm>
#include <utility>

namespace libchiaking::play {

static_assert(
    static_cast<int>(media::AudioChannelPosition::mono) ==
    static_cast<int>(net::protocol::remote_play::AudioChannelPosition::mono));
static_assert(media::AudioFormat::max_channels ==
              net::protocol::remote_play::audio_max_channels);
static_assert(media::StreamDescription::audio_header_size ==
              net::protocol::remote_play::event::audio_header_size);

auto MediaUnits::codec(net::protocol::remote_play::VideoCodec wired) noexcept
    -> media::VideoCodec {
  switch (wired) {
    case net::protocol::remote_play::VideoCodec::h264:
      return media::VideoCodec::h264;
    case net::protocol::remote_play::VideoCodec::h265:
      return media::VideoCodec::h265;
    case net::protocol::remote_play::VideoCodec::h265_hdr:
      return media::VideoCodec::h265_hdr;
  }
  return media::VideoCodec::h264;
}

auto MediaUnits::format(
    const net::protocol::remote_play::event::AudioFormat& wired) noexcept
    -> media::AudioFormat {
  media::AudioFormat typed;
  typed.channels = wired.channels;
  typed.bits = wired.bits;
  typed.rate = wired.rate;
  typed.frame_size = wired.frame_size;
  typed.streams = wired.streams;
  typed.coupled_streams = wired.coupled_streams;
  typed.mapping = wired.mapping;
  std::ranges::transform(
      wired.positions, typed.positions.begin(),
      [](net::protocol::remote_play::AudioChannelPosition position) {
        return static_cast<media::AudioChannelPosition>(position);
      });
  return typed;
}

auto MediaUnits::description(
    const net::protocol::remote_play::event::StreamInfoReady& info)
    -> media::StreamDescription {
  media::StreamDescription description;
  description.audio = format(info.audio);
  description.profiles.reserve(info.profiles.size());
  for (const auto& profile : info.profiles)
    description.profiles.push_back(
        {profile.width, profile.height, profile.video_header});
  if (info.launch)
    description.picture = media::GrantedPicture{
        codec(info.launch->codec), info.launch->width, info.launch->height,
        info.launch->max_fps, info.launch->bitrate_kbps};
  description.audio_header = info.audio_header;
  return description;
}

auto MediaUnits::audio(net::protocol::remote_play::event::EncodedMedia&& unit)
    -> media::AudioUnit {
  using net::protocol::remote_play::EncodedMediaKind;
  media::AudioUnit typed;
  typed.kind = unit.kind == EncodedMediaKind::audio_profile ||
                       unit.kind == EncodedMediaKind::voice_profile
                   ? media::AudioUnit::Kind::format
                   : media::AudioUnit::Kind::samples;
  typed.voice = unit.kind == EncodedMediaKind::voice_audio ||
                unit.kind == EncodedMediaKind::voice_profile;
  if (unit.kind == EncodedMediaKind::controller_speaker)
    typed.controller_speaker = unit.pad_index;
  typed.bytes = std::move(unit.bytes);
  typed.format = format(unit.audio);
  typed.raw_pcm = unit.audio_raw_pcm;
  return typed;
}

auto MediaUnits::colour(std::span<const std::byte> rgb) noexcept
    -> controllers::SeatLight {
  return {octet(rgb[0]), octet(rgb[1]), octet(rgb[2])};
}

auto MediaUnits::level(std::byte value) noexcept
    -> controllers::FeedbackIntensity {
  return static_cast<controllers::FeedbackIntensity>(octet(value) &
                                                     intensity_mask);
}

auto MediaUnits::effect(std::span<const std::byte> bytes, std::size_t mode,
                        std::size_t parameters) noexcept
    -> controllers::TriggerEffect {
  controllers::TriggerEffect typed;
  typed.mode = octet(bytes[mode]);
  std::ranges::transform(
      bytes.subspan(parameters, controllers::TriggerEffect::parameter_count),
      typed.parameters.begin(), octet);
  return typed;
}

auto MediaUnits::feedback(
    const net::protocol::remote_play::event::EncodedMedia& unit)
    -> std::optional<controllers::ControllerFeedback> {
  const std::span<const std::byte> bytes{unit.bytes};
  const auto seat = controllers::SeatNumber::masked(unit.pad_index);
  switch (unit.kind) {
    case net::protocol::remote_play::EncodedMediaKind::haptics:
      if (const auto played = controllers::SeatNumber::create(unit.pad_index))
        return controllers::ControllerFeedback{*played,
                                               controllers::HapticAudio{bytes}};
      break;
    case net::protocol::remote_play::EncodedMediaKind::rumble:
      if (bytes.size() == rumble_size)
        return controllers::ControllerFeedback{
            controllers::SeatNumber::masked(octet(bytes[0])),
            controllers::Rumble{octet(bytes[1]), octet(bytes[2])}};
      break;
    case net::protocol::remote_play::EncodedMediaKind::trigger_effects:
      if (bytes.size() == trigger_size)
        return controllers::ControllerFeedback{
            controllers::SeatNumber::masked(octet(bytes[0])),
            controllers::TriggerEffects{
                effect(bytes, 1, trigger_left_parameters),
                effect(bytes, 2, trigger_right_parameters)}};
      break;
    case net::protocol::remote_play::EncodedMediaKind::motion_reset:
      return controllers::ControllerFeedback{seat, controllers::MotionReset{}};
    case net::protocol::remote_play::EncodedMediaKind::motion_settings:
      if (bytes.size() == motion_settings_size)
        return controllers::ControllerFeedback{
            seat,
            controllers::MotionSettings{octet(bytes[0]), octet(bytes[1])}};
      break;
    case net::protocol::remote_play::EncodedMediaKind::haptic_intensity:
      if (bytes.size() == intensity_size)
        return controllers::ControllerFeedback{
            seat, controllers::HapticIntensity{level(bytes[0])}};
      break;
    case net::protocol::remote_play::EncodedMediaKind::trigger_intensity:
      if (bytes.size() == intensity_size)
        return controllers::ControllerFeedback{
            seat, controllers::TriggerIntensity{level(bytes[0])}};
      break;
    case net::protocol::remote_play::EncodedMediaKind::led_color:
      if (bytes.size() == colour_size)
        return controllers::ControllerFeedback{
            seat, controllers::Lightbar{colour(bytes)}};
      break;
    case net::protocol::remote_play::EncodedMediaKind::player_index:
      if (bytes.size() == player_size)
        return controllers::ControllerFeedback{
            seat, controllers::PlayerLeds{octet(bytes[0])}};
      break;
    case net::protocol::remote_play::EncodedMediaKind::pad_confirmed:
      if (bytes.size() == confirmed_size)
        return controllers::ControllerFeedback{
            controllers::SeatNumber::masked(octet(bytes[0])),
            controllers::SeatConfirmed{colour(bytes.subspan(1))}};
      break;
    case net::protocol::remote_play::EncodedMediaKind::audio:
    case net::protocol::remote_play::EncodedMediaKind::video:
    case net::protocol::remote_play::EncodedMediaKind::video_fec_failure:
    case net::protocol::remote_play::EncodedMediaKind::audio_profile:
    case net::protocol::remote_play::EncodedMediaKind::voice_audio:
    case net::protocol::remote_play::EncodedMediaKind::voice_profile:
    case net::protocol::remote_play::EncodedMediaKind::controller_speaker:
      break;
  }
  return std::nullopt;
}

auto MediaUnits::kind(controllers::ControllerFeedback::Kind typed) noexcept
    -> net::protocol::remote_play::EncodedMediaKind {
  using Kind = controllers::ControllerFeedback::Kind;
  switch (typed) {
    case Kind::haptics:
      return net::protocol::remote_play::EncodedMediaKind::haptics;
    case Kind::rumble:
      return net::protocol::remote_play::EncodedMediaKind::rumble;
    case Kind::trigger_effects:
      return net::protocol::remote_play::EncodedMediaKind::trigger_effects;
    case Kind::motion_reset:
      return net::protocol::remote_play::EncodedMediaKind::motion_reset;
    case Kind::haptic_intensity:
      return net::protocol::remote_play::EncodedMediaKind::haptic_intensity;
    case Kind::trigger_intensity:
      return net::protocol::remote_play::EncodedMediaKind::trigger_intensity;
    case Kind::led_color:
      return net::protocol::remote_play::EncodedMediaKind::led_color;
    case Kind::player_index:
      return net::protocol::remote_play::EncodedMediaKind::player_index;
    case Kind::motion_settings:
      return net::protocol::remote_play::EncodedMediaKind::motion_settings;
    case Kind::seat_confirmed:
      break;
  }
  return net::protocol::remote_play::EncodedMediaKind::pad_confirmed;
}

auto MediaUnits::wire(const controllers::ControllerFeedback& feedback)
    -> net::protocol::remote_play::event::EncodedMedia {
  net::protocol::remote_play::event::EncodedMedia into{kind(feedback.kind()),
                                                       {}};
  const auto seat = std::byte{feedback.seat.index()};
  const auto place = [](const controllers::SeatLight& light,
                        std::span<std::byte> rgb) {
    rgb[0] = std::byte{light.red};
    rgb[1] = std::byte{light.green};
    rgb[2] = std::byte{light.blue};
  };
  auto& bytes = into.bytes;
  into.pad_index = feedback.seat.index();
  if (const auto* audio = feedback.get<controllers::HapticAudio>()) {
    bytes.assign(audio->pcm.begin(), audio->pcm.end());
  } else if (const auto* rumble = feedback.get<controllers::Rumble>()) {
    bytes.assign(rumble_size, std::byte{});
    bytes[0] = seat;
    bytes[1] = std::byte{rumble->left};
    bytes[2] = std::byte{rumble->right};
  } else if (const auto* effects =
                 feedback.get<controllers::TriggerEffects>()) {
    bytes.assign(trigger_size, std::byte{});
    bytes[0] = seat;
    bytes[1] = std::byte{effects->left.mode};
    bytes[2] = std::byte{effects->right.mode};
    for (std::size_t index = 0;
         index < controllers::TriggerEffect::parameter_count; ++index) {
      bytes[trigger_left_parameters + index] =
          std::byte{effects->left.parameters[index]};
      bytes[trigger_right_parameters + index] =
          std::byte{effects->right.parameters[index]};
    }
  } else if (const auto* haptic =
                 feedback.get<controllers::HapticIntensity>()) {
    bytes.assign(intensity_size, static_cast<std::byte>(haptic->level));
  } else if (const auto* trigger =
                 feedback.get<controllers::TriggerIntensity>()) {
    bytes.assign(intensity_size, static_cast<std::byte>(trigger->level));
  } else if (const auto* lightbar = feedback.get<controllers::Lightbar>()) {
    bytes.assign(colour_size, std::byte{});
    place(lightbar->colour, bytes);
  } else if (const auto* leds = feedback.get<controllers::PlayerLeds>()) {
    bytes.assign(player_size, std::byte{leds->player});
  } else if (const auto* settings =
                 feedback.get<controllers::MotionSettings>()) {
    bytes.assign(motion_settings_size, std::byte{});
    bytes[0] = std::byte{settings->tilt_correction};
    bytes[1] = std::byte{settings->angular_velocity_deadband};
  } else if (const auto* confirmed =
                 feedback.get<controllers::SeatConfirmed>()) {
    bytes.assign(confirmed_size, std::byte{});
    bytes[0] = seat;
    place(confirmed->colour, std::span{bytes}.subspan(1));
  }
  return into;
}

}  // namespace libchiaking::play
