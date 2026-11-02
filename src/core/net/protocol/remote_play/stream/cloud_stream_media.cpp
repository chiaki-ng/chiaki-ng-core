#include "src/core/net/protocol/remote_play/stream/cloud_stream_media.hpp"
#include "src/core/utils/bytes/endian.hpp"

#include <algorithm>
#include <utility>

namespace libchiaking::net::protocol::remote_play {

auto CloudStreamMediaAssembly::encoded_audio(ConsoleAudioFrame frame)
    -> takion::client_event::EncodedAudio {
  takion::client_event::EncodedAudio audio{std::move(frame.bytes)};
  audio.av_tag = frame.av_tag;
  audio.audio_profile = frame.audio_profile;
  return audio;
}

auto CloudStreamMediaAssembly::cloud_lane(
    std::optional<std::uint8_t> tag) noexcept -> std::optional<std::size_t> {
  const auto lane = tag.value_or(0);
  switch (lane) {
    case 0:
    case 1:
    case 6:
    case 7:
    case 8:
    case 9:
      return lane;
    default:
      return std::nullopt;
  }
}

auto CloudStreamMediaAssembly::cloud_audio_events(
    std::vector<ConsoleAudioFrame> frames, std::size_t lane, bool count_frames)
    -> std::vector<AssembledMedia> {
  std::vector<AssembledMedia> events;
  events.reserve(frames.size());
  for (auto& frame : frames) {
    if (count_frames && lane == 0 && !frame.bytes.empty())
      stats_.audio(frame.index);
    events.emplace_back(encoded_audio(std::move(frame)));
  }
  return events;
}

auto ConsoleAudioAssembler::consume(const takion::AvPacketValues& packet,
                                    bool count_only)
    -> Expected<std::vector<ConsoleAudioFrame>, ConsoleAudioError> {
  const std::size_t total = packet.units_in_frame_total;
  if (packet.codec != audio_codec_opus || total == 0 || packet.data.empty() ||
      packet.data.size() % total != 0)
    return unexpected(ConsoleAudioError::invalid_packet);
  const bool temporal = count_only && !packet.is_haptics &&
                        packet.audio_mode == audio_mode_temporal_redundancy;
  const std::size_t count =
      packet.units_in_frame_fec & (temporal ? takion::av_audio_packed_field_mask
                                            : audio_fec_count_only_source_mask);
  if (temporal && count >= total)
    return unexpected(ConsoleAudioError::invalid_units);
  const std::size_t source =
      count_only ? (temporal ? total - count : count)
                 : packet.units_in_frame_fec & audio_fec_nibble_mask;
  const std::size_t fec =
      count_only ? total - std::min(source, total)
                 : (packet.units_in_frame_fec >> audio_fec_count_shift) &
                       audio_fec_nibble_mask;
  const std::size_t size =
      count_only ? packet.data.size() / total
                 : packet.units_in_frame_fec >> audio_unit_size_shift;
  if ((count_only && source == 0) || source > total || source + fec != total ||
      size == 0 || size * total != packet.data.size())
    return unexpected(ConsoleAudioError::invalid_units);
  if (packet.frame_index > audio_startup_frame_limit) startup_ = false;
  std::vector<ConsoleAudioFrame> result;
  for (std::size_t position = 0; position < total; ++position) {
    const auto index =
        packet.is_haptics ? position : (position + source) % total;
    std::uint16_t frame;
    if (index < source) {
      frame = static_cast<std::uint16_t>(packet.frame_index + index);
    } else {
      const auto fec_index = index - source;
      if (startup_ && packet.frame_index + fec_index < fec + 1) continue;
      frame = static_cast<std::uint16_t>(packet.frame_index - fec + fec_index);
    }
    push_frame(frame, std::span{packet.data}.subspan(index * size, size),
               packet, result);
  }
  return result;
}

void ConsoleAudioAssembler::push_frame(std::uint16_t frame,
                                       std::span<const std::byte> bytes,
                                       const takion::AvPacketValues& packet,
                                       std::vector<ConsoleAudioFrame>& result) {
  ConsoleAudioFrame entry{
      frame, {bytes.begin(), bytes.end()}, packet.av_tag, packet.audio_profile};
  if (packet.is_haptics) {
    if (newer(frame, last_haptic_)) {
      last_haptic_ = frame;
      result.push_back(std::move(entry));
    }
    return;
  }
  if (next_ && newer(*next_, frame)) return;
  if (std::any_of(buffer_.begin(), buffer_.end(),
                  [frame](const auto& value) { return value.index == frame; }))
    return;
  if (buffer_.size() == audio_jitter_buffer_size) {
    auto newest = extreme(false);
    if (!newer(buffer_[newest].index, frame)) return;
    buffer_.erase(buffer_.begin() + static_cast<std::ptrdiff_t>(newest));
  }
  buffer_.push_back(std::move(entry));
  if (!next_ && buffer_.size() >= audio_jitter_prefill)
    next_ = buffer_[extreme(true)].index;
  drain(result);
}

void ConsoleAudioAssembler::reset() noexcept {
  buffer_.clear();
  next_.reset();
  last_haptic_ = 0;
  startup_ = true;
}

auto ConsoleAudioAssembler::newer(std::uint16_t a, std::uint16_t b) noexcept
    -> bool {
  return FrameIndex{a}.is_newer_than(FrameIndex{b});
}

auto ConsoleAudioAssembler::extreme(bool oldest) const noexcept -> std::size_t {
  std::size_t selected = 0;
  for (std::size_t i = 1; i < buffer_.size(); ++i) {
    if (oldest ? newer(buffer_[selected].index, buffer_[i].index)
               : newer(buffer_[i].index, buffer_[selected].index))
      selected = i;
  }
  return selected;
}

void ConsoleAudioAssembler::drain(std::vector<ConsoleAudioFrame>& result) {
  while (next_ && !buffer_.empty()) {
    const auto found = std::find_if(
        buffer_.begin(), buffer_.end(),
        [this](const auto& frame) { return frame.index == *next_; });
    if (found != buffer_.end()) {
      result.push_back(std::move(*found));
      buffer_.erase(found);
      ++*next_;
      continue;
    }
    const auto oldest = buffer_[extreme(true)].index;
    const auto newest = buffer_[extreme(false)].index;
    if (!newer(oldest, *next_) ||
        (buffer_.size() >= audio_jitter_prefill &&
         !newer(newest, static_cast<std::uint16_t>(
                            *next_ + static_cast<std::uint16_t>(
                                         audio_jitter_prefill - 1)))))
      break;
    result.push_back({*next_, {}});
    ++*next_;
  }
}

auto CloudStreamAudioAssembler::validate_unit(
    const takion::AvPacketValues& packet)
    -> Expected<CloudStreamAudioUnit, CloudStreamAudioUnitError> {
  if (packet.type.family() != takion::PacketFamily::audio_and_haptics)
    return unexpected(CloudStreamAudioUnitError::invalid_family);
  if (packet.codec != audio_codec_opus)
    return unexpected(CloudStreamAudioUnitError::invalid_codec);
  const auto total = packet.units_in_frame_total;
  const auto fec = packet.units_in_frame_fec;
  if (total == 0 || total > max_total_units || fec >= total)
    return unexpected(CloudStreamAudioUnitError::invalid_count);
  if (packet.unit_index >= total)
    return unexpected(CloudStreamAudioUnitError::invalid_unit_index);
  if (packet.data.empty())
    return unexpected(CloudStreamAudioUnitError::empty_data);
  if (packet.data.size() > max_unit_bytes)
    return unexpected(CloudStreamAudioUnitError::data_too_large);

  return CloudStreamAudioUnit{packet.frame_index, packet.unit_index,
                              static_cast<std::uint16_t>(total - fec), fec,
                              packet.data};
}

auto CloudStreamAudioAssembler::accept(const CloudStreamAudioUnit& unit,
                                       std::uint64_t now_us)
    -> Expected<std::vector<CloudStreamAudioFrame>,
                CloudStreamAudioAssemblerError> {
  const auto total =
      static_cast<std::size_t>(unit.source_count) + unit.fec_count;
  if (unit.source_count == 0 || total > max_total_units ||
      unit.unit_index >= total || unit.data.empty() ||
      unit.data.size() > max_unit_bytes)
    return unexpected(CloudStreamAudioAssemblerError::invalid_unit);
  const auto base =
      numbering_ == AudioGenerationNumbering::per_frame
          ? unit.frame_index
          : static_cast<std::uint16_t>(unit.frame_index - unit.unit_index);
  std::vector<CloudStreamAudioFrame> frames;
  bool new_generation = !active_;
  if (active_) {
    const bool restarts =
        unit.unit_index == 0 && (base != active_->base ||
                                 (!active_->emitted && active_->slots.front()));
    if (!restarts && base != active_->base && !newer(base, active_->base))
      return unexpected(CloudStreamAudioAssemblerError::stale_generation);
    const bool counts_match = unit.source_count == active_->source_count &&
                              unit.fec_count == active_->fec_count;
    const bool timed_out = now_us >= active_->started_us &&
                           now_us - active_->started_us > generation_timeout_us;
    if (restarts || base != active_->base || !counts_match) {
      new_generation = true;
    } else if (active_->emitted) {
      if (numbering_ == AudioGenerationNumbering::per_frame) return frames;
      return unexpected(CloudStreamAudioAssemblerError::stale_generation);
    } else if (active_->slots[unit.unit_index]) {
      return unexpected(CloudStreamAudioAssemblerError::duplicate_unit);
    } else if (timed_out) {
      new_generation = true;
    } else if (unit.data.size() > max_held_bytes - held_bytes_) {
      return unexpected(CloudStreamAudioAssemblerError::hold_limit_exceeded);
    }
  }
  if (new_generation) {
    if (unit.data.size() > max_held_bytes)
      return unexpected(CloudStreamAudioAssemblerError::hold_limit_exceeded);
    if (active_ && !active_->emitted) flush(frames);
    active_.emplace(unit.source_count, unit.fec_count, base, total,
                    unit.data.size(), now_us);
    held_bytes_ = 0;
  }

  active_->slots[unit.unit_index] = unit.data;
  held_bytes_ += unit.data.size();
  std::size_t received = 0;
  bool missing_source = false;
  for (const auto& slot : active_->slots) received += slot ? 1U : 0U;
  for (std::size_t index = 0; index < active_->source_count; ++index)
    missing_source |= !active_->slots[index];
  if (!missing_source) {
    for (std::size_t index = 0; index < active_->source_count; ++index)
      frames.push_back(CloudStreamAudioFrame{
          static_cast<std::uint16_t>(active_->base + index),
          std::move(*active_->slots[index])});
    finish();
    return frames;
  }
  if (received < active_->source_count) return frames;
  if (active_->source_count > max_held_bytes / active_->unit_size) {
    finish();
    return unexpected(CloudStreamAudioAssemblerError::hold_limit_exceeded);
  }
  auto recovered = recover();
  if (!recovered) return frames;
  for (std::size_t index = 0; index < active_->source_count; ++index)
    frames.push_back(
        CloudStreamAudioFrame{static_cast<std::uint16_t>(active_->base + index),
                              std::move(recovered.value()[index])});
  finish();
  return frames;
}

CloudStreamAudioAssembler::CloudStreamAudioAssembler(
    AudioGenerationNumbering numbering) noexcept
    : numbering_(numbering) {}

void CloudStreamAudioAssembler::reset() noexcept {
  active_.reset();
  held_bytes_ = 0;
}

CloudStreamAudioAssembler::Generation::Generation(
    std::uint16_t source, std::uint16_t fec, std::uint16_t frame,
    std::size_t total, std::size_t first_unit_size, std::uint64_t now_us)
    : source_count(source),
      fec_count(fec),
      base(frame),
      unit_size(first_unit_size),
      started_us(now_us),
      slots(total) {}

auto CloudStreamAudioAssembler::newer(std::uint16_t candidate,
                                      std::uint16_t current) noexcept -> bool {
  return FrameIndex{candidate}.is_newer_than(FrameIndex{current});
}

void CloudStreamAudioAssembler::finish() noexcept {
  active_->emitted = true;
  for (auto& slot : active_->slots) slot.reset();
  held_bytes_ = 0;
}

auto CloudStreamAudioAssembler::recover()
    -> Expected<std::vector<std::vector<std::byte>>, CauchyFec8Error> {
  std::vector<std::optional<std::vector<std::byte>>> padded(
      active_->slots.size());
  for (std::size_t index = 0; index < padded.size(); ++index) {
    if (!active_->slots[index]) continue;
    const auto& data = *active_->slots[index];
    padded[index].emplace(active_->unit_size, std::byte{0});
    std::copy_n(data.begin(), std::min(data.size(), active_->unit_size),
                padded[index]->begin());
  }
  return decoder_.recover(std::move(padded), active_->source_count,
                          active_->fec_count);
}

void CloudStreamAudioAssembler::flush(
    std::vector<CloudStreamAudioFrame>& frames) {
  std::size_t received = 0;
  for (const auto& slot : active_->slots) received += slot ? 1U : 0U;
  if (received >= active_->source_count &&
      active_->source_count <= max_held_bytes / active_->unit_size) {
    if (auto recovered = recover()) {
      for (std::size_t index = 0; index < active_->source_count; ++index)
        frames.push_back(CloudStreamAudioFrame{
            static_cast<std::uint16_t>(active_->base + index),
            std::move(recovered.value()[index])});
      finish();
      return;
    }
  }
  for (std::size_t index = 0; index < active_->source_count; ++index)
    if (active_->slots[index])
      frames.push_back(CloudStreamAudioFrame{
          static_cast<std::uint16_t>(active_->base + index),
          std::move(*active_->slots[index])});
  finish();
}

void MediaPacketStats::video(const takion::AvPacketValues& packet) noexcept {
  ++received_;
  if (!video_newest_) {
    video_newest_ = packet.packet_index;
    ++video_span_;
    return;
  }
  if (!newer(packet.packet_index, *video_newest_)) return;
  video_span_ +=
      static_cast<std::uint16_t>(packet.packet_index - *video_newest_);
  video_newest_ = packet.packet_index;
}

void MediaPacketStats::audio(std::uint16_t frame,
                             std::uint16_t units) noexcept {
  ++audio_received_;
  audio_units_ = std::max<std::uint16_t>(units, 1);
  if (newer(frame, audio_max_)) audio_max_ = frame;
}

auto MediaPacketStats::take(double packet_loss_max) noexcept
    -> takion::CongestionValues {
  const auto difference = static_cast<std::uint16_t>(audio_max_ - audio_min_);
  const auto audio_expected =
      static_cast<std::uint64_t>(difference) * audio_units_;
  auto lost =
      (received_ < video_span_ ? video_span_ - received_ : 0) +
      (audio_received_ < audio_expected ? audio_expected - audio_received_ : 0);
  const auto server_sent = received_ + audio_received_ + lost;
  const double packet_loss =
      server_sent ? static_cast<double>(lost) / static_cast<double>(server_sent)
                  : 0;
  if (packet_loss > packet_loss_max)
    lost = static_cast<std::uint64_t>(static_cast<double>(server_sent) *
                                      packet_loss_max);
  received_ = 0;
  video_span_ = 0;
  audio_received_ = 0;
  audio_min_ = audio_max_;
  constexpr std::uint64_t most = 0xffff;
  const auto reported_sent = std::min(server_sent, most);
  const auto reported_lost =
      server_sent > most ? lost * most / server_sent : lost;
  return {0, static_cast<std::uint16_t>(reported_sent),
          static_cast<std::uint16_t>(reported_lost)};
}

void MediaPacketStats::reset() noexcept { *this = {}; }

auto MediaPacketStats::newer(std::uint16_t a, std::uint16_t b) noexcept
    -> bool {
  return FrameIndex{a}.is_newer_than(FrameIndex{b});
}

CloudStreamMediaAssembly::CloudStreamMediaAssembly(AudioAssemblyPolicy policy,
                                                   bool count_only) noexcept
    : policy_(policy),
      count_only_(count_only),
      audio_(policy == AudioAssemblyPolicy::console_by_mode
                 ? AudioGenerationNumbering::per_frame
                 : AudioGenerationNumbering::per_unit) {}

auto CloudStreamMediaAssembly::consume(takion::AvPacketValues packet,
                                       std::uint64_t now_us,
                                       std::uint32_t expected_video_blocks)
    -> Expected<std::vector<AssembledMedia>, CloudStreamMediaAssemblyError> {
  last_video_error_.reset();
  const auto family = packet.type.family();
  if (family == takion::PacketFamily::video) {
    stats_.video(packet);
    auto frame = video_.push(std::move(packet), expected_video_blocks);
    if (!frame) {
      last_video_error_ = frame.error();
      return unexpected(CloudStreamMediaAssemblyError::video_rejected);
    }
    if (!frame.value()) return std::vector<AssembledMedia>{};
    if (!frame.value()->restored_units.empty()) ++fec_repaired_frames_;
    takion::client_event::EncodedVideo video{
        std::move(frame.value()->bytes), frame.value()->frame_index,
        frame.value()->codec, frame.value()->adaptive_stream_index};
    video.dynamic_range_flag = frame.value()->dynamic_range_flag;
    video.suppress_presentation = frame.value()->suppress_presentation;
    std::vector<AssembledMedia> events;
    events.emplace_back(std::move(video));
    return events;
  }
  if (family == takion::PacketFamily::audio_and_haptics) {
    const auto pad =
        packet.is_haptics && packet.av_tag && *packet.av_tag >= haptics_tag_base
            ? static_cast<std::uint8_t>(*packet.av_tag - haptics_tag_base)
            : std::uint8_t{};
    const bool cloud = policy_ == AudioAssemblyPolicy::cloud_generation;
    const auto lane =
        cloud && !packet.is_haptics ? cloud_lane(packet.av_tag) : std::nullopt;
    if (cloud && !packet.is_haptics && !lane)
      return unexpected(CloudStreamMediaAssemblyError::audio_unit_rejected);
    if (cloud && !packet.is_haptics &&
        packet.audio_mode == complete_frame_mode) {
      if (packet.codec != audio_codec_opus || packet.data.empty() ||
          packet.data.size() > CloudStreamAudioAssembler::max_unit_bytes ||
          packet.units_in_frame_total == 0 ||
          packet.units_in_frame_total >
              CloudStreamAudioAssembler::max_total_units ||
          packet.unit_index >= packet.units_in_frame_total)
        return unexpected(CloudStreamMediaAssemblyError::audio_unit_rejected);
      std::vector<ConsoleAudioFrame> frames;
      cloud_audio_jitter_[*lane].push_frame(packet.frame_index, packet.data,
                                            packet, frames);
      return cloud_audio_events(std::move(frames), *lane, true);
    }
    if (uses_console_units(packet)) {
      const bool voice = policy_ == AudioAssemblyPolicy::console_units &&
                         !packet.is_haptics &&
                         packet.av_tag == console_voice_lane;
      auto& assembler = packet.is_haptics ? console_haptics_
                        : cloud           ? cloud_audio_jitter_[*lane]
                        : voice           ? console_voice_
                                          : console_audio_;
      auto frames = assembler.consume(packet, count_only_);
      if (!frames)
        return unexpected(CloudStreamMediaAssemblyError::audio_unit_rejected);
      if (cloud && !packet.is_haptics) {
        if (*lane == 0) stats_.audio(packet.frame_index);
        return cloud_audio_events(std::move(frames.value()), *lane, false);
      }
      if (!packet.is_haptics && !voice) stats_.audio(packet.frame_index);
      std::vector<AssembledMedia> events;
      for (auto& frame : frames.value()) {
        if (packet.is_haptics) {
          events.emplace_back(
              takion::client_event::Haptics{std::move(frame.bytes), pad});
        } else {
          auto audio = encoded_audio(std::move(frame));
          audio.voice = voice;
          events.emplace_back(std::move(audio));
        }
      }
      return events;
    }
    auto unit = CloudStreamAudioAssembler::validate_unit(packet);
    if (!unit)
      return unexpected(CloudStreamMediaAssemblyError::audio_unit_rejected);
    auto frames = (packet.is_haptics ? cloud_haptics_
                   : cloud           ? cloud_audio_units_[*lane]
                                     : audio_)
                      .accept(unit.value(), now_us);
    if (!frames)
      return unexpected(CloudStreamMediaAssemblyError::audio_rejected);
    std::vector<ConsoleAudioFrame> jittered;
    if (!packet.is_haptics && policy_ == AudioAssemblyPolicy::console_by_mode)
      stats_.audio(packet.frame_index, packet.units_in_frame_total);
    for (auto& frame : frames.value()) {
      if (!packet.is_haptics &&
          policy_ != AudioAssemblyPolicy::console_by_mode &&
          (!cloud || *lane == 0))
        stats_.audio(frame.frame_index);
      if (!packet.is_haptics && policy_ == AudioAssemblyPolicy::console_by_mode)
        frame.bytes = without_unit_header(frame.bytes);
      (packet.is_haptics ? console_haptics_
       : cloud           ? cloud_audio_jitter_[*lane]
                         : console_audio_)
          .push_frame(frame.frame_index, frame.bytes, packet, jittered);
    }
    if (cloud && !packet.is_haptics)
      return cloud_audio_events(std::move(jittered), *lane, false);
    std::vector<AssembledMedia> events;
    events.reserve(jittered.size());
    for (auto& frame : jittered) {
      if (packet.is_haptics)
        events.emplace_back(
            takion::client_event::Haptics{std::move(frame.bytes), pad});
      else
        events.emplace_back(encoded_audio(std::move(frame)));
    }
    return events;
  }
  return unexpected(CloudStreamMediaAssemblyError::unsupported_family);
}

auto CloudStreamMediaAssembly::without_unit_header(
    std::span<const std::byte> unit) -> std::vector<std::byte> {
  if (unit.size() < audio_unit_header_size) return {};
  const auto data = unit.subspan(audio_unit_header_size);
  return {data.begin(), data.end()};
}

auto CloudStreamMediaAssembly::uses_console_units(
    const takion::AvPacketValues& packet) const noexcept -> bool {
  const bool temporal = packet.audio_mode == audio_mode_temporal_redundancy;
  switch (policy_) {
    case AudioAssemblyPolicy::console_units:
      return true;
    case AudioAssemblyPolicy::console_by_mode:
      return !count_only_ || packet.audio_mode == 0 || temporal;
    case AudioAssemblyPolicy::cloud_generation:
      return count_only_ && temporal;
  }
  return true;
}

auto CloudStreamMediaAssembly::take_video_loss() noexcept
    -> std::optional<VideoFrameLoss> {
  return video_.take_loss();
}

void CloudStreamMediaAssembly::mark_video_complete(
    std::uint16_t frame_index) noexcept {
  video_.mark_complete(frame_index);
}

void CloudStreamMediaAssembly::rewind_video_complete(
    std::uint16_t first_lost) noexcept {
  video_.rewind_complete(first_lost);
}

auto CloudStreamMediaAssembly::last_video_complete() const noexcept
    -> std::uint16_t {
  return video_.last_complete();
}

auto CloudStreamMediaAssembly::last_video_frame() const noexcept
    -> std::optional<std::uint16_t> {
  return video_.last_frame_index();
}

auto CloudStreamMediaAssembly::video_frame_stale(
    std::uint16_t frame_index) const noexcept -> bool {
  return video_.stale(frame_index);
}

auto CloudStreamMediaAssembly::take_congestion(double packet_loss_max) noexcept
    -> takion::CongestionValues {
  return stats_.take(packet_loss_max);
}

void CloudStreamMediaAssembly::on_stream_info() noexcept {
  console_audio_.reset();
  console_voice_.reset();
  for (auto& lane : cloud_audio_units_) lane.reset();
  for (auto& lane : cloud_audio_jitter_) lane.reset();
}

void CloudStreamMediaAssembly::reset() noexcept {
  video_.reset();
  fec_repaired_frames_ = 0;
  stats_.reset();
  audio_.reset();
  for (auto& lane : cloud_audio_units_) lane.reset();
  for (auto& lane : cloud_audio_jitter_) lane.reset();
  cloud_haptics_.reset();
  console_audio_.reset();
  console_voice_.reset();
  console_haptics_.reset();
}

}  // namespace libchiaking::net::protocol::remote_play
