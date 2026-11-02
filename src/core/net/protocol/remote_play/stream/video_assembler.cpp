#include "src/core/net/protocol/remote_play/stream/video_assembler.hpp"
#include "src/core/utils/bytes/endian.hpp"

#include "src/core/net/protocol/remote_play/stream/cauchy_fec8.hpp"
#include "src/core/utils/numeric/sequence_number.hpp"

#include <algorithm>
#include <span>
#include <utility>

namespace libchiaking::net::protocol::remote_play {

auto VideoAssembler::push(takion::AvPacketValues unit,
                          std::uint32_t expected_blocks)
    -> Expected<std::optional<AssembledVideoFrame>, VideoAssemblerError> {
  if (unit.type.family() != takion::PacketFamily::video)
    return unexpected(VideoAssemblerError::invalid_family);
  if (unit.data.empty() || unit.unit_index >= max_units)
    return unexpected(VideoAssemblerError::invalid_unit);
  const auto total = unit.units_in_frame_total;
  const auto fec = unit.units_in_frame_fec;
  const auto index = static_cast<std::size_t>(unit.unit_index);
  const bool counts_sane = total != 0 && total <= max_units && fec < total;
  const bool header_sane = counts_sane && index < total;
  if (unit.video_word == 0 && !header_sane)
    return unexpected(counts_sane ? VideoAssemblerError::invalid_unit
                                  : VideoAssemblerError::invalid_count);
  const bool frame_continues =
      active_ && last_frame_index_ && unit.frame_index == *last_frame_index_;
  const bool source =
      !header_sane || (frame_continues && source_count_
                           ? index < *source_count_
                           : index < static_cast<std::size_t>(total - fec));
  if (source && unit.data.size() < 2)
    return unexpected(VideoAssemblerError::short_source);

  if (unit.encrypted || stale(unit.frame_index))
    return unexpected(VideoAssemblerError::stale_frame);
  if (last_frame_index_) {
    if (active_ && unit.frame_index != *last_frame_index_) {
      report_failed_frame();
      clear_active();
    }
  }
  if (!active_) {
    const auto expected = static_cast<std::uint16_t>(last_complete_ + 1U);
    if (numeric::SequenceNumber{unit.frame_index}.is_newer_than(
            numeric::SequenceNumber{expected}))
      push_loss(VideoFrameLoss{
          expected, static_cast<std::uint16_t>(unit.frame_index - 1U), false});
    start(unit, expected_blocks);
  }

  if (!source_count_ && header_sane) {
    source_count_ = static_cast<std::size_t>(total - fec);
    parity_count_ = fec;
    count_sources_in_place();
  }
  if (index >= slots_.size()) slots_.resize(index + 1);
  if (slots_[index])
    return reject_unit(unit, VideoAssemblerError::duplicate_unit);
  if (unit.data.size() > max_frame_bytes - stored_bytes_)
    return reject_unit(unit, VideoAssemblerError::frame_too_large);
  adaptive_stream_index_ = unit.adaptive_stream_index;
  stored_bytes_ += unit.data.size();
  slots_[index] =
      Slot{std::move(unit.data), unit.video_word, source, false, unit.slice};
  dynamic_range_flag_ = dynamic_range_flag_ || unit.dynamic_range_flag;
  suppress_presentation_ = suppress_presentation_ || unit.suppress_presentation;
  if (source) {
    ++source_received_;
    if (source_count_ && index < *source_count_) ++sources_in_place_;
    if (!header_sane) counts_ignored_ = true;
    if (unit.video_word != 0) account_coverage(index);
  } else {
    ++parity_received_;
  }

  if (source_count_) {
    if (sources_complete())
      return std::optional<AssembledVideoFrame>{concatenate()};
    if (parity_received_ &&
        source_received_ + parity_received_ >= *source_count_) {
      if (*source_count_ + parity_count_ > video_max_total_units) {
        report_failed_frame();
        clear_active();
        return std::optional<AssembledVideoFrame>{};
      }
      auto recovered = try_recover();
      if (!recovered) {
        report_failed_frame();
        clear_active();
        return unexpected(recovered.error());
      }
      clear_active();
      return std::optional<AssembledVideoFrame>{std::move(recovered.value())};
    }
    if (!source && index + 1 == static_cast<std::size_t>(total)) {
      report_failed_frame();
      clear_active();
    }
    return std::optional<AssembledVideoFrame>{};
  }
  if (expected_blocks_ != 0 && coverage_ == expected_blocks_) {
    source_count_ = source_received_;
    return std::optional<AssembledVideoFrame>{concatenate()};
  }
  return std::optional<AssembledVideoFrame>{};
}

auto VideoAssembler::stale(std::uint16_t frame_index) const noexcept -> bool {
  if (!last_frame_index_) return false;
  if (frame_index == *last_frame_index_) return !active_;
  return !numeric::SequenceNumber{frame_index}.is_newer_than(
      numeric::SequenceNumber{*last_frame_index_});
}

auto VideoAssembler::sources_complete() const noexcept -> bool {
  return source_count_ && sources_in_place_ == *source_count_;
}

void VideoAssembler::count_sources_in_place() noexcept {
  sources_in_place_ = static_cast<std::size_t>(std::ranges::count_if(
      std::span{slots_}.first(std::min(slots_.size(), *source_count_)),
      [](const std::optional<Slot>& slot) { return slot && slot->source; }));
}

void VideoAssembler::account_coverage(std::size_t index) {
  auto& slot = *slots_[index];
  if (!slot.slice) {
    coverage_ += slot.word;
    slot.counted = true;
    return;
  }
  const auto same_slice = [&](std::size_t candidate) {
    return candidate < slots_.size() && slots_[candidate] &&
           slots_[candidate]->source && slots_[candidate]->slice &&
           !slots_[candidate]->counted &&
           slots_[candidate]->slice->index == slot.slice->index;
  };
  std::size_t first = index;
  while (!slots_[first]->slice->start) {
    if (first == 0 || !same_slice(first - 1)) return;
    --first;
  }
  std::size_t last = index;
  while (!slots_[last]->slice->end) {
    if (!same_slice(last + 1)) return;
    ++last;
  }
  coverage_ += slot.word;
  for (std::size_t member = first; member <= last; ++member)
    slots_[member]->counted = true;
}

auto VideoAssembler::concatenate() -> AssembledVideoFrame {
  AssembledVideoFrame frame;
  frame.frame_index = *last_frame_index_;
  frame.codec = codec_;
  frame.adaptive_stream_index = adaptive_stream_index_;
  frame.units_total = static_cast<std::uint16_t>(
      source_count_.value_or(source_received_) + parity_count_);
  frame.units_fec = static_cast<std::uint16_t>(parity_count_);
  frame.units_received =
      static_cast<std::uint16_t>(source_received_ + parity_received_);
  frame.counts_ignored = counts_ignored_;
  frame.dynamic_range_flag = dynamic_range_flag_;
  frame.suppress_presentation = suppress_presentation_;
  frame.bytes.reserve(stored_bytes_ + buffer_padding);
  for (const auto& slot : slots_) {
    if (!slot || !slot->source) continue;
    frame.bytes.insert(frame.bytes.end(), slot->bytes.begin() + 2,
                       slot->bytes.end());
  }
  clear_active();
  return frame;
}

auto VideoAssembler::take_loss() noexcept -> std::optional<VideoFrameLoss> {
  if (losses_.empty()) return std::nullopt;
  const auto loss = losses_.front();
  losses_.pop_front();
  return loss;
}

void VideoAssembler::mark_complete(std::uint16_t frame_index) noexcept {
  last_complete_ = frame_index;
}

void VideoAssembler::rewind_complete(std::uint16_t first_lost) noexcept {
  const auto previous = static_cast<std::uint16_t>(first_lost - 1U);
  if (numeric::SequenceNumber{last_complete_}.is_newer_than(
          numeric::SequenceNumber{previous}))
    last_complete_ = previous;
}

void VideoAssembler::reset() noexcept {
  clear_active();
  last_frame_index_.reset();
  last_complete_ = 0;
  losses_.clear();
}

auto VideoAssembler::try_recover()
    -> Expected<AssembledVideoFrame, VideoAssemblerError> {
  const auto source_count = *source_count_;
  const auto total = source_count + parity_count_;
  std::optional<std::size_t> unit_size;
  for (std::size_t index = source_count; index < total && index < slots_.size();
       ++index) {
    if (!slots_[index] || slots_[index]->source) continue;
    if (unit_size && *unit_size != slots_[index]->bytes.size())
      return unexpected(VideoAssemblerError::invalid_unit);
    unit_size = slots_[index]->bytes.size();
  }
  if (!unit_size || *unit_size < 2)
    return unexpected(VideoAssemblerError::invalid_unit);
  if (source_count > max_frame_bytes / *unit_size)
    return unexpected(VideoAssemblerError::frame_too_large);

  std::vector<std::optional<std::vector<std::byte>>> padded(total);
  for (std::size_t index = 0; index < total && index < slots_.size(); ++index) {
    if (!slots_[index]) continue;
    if (index < source_count && !slots_[index]->source) continue;
    if (index >= source_count && slots_[index]->source) continue;
    padded[index] = std::move(slots_[index]->bytes);
    if (index >= source_count) continue;
    auto& bytes = *padded[index];
    if (bytes.size() > *unit_size ||
        bytes::load_be<std::uint16_t>(bytes) != *unit_size - bytes.size())
      return unexpected(VideoAssemblerError::invalid_unit);
    bytes.resize(*unit_size, std::byte{0});
  }
  AssembledVideoFrame frame;
  for (std::size_t index = 0; index < source_count; ++index)
    if (!padded[index])
      frame.restored_units.push_back(static_cast<std::uint16_t>(index));
  auto recovered =
      decoder_.recover(std::move(padded), source_count, parity_count_);
  if (!recovered) return unexpected(VideoAssemblerError::invalid_unit);

  frame.frame_index = *last_frame_index_;
  frame.codec = codec_;
  frame.adaptive_stream_index = adaptive_stream_index_;
  frame.units_total = static_cast<std::uint16_t>(total);
  frame.units_fec = static_cast<std::uint16_t>(parity_count_);
  frame.units_received =
      static_cast<std::uint16_t>(source_received_ + parity_received_);
  frame.counts_ignored = counts_ignored_;
  frame.dynamic_range_flag = dynamic_range_flag_;
  frame.suppress_presentation = suppress_presentation_;
  frame.bytes.reserve(source_count * *unit_size + buffer_padding);
  for (const auto& bytes : recovered.value()) {
    if (bytes.size() != *unit_size)
      return unexpected(VideoAssemblerError::invalid_unit);
    const auto padding = bytes::load_be<std::uint16_t>(bytes);
    if (padding > *unit_size - 2)
      return unexpected(VideoAssemblerError::invalid_unit);
    const auto data_size = *unit_size - padding;
    frame.bytes.insert(frame.bytes.end(), bytes.begin() + 2,
                       bytes.begin() + static_cast<std::ptrdiff_t>(data_size));
  }
  return frame;
}

auto VideoAssembler::reject_unit(const takion::AvPacketValues& unit,
                                 VideoAssemblerError error)
    -> Expected<std::optional<AssembledVideoFrame>, VideoAssemblerError> {
  if (unit.video_word == 0 &&
      unit.unit_index + 1U == unit.units_in_frame_total) {
    report_failed_frame();
    clear_active();
  }
  return unexpected(error);
}

void VideoAssembler::report_failed_frame() noexcept {
  push_loss(VideoFrameLoss{static_cast<std::uint16_t>(last_complete_ + 1U),
                           *last_frame_index_, true});
}

void VideoAssembler::push_loss(VideoFrameLoss loss) {
  if (losses_.size() == max_pending_losses) losses_.pop_front();
  losses_.push_back(loss);
}

void VideoAssembler::start(const takion::AvPacketValues& unit,
                           std::uint32_t expected_blocks) {
  active_ = true;
  last_frame_index_ = unit.frame_index;
  codec_ = unit.codec;
  adaptive_stream_index_ = unit.adaptive_stream_index;
  slots_.clear();
  source_received_ = 0;
  parity_received_ = 0;
  stored_bytes_ = 0;
  source_count_.reset();
  sources_in_place_ = 0;
  parity_count_ = 0;
  coverage_ = 0;
  expected_blocks_ = expected_blocks;
  counts_ignored_ = false;
  dynamic_range_flag_ = false;
  suppress_presentation_ = false;
}

void VideoAssembler::clear_active() noexcept {
  active_ = false;
  slots_.clear();
  source_received_ = 0;
  parity_received_ = 0;
  stored_bytes_ = 0;
  source_count_.reset();
  sources_in_place_ = 0;
  parity_count_ = 0;
  coverage_ = 0;
  counts_ignored_ = false;
  dynamic_range_flag_ = false;
  suppress_presentation_ = false;
}

}  // namespace libchiaking::net::protocol::remote_play
