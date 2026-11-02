#include "active_session.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"

#include <algorithm>
#include "active_handshake.hpp"
#include "limits.hpp"
#include "src/core/net/protocol/remote_play/stream/audio_negotiation.hpp"
#include "src/core/net/protocol/remote_play/stream/controller_delivery.hpp"
#include "src/core/net/protocol/takion/control_chunk_codec.hpp"
#include "src/core/utils/timing/deadline.hpp"

#include <array>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace libchiaking::net::protocol::remote_play {

auto ActiveSession::inbound_activity(const takion::Event& event) noexcept
    -> bool {
  if (const auto* rejected =
          std::get_if<takion::client_event::PacketRejected>(&event))
    return !takion::is_transient_send_error(rejected->error) &&
           rejected->error != takion::ClientError::reliable_channel_failure;
  return !std::holds_alternative<takion::client_event::ProbeSent>(event) &&
         !std::holds_alternative<takion::client_event::ControlSent>(event) &&
         !std::holds_alternative<takion::client_event::Closed>(event) &&
         !std::holds_alternative<takion::client_event::TerminalFailure>(event);
}

auto ActiveHandshakeTimer::window(ActiveHandshakeState state) noexcept
    -> Window {
  switch (state) {
    case ActiveHandshakeState::awaiting_connection:
    case ActiveHandshakeState::awaiting_profile:
      return Window::connect;
    case ActiveHandshakeState::awaiting_bang:
    case ActiveHandshakeState::verifying_bang:
    case ActiveHandshakeState::awaiting_crypto_install:
      return Window::bang;
    case ActiveHandshakeState::awaiting_stream_info:
    case ActiveHandshakeState::validating_stream_info:
      return Window::stream_info;
    case ActiveHandshakeState::ready:
    case ActiveHandshakeState::closed:
    case ActiveHandshakeState::failed:
      return Window::none;
  }
  return Window::none;
}

auto ActiveHandshakeTimer::expired(ActiveHandshakeState state,
                                   std::uint64_t now_us) const noexcept
    -> bool {
  return window(state) != Window::none && now_us >= started_us_ &&
         now_us - started_us_ >= handshake_stage_timeout_us;
}

auto ActiveHandshakeTimer::deadline_us(
    ActiveHandshakeState state) const noexcept -> std::optional<std::uint64_t> {
  if (window(state) == Window::none) return std::nullopt;
  return started_us_ + handshake_stage_timeout_us;
}

auto ActiveHandshakeTimer::moved_to(ActiveHandshakeState state,
                                    std::uint64_t now_us) noexcept -> bool {
  if (state == timed_stage_) return false;
  if (window(state) != window(timed_stage_)) {
    started_us_ = now_us;
    if (window(refused_stage_) != window(state)) refusal_.reset();
  }
  timed_stage_ = state;
  return true;
}

auto ActiveMediaQueue::media_size(const ActiveMediaEvent& event) noexcept
    -> std::size_t {
  return std::visit([](const auto& value) { return value.bytes.size(); },
                    event);
}

auto ActiveMediaQueue::profile_header(const ActiveMediaEvent& event) noexcept
    -> bool {
  const auto* video = std::get_if<takion::client_event::EncodedVideo>(&event);
  return video && video->profile_header;
}

auto ActiveMediaQueue::concealment_marker(
    const ActiveMediaEvent& event) noexcept -> bool {
  const auto* audio = std::get_if<takion::client_event::EncodedAudio>(&event);
  return audio && audio->bytes.empty() && !audio->profile_header;
}

auto ActiveMediaQueue::audio_profile_header(
    const ActiveMediaEvent& event) noexcept -> bool {
  const auto* audio = std::get_if<takion::client_event::EncodedAudio>(&event);
  return audio && audio->profile_header;
}

void ActiveSession::apply_link_metrics(
    StreamDiagnostics& result, const takion::LinkMetrics& link) noexcept {
  result.takion_features = link.features;
  const auto round_trip = link.round_trip.value_or(takion::RoundTripMetrics{});
  result.round_trip = StreamRoundTripDiagnostics{
      link.round_trip.has_value(), round_trip.smoothed_us,
      round_trip.variation_us, round_trip.spread};
  const auto extension = link.extension.value_or(takion::ExtensionMetrics{});
  result.extension = StreamExtensionDiagnostics{
      link.extension.has_value(), extension.jitter_us, extension.received,
      extension.lost, extension.loss_ratio};
  const auto video = link.video.value_or(takion::VideoMetrics{});
  result.video_metrics = StreamVideoMetricsDiagnostics{
      link.video.has_value(), video.reports_present,
      video.throughput_bps,   video.loss_ratio,
      video.send_interval_us, video.receive_interval_us,
      video.transfer_time_us, video.transmission_time_us,
      video.frame_bytes,      video.frame_number,
      video.frame_estimated,  video.reports_sent};
  const auto tos = link.tos.value_or(takion::TosMetrics{});
  const auto last = tos.last.value_or(takion::TrafficClass{});
  result.tos = StreamTosDiagnostics{link.tos.has_value(),
                                    tos.last.has_value(),
                                    last.dscp,
                                    last.ecn,
                                    tos.packets,
                                    tos.reports,
                                    tos.dscp_counts,
                                    tos.ecn_counts};
}

void ActiveMediaQueue::pop_front(Lane& lane) noexcept {
  lane.bytes -= media_size(lane.entries.front().event);
  lane.entries.pop_front();
}

auto ActiveMediaQueue::video_overflows(std::size_t incoming) const noexcept
    -> bool {
  const auto& lane = lanes_[1];
  return lane.entries.size() - video_headers_ == video_capacity ||
         lane.bytes > max_bytes - incoming;
}

auto ActiveMediaQueue::drop_video() -> std::optional<VideoFrameLoss> {
  auto& lane = lanes_[1];
  std::optional<Entry> header;
  std::optional<VideoFrameLoss> loss;
  for (auto& entry : lane.entries) {
    const auto& video =
        std::get<takion::client_event::EncodedVideo>(entry.event);
    if (video.profile_header) {
      header = std::move(entry);
      continue;
    }
    if (!loss) loss = VideoFrameLoss{video.frame_index, video.frame_index};
    loss->last = video.frame_index;
    ++dropped_;
  }
  lane.entries.clear();
  lane.bytes = 0;
  video_headers_ = header ? 1 : 0;
  if (header) {
    lane.bytes = media_size(header->event);
    lane.entries.push_back(std::move(*header));
  }
  return loss;
}

void ActiveMediaQueue::push_audio(Entry entry, std::size_t incoming,
                                  bool voice) {
  auto& lane = lanes_[voice ? voice_lane : 0];
  std::optional<std::uint64_t> concealed;
  const auto leading_header = [&lane]() -> std::size_t {
    return !lane.entries.empty() &&
                   audio_profile_header(lane.entries.front().event)
               ? 1U
               : 0U;
  };
  const auto evict = [&] {
    auto victim =
        lane.entries.begin() +
        static_cast<std::ptrdiff_t>(
            lane.entries.size() > leading_header() ? leading_header() : 0U);
    if (victim != lane.entries.begin() && audio_profile_header(victim->event)) {
      victim = lane.entries.begin();
    } else if (!audio_profile_header(victim->event)) {
      if (!concealment_marker(victim->event)) ++dropped_;
      if (!concealed) concealed = victim->sequence;
    }
    lane.bytes -= media_size(victim->event);
    lane.entries.erase(victim);
  };
  while (!lane.entries.empty() && (lane.entries.size() >= audio_capacity ||
                                   lane.bytes > max_bytes - incoming))
    evict();
  const auto marker_slot = leading_header();
  if (concealed && (lane.entries.size() <= marker_slot ||
                    !concealment_marker(lane.entries[marker_slot].event))) {
    if (lane.entries.size() + 1 >= audio_capacity) evict();
    takion::client_event::EncodedAudio marker;
    marker.voice = voice;
    lane.entries.insert(
        lane.entries.begin() + static_cast<std::ptrdiff_t>(leading_header()),
        Entry{*concealed, ActiveMediaEvent{std::move(marker)}});
  }
  lane.bytes += incoming;
  lane.entries.push_back(std::move(entry));
}

auto ActiveMediaQueue::push(ActiveMediaEvent event)
    -> std::vector<VideoFrameLoss> {
  std::vector<VideoFrameLoss> losses;
  const auto incoming = media_size(event);
  const auto index = event.index();
  if (index == 1) {
    const auto frame =
        std::get<takion::client_event::EncodedVideo>(event).frame_index;
    if (incoming > max_bytes) {
      ++dropped_;
      losses.push_back({frame, frame});
      return losses;
    }
    if (video_overflows(incoming))
      if (auto dropped = drop_video()) losses.push_back(*dropped);
  }
  Entry entry{next_sequence_++, std::move(event)};
  if (index == 0) {
    const bool voice =
        std::get<takion::client_event::EncodedAudio>(entry.event).voice;
    if (incoming > max_bytes) {
      ++dropped_;
      takion::client_event::EncodedAudio marker;
      marker.voice = voice;
      entry.event = ActiveMediaEvent{std::move(marker)};
      push_audio(std::move(entry), 0, voice);
    } else {
      push_audio(std::move(entry), incoming, voice);
    }
    return losses;
  }
  auto& lane = lanes_[index];
  if (index == 1 && profile_header(entry.event)) ++video_headers_;
  if (index == 2 || index == 3) {
    if (incoming > max_bytes) {
      ++dropped_;
      return losses;
    }
    const auto bound = index == 2 ? haptics_capacity : speaker_capacity;
    while (!lane.entries.empty() && (lane.entries.size() >= bound ||
                                     lane.bytes > max_bytes - incoming)) {
      pop_front(lane);
      ++dropped_;
    }
  }
  lane.bytes += incoming;
  lane.entries.push_back(std::move(entry));
  return losses;
}

