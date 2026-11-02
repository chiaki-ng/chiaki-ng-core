#include "src/core/net/protocol/remote_play/stream/video_reorder_queue.hpp"

#include "src/core/utils/numeric/sequence_number.hpp"

#include <utility>

namespace libchiaking::net::protocol::remote_play {

void VideoReorderQueue::push(takion::AvPacketValues unit, std::uint64_t now_us,
                             std::vector<takion::AvPacketValues>& ready) {
  const auto sequence = unit.packet_index;
  if (!initialized_) {
    begin_ = static_cast<std::uint16_t>(sequence - unit.unit_index);
    count_ = 0;
    head_wait_start_us_.reset();
    head_wait_sequence_ = begin_;
    initialized_ = true;
  }
  insert(sequence, std::move(unit));
  flush(now_us, ready);
}

void VideoReorderQueue::flush(std::uint64_t now_us,
                              std::vector<takion::AvPacketValues>& ready) {
  bool progressed = true;
  while (progressed) {
    progressed = false;
    while (count_ != 0 && slot(begin_)) {
      ready.push_back(std::move(*slot(begin_)));
      slot(begin_).reset();
      ++begin_;
      --count_;
      progressed = true;
    }
    if (progressed) head_wait_start_us_.reset();
    if (count_ == 0) break;
    if (head_wait_start_us_ && begin_ != head_wait_sequence_) {
      if (numeric::SequenceNumber{begin_}.is_newer_than(
              numeric::SequenceNumber{head_wait_sequence_})) {
        head_wait_sequence_ = begin_;
      } else {
        head_wait_start_us_ = now_us;
        head_wait_sequence_ = begin_;
        break;
      }
    }
    if (!head_wait_start_us_) {
      head_wait_start_us_ = now_us;
      head_wait_sequence_ = begin_;
      break;
    }
    if (now_us < *head_wait_start_us_ ||
        now_us - *head_wait_start_us_ <= head_timeout_us)
      break;
    std::size_t skipped = 0;
    while (skipped < count_ &&
           !slot(static_cast<std::uint16_t>(begin_ + skipped)))
      ++skipped;
    if (skipped >= count_) break;
    begin_ = static_cast<std::uint16_t>(begin_ + skipped);
    count_ -= skipped;
    skipped_ += skipped;
    head_wait_start_us_.reset();
    progressed = true;
  }
}

auto VideoReorderQueue::waiting() const noexcept -> bool { return count_ != 0; }

auto VideoReorderQueue::next_deadline_us() const noexcept
    -> std::optional<std::uint64_t> {
  if (count_ == 0 || !head_wait_start_us_) return std::nullopt;
  return *head_wait_start_us_ + head_timeout_us + 1;
}

void VideoReorderQueue::reset() noexcept {
  for (auto& entry : slots_) entry.reset();
  initialized_ = false;
  begin_ = 0;
  count_ = 0;
  head_wait_start_us_.reset();
  head_wait_sequence_ = 0;
}

auto VideoReorderQueue::slot(std::uint16_t sequence) noexcept
    -> std::optional<takion::AvPacketValues>& {
  return slots_[sequence % capacity];
}

void VideoReorderQueue::insert(std::uint16_t sequence,
                               takion::AvPacketValues unit) {
  const auto end = static_cast<std::uint16_t>(begin_ + count_);
  const auto offset = static_cast<std::uint16_t>(sequence - begin_);
  if (offset < count_) {
    if (slot(sequence)) {
      ++dropped_;
      return;
    }
    slot(sequence) = std::move(unit);
    return;
  }
  if (numeric::SequenceNumber{begin_}.is_newer_than(
          numeric::SequenceNumber{sequence})) {
    ++dropped_;
    return;
  }
  if (sequence != end && !numeric::SequenceNumber{sequence}.is_newer_than(
                             numeric::SequenceNumber{end})) {
    if (count_ != 0) {
      ++dropped_;
      return;
    }
    begin_ = sequence;
  }
  const auto distance =
      static_cast<std::size_t>(static_cast<std::uint16_t>(sequence - begin_)) +
      1U;
  if (distance > capacity) {
    const auto required = distance - capacity;
    for (std::size_t index = 0; index < required && count_ != 0; ++index) {
      if (slot(begin_)) {
        slot(begin_).reset();
        ++dropped_;
      }
      ++begin_;
      --count_;
    }
    if (count_ == 0) begin_ = sequence;
  }
  const auto new_count =
      static_cast<std::size_t>(static_cast<std::uint16_t>(sequence - begin_)) +
      1U;
  for (auto index = count_; index < new_count; ++index)
    slot(static_cast<std::uint16_t>(begin_ + index)).reset();
  count_ = new_count;
  slot(sequence) = std::move(unit);
}

}  // namespace libchiaking::net::protocol::remote_play
