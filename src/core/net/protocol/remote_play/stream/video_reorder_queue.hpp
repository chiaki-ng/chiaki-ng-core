/// @file
/// @brief Declares VideoReorderQueue, which puts the video units of a stream
/// back into packet order before they are assembled.
#pragma once

#include "src/core/net/protocol/remote_play/stream/limits.hpp"
#include "src/core/net/protocol/takion/detail/packet_codec.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace libchiaking::net::protocol::remote_play {

/// @brief Restores the packet order of video units that arrive out of order.
///
/// Units are released in order of their 16-bit packet index (which wraps).
/// When the next unit is missing the queue waits up to head_timeout_us, then
/// skips the gap. Units older than the window, duplicates, and units pushed
/// out by a window wider than capacity are dropped and counted. The first
/// unit starts the window at the first packet of its frame. Not thread-safe.
class VideoReorderQueue {
 public:
  /// @brief The widest window of packet indexes held.
  static constexpr std::size_t capacity = queue_capacity;
  /// @brief How long a gap at the head is waited for, in microseconds (16 ms).
  static constexpr std::uint64_t head_timeout_us = 16'000;

  /// @brief Adds a unit and releases what is now in order.
  ///
  /// @param unit The unit.
  /// @param now_us Monotonic microseconds now.
  /// @param ready Where released units are appended, in order.
  void push(takion::AvPacketValues unit, std::uint64_t now_us,
            std::vector<takion::AvPacketValues>& ready);

  /// @brief Releases what is in order, skipping a gap that has waited too
  /// long.
  ///
  /// @param now_us Monotonic microseconds now.
  /// @param ready Where released units are appended, in order.
  void flush(std::uint64_t now_us, std::vector<takion::AvPacketValues>& ready);

  /// @brief Whether units are held.
  ///
  /// @return True when any unit waits.
  auto waiting() const noexcept -> bool;
  /// @brief When the head gap should be skipped.
  ///
  /// @return Monotonic microseconds just past the timeout, or nothing when no
  ///     gap is being waited for.
  auto next_deadline_us() const noexcept -> std::optional<std::uint64_t>;
  /// @brief Packet indexes skipped over gaps.
  ///
  /// @return The count.
  auto skipped() const noexcept -> std::uint64_t { return skipped_; }
  /// @brief Units dropped as late, repeated or pushed out.
  ///
  /// @return The count.
  auto dropped() const noexcept -> std::uint64_t { return dropped_; }

  /// @brief Forgets everything except the counts.
  void reset() noexcept;

 private:
  /// @brief The slot of a packet index.
  ///
  /// @param sequence The index.
  /// @return The slot, which may hold another index modulo capacity.
  auto slot(std::uint16_t sequence) noexcept
      -> std::optional<takion::AvPacketValues>&;

  /// @brief Places a unit in the window, moving the window when needed.
  ///
  /// @param sequence The unit's packet index.
  /// @param unit The unit.
  void insert(std::uint16_t sequence, takion::AvPacketValues unit);

  /// @brief The held units, by packet index modulo capacity.
  std::array<std::optional<takion::AvPacketValues>, capacity> slots_{};
  /// @brief Whether the window has started.
  bool initialized_{};
  /// @brief The packet index at the window's head.
  std::uint16_t begin_{};
  /// @brief How many indexes the window spans.
  std::size_t count_{};
  /// @brief When waiting for the head began, in monotonic microseconds.
  std::optional<std::uint64_t> head_wait_start_us_;
  /// @brief The head index being waited for.
  std::uint16_t head_wait_sequence_{};
  /// @brief Indexes skipped.
  std::uint64_t skipped_{};
  /// @brief Units dropped.
  std::uint64_t dropped_{};
};

}  // namespace libchiaking::net::protocol::remote_play