auto ActiveMediaQueue::take() -> std::optional<ActiveMediaEvent> {
  Lane* selected = nullptr;
  for (auto& lane : lanes_)
    if (!lane.entries.empty() &&
        (!selected ||
         lane.entries.front().sequence < selected->entries.front().sequence))
      selected = &lane;
  if (!selected) return std::nullopt;
  selected->bytes -= media_size(selected->entries.front().event);
  if (profile_header(selected->entries.front().event)) --video_headers_;
  auto event = std::move(selected->entries.front().event);
  selected->entries.pop_front();
  return event;
}

void ActiveMediaQueue::clear() noexcept {
  for (auto& lane : lanes_) {
    lane.entries.clear();
    lane.bytes = 0;
  }
  video_headers_ = 0;
}

void ActiveFeedbackQueue::push(ActiveFeedbackEvent event) {
  static_cast<void>(events_.push(std::move(event)));
}

auto ActiveFeedbackQueue::take() -> std::optional<ActiveFeedbackEvent> {
  return events_.take();
}

void ActiveFeedbackQueue::clear() noexcept { events_.clear(); }

ActiveMediaPipeline::ActiveMediaPipeline(
    ActiveMediaSink& sink, ActiveMediaOptions options,
    StreamDiagnostics& diagnostics) noexcept
    : sink_(sink),
      options_(options),
      diagnostics_(diagnostics),
      assembly_(options.audio_assembly, options.count_only),
      dependencies_(options.hevc) {}

void ActiveMediaPipeline::set_count_only(bool count_only) noexcept {
  options_.count_only = count_only;
  assembly_.set_count_only(count_only);
}

void ActiveMediaPipeline::on_stream_info(const ValidatedStreamInfo& info,
                                         takion::FeatureSet features) {
  if (!profiles_.empty()) return;
  if (options_.audio_assembly == AudioAssemblyPolicy::cloud_generation) {
    auto& audio = diagnostics_.audio;
    audio = {};
    audio.present = true;
    audio.top_channels = std::to_integer<std::uint32_t>(info.audio_header[0]);
    audio.current_channels = audio.top_channels;
    for (const auto& channel : info.audio_channels)
      if (channel.type < 32)
        audio.channel_type_bits |= std::uint32_t{1} << channel.type;
    for (const auto& channel : info.audio_channels) {
      if (channel.type != 0) continue;
      audio.main_present = true;
      audio.main_channels = std::to_integer<std::uint32_t>(channel.header[0]);
      for (const auto& profile : channel.profiles)
        if (profile.first < 32)
          audio.main_profile_bits |= std::uint32_t{1} << profile.first;
      break;
    }
  }
  audio_profiles_.configure(info, features);
  voice_format_.reset();
  for (const auto& resolution : info.resolutions) {
    profiles_.push_back(resolution.video_header);
    profile_dimensions_.emplace_back(resolution.width, resolution.height);
  }
  video_profile_.reset();
  assembly_.on_stream_info();
}

void ActiveMediaPipeline::clear() noexcept {
  events_.clear();
  assembly_.reset();
  completions_.clear();
}

void ActiveMediaPipeline::count_lost(std::uint32_t frames) noexcept {
  const auto room = static_cast<std::uint32_t>(
      std::numeric_limits<std::int32_t>::max() - frames_lost_);
  frames_lost_ += static_cast<std::int32_t>(std::min(frames, room));
}

auto ActiveMediaPipeline::reportable(std::uint16_t first,
                                     std::uint16_t last) noexcept -> bool {
  return FrameIndex{first}.distance_to(FrameIndex{last}) <
         FrameIndex::half_range;
}

auto ActiveMediaPipeline::report_lost_range(std::uint16_t first,
                                            std::uint16_t last)
    -> Expected<void, ActiveSessionError> {
  if (!reportable(first, last)) return recover_unreportable_loss();
  auto posted = sink_.post_corrupt_frame(first, last);
  if (!posted) return posted;
  dependencies_.invalidate(first, last);
  assembly_.rewind_video_complete(first);
  const auto complete = assembly_.last_video_complete();
  while (!completions_.empty() &&
         static_cast<std::uint16_t>(completions_.back().frame - complete) - 1U <
             0x7fffU)
    completions_.pop_back();
  return {};
}

void ActiveMediaPipeline::record_completion(std::uint16_t frame) {
  if (completions_.size() == completion_capacity) completions_.pop_front();
  completions_.push_back(Completion{frame, assembly_.last_video_complete()});
  assembly_.mark_video_complete(frame);
}

auto ActiveMediaPipeline::report_loss(std::uint16_t first, std::uint16_t last)
    -> Expected<void, ActiveSessionError> {
  if (!reportable(first, last)) return recover_unreportable_loss();
  const auto span = static_cast<std::uint16_t>(last - first);
  const auto in_range = [first, span](std::uint16_t frame) noexcept {
    return static_cast<std::uint16_t>(frame - first) <= span;
  };
  const auto origin = static_cast<std::uint16_t>(first - 0x4000U);
  const auto offset = [origin](std::uint16_t frame) noexcept -> std::uint32_t {
    return static_cast<std::uint16_t>(frame - origin);
  };
  std::vector<std::pair<std::uint32_t, std::uint32_t>> posts;
  const auto post = [&](std::uint16_t from, std::uint16_t to) {
    if (FrameIndex{from}.distance_to(FrameIndex{to}) < FrameIndex::half_range)
      posts.emplace_back(offset(from), offset(to));
  };

  std::vector<std::uint16_t> matched;
  std::deque<Completion> kept;
  std::optional<std::uint16_t> carried;
  for (const auto& completion : completions_) {
    if (in_range(completion.frame)) {
      matched.push_back(completion.frame);
      if (!carried) carried = completion.previous;
      continue;
    }
    auto next = completion;
    if (carried) {
      next.previous = *carried;
      carried.reset();
      post(static_cast<std::uint16_t>(next.previous + 1U),
           static_cast<std::uint16_t>(next.frame - 1U));
    }
    kept.push_back(next);
  }
  if (carried) {
    const auto removed_tail = matched.back();
    assembly_.mark_video_complete(*carried);
    const auto started = assembly_.last_video_frame();
    if (started && FrameIndex{*started}.is_newer_than(FrameIndex{removed_tail}))
      post(static_cast<std::uint16_t>(*carried + 1U),
           static_cast<std::uint16_t>(*started - 1U));
  }
  completions_ = std::move(kept);

  std::uint32_t cursor = offset(first);
  const auto end = offset(last);
  std::sort(matched.begin(), matched.end(), [&](auto left, auto right) {
    return offset(left) < offset(right);
  });
  for (const auto frame : matched) {
    const auto at = offset(frame);
    if (at > cursor) posts.emplace_back(cursor, at - 1U);
    cursor = at + 1U;
  }
  if (cursor <= end) posts.emplace_back(cursor, end);

  dependencies_.invalidate(first, last);
  std::sort(posts.begin(), posts.end());
  std::vector<std::pair<std::uint32_t, std::uint32_t>> merged;
  for (const auto& range : posts) {
    if (!merged.empty() && range.first <= merged.back().second + 1U)
      merged.back().second = std::max(merged.back().second, range.second);
    else
      merged.push_back(range);
  }
  for (const auto& range : merged) {
    auto posted = sink_.post_corrupt_frame(
        static_cast<std::uint16_t>(origin + range.first),
        static_cast<std::uint16_t>(origin + range.second));
    if (!posted) return posted;
  }
  return {};
}

auto ActiveMediaPipeline::report_lost_frames(std::uint16_t first,
                                             std::uint16_t last)
    -> Expected<void, ActiveSessionError> {
  if (!reportable(first, last)) {
    count_lost(1);
    if (!options_.enable_idr_on_fec_failure) return {};
    return recover_unreportable_loss();
  }
  auto reported = report_lost_range(first, last);
  if (!reported) return reported;
  count_lost(
      static_cast<std::uint32_t>(static_cast<std::uint16_t>(last - first)) +
      1U);
  return request_idr();
}

auto ActiveMediaPipeline::recover_unreportable_loss()
    -> Expected<void, ActiveSessionError> {
  dependencies_.invalidate_all();
  return send_idr();
}

auto ActiveMediaPipeline::discard_requested(std::uint16_t frame) noexcept
    -> bool {
  if (dropping_frame_) {
    if (*dropping_frame_ == frame) return true;
    dropping_frame_.reset();
  }
  if (!drop_requested_) return false;
  const auto newest = assembly_.last_video_frame();
  if (newest && *newest == frame) return false;
  drop_requested_ = false;
  dropping_frame_ = frame;
  return true;
}

auto ActiveMediaPipeline::request_idr() -> Expected<void, ActiveSessionError> {
  if (!options_.enable_idr_on_fec_failure) return {};
  return send_idr();
}

auto ActiveMediaPipeline::send_idr() -> Expected<void, ActiveSessionError> {
  if (!dependencies_.request_idr()) return {};
  auto sent = sink_.post_idr_after_loss();
  if (sent && sent.value()) {
    idr_sent_us_ = now_us_;
    return {};
  }
  dependencies_.cancel_idr();
  if (!sent) return unexpected(sent.error());
  return {};
}

