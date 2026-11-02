#include "src/core/play/adapters/media/media_delivery.hpp"

#include "src/core/play/adapters/media/media_units.hpp"

#include <bit>
#include <magic_enum/magic_enum.hpp>

#include <format>
#include <utility>

namespace libchiaking::play {

MediaDelivery::MediaDelivery(media::MediaSink& sink,
                             platform::ThreadingProvider& threading,
                             platform::ClockProvider& clock,
                             platform::DiagnosticsProvider& log,
                             MediaSettings settings)
    : sink_(sink),
      log_(log),
      stages_(std::move(settings.stages)),
      audio_failure_ends_run_(settings.audio_failure_ends_run),
      video_(clock, settings.video),
      mutex_(threading) {}

void MediaDelivery::log(platform::LogSeverity severity,
                        std::string_view message) const noexcept {
  log_.write({severity, message});
}

auto MediaDelivery::configure(
    const net::protocol::remote_play::event::StreamInfoReady& info) -> bool {
  const auto configured = video_.configure(info);
  if (!configured)
    fail(configured.error(), stages_.profile_validation, validation_cause);
  sink_.on_stream_description(MediaUnits::description(info));
  return configured.has_value();
}

void MediaDelivery::deliver(media::VideoFrame frame) {
  const auto index = frame.frame_index();
  const auto unit = video_.admit(std::move(frame));
  if (!unit) {
    fail(unit.error(), stages_.frame_validation, validation_cause);
    return;
  }
  const auto result = sink_.on_video(unit.value());
  if (const auto settled = video_.settle(unit.value(), result); !settled) {
    fail(settled.error(),
         result.verdict == media::VideoVerdict::format_failed
             ? stages_.format_setup
             : stages_.submission,
         result.detail);
    return;
  }
  if (result.verdict == media::VideoVerdict::recover) {
    const auto rejections = video_.decoder_rejections();
    if (std::has_single_bit(rejections))
      log(platform::LogSeverity::warning,
          "video recovery: frame=" + std::to_string(index) +
              " decoder=" + std::to_string(result.detail) +
              " rejected=" + std::to_string(rejections));
  }
  if (result.verdict == media::VideoVerdict::dropped) {
    const auto lost = video_.lost();
    if (std::has_single_bit(lost))
      log(platform::LogSeverity::warning,
          "video dropped while decoder buffers were full: frame=" +
              std::to_string(index) + " dropped=" + std::to_string(lost));
  }
}

void MediaDelivery::deliver(
    net::protocol::remote_play::event::EncodedMedia&& media) {
  switch (media.kind) {
    case net::protocol::remote_play::EncodedMediaKind::video:
      return;
    case net::protocol::remote_play::EncodedMediaKind::audio:
    case net::protocol::remote_play::EncodedMediaKind::audio_profile:
    case net::protocol::remote_play::EncodedMediaKind::voice_audio:
    case net::protocol::remote_play::EncodedMediaKind::voice_profile:
    case net::protocol::remote_play::EncodedMediaKind::controller_speaker:
      if (const auto result =
              sink_.on_audio(MediaUnits::audio(std::move(media)));
          result.verdict == media::AudioVerdict::failed)
        audio_failed(result.detail);
      return;
    case net::protocol::remote_play::EncodedMediaKind::video_fec_failure:
      video_.fec_failed(!media.bytes.empty() &&
                        media.bytes.front() != std::byte{0});
      sink_.on_video_gap();
      return;
    default:
      if (const auto feedback = MediaUnits::feedback(media))
        sink_.on_feedback(*feedback);
      return;
  }
}

auto MediaDelivery::presented() noexcept -> media::PresentationLoss {
  return video_.presented();
}

void MediaDelivery::rejected(std::uint16_t frame, int cause) {
  video_.rejected(frame, cause);
  const auto rejections = video_.decoder_rejections();
  if (std::has_single_bit(rejections))
    log(platform::LogSeverity::warning,
        "video recovery: frame=" + std::to_string(frame) + " decoder=" +
            std::to_string(cause) + " rejected=" + std::to_string(rejections));
}

void MediaDelivery::output_failed(int cause) {
  fail(media::MediaFailure::decoder_output, stages_.decoder_output, cause);
}

void MediaDelivery::recovery_failed(int cause) {
  fail(media::MediaFailure::recovery_request, stages_.recovery_request, cause);
}

void MediaDelivery::audio_failed(int cause) {
  std::uint64_t failures = 0;
  {
    const platform::Lock lock(mutex_);
    failures = ++audio_failures_;
  }
  if (audio_failure_ends_run_) {
    fail(media::MediaFailure::audio_output, stages_.audio_output, cause);
    return;
  }
  if (std::has_single_bit(failures))
    log(platform::LogSeverity::warning,
        "audio output failure: cause=" + std::to_string(cause) +
            " failures=" + std::to_string(failures));
}

void MediaDelivery::fail(media::MediaFailure kind, std::string_view stage,
                         int cause) {
  {
    const platform::Lock lock(mutex_);
    if (fault_ || sealed_) return;
    fault_ = media::MediaFault{kind, std::string{stage}, cause};
  }
  log(platform::LogSeverity::error,
      std::format("media failure: kind={} cause={}",
                  magic_enum::enum_name(kind), cause));
}

auto MediaDelivery::fault() const -> std::optional<media::MediaFault> {
  const platform::Lock lock(mutex_);
  return fault_;
}

auto MediaDelivery::pardon() -> std::optional<media::MediaFault> {
  const platform::Lock lock(mutex_);
  return std::exchange(fault_, std::nullopt);
}

void MediaDelivery::seal() {
  const platform::Lock lock(mutex_);
  sealed_ = true;
}

auto MediaDelivery::audio_failures() const -> std::uint64_t {
  const platform::Lock lock(mutex_);
  return audio_failures_;
}

}  // namespace libchiaking::play
