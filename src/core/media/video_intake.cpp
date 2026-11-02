#include "src/core/media/video_intake.hpp"

#include "libchiaking/platform/runtime/time_units.hpp"
#include "src/core/net/protocol/remote_play/stream/video_bitstream.hpp"

#include <algorithm>
#include <utility>

namespace libchiaking::media {

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<std::int32_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

auto VideoIntake::sniff_hevc(std::span<const std::byte> bytes) noexcept
    -> std::optional<bool> {
  for (std::size_t index = 0; index + 3 < bytes.size(); ++index) {
    if (bytes[index] != std::byte{0} || bytes[index + 1] != std::byte{0} ||
        bytes[index + 2] != std::byte{1})
      continue;
    const auto header = std::to_integer<unsigned>(bytes[index + 3]);
    if ((header & net::protocol::remote_play::h264_nal_type_mask) ==
        static_cast<unsigned>(net::protocol::remote_play::NalType::h264_sps))
      return false;
    if (((header >> 1U) & net::protocol::remote_play::hevc_nal_type_mask) ==
        static_cast<unsigned>(net::protocol::remote_play::NalType::hevc_vps))
      return true;
    return std::nullopt;
  }
  return std::nullopt;
}

VideoIntake::VideoIntake(platform::ClockProvider& clock, VideoLimits limits)
    : clock_(clock), limits_(limits) {}

auto VideoIntake::configure(
    const net::protocol::remote_play::event::StreamInfoReady& info)
    -> Expected<void, MediaFailure> {
  if (info.profiles.empty() || info.profiles.size() > limits_.profiles)
    return unexpected(MediaFailure::profile_validation);
  const auto valid =
      [this](const net::protocol::remote_play::event::VideoProfile& profile) {
        const bool sized =
            profile.width != 0 && profile.height != 0 &&
            (!limits_.dimension || (profile.width <= *limits_.dimension &&
                                    profile.height <= *limits_.dimension));
        return sized && !profile.video_header.empty() &&
               profile.video_header.size() <= limits_.header_bytes;
      };
  if (!std::ranges::all_of(info.profiles, valid))
    return unexpected(MediaFailure::profile_validation);
  std::vector<Profile> next;
  next.reserve(info.profiles.size());
  for (const auto& profile : info.profiles)
    next.push_back({profile.width, profile.height, profile.video_header,
                    sniff_hevc(profile.video_header)});
  profiles_ = std::move(next);
  selected_.reset();
  return {};
}

auto VideoIntake::frame(net::protocol::remote_play::event::EncodedMedia&& media)
    -> std::optional<VideoFrame> {
  if (media.kind != net::protocol::remote_play::EncodedMediaKind::video)
    return std::nullopt;
  VideoFrame::Facts facts;
  facts.bytes = std::move(media.bytes);
  facts.frame_index = media.frame_index;
  facts.codec = media.codec;
  facts.adaptive_stream_index = media.adaptive_stream_index;
  facts.frames_lost = media.frames_lost;
  facts.recovered = media.frame_recovered;
  facts.profile_header = media.profile_header;
  facts.dynamic_range_flag = media.dynamic_range_flag;
  facts.suppress_presentation = media.suppress_presentation;
  return VideoFrame{std::move(facts)};
}

auto VideoIntake::admit(VideoFrame frame) -> Expected<VideoUnit, MediaFailure> {
  const std::size_t index = frame.adaptive_stream_index();
  if (frame.size() == 0 || frame.size() > limits_.frame_bytes ||
      index >= profiles_.size())
    return unexpected(MediaFailure::frame_validation);
  const auto& profile = profiles_[index];
  units_.fetch_add(1, std::memory_order_relaxed);
  bytes_.fetch_add(frame.size(), std::memory_order_relaxed);
  if (frame.frames_lost() > 0) {
    lost_.fetch_add(static_cast<std::uint64_t>(frame.frames_lost()),
                    std::memory_order_relaxed);
    pending_lost_.fetch_add(frame.frames_lost(), std::memory_order_relaxed);
  }
  if (frame.recovered()) {
    recovered_.fetch_add(1, std::memory_order_relaxed);
    pending_recovered_.store(true, std::memory_order_relaxed);
  }
  const bool hevc = profile.hevc.value_or(frame.codec() != 0);
  const bool changed = selected_ != index;
  return VideoUnit{std::move(frame), index, changed,
                   profile.header,   hevc,  profile.width,
                   profile.height};
}

auto VideoIntake::settle(const VideoUnit& unit, VideoResult result)
    -> Expected<void, MediaFailure> {
  if (result.verdict == VideoVerdict::format_failed ||
      result.verdict == VideoVerdict::submit_failed)
    return unexpected(MediaFailure::decoder_submission);
  selected_ = unit.profile_index();
  switch (result.verdict) {
    case VideoVerdict::presented:
      rejections_since_output_.store(0, std::memory_order_relaxed);
      pending_lost_.store(0, std::memory_order_relaxed);
      pending_recovered_.store(false, std::memory_order_relaxed);
      break;
    case VideoVerdict::recover:
      rejected(unit.frame().frame_index(), result.detail);
      break;
    case VideoVerdict::dropped:
      lost_.fetch_add(1, std::memory_order_relaxed);
      pending_lost_.fetch_add(1, std::memory_order_relaxed);
      lose(unit.frame().frame_index());
      loss_signalled_.store(true, std::memory_order_release);
      break;
    case VideoVerdict::accepted:
    case VideoVerdict::format_failed:
    case VideoVerdict::submit_failed:
      break;
  }
  return {};
}

void VideoIntake::fec_failed(bool idr_already_requested) noexcept {
  if (idr_already_requested) {
    idr_requested_us_.store(clock_.now_microseconds(),
                            std::memory_order_relaxed);
    idr_requested_.store(true, std::memory_order_release);
    return;
  }
  loss_signalled_.store(true, std::memory_order_release);
}

void VideoIntake::lose(std::uint16_t frame) noexcept {
  auto packed = loss_.load(std::memory_order_relaxed);
  while (!loss_.compare_exchange_weak(packed, widened(packed, frame),
                                      std::memory_order_release,
                                      std::memory_order_relaxed)) {
  }
}

void VideoIntake::rejected(std::uint16_t frame, int cause) noexcept {
  decoder_rejections_.fetch_add(1, std::memory_order_relaxed);
  rejections_since_output_.fetch_add(1, std::memory_order_relaxed);
  last_decoder_error_.store(cause, std::memory_order_relaxed);
  lose(frame);
}

auto VideoIntake::presented() noexcept -> PresentationLoss {
  rejections_since_output_.store(0, std::memory_order_relaxed);
  return {pending_lost_.exchange(0, std::memory_order_relaxed),
          pending_recovered_.exchange(false, std::memory_order_relaxed)};
}

void VideoIntake::request_idr() noexcept {
  idr_forced_.store(true, std::memory_order_release);
}

void VideoIntake::request_drop() noexcept {
  drop_forced_.store(true, std::memory_order_release);
}

auto VideoIntake::drop_due() noexcept -> bool {
  return drop_forced_.exchange(false, std::memory_order_acq_rel);
}

auto VideoIntake::take_loss_report() noexcept -> std::optional<VideoLossRange> {
  return unpack(loss_.exchange(0, std::memory_order_acq_rel));
}

auto VideoIntake::idr_due() noexcept -> bool {
  const auto now = clock_.now_microseconds();
  const auto interval =
      static_cast<std::uint64_t>(limits_.idr_interval.count()) *
      microseconds_per_millisecond;
  const bool forced = idr_forced_.exchange(false, std::memory_order_acq_rel);
  const bool lossy = rejections_since_output_.load(std::memory_order_relaxed) >=
                         limits_.idr_rejections ||
                     loss_signalled_.load(std::memory_order_acquire);
  const bool spaced =
      !idr_requested_.load(std::memory_order_acquire) ||
      now - idr_requested_us_.load(std::memory_order_relaxed) >= interval;
  if (!forced && !(lossy && spaced)) return false;
  idr_requested_us_.store(now, std::memory_order_relaxed);
  idr_requested_.store(true, std::memory_order_release);
  loss_signalled_.store(false, std::memory_order_release);
  keyframe_requests_.fetch_add(1, std::memory_order_relaxed);
  return true;
}

auto VideoIntake::work_pending() const noexcept -> bool {
  return loss_.load(std::memory_order_acquire) != 0 ||
         idr_forced_.load(std::memory_order_acquire) ||
         drop_forced_.load(std::memory_order_acquire);
}

auto VideoIntake::profile_count() const noexcept -> std::size_t {
  return profiles_.size();
}

auto VideoIntake::statistics() const noexcept -> VideoStatistics {
  return {units_.load(std::memory_order_relaxed),
          bytes_.load(std::memory_order_relaxed),
          lost_.load(std::memory_order_relaxed),
          recovered_.load(std::memory_order_relaxed),
          decoder_rejections_.load(std::memory_order_relaxed),
          keyframe_requests_.load(std::memory_order_relaxed),
          last_decoder_error_.load(std::memory_order_relaxed)};
}

auto VideoIntake::decoder_rejections() const noexcept -> std::uint64_t {
  return decoder_rejections_.load(std::memory_order_relaxed);
}

auto VideoIntake::lost() const noexcept -> std::uint64_t {
  return lost_.load(std::memory_order_relaxed);
}

}  // namespace libchiaking::media