auto ActiveMediaPipeline::switch_profile(std::uint8_t index,
                                         std::uint16_t frame_index,
                                         std::uint8_t codec)
    -> Expected<void, ActiveSessionError> {
  if (video_profile_ == index || index >= profiles_.size()) return {};
  video_profile_ = index;
  const auto& profile = profiles_[index];
  dependencies_.header(profile);
  takion::client_event::EncodedVideo header;
  header.bytes.reserve(profile.size() + VideoAssembler::buffer_padding);
  header.bytes.assign(profile.begin(), profile.end());
  header.frame_index = frame_index;
  header.codec = codec;
  header.adaptive_stream_index = index;
  header.profile_header = true;
  return queue_media(ActiveMediaEvent{std::move(header)});
}

auto ActiveMediaPipeline::accept_video(
    takion::client_event::EncodedVideo& video) -> bool {
  if (video.adaptive_stream_index >= profiles_.size()) return false;
  const bool waiting = dependencies_.waiting_for_idr();
  const auto accepted = dependencies_.accept(video.frame_index, video.bytes);
  if (accepted) {
    refusals_ = 0;
    if (accepted.repaired) ++diagnostics_.repaired_frames;
    video.frames_lost = frames_lost_;
    video.frame_recovered = accepted.repaired;
    frames_lost_ = 0;
    return true;
  }
  if (!waiting) {
    count_lost(1);
    ++refusals_;
  }
  ++rejected_units_;
  ++diagnostics_.dependency_rejections;
  return false;
}

auto ActiveMediaPipeline::queue_media(ActiveMediaEvent event)
    -> Expected<void, ActiveSessionError> {
  for (const auto& loss : events_.push(std::move(event))) {
    auto reported = report_lost_range(loss.first, loss.last);
    if (!reported) return reported;
    count_lost(static_cast<std::uint32_t>(
                   static_cast<std::uint16_t>(loss.last - loss.first)) +
               1U);
  }
  return {};
}

auto ActiveMediaPipeline::queue_audio(takion::client_event::EncodedAudio audio)
    -> Expected<void, ActiveSessionError> {
  if (audio.voice) return queue_voice(std::move(audio));
  if (options_.audio_assembly == AudioAssemblyPolicy::cloud_generation &&
      audio.av_tag && *audio.av_tag < 32)
    diagnostics_.audio.lane_bits |= std::uint32_t{1} << *audio.av_tag;
  if (options_.audio_assembly == AudioAssemblyPolicy::cloud_generation &&
      audio.av_tag && *audio.av_tag != 0) {
    if (!options_.controller_speaker ||
        *audio.av_tag < controller_speaker_lane_base ||
        *audio.av_tag >=
            controller_speaker_lane_base + controller_speaker_lanes)
      return {};
    return queue_media(ActiveMediaEvent{takion::client_event::ControllerSpeaker{
        std::move(audio.bytes),
        static_cast<std::uint8_t>(*audio.av_tag -
                                  controller_speaker_lane_base)}});
  }
  if (const auto change =
          audio_profiles_.select(audio.av_tag, audio.audio_profile)) {
    if (options_.audio_assembly == AudioAssemblyPolicy::cloud_generation) {
      diagnostics_.audio.current_channels =
          std::to_integer<std::uint32_t>(change->header[0]);
      diagnostics_.audio.selection_present = true;
      diagnostics_.audio.selection_lane = audio.av_tag.value_or(0);
      diagnostics_.audio.selection_profile = audio.audio_profile;
    }
    takion::client_event::EncodedAudio header{
        {change->header.begin(), change->header.end()}};
    header.profile_header = true;
    header.raw_pcm = change->raw_pcm;
    auto queued = queue_media(ActiveMediaEvent{std::move(header)});
    if (!queued) return queued;
  }
  return queue_media(ActiveMediaEvent{std::move(audio)});
}

auto ActiveMediaPipeline::queue_voice(takion::client_event::EncodedAudio audio)
    -> Expected<void, ActiveSessionError> {
  const auto format = audio_profiles_.header_of(console_voice_lane);
  if (!format) return {};
  if (format != voice_format_) {
    voice_format_ = format;
    takion::client_event::EncodedAudio header{
        {format->header.begin(), format->header.end()}};
    header.profile_header = true;
    header.raw_pcm = format->raw_pcm;
    header.voice = true;
    auto queued = queue_media(ActiveMediaEvent{std::move(header)});
    if (!queued) return queued;
  }
  return queue_media(ActiveMediaEvent{std::move(audio)});
}

auto ActiveMediaPipeline::queue_video(takion::client_event::EncodedVideo video,
                                      bool assembled)
    -> Expected<void, ActiveSessionError> {
  ++diagnostics_.assembled_video;
  if (events_.video_overflows(video.bytes.size())) {
    if (const auto dropped = events_.drop_video()) {
      auto reported = report_lost_frames(dropped->first, dropped->last);
      if (!reported) return reported;
    }
  }
  if (!assembled) {
    auto switched = switch_profile(video.adaptive_stream_index,
                                   video.frame_index, video.codec);
    if (!switched) return switched;
  }
  if (!accept_video(video)) {
    if (refusals_ < refusals_before_idr) return {};
    refusals_ = 0;
    return request_idr();
  }
  if (assembled) record_completion(video.frame_index);
  return queue_media(ActiveMediaEvent{std::move(video)});
}

auto ActiveMediaPipeline::video_frame_stale(
    std::uint16_t frame_index) const noexcept -> bool {
  return assembly_.video_frame_stale(frame_index);
}

auto ActiveMediaPipeline::expected_video_blocks(
    const takion::AvPacketValues& values) const noexcept -> std::uint32_t {
  if (values.adaptive_stream_index >= profile_dimensions_.size()) return 0;
  const auto [width, height] =
      profile_dimensions_[values.adaptive_stream_index];
  const unsigned shift = values.codec == video_codec_avc    ? 4U
                         : values.codec == video_codec_hevc ? 6U
                         : options_.hevc                    ? 6U
                                                            : 4U;
  const std::uint32_t block = 1U << shift;
  return ((width + block - 1U) >> shift) * ((height + block - 1U) >> shift);
}

auto ActiveMediaPipeline::consume(takion::AvPacketValues values,
                                  std::uint64_t now_us)
    -> Expected<void, ActiveSessionError> {
  now_us_ = now_us;
  if (dependencies_.waiting_for_idr() && now_us >= idr_sent_us_ &&
      now_us - idr_sent_us_ >= idr_retry_interval_us) {
    dependencies_.cancel_idr();
    auto resent = send_idr();
    if (!resent) return resent;
  }
  if (options_.audio_assembly == AudioAssemblyPolicy::cloud_generation &&
      values.type.family() == takion::PacketFamily::audio_and_haptics &&
      !values.is_haptics && values.av_tag.value_or(0) == 0 &&
      !diagnostics_.audio.packet_present) {
    auto& audio = diagnostics_.audio;
    audio.packet_present = true;
    audio.packet_lane = values.av_tag.value_or(0);
    audio.packet_profile = values.audio_profile;
    audio.packet_mode = values.audio_mode;
    audio.packet_frame = values.frame_index;
    audio.packet_unit = values.unit_index;
    audio.packet_total = values.units_in_frame_total;
    audio.packet_fec = values.units_in_frame_fec;
    audio.packet_bytes = static_cast<std::uint64_t>(values.data.size());
  }
  if (values.type.family() == takion::PacketFamily::video &&
      (values.units_in_frame_total == 0 ||
       values.units_in_frame_total > max_units ||
       values.units_in_frame_fec >= values.units_in_frame_total ||
       values.unit_index >= values.units_in_frame_total)) {
    auto& counts = diagnostics_.video_counts;
    ++counts.packets;
    if (!counts.present) {
      counts.present = true;
      counts.frame = values.frame_index;
      counts.unit = values.unit_index;
      counts.total = values.units_in_frame_total;
      counts.fec = values.units_in_frame_fec;
      counts.video_word = values.video_word;
      counts.bytes = static_cast<std::uint64_t>(values.data.size());
    }
  }
  if (values.type.family() == takion::PacketFamily::video &&
      discard_requested(values.frame_index)) {
    ++diagnostics_.dropped_on_request;
    return {};
  }
  if (values.type.family() == takion::PacketFamily::video &&
      values.adaptive_stream_index >= profiles_.size()) {
    ++rejected_units_;
    ++diagnostics_.assembly_rejections;
    ++diagnostics_.video_invalid;
    return {};
  }
  if (values.type.family() == takion::PacketFamily::video) {
    const auto newest = assembly_.last_video_frame();
    const auto behind =
        newest ? static_cast<std::uint16_t>(*newest - values.frame_index)
               : std::uint16_t{};
    if (behind == 0 || behind >= FrameIndex::half_range) {
      auto switched = switch_profile(values.adaptive_stream_index,
                                     values.frame_index, values.codec);
      if (!switched) return switched;
    }
  }
  const auto blocks = expected_video_blocks(values);
  const bool parity = values.type.family() == takion::PacketFamily::video &&
                      values.units_in_frame_fec != 0 &&
                      values.units_in_frame_fec < values.units_in_frame_total &&
                      values.unit_index >= values.units_in_frame_total -
                                               values.units_in_frame_fec;
  if (values.type.family() == takion::PacketFamily::audio_and_haptics &&
      !values.is_haptics) {
    auto& lanes = diagnostics_.audio_lane_units;
    ++lanes[std::min<std::size_t>(values.av_tag.value_or(0), lanes.size() - 1)];
  }
  auto assembled = assembly_.consume(std::move(values), now_us, blocks);
  diagnostics_.video_fec_repaired = assembly_.fec_repaired_frames();
  while (auto lost = assembly_.take_video_loss()) {
    ++diagnostics_.lost_ranges;
    if (lost->fec_failure) ++diagnostics_.unrepaired_ranges;
    auto recovery = lost->fec_failure
                        ? report_lost_frames(lost->first, lost->last)
                        : report_lost_range(lost->first, lost->last);
    if (!recovery) return recovery;
    if (lost->fec_failure && options_.enable_idr_on_fec_failure)
      sink_.on_video_fec_failure(lost->last, dependencies_.waiting_for_idr());
  }
  if (assembled) {
    for (auto& media : assembled.value()) {
      auto queued = std::visit(
          [this](auto& value) {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value,
                                         takion::client_event::EncodedVideo>)
              return queue_video(std::move(value), true);
            else if constexpr (std::is_same_v<
                                   Value, takion::client_event::EncodedAudio>)
              return queue_audio(std::move(value));
            else
              return queue_media(ActiveMediaEvent{std::move(value)});
          },
          media);
      if (!queued) return queued;
    }
  } else if (parity &&
             assembly_.last_video_error() == VideoAssemblerError::stale_frame) {
    ++diagnostics_.video_late_parity;
  } else {
    ++rejected_units_;
    ++diagnostics_.assembly_rejections;
    if (const auto reason = assembly_.last_video_error()) {
      switch (*reason) {
        case VideoAssemblerError::stale_frame:
          ++diagnostics_.video_stale;
          break;
        case VideoAssemblerError::metadata_mismatch:
          ++diagnostics_.video_metadata;
          break;
        case VideoAssemblerError::duplicate_unit:
          ++diagnostics_.video_duplicate;
          break;
        default:
          ++diagnostics_.video_invalid;
          break;
      }
    }
  }
  return {};
}

ActiveSession::ActiveSession(ActiveHandshake handshake,
                             platform::ClockProvider& clock,
                             platform::CryptoProvider& crypto,
                             takion::CryptoProvider& takion_crypto,
                             SecretArray<ecdh_handshake_key_size> handshake_key,
                             ActiveControlProfile control_profile) noexcept
    : handshake_(std::move(handshake)),
      clock_(clock),
      stage_timer_(clock.now_microseconds()),
      keepalive_(stage_timer_.started_us()),
      crypto_(crypto),
      takion_crypto_(takion_crypto),
      handshake_key_(std::move(handshake_key)),
      control_profile_(control_profile),
      media_(
          *this,
          ActiveMediaOptions{control_profile.audio_assembly,
                             handshake_.profile().features().contains(
                                 takion::Feature::av_unit_count_only_encoding),
                             control_profile.hevc,
                             control_profile.enable_idr_on_fec_failure,
                             control_profile.controller_speaker},
          diagnostics_),
      pads_(feedback_events_, control_profile.controller_type) {}

ActiveSession::~ActiveSession() { takion_.reset(); }

void ActiveSession::trace_state() noexcept {
  const auto state = handshake_.state();
  if (!trace_ || logged_stage_ == state) return;
  logged_stage_ = state;
  trace_->record_state(diagnostics::Operation::active_state, state);
}

void ActiveSession::stop_route_once() noexcept {
  if (route_.stopped() || !takion_) return;
  route_.halt();
  if (!route_.keeps_media()) media_.clear();
  video_reorder_.reset();
  static_cast<void>(takion_->client().request_stop_with_disconnect(
      takion::Disconnect{"Client Disconnecting", std::nullopt}));
}

auto ActiveSession::fail(ActiveSessionError error,
                         std::optional<takion::ClientError> transport,
                         std::optional<std::string> peer_reason)
    -> Expected<void, ActiveSessionError> {
  if (!failure_)
    failure_ = ActiveSessionFailure{failure_stage_, error, transport,
                                    std::move(peer_reason)};
  if (trace_ && error != ActiveSessionError::peer_disconnected)
    trace_->record(diagnostics::Operation::active_state,
                   diagnostics::Outcome::failed, static_cast<unsigned>(error));
  handshake_.on_terminal_failure();
  trace_state();
  stop_route_once();
  return unexpected(failure_->cause);
}

auto ActiveSession::validate_and_ack(takion::StreamInfo info)
    -> Expected<void, ActiveSessionError> {
  auto validated = StreamInfoCodec::validate(std::move(info),
                                             handshake_.profile().features());
  if (!validated) {
    handshake_.on_stream_info_rejected();
    stage_timer_.refused(ActiveSessionError::handshake_failed,
                         handshake_.state());
    return {};
  }
  auto action =
      handshake_.on_stream_info_validated(std::move(validated.value()));
  if (!action) {
    handshake_.on_terminal_failure();
    stop_route_once();
    return fail(ActiveSessionError::handshake_failed);
  }
  if (const auto interval = action.value().info.congestion_control_interval_ms)
    keepalive_.set_congestion_interval_ms(std::uint64_t{*interval});
  media_.on_stream_info(action.value().info, handshake_.profile().features());
  static_cast<void>(takion_->client().post(
      takion::TakionCommand{std::move(action.value().ack)}));
  if (!announce_pads(control_profile_.pad_count)) {
    handshake_.on_stream_info_unacknowledged();
    stage_timer_.refused(ActiveSessionError::post_failed, handshake_.state());
    return {};
  }
  takion::StreamInfo microphone;
  microphone.audio_header.assign(microphone_audio_header.begin(),
                                 microphone_audio_header.end());
  if (!takion_->client().post(takion::TakionCommand{takion::SendControl{
          takion::ControlMessage{std::move(microphone)}}})) {
    handshake_.on_stream_info_unacknowledged();
    stage_timer_.refused(ActiveSessionError::post_failed, handshake_.state());
    return {};
  }
  ready_info_ = std::move(action.value().info);
  return {};
}

auto ActiveSession::post_controller_state(const ControllerState& state)
    -> Expected<void, ActiveSessionError> {
  if (route_.stopped() || handshake_.state() != ActiveHandshakeState::ready)
    return unexpected(ActiveSessionError::not_ready);
  auto values = ControllerSample::feedback_state(state);
  if (!values) return unexpected(ActiveSessionError::invalid_controller_state);
  if (!takion_->client().post(takion::TakionCommand{
          takion::SendFeedbackState{values.value(), feedback_sequence_}})) {
    ++diagnostics_.input.state_refused;
    return unexpected(ActiveSessionError::post_failed);
  }
  feedback_sequence_ = static_cast<std::uint16_t>(feedback_sequence_ + 1U);
  ++diagnostics_.input.state_posts;
  diagnostics_.input.state_sequence = feedback_sequence_;
  return {};
}

auto ActiveSession::post_controller_history_events(
    std::uint8_t controller_id,
    std::vector<takion::FeedbackHistoryEvent> events)
    -> Expected<void, ActiveSessionError> {
  if (route_.stopped() || handshake_.state() != ActiveHandshakeState::ready)
    return unexpected(ActiveSessionError::not_ready);
  if (controller_id >= Pad::count)
    return unexpected(ActiveSessionError::invalid_controller_state);
  if (events.empty()) return {};
  const auto fresh = events.size();
  return post_history_packet(std::move(events), controller_id, {}, fresh);
}

auto ActiveSession::post_controller_history_window(
    std::vector<takion::FeedbackHistoryEvent> events,
    std::vector<std::uint8_t> controller_ids, std::size_t fresh)
    -> Expected<void, ActiveSessionError> {
  if (route_.stopped() || handshake_.state() != ActiveHandshakeState::ready)
    return unexpected(ActiveSessionError::not_ready);
  if (controller_ids.size() != events.size() ||
      std::any_of(controller_ids.begin(), controller_ids.end(),
                  [](std::uint8_t id) { return id >= Pad::count; }))
    return unexpected(ActiveSessionError::invalid_controller_state);
  if (events.empty()) return {};
  return post_history_packet(std::move(events), 0, std::move(controller_ids),
                             fresh);
}

auto ActiveSession::post_history_packet(
    std::vector<takion::FeedbackHistoryEvent> events,
    std::uint8_t controller_id, std::vector<std::uint8_t> controller_ids,
    std::size_t fresh) -> Expected<void, ActiveSessionError> {
  const auto numbered = history_numbered_ + fresh;
  const auto sequence = static_cast<std::uint16_t>(numbered - 1U);
  if (!takion_->client().post(takion::TakionCommand{takion::SendFeedbackHistory{
          sequence, std::move(events), controller_id,
          std::move(controller_ids)}})) {
    ++diagnostics_.input.history_refused;
    return unexpected(ActiveSessionError::post_failed);
  }
  history_numbered_ = numbered;
  diagnostics_.input.history_wraps = numbered == 0 ? 0 : (numbered - 1U) >> 16U;
  ++diagnostics_.input.history_posts;
  diagnostics_.input.history_sequence = sequence;
  return {};
}

auto ActiveSession::post_controller_history_window(
    std::span<const takion::FeedbackHistoryEvent> events,
    std::span<const std::uint8_t> controller_ids, std::size_t fresh)
    -> Expected<void, ActiveSessionError> {
  return post_controller_history_window(
      std::vector<takion::FeedbackHistoryEvent>{events.begin(), events.end()},
      std::vector<std::uint8_t>{controller_ids.begin(), controller_ids.end()},
      fresh);
}

auto ActiveSession::post_controller_history(const ControllerState& previous,
                                            const ControllerState& current)
    -> Expected<void, ActiveSessionError> {
  if (route_.stopped() || handshake_.state() != ActiveHandshakeState::ready)
    return unexpected(ActiveSessionError::not_ready);
  auto delta = ControllerSample::history_delta(previous, current);
  if (!delta) return unexpected(ActiveSessionError::invalid_controller_state);
  return post_controller_history_events(current.controller_id,
                                        std::move(delta.value()));
}

auto ActiveSession::take_media_event() -> std::optional<ActiveMediaEvent> {
  return media_.take();
}

auto ActiveSession::post_corrupt_frame(std::uint16_t first, std::uint16_t last)
    -> Expected<void, ActiveSessionError> {
  if (route_.stopped() || handshake_.state() != ActiveHandshakeState::ready)
    return unexpected(ActiveSessionError::not_ready);
  return post_best_effort(
      takion::SendControl{takion::CorruptFrame{first, last}, {}});
}

auto ActiveSession::post_idr_after_loss()
    -> Expected<bool, ActiveSessionError> {
  auto requested = post_idr();
  if (requested) return true;
  if (requested.error() != takion::ClientError::mailbox_full) {
    auto failed = fail(ActiveSessionError::post_failed, requested.error());
    return unexpected(failed.error());
  }
  ++diagnostics_.dropped_posts;
  return false;
}

void ActiveSession::on_video_fec_failure(std::uint16_t frame_index,
                                         bool idr_request_sent) {
  feedback_events_.push(
      feedback_event::VideoFecFailure{frame_index, idr_request_sent});
}

auto ActiveSession::post_idr() -> takion::Result<void> {
  return takion_->client().post(takion::SendControl{takion::IdrRequest{}, {}});
}

auto ActiveSession::commit_profile(std::uint32_t version)
    -> Expected<void, ActiveSessionError> {
  std::optional<PreparedEcdh> replacement;
  const auto resolved =
      handshake_.state() == ActiveHandshakeState::awaiting_profile &&
              handshake_.accepts_version(version)
          ? takion::resolve_takion_wire_profile(version)
          : std::optional<takion::WireProfile>{};
  if (resolved && resolved->ecdh_curve() != handshake_.profile().ecdh_curve()) {
    auto prepared = EcdhWireCodec::prepare(
        crypto_, resolved->ecdh_curve(),
        std::span<const std::byte>{handshake_key_.data(),
                                   handshake_key_.size()});
    if (!prepared) {
      stop_route_once();
      return fail(ActiveSessionError::handshake_failed);
    }
    replacement = std::move(prepared.value());
  }
  auto action =
      handshake_.on_profile_committed(version, std::move(replacement));
  if (!action) {
    stop_route_once();
    return fail(ActiveSessionError::handshake_failed);
  }
  if (resolved) {
    committed_version_ = version;
    media_.set_count_only(handshake_.profile().features().contains(
        takion::Feature::av_unit_count_only_encoding));
  }
  if (action.value() && !takion_->client().post(takion::TakionCommand{
                            std::move(*action.value())})) {
    handshake_.on_terminal_failure();
    stop_route_once();
    return fail(ActiveSessionError::post_failed);
  }
  return {};
}

auto ActiveSession::post_video_frame_drop()
    -> Expected<void, ActiveSessionError> {
  if (route_.stopped() || handshake_.state() != ActiveHandshakeState::ready)
    return unexpected(ActiveSessionError::not_ready);
  media_.drop_next_video_frame();
  return {};
}

auto ActiveSession::post_idr_request() -> Expected<void, ActiveSessionError> {
  if (route_.stopped() || handshake_.state() != ActiveHandshakeState::ready)
    return unexpected(ActiveSessionError::not_ready);
  auto requested = post_idr();
  if (requested) return {};
  if (requested.error() == takion::ClientError::mailbox_full) {
    ++diagnostics_.dropped_posts;
    return unexpected(ActiveSessionError::post_failed);
  }
  return fail(ActiveSessionError::post_failed, requested.error());
}

auto ActiveSession::keyboard_record(std::uint8_t modifiers, std::uint8_t leds,
                                    std::span<const std::uint16_t> keys)
    -> std::optional<takion::FeedbackKind0Record> {
  if (keys.size() > takion::feedback_kind0_max_entries ||
      leds > keyboard_leds_max)
    return std::nullopt;
  takion::FeedbackKind0Record record{
      .raw_1 = 0, .raw_2 = modifiers, .upper_3 = leds};
  record.entries.reserve(keys.size());
  for (const auto key : keys)
    record.entries.push_back({static_cast<std::uint8_t>(key & 0xffU),
                              static_cast<std::uint8_t>(key >> 8U)});
  return record;
}

auto ActiveSession::keyboard_event(std::uint8_t modifiers, std::uint8_t leds,
                                   std::span<const std::uint16_t> keys) const
    -> Expected<takion::FeedbackHistoryEvent, ActiveSessionError> {
  if (route_.stopped() || handshake_.state() != ActiveHandshakeState::ready)
    return unexpected(ActiveSessionError::not_ready);
  if (handshake_.profile().version() < takion::feedback_kind0_version)
    return unexpected(ActiveSessionError::unsupported_request);
  auto record = keyboard_record(modifiers, leds, keys);
  if (!record) return unexpected(ActiveSessionError::invalid_controller_state);
  return takion::FeedbackHistoryEvent{std::move(*record)};
}

auto ActiveSession::post_audio_state(AudioStateKind kind,
                                     std::span<const std::byte> data)
    -> Expected<void, ActiveSessionError> {
  if (route_.stopped() || handshake_.state() != ActiveHandshakeState::ready)
    return unexpected(ActiveSessionError::not_ready);
  auto state =
      AudioStateCodec::build(kind, data, handshake_.profile().features());
  if (!state) return unexpected(ActiveSessionError::unsupported_request);
  return post_request(takion::SendControl{
      takion::ControlMessage{std::move(state).value()}, {}});
}

auto ActiveSession::post_generic_data(std::vector<std::byte> data,
                                      std::string metadata)
    -> Expected<void, ActiveSessionError> {
  if (route_.stopped() || handshake_.state() != ActiveHandshakeState::ready)
    return unexpected(ActiveSessionError::not_ready);
  return post_request(
      takion::SendGenericData{std::move(data), std::move(metadata)});
}

auto ActiveSession::post_deep_link(const takion::DeepLink& link)
    -> Expected<void, ActiveSessionError> {
  if (route_.stopped() || handshake_.state() != ActiveHandshakeState::ready)
    return unexpected(ActiveSessionError::not_ready);
  return post_request(takion::SendControl{takion::ControlMessage{link}, {}});
}

auto ActiveSession::post_microphone_connection()
    -> Expected<void, ActiveSessionError> {
  if (route_.stopped() || handshake_.state() != ActiveHandshakeState::ready)
    return unexpected(ActiveSessionError::not_ready);
  return post_request(takion::SendControl{
      takion::ControlMessage{takion::MicConnection{0, true, std::nullopt}},
      {}});
}

auto ActiveSession::post_system_menu_toggle(std::uint32_t pad)
    -> Expected<void, ActiveSessionError> {
  if (route_.stopped() || handshake_.state() != ActiveHandshakeState::ready)
    return unexpected(ActiveSessionError::not_ready);
  return post_request(
      takion::SendControl{takion::ControlMessage{takion::XmbCommand{
                              takion::XmbCommand::Command::toggle, pad}},
                          {}});
}

auto ActiveSession::post_microphone_mute(bool muted)
    -> Expected<void, ActiveSessionError> {
  if (route_.stopped() || handshake_.state() != ActiveHandshakeState::ready)
    return unexpected(ActiveSessionError::not_ready);
  if (!handshake_.profile().features().contains(
          takion::Feature::direct_message_encryption))
    return {};
  takion::MicInfo info;
  info.type = takion::MicInfo::Type::mute;
  info.controller_id = 0;
  info.muted = muted;
  return post_request(takion::SendControl{takion::ControlMessage{info}, {}});
}

auto ActiveSession::post_microphone(std::span<const std::byte> opus_frame)
    -> Expected<void, ActiveSessionError> {
  if (route_.stopped() || handshake_.state() != ActiveHandshakeState::ready)
    return unexpected(ActiveSessionError::not_ready);
  const auto& profile = handshake_.profile();
  auto command = microphone_.push(
      opus_frame, microphone_header_tagged(profile.av_layout()),
      profile.features().contains(
          takion::Feature::av_unit_count_only_encoding));
  if (!command) return {};
  return post_best_effort(std::move(*command));
}

auto ActiveSession::on_takion_event(takion::Event& event, std::uint64_t now)
    -> Expected<void, ActiveSessionError> {
  trace_state();
  failure_stage_ = handshake_.state();
  if (inbound_activity(event)) note_inbound(now);
  if (const auto* rejected =
          std::get_if<takion::client_event::PacketRejected>(&event)) {
    note_rejection(*rejected);
  } else if (std::holds_alternative<takion::client_event::Connected>(event)) {
    handshake_.on_connected();
  } else if (const auto* profile =
                 std::get_if<takion::client_event::ProfileCommitted>(&event)) {
    if (auto committed = commit_profile(profile->version); !committed)
      return committed;
  } else if (std::holds_alternative<takion::client_event::Closed>(event)) {
    handshake_.on_closed();
    route_.halt();
    media_.clear();
    video_reorder_.reset();
  } else if (const auto* terminal =
                 std::get_if<takion::client_event::TerminalFailure>(&event)) {
    if (handshake_.state() != ActiveHandshakeState::awaiting_connection)
      return fail(ActiveSessionError::pump_failed, terminal->error);
    route_.lose_takion();
  } else if (const auto* submitted =
                 std::get_if<takion::client_event::GenericDataSubmitted>(
                     &event)) {
    unrecognised_.keep(
        UnrecognisedMessage{UnrecognisedMessage::Kind::generic_data_outcome,
                            submitted->transfer_id,
                            "submitted",
                            {}});
  } else if (const auto* ended =
                 std::get_if<takion::client_event::GenericDataTransferEnded>(
                     &event)) {
    unrecognised_.keep(
        UnrecognisedMessage{UnrecognisedMessage::Kind::generic_data_outcome,
                            ended->transfer_id,
                            describe(*ended),
                            {}});
  } else if (auto* generic =
                 std::get_if<takion::client_event::GenericDataReceived>(
                     &event)) {
    unrecognised_.keep(UnrecognisedMessage{
        UnrecognisedMessage::Kind::generic_data, generic->transfer_id,
        std::move(generic->metadata), std::move(generic->data)});
  } else if (auto* control =
                 std::get_if<takion::client_event::ControlReceived>(&event)) {
    if (auto handled = on_control(*control); !handled) return handled;
  } else if (auto* unit =
                 std::get_if<takion::client_event::AvUnitReceived>(&event)) {
    if (auto handled = on_av_unit(unit->values); !handled) return handled;
  } else if (auto* audio =
                 std::get_if<takion::client_event::EncodedAudio>(&event)) {
    if (handshake_.state() == ActiveHandshakeState::ready) {
      auto queued = media_.queue_audio(std::move(*audio));
      if (!queued) return queued;
    }
  } else if (auto* video =
                 std::get_if<takion::client_event::EncodedVideo>(&event)) {
    if (handshake_.state() == ActiveHandshakeState::ready) {
      auto queued = media_.queue_video(std::move(*video), false);
      if (!queued) return queued;
    }
  } else if (auto* haptics =
                 std::get_if<takion::client_event::Haptics>(&event)) {
    if (handshake_.state() == ActiveHandshakeState::ready) {
      auto queued = media_.queue_media(ActiveMediaEvent{std::move(*haptics)});
      if (!queued) return queued;
    }
  } else {
    keep_feedback_event(event);
  }
  return {};
}

void ActiveSession::note_inbound(std::uint64_t now) noexcept {
  const bool quiet = keepalive_.heard(now);
  if (quiet && handshake_.state() == ActiveHandshakeState::ready)
    diagnostics_.network_trouble_us = now;
}

void ActiveSession::note_rejection(
    const takion::client_event::PacketRejected& rejected) {
  if (takion::is_transient_send_error(rejected.error)) {
    ++diagnostics_.send_dropped;
    return;
  }
  ++diagnostics_.rejected_packets;
  if (diagnostics_.rejected_packets <= UnrecognisedMessage::reported_rejections)
    unrecognised_.keep(
        UnrecognisedMessage{UnrecognisedMessage::Kind::rejection,
                            static_cast<std::uint32_t>(rejected.error),
                            std::to_string(rejected.line), rejected.bytes});
  if (rejected.error == takion::ClientError::crypto_failure)
    ++diagnostics_.crypto_rejections;
}

auto ActiveSession::describe(
    const takion::client_event::GenericDataTransferEnded& ended)
    -> std::string {
  constexpr std::array<std::string_view, 3> outcomes{"completed", "rejected",
                                                     "timed out"};
  const auto index = static_cast<std::size_t>(ended.outcome);
  return std::string{index < outcomes.size() ? outcomes[index]
                                             : std::string_view{"ended"}};
}

auto ActiveSession::describe(const takion::DeepLink& link) -> std::string {
  return std::format(
      "request {}, raw \"{}\", invitation \"{}\", session "
      "\"{}\", item \"{}\", pad {}, result {}, show {}",
      link.request_id ? std::to_string(*link.request_id) : std::string{"none"},
      link.raw_data, link.invitation_id, link.session_id, link.item_id,
      link.pad_unique_id ? std::to_string(*link.pad_unique_id)
                         : std::string{"none"},
      link.result ? std::to_string(*link.result) : std::string{"none"},
      link.should_show ? (*link.should_show ? "yes" : "no") : "none");
}

auto ActiveSession::describe(const takion::MicConnection& connection)
    -> std::string {
  return std::format("connection controller {} connected {} result {}",
                     connection.controller_id, connection.connected,
                     connection.result
                         ? (*connection.result ? "accepted" : "refused")
                         : "none");
}

auto ActiveSession::describe(const takion::MicInfo& microphone) -> std::string {
  const auto shown = [](std::optional<bool> value) {
    return value ? (*value ? "yes" : "no") : "none";
  };
  return std::format(
      "info {} controller {} connected {} muted {} result {}",
      microphone.type == takion::MicInfo::Type::mute ? "mute" : "connect",
      microphone.controller_id ? std::to_string(*microphone.controller_id)
                               : std::string{"none"},
      shown(microphone.connected), shown(microphone.muted),
      shown(microphone.result));
}

auto ActiveSession::on_control(takion::client_event::ControlReceived& control)
    -> Expected<void, ActiveSessionError> {
  if (auto* disconnected = std::get_if<takion::Disconnect>(&control.message)) {
    if (handshake_.state() == ActiveHandshakeState::ready) {
      video_reorder_.reset();
      route_.keep_media();
    }
    return fail(ActiveSessionError::peer_disconnected, {},
                disconnected->reason);
  } else if (std::holds_alternative<takion::Heartbeat>(control.message)) {
    auto echoed = post_best_effort(takion::SendControl{takion::Heartbeat{}});
    if (!echoed) return echoed;
  } else if (const auto* left =
                 std::get_if<takion::PlayTimeLeft>(&control.message)) {
    play_time_left_ = *left;
  } else if (const auto* overlay =
                 std::get_if<takion::MetricsOverlay>(&control.message)) {
    note_service_metrics(*overlay);
  } else if (const auto* link =
                 std::get_if<takion::DeepLink>(&control.message)) {
    unrecognised_.keep(
        UnrecognisedMessage{UnrecognisedMessage::Kind::deep_link,
                            static_cast<std::uint32_t>(link->type),
                            describe(*link),
                            {}});
  } else if (const auto* connection =
                 std::get_if<takion::MicConnection>(&control.message)) {
    unrecognised_.keep(UnrecognisedMessage{
        UnrecognisedMessage::Kind::microphone,
        static_cast<std::uint32_t>(takion::ControlPayloadType::mic_connection),
        describe(*connection),
        {}});
  } else if (const auto* microphone =
                 std::get_if<takion::MicInfo>(&control.message)) {
    unrecognised_.keep(UnrecognisedMessage{
        UnrecognisedMessage::Kind::microphone,
        static_cast<std::uint32_t>(takion::ControlPayloadType::direct_message),
        describe(*microphone),
        {}});
  } else if (auto* server =
                 std::get_if<takion::ServerMessage>(&control.message)) {
    unrecognised_.keep(UnrecognisedMessage{
        UnrecognisedMessage::Kind::server_message,
        static_cast<std::uint32_t>(takion::ControlPayloadType::server_message),
        std::move(server->text),
        {}});
  } else if (const auto* unsupported =
                 std::get_if<takion::UnsupportedControlMessage>(
                     &control.message)) {
    unrecognised_.keep(
        UnrecognisedMessage{UnrecognisedMessage::Kind::control,
                            static_cast<std::uint32_t>(unsupported->type),
                            {},
                            std::move(control.unsupported_payload)});
  } else if (auto* info = std::get_if<takion::StreamInfo>(&control.message)) {
    auto forwarded = handshake_.on_stream_info(std::move(*info));
    if (forwarded) {
      auto result = validate_and_ack(std::move(*forwarded));
      if (!result) return unexpected(result.error());
    }
  } else if (auto* bang = std::get_if<takion::Bang>(&control.message)) {
    return on_bang(std::move(*bang));
  }
  return {};
}

auto ActiveSession::on_bang(takion::Bang bang)
    -> Expected<void, ActiveSessionError> {
  auto install = ActiveHandshake::authenticate_bang(
      handshake_, crypto_, takion_crypto_,
      std::span<const std::byte>{handshake_key_.data(), handshake_key_.size()},
      std::move(bang));
  if (!install) {
    if (handshake_.state() == ActiveHandshakeState::awaiting_bang) {
      stage_timer_.refused(ActiveSessionError::bang_authentication_failed,
                           handshake_.state());
      return {};
    }
    handshake_.on_terminal_failure();
    stop_route_once();
    return fail(ActiveSessionError::bang_authentication_failed);
  }
  if (install.value() && handshake_.session_key_mismatch_tolerated() && trace_)
    trace_->decision(diagnostics::Trace::Decision::bang_session_key_mismatch);
  if (install.value() && !takion_->client().post(takion::TakionCommand{
                             std::move(*install.value())})) {
    handshake_.on_terminal_failure();
    stop_route_once();
    return fail(ActiveSessionError::post_failed);
  }
  return {};
}

auto ActiveSession::on_av_unit(takion::AvPacketValues& values)
    -> Expected<void, ActiveSessionError> {
  const bool video = values.type.family() == takion::PacketFamily::video;
  if ((video && control_profile_.drop_video) ||
      (!video && control_profile_.drop_audio && !values.is_haptics))
    return {};
  if (video) ++diagnostics_.video_units;
  if (handshake_.state() == ActiveHandshakeState::ready) {
    if (video) {
      video_reorder_.push(std::move(values), clock_.now_microseconds(),
                          reordered_units_);
      auto drained = consume_reordered_units();
      if (!drained) return drained;
    } else {
      auto consumed = consume_av_unit(std::move(values));
      if (!consumed) return consumed;
    }
  }
  return {};
}

auto ActiveSession::pump_once() -> Expected<void, ActiveSessionError> {
  if (route_.stopped())
    return unexpected(failure_ ? failure_->cause
                               : ActiveSessionError::pump_failed);
  trace_state();
  failure_stage_ = handshake_.state();
  const auto now = clock_.now_microseconds();
  if (stage_timer_.expired(handshake_.state(), now))
    return fail(stage_timer_.timeout_cause());
  if (route_.takion_lost()) return {};
  note_read(now);
  const auto pumped = takion_->client().pump_once();
  if (!pumped) {
    if (handshake_.state() != ActiveHandshakeState::awaiting_connection) {
      keep_feedback_events();
      return fail(ActiveSessionError::pump_failed, pumped.error());
    }
    route_.lose_takion();
  }

  while (auto event = inbox_.take_next())
    if (auto handled = on_takion_event(*event, now); !handled) return handled;
  if (auto drained = flush_reorder_queue(); !drained) return drained;
  if (auto accepted = accept_buffered_stream_info(); !accepted) return accepted;
  trace_state();
  const auto current = handshake_.state();
  retime_stage(current, now);
  return keep_alive(current, now);
}

auto ActiveSession::flush_reorder_queue()
    -> Expected<void, ActiveSessionError> {
  if (route_.stopped() || handshake_.state() != ActiveHandshakeState::ready ||
      !video_reorder_.waiting())
    return {};
  video_reorder_.flush(clock_.now_microseconds(), reordered_units_);
  return consume_reordered_units();
}

auto ActiveSession::accept_buffered_stream_info()
    -> Expected<void, ActiveSessionError> {
  if (handshake_.state() != ActiveHandshakeState::awaiting_crypto_install ||
      !takion_->client().crypto_installed())
    return {};
  auto buffered = handshake_.on_crypto_installed();
  if (!buffered) return {};
  return validate_and_ack(std::move(*buffered));
}

void ActiveSession::retime_stage(ActiveHandshakeState current,
                                 std::uint64_t now) noexcept {
  if (!stage_timer_.moved_to(current, now)) return;
  if (current == ActiveHandshakeState::ready) keepalive_.ready(now);
}

auto ActiveSession::keep_alive(ActiveHandshakeState current, std::uint64_t now)
    -> Expected<void, ActiveSessionError> {
  if (!route_.stopped() && current == ActiveHandshakeState::ready &&
      keepalive_.silent(now, control_profile_.peer_silence_timeout_us))
    return fail(ActiveSessionError::peer_silent);
  if (!route_.stopped() && !route_.takion_lost() &&
      current != ActiveHandshakeState::closed &&
      current != ActiveHandshakeState::failed &&
      keepalive_.congestion_due(now)) {
    auto posted = post_best_effort(takion::SendCongestion{
        media_.take_congestion(control_profile_.packet_loss_max)});
    if (!posted) return posted;
    keepalive_.congestion_sent(now);
  }
  if (!route_.stopped() && current == ActiveHandshakeState::ready &&
      control_profile_.periodic_heartbeat && keepalive_.heartbeat_due(now)) {
    failure_stage_ = current;
    auto posted = post_best_effort(takion::SendControl{takion::Heartbeat{}});
    if (!posted) return posted;
    keepalive_.heartbeat_sent(now);
  }
  if (!route_.stopped() && !route_.takion_lost()) {
    const auto flushed = takion_->client().flush_commands();
    if (!flushed) return fail(ActiveSessionError::pump_failed, flushed.error());
  }
  return {};
}

auto ActiveSession::next_deadline_us() const noexcept
    -> std::optional<std::uint64_t> {
  if (route_.stopped()) return std::nullopt;
  std::optional<std::uint64_t> next;
  const auto consider = [&next](std::uint64_t at) noexcept {
    if (!next || at < *next) next = at;
  };
  const auto state = handshake_.state();
  if (const auto window_ends = stage_timer_.deadline_us(state))
    consider(*window_ends);
  if (!route_.takion_lost() && state != ActiveHandshakeState::closed &&
      state != ActiveHandshakeState::failed)
    consider(keepalive_.congestion_deadline_us());
  if (state == ActiveHandshakeState::ready) {
    if (control_profile_.periodic_heartbeat)
      consider(keepalive_.heartbeat_deadline_us());
    if (control_profile_.peer_silence_timeout_us != 0)
      consider(keepalive_.silence_deadline_us(
          control_profile_.peer_silence_timeout_us));
    if (const auto reorder = video_reorder_.next_deadline_us())
      consider(*reorder);
  }
  return next;
}

auto ActiveSession::next_wake_us() const -> std::optional<std::uint64_t> {
  auto next = next_deadline_us();
  if (route_.stopped() || route_.takion_lost() || !takion_) return next;
  return timing::Deadline::earliest(next, takion_->client().next_deadline_us());
}

void ActiveSession::note_route_readiness(const platform::Pollable& polled,
                                         bool readable) {
  if (route_.stopped() || route_.takion_lost() || !takion_) return;
  takion_->client().note_readiness(polled, readable);
}

auto ActiveSession::request_stop() -> Expected<void, ActiveSessionError> {
  if (route_.stopped()) return {};
  route_.halt();
  media_.clear();
  feedback_events_.clear();
  video_reorder_.reset();
  auto stopped = takion_->client().request_stop_with_disconnect(
      takion::Disconnect{"Client Disconnecting", std::nullopt},
      disconnect_drain_);
  handshake_.on_closed();
  if (!stopped) {
    stop_failure_ = stopped.error();
    return unexpected(ActiveSessionError::pump_failed);
  }
  return {};
}

auto ActiveSession::snapshot() const -> takion::Snapshot {
  return takion_->client().snapshot();
}

void ActiveSession::note_service_metrics(
    const takion::MetricsOverlay& overlay) noexcept {
  auto& service = diagnostics_.service;
  const auto copy = [](std::array<char, 16>& into, const std::string& text) {
    into.fill('\0');
    std::copy_n(text.begin(), std::min(text.size(), into.size() - 1),
                into.begin());
  };
  service.present = true;
  service.reports += 1;
  service.available_down_kbps = overlay.available_down_kbps.value_or(0);
  service.used_down_kbps = overlay.used_down_kbps.value_or(0);
  service.used_up_kbps = overlay.used_up_kbps.value_or(0);
  service.round_trip_ms = overlay.round_trip_ms.value_or(0);
  service.loss_percent = overlay.loss_percent.value_or(0.0F);
  service.upstream_loss_percent = overlay.upstream_loss_percent.value_or(0.0F);
  service.gateway_round_trip_us = overlay.gateway_round_trip_us.value_or(0);
  service.audio_channels = overlay.audio_channels.value_or(0);
  service.width = overlay.width.value_or(0);
  service.height = overlay.height.value_or(0);
  service.corrupt_frames = overlay.corrupt_frames.value_or(0);
  copy(service.dynamic_range, overlay.dynamic_range);
  copy(service.video_codec, overlay.video_codec);
}

auto ActiveSession::diagnostics() const noexcept -> StreamDiagnostics {
  auto result = diagnostics_;
  if (!takion_) return result;
  try {
    apply_link_metrics(result, takion_->client().snapshot().link);
  } catch (...) {
    result.available = false;
  }
  return result;
}

auto ActiveSession::report_video_loss(std::uint16_t first, std::uint16_t last)
    -> Expected<void, ActiveSessionError> {
  return media_.report_loss(first, last);
}

auto ActiveSession::consume_av_unit(takion::AvPacketValues values)
    -> Expected<void, ActiveSessionError> {
  return media_.consume(std::move(values), clock_.now_microseconds());
}

auto ActiveSession::consume_reordered_units()
    -> Expected<void, ActiveSessionError> {
  for (auto& values : reordered_units_) {
    auto consumed = consume_av_unit(std::move(values));
    if (!consumed) {
      reordered_units_.clear();
      return consumed;
    }
  }
  reordered_units_.clear();
  diagnostics_.reorder_skipped = video_reorder_.skipped();
  diagnostics_.reorder_dropped = video_reorder_.dropped();
  return {};
}

void ActiveSession::note_read(std::uint64_t now_us) noexcept {
  if (!receive_buffer_read_) {
    receive_buffer_read_ = true;
    if (const auto buffer = takion_->client().snapshot().kernel_receive_buffer)
      diagnostics_.socket_receive_buffer = *buffer;
  }
  if (last_read_us_ && diagnostics_.video_units != 0 &&
      now_us >= *last_read_us_) {
    const auto gap = now_us - *last_read_us_;
    diagnostics_.longest_read_gap_us =
        std::max(diagnostics_.longest_read_gap_us, gap);
    if (gap > late_read_us) ++diagnostics_.late_reads;
  }
  last_read_us_ = now_us;
}

void ActiveSession::keep_feedback_events() {
  while (auto event = inbox_.take_next()) keep_feedback_event(*event);
}

void ActiveSession::keep_feedback_event(takion::Event& event) {
  if (auto* rumble = std::get_if<takion::client_event::Rumble>(&event)) {
    if (rumble->player_index < pads_.count())
      feedback_events_.push(ActiveFeedbackEvent{std::move(*rumble)});
  } else if (auto* triggers =
                 std::get_if<takion::client_event::TriggerEffects>(&event)) {
    if (triggers->player_index < pads_.count())
      feedback_events_.push(ActiveFeedbackEvent{std::move(*triggers)});
  } else if (const auto* pad =
                 std::get_if<takion::client_event::PadInfo>(&event)) {
    on_pad_info(*pad);
  }
}

void ActiveSession::on_pad_info(const takion::client_event::PadInfo& info) {
  if (info.motion_reset) feedback_events_.push(feedback_event::MotionReset{});
  if (info.motion_settings && info.motion_settings != motion_settings_) {
    motion_settings_ = info.motion_settings;
    feedback_events_.push(
        feedback_event::MotionSettings{*info.motion_settings});
  }
  if (info.haptic_intensity && *info.haptic_intensity != haptic_intensity_) {
    haptic_intensity_ = *info.haptic_intensity;
    feedback_events_.push(feedback_event::HapticIntensity{haptic_intensity_});
  }
  if (info.trigger_intensity && *info.trigger_intensity != trigger_intensity_) {
    trigger_intensity_ = *info.trigger_intensity;
    feedback_events_.push(feedback_event::TriggerIntensity{trigger_intensity_});
  }
  pads_.on_pad_info(info);
}

void ActivePads::on_pad_info(const takion::client_event::PadInfo& info) {
  if (info.haptic_intensity && info.index == 0) {
    if (info.led != led_state_) {
      led_state_ = info.led;
      feedback_.push(feedback_event::LedColor{led_state_});
    }
    if (info.index != player_index_) {
      player_index_ = info.index;
      feedback_.push(feedback_event::PlayerIndex{player_index_});
    }
  }
  if (info.index >= leds_.size() || !info.user_valid) return;
  if (info.index >= count_) {
    held_leds_[info.index] = info.led;
    return;
  }
  report_led(info.index, info.led);
}

void ActivePads::report_led(std::uint8_t pad,
                            const std::array<std::uint8_t, 3>& led) {
  if (led_known_[pad] && leds_[pad] == led) return;
  led_known_[pad] = true;
  leds_[pad] = led;
  feedback_.push(feedback_event::PadConfirmed{pad, led});
}

void ActivePads::set_controller_type(std::uint8_t pad,
                                     std::uint8_t type) noexcept {
  if (pad >= types_.size()) return;
  if (type <=
      static_cast<std::uint8_t>(takion::ControllerConnectionType::vr2sense))
    types_[pad] = static_cast<takion::ControllerConnectionType>(type);
  else
    types_[pad] = takion::UnknownControllerConnectionType{type};
}

auto ActivePads::controller_type(std::uint8_t pad) const noexcept
    -> takion::ControllerConnectionTypeValue {
  return pad < types_.size() ? types_[pad] : default_type_;
}

void ActivePads::admit(std::uint8_t pad, bool publish_held) noexcept {
  if (pad >= leds_.size()) return;
  const auto previous = count_;
  count_ = std::max(count_, static_cast<std::uint8_t>(pad + 1U));
  for (std::uint8_t index = previous; index < count_; ++index) {
    auto held = std::exchange(held_leds_[index], std::nullopt);
    if (!held) continue;
    if (publish_held || index != pad) {
      report_led(index, *held);
    } else {
      led_known_[index] = true;
      leds_[index] = *held;
    }
  }
}

void ActivePads::release(std::uint8_t pad) noexcept {
  if (pad >= leds_.size()) return;
  led_known_[pad] = false;
  held_leds_[pad].reset();
}

auto ActivePads::led(std::uint8_t pad) const noexcept
    -> std::array<std::uint8_t, 3> {
  return pad < leds_.size() && led_known_[pad] ? leds_[pad]
                                               : std::array<std::uint8_t, 3>{};
}

void ActiveSession::set_pad_controller_type(std::uint8_t pad,
                                            std::uint8_t type) noexcept {
  pads_.set_controller_type(pad, type);
}

auto ActiveSession::pad_controller_type(std::uint8_t pad) const noexcept
    -> takion::ControllerConnectionTypeValue {
  return pads_.controller_type(pad);
}

auto ActiveSession::announce_pad(std::uint8_t pad, bool uses_controller_id)
    -> Expected<void, ActiveSessionError> {
  if (route_.stopped() || pad >= Pad::count)
    return unexpected(ActiveSessionError::not_ready);
  takion::ControllerConnection connection;
  if (uses_controller_id) connection.controller_id = pad;
  connection.connected = true;
  connection.controller_type = pads_.controller_type(pad);
  if (!takion_->client().post(takion::TakionCommand{
          takion::SendControl{takion::ControlMessage{std::move(connection)}}}))
    return unexpected(ActiveSessionError::post_failed);
  return {};
}

auto ActiveSession::announce_pads(std::uint8_t session_pad_count)
    -> Expected<void, ActiveSessionError> {
  const bool uses_controller_id =
      std::max(pads_.count(), session_pad_count) > 1;
  for (std::uint8_t pad = 0; pad < pads_.count(); ++pad) {
    auto announced = announce_pad(pad, uses_controller_id);
    if (!announced) return announced;
  }
  return {};
}

auto ActiveSession::disconnect_pad(std::uint8_t pad)
    -> Expected<void, ActiveSessionError> {
  if (route_.stopped() || pad >= Pad::count)
    return unexpected(ActiveSessionError::not_ready);
  takion::ControllerConnection connection;
  connection.controller_id = pad;
  connection.connected = false;
  connection.controller_type = takion::ControllerConnectionType::not_set;
  if (!takion_->client().post(takion::TakionCommand{
          takion::SendControl{takion::ControlMessage{std::move(connection)}}}))
    return unexpected(ActiveSessionError::post_failed);
  return {};
}

void ActiveSession::admit_pad(std::uint8_t pad, bool publish_held) noexcept {
  pads_.admit(pad, publish_held);
}

void ActiveSession::release_pad(std::uint8_t pad) noexcept {
  pads_.release(pad);
}

auto ActiveSession::pad_led(std::uint8_t pad) const noexcept
    -> std::array<std::uint8_t, 3> {
  return pads_.led(pad);
}

auto ActiveSession::post_request(takion::TakionCommand command)
    -> Expected<void, ActiveSessionError> {
  auto posted = takion_->client().post(std::move(command));
  if (posted) return {};
  if (posted.error() == takion::ClientError::mailbox_full) {
    ++diagnostics_.dropped_posts;
    return unexpected(ActiveSessionError::post_failed);
  }
  return fail(ActiveSessionError::post_failed, posted.error());
}

auto ActiveSession::post_best_effort(takion::TakionCommand command)
    -> Expected<void, ActiveSessionError> {
  auto posted = takion_->client().post(std::move(command));
  if (posted) return {};
  if (posted.error() == takion::ClientError::mailbox_full) {
    ++diagnostics_.dropped_posts;
    return {};
  }
  return fail(ActiveSessionError::post_failed, posted.error());
}

auto ActiveSession::start(
    EstablishedRoute route, platform::ClockProvider& clock,
    platform::ThreadingProvider& threading,
    platform::SecureRandomProvider& random, platform::CryptoProvider& crypto,
    takion::CryptoProvider& takion_crypto,
    SecretArray<ecdh_handshake_key_size> handshake_key, PreparedEcdh prepared,
    std::shared_ptr<const SecretString> session_key,
    std::shared_ptr<const SecretString> encrypted_launch_spec,
    takion::ClientConfig config, ActiveControlProfile control_profile)
    -> Expected<std::unique_ptr<ActiveSession>, ActiveSessionError> {
  const auto profile = route.calibration.wire_profile;
  if (!control_profile.takion_offer.empty()) {
    config.profile_selection =
        takion::NegotiatedTakionProfiles{control_profile.takion_offer};
    config.negotiation_timeout_us = takion_negotiation_timeout_us;
  }
  auto owner = std::unique_ptr<ActiveSession>(new ActiveSession(
      ActiveHandshake{std::move(prepared), profile, std::move(session_key),
                      std::move(encrypted_launch_spec),
                      control_profile.bang_session_key,
                      control_profile.takion_offer},
      clock, crypto, takion_crypto, std::move(handshake_key), control_profile));
  config.source_tap = control_profile.source_tap;
  config.video_gate = &owner->media_;
  config.video_frame_loss_max = control_profile.packet_loss_max;
  auto active = ActiveTakion::start(std::move(route), clock, threading, random,
                                    owner->inbox_, std::move(config),
                                    control_profile.datagram_budget);
  if (!active) return unexpected(ActiveSessionError::start_failed);
  owner->takion_ = std::move(active.value());
  return owner;
}

void ActiveControlProfile::apply_session_tuning(
    ActiveControlProfile& profile, const SessionTuning& tuning) noexcept {
  profile.drop_audio = tuning.disable_audio;
  profile.drop_video = tuning.disable_video;
  profile.controller_speaker = tuning.controller_speaker;
  profile.packet_loss_max = tuning.packet_loss_max;
  profile.source_tap = tuning.source_tap;
}

void ActiveEventInbox::on_event(takion::Event event) {
  static_cast<void>(events_.push(std::move(event)));
}

auto ActiveEventInbox::take_next() -> std::optional<takion::Event> {
  return events_.take();
}

}  // namespace libchiaking::net::protocol::remote_play
