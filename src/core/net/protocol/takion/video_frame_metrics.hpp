/// @file
/// @brief Declares the video frame metrics a session derives from the
/// extended headers of the console's video packets, and the worker that
/// decides when to report them, for versions with the video_frame_metrics
/// feature.
#pragma once

#include "src/core/net/protocol/takion/ext_message_codec.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace libchiaking::net::protocol::takion {

/// @brief Frame interval assumed before the first measurement, in
/// microseconds (one 60 fps frame).
inline constexpr std::uint32_t video_frame_report_initial_send_interval_us =
    16666;
/// @brief Report timeout base before the first consecutive frame is measured,
/// in microseconds (500 ms).
inline constexpr std::uint32_t video_frame_report_initial_baseline_us = 500000;
/// @brief Length of a throughput window, in microseconds (200 ms).
inline constexpr std::uint64_t video_frame_throughput_window_us = 200000;
/// @brief Longest loss window, in microseconds (100 ms).
inline constexpr std::uint64_t video_frame_loss_window_us = 100000;
/// @brief Packets after which a loss window closes early.
inline constexpr std::int64_t video_frame_loss_window_packets = 100;
/// @brief Fewest units, expected and received, for an incomplete frame's
/// timing to be estimated.
inline constexpr std::uint16_t video_frame_estimate_minimum_units = 3;
/// @brief Smallest received fraction of a frame for its timing to be
/// estimated.
inline constexpr double video_frame_estimate_minimum_ratio = 0.8;

/// @brief Unwraps a counter that wraps at `Modulus` into a wider count.
///
/// A wrap is recognised when the counter falls into the lowest 1/16 of its
/// range just after being in the highest 1/16; until the counter passes
/// the middle again (1/2 to 10/16), late values from the top 1/16 are kept in
/// the previous epoch. A plain value.
///
/// @tparam Wide The unwrapped integer type.
/// @tparam Modulus The counter's range.
template <class Wide, std::uint64_t Modulus>
class ThresholdLatchExpander {
 public:
  /// @brief Starts at epoch 0.
  constexpr ThresholdLatchExpander() noexcept = default;
  /// @brief Starts from a saved state.
  ///
  /// @param previous The last counter value seen.
  /// @param epoch The current epoch's base.
  /// @param latch Whether a wrap is still being settled.
  constexpr ThresholdLatchExpander(std::uint64_t previous, Wide epoch,
                                   bool latch) noexcept
      : previous_(previous), epoch_(epoch), latch_(latch) {}

  /// @brief Unwraps one counter value.
  ///
  /// @param current The counter value.
  /// @return The unwrapped value.
  constexpr Wide expand(std::uint64_t current) noexcept {
    if (current <= low_bound && previous_ >= high_bound) {
      if (!latch_)
        epoch_ = static_cast<Wide>(epoch_ + static_cast<Wide>(Modulus));
      latch_ = true;
    } else if (current >= reset_low && current <= reset_high) {
      latch_ = false;
    }
    auto expanded = static_cast<Wide>(epoch_ + static_cast<Wide>(current));
    if (latch_ && current >= high_bound)
      expanded = static_cast<Wide>(expanded - static_cast<Wide>(Modulus));
    previous_ = current;
    return expanded;
  }

  /// @brief Unwraps a value in the current epoch, without the latch's
  /// correction or any state change.
  ///
  /// @param current The counter value.
  /// @return epoch + value.
  constexpr Wide unlatched(std::uint64_t current) const noexcept {
    return static_cast<Wide>(epoch_ + static_cast<Wide>(current));
  }

  /// @brief Reads the last value seen.
  ///
  /// @return The value.
  constexpr std::uint64_t previous() const noexcept { return previous_; }
  /// @brief Reads the epoch base.
  ///
  /// @return The base.
  constexpr Wide epoch() const noexcept { return epoch_; }
  /// @brief Tells whether a wrap is being settled.
  ///
  /// @return Whether it is.
  constexpr bool latched() const noexcept { return latch_; }

 private:
  /// @brief Counter values treated as just after a wrap.
  static constexpr std::uint64_t low_bound = Modulus / 16;
  /// @brief Counter values treated as just before a wrap.
  static constexpr std::uint64_t high_bound = 15 * (Modulus / 16);
  /// @brief Start of the range that ends a wrap's settling.
  static constexpr std::uint64_t reset_low = Modulus / 2;
  /// @brief End of the range that ends a wrap's settling.
  static constexpr std::uint64_t reset_high = 10 * (Modulus / 16);

  /// @brief The last value seen.
  std::uint64_t previous_{};
  /// @brief The epoch base.
  Wide epoch_{};
  /// @brief Whether a wrap is being settled.
  bool latch_{};
};

/// @brief Unwraps 16-bit frame indices.
using FrameNumberExpander = ThresholdLatchExpander<std::uint32_t, 0x10000>;
/// @brief Unwraps the 32-bit sender timestamps of the extended header.
using SenderTimestampExpander =
    ThresholdLatchExpander<std::uint64_t, 0x100000000>;
/// @brief Unwraps 16-bit packet indices.
using PacketIndexExpander = ThresholdLatchExpander<std::uint64_t, 0x10000>;

/// @brief One received video unit, as the metrics need it.
struct VideoFrameObservation {
  /// @brief When it arrived, in monotonic microseconds.
  std::uint64_t arrival_us{};
  /// @brief Its unit index.
  std::uint16_t unit_index{};
  /// @brief Units in its frame.
  std::uint16_t total_units{};
  /// @brief Its 16-bit frame index.
  std::uint16_t frame_index{};
  /// @brief Its 16-bit packet index.
  std::uint16_t packet_index{};
  /// @brief The extended header's timestamp, in sender microseconds.
  std::uint32_t sender_timestamp_us{};
  /// @brief The extended header's counter.
  std::uint32_t sender_counter{};
  /// @brief Bytes of unit data.
  std::uint32_t payload_size{};
  /// @brief Bytes of the rest of the packet.
  std::uint32_t overhead_size{};
  /// @brief Adaptive stream index.
  std::uint8_t profile{};
  /// @brief FEC units in its frame.
  std::uint16_t fec_units{};
  /// @brief Its slice index, from the NALU descriptor; 0 without one.
  std::uint16_t slice_index{};
  /// @brief Slices in its frame; 0 without a descriptor.
  std::uint16_t slice_count{};
};

/// @brief The latest video frame figures, as reported. Times are
/// microseconds.
struct VideoFrameSnapshot {
  /// @brief Sender time between the starts of the last two frames.
  std::int32_t send_int_us{
      static_cast<std::int32_t>(video_frame_report_initial_send_interval_us)};
  /// @brief Arrival time between the first units of the last two frames.
  std::int32_t recv_int_us{};
  /// @brief Arrival-to-complete time of the last timed frame, less slice gaps.
  std::int32_t transfer_time_us{};
  /// @brief Bytes of the last timed frame.
  std::uint32_t recv_size{};
  /// @brief Unwrapped number of the latest frame.
  std::uint32_t frame_no_wide{};
  /// @brief Frame index of the last timed frame.
  std::uint16_t frame_no_at_tt_updated{};
  /// @brief Whether the last timing was estimated.
  bool is_trans_time_estimated{};
  /// @brief Whether the latest frame directly followed the one before.
  bool is_consecutive_frame_no{};
  /// @brief Sender time across the last timed frame's units, less slice gaps.
  std::int32_t transmission_time_us{};
  /// @brief Throughput, in bits per second.
  std::uint32_t throughput_bps{};
  /// @brief Packet loss, a fraction.
  double loss_ratio{};
  /// @brief Arrival of the latest frame's first unit, in monotonic
  /// microseconds.
  std::uint64_t timestamp_us{};

  /// @brief Compares every field.
  ///
  /// @return Whether both snapshots are the same.
  bool operator==(const VideoFrameSnapshot&) const = default;
};

/// @brief Throughput and loss over rolling windows of video packets.
class MetricWindows {
 public:
  /// @brief Counts one packet.
  ///
  /// Throughput is recomputed every 200 ms as bits over the elapsed time; loss
  /// every 100 ms or 100 packets as the share of packet indices skipped.
  ///
  /// @param now_us When it arrived, in monotonic microseconds.
  /// @param expanded_sequence Its unwrapped packet index.
  /// @param bytes Its size.
  void receive(std::uint64_t now_us, std::uint64_t expanded_sequence,
               std::uint64_t bytes) noexcept;

  /// @brief Reads the latest throughput.
  ///
  /// @return Bits per second.
  std::uint64_t throughput_bps() const noexcept { return throughput_bps_; }
  /// @brief Reads the latest loss.
  ///
  /// @return A fraction.
  double loss_ratio() const noexcept { return loss_ratio_; }
  /// @brief Reads the bytes counted in the current throughput window.
  ///
  /// @return Bytes.
  std::uint64_t pending_bytes() const noexcept { return byte_count_; }
  /// @brief Reads the packets counted in the current loss window.
  ///
  /// @return The count.
  std::uint32_t pending_received() const noexcept { return received_count_; }
  /// @brief Tells whether the last packet closed a loss window.
  ///
  /// @return Whether it did.
  bool loss_updated() const noexcept { return loss_updated_; }

 private:
  /// @brief Latest throughput, in bits per second.
  std::uint64_t throughput_bps_{};
  /// @brief Latest loss, a fraction.
  double loss_ratio_{};
  /// @brief Bytes in the current throughput window.
  std::uint64_t byte_count_{};
  /// @brief Packets in the current loss window.
  std::uint32_t received_count_{};
  /// @brief When the current throughput window began.
  std::uint64_t throughput_start_us_{};
  /// @brief When the current loss window began.
  std::uint64_t loss_start_us_{};
  /// @brief Unwrapped packet index at the start of the loss window.
  std::uint64_t previous_sequence_{};
  /// @brief Whether a packet has been counted.
  bool started_{};
  /// @brief Whether the last packet closed a loss window.
  bool loss_updated_{};
};

/// @brief Follows the console's video frames unit by unit and keeps the
/// figures the video frame metrics report carries.
///
/// Two frames are tracked at once. A frame is timed when its unit indices add
/// up to a complete frame; when a newer frame displaces an unfinished one,
/// its timing is estimated if at least 80% of at least 3 units arrived. Owned
/// by the client's inbound path and used under the client's lock.
class VideoFrameMetricsCollector {
 public:
  /// @brief Takes one received video unit.
  ///
  /// @param observation The unit.
  void observe(const VideoFrameObservation& observation);

  /// @brief Reads the latest figures.
  ///
  /// @return The snapshot.
  const VideoFrameSnapshot& snapshot() const noexcept { return snapshot_; }
  /// @brief Tells whether a new frame started since take_signal() last
  /// cleared it, leaving it set.
  ///
  /// @return Whether one did.
  bool signalled() const noexcept { return signalled_; }
  /// @brief Tells whether a new frame started since last asked, and clears it.
  ///
  /// @return Whether one did.
  [[nodiscard]] bool take_signal() noexcept {
    const bool signalled = signalled_;
    signalled_ = false;
    return signalled;
  }
  /// @brief Reads the highest frame finished with.
  ///
  /// @return The unwrapped frame number.
  std::uint32_t finalized_frame() const noexcept { return finalized_; }
  /// @brief Reads which frame a tracking slot holds.
  ///
  /// @param slot 0 or 1.
  /// @return The unwrapped frame number; 0 when empty.
  std::uint32_t slot_frame(std::size_t slot) const noexcept {
    return slots_[slot].frame;
  }
  /// @brief Reads the throughput and loss windows.
  ///
  /// @return The windows.
  const MetricWindows& windows() const noexcept { return windows_; }

 private:
  /// @brief One frame being tracked.
  struct Record {
    /// @brief Unwrapped frame number; 0 for an empty slot.
    std::uint32_t frame{};
    /// @brief The frame's 16-bit index.
    std::uint16_t wire_frame{};
    /// @brief When its first unit arrived.
    std::uint64_t first_arrival_us{};
    /// @brief When its latest unit arrived.
    std::uint64_t last_arrival_us{};
    /// @brief Bytes received, headers included.
    std::uint32_t total_bytes{};
    /// @brief Units expected.
    std::uint16_t expected_units{};
    /// @brief Units received.
    std::uint16_t received_units{};
    /// @brief Unit data bytes received.
    std::uint32_t payload_bytes{};
    /// @brief Adaptive stream index.
    std::uint8_t profile{};
    /// @brief Earliest sender timestamp, unwrapped.
    std::uint64_t first_sender_us{};
    /// @brief Latest sender timestamp, unwrapped.
    std::uint64_t last_sender_us{};
    /// @brief Sum of the unit indices received; reaches n(n-1)/2 when all n
    /// arrived.
    std::uint32_t index_sum{};
    /// @brief Most FEC units seen.
    std::uint16_t fec_units{};
    /// @brief Slices in the frame.
    std::uint32_t slice_count{};
    /// @brief Highest slice index seen.
    std::uint32_t slice_index{};
    /// @brief Latest sender timestamp within that slice.
    std::uint64_t slice_last_sender_us{};
    /// @brief Sender time between slices, excluded from the timings.
    std::int32_t excluded_gap_us{};
    /// @brief Lowest unwrapped packet index.
    std::uint64_t first_packet{};
    /// @brief Highest unwrapped packet index.
    std::uint64_t last_packet{};
  };

  /// @brief Starts tracking a frame from its first unit, and signals a new
  /// frame.
  ///
  /// @param record The slot.
  /// @param observation The unit.
  /// @param frame Its unwrapped frame number.
  /// @param sender Its unwrapped sender timestamp.
  /// @param packet Its unwrapped packet index.
  void initialize(Record& record, const VideoFrameObservation& observation,
                  std::uint32_t frame, std::uint64_t sender,
                  std::uint64_t packet) noexcept;
  /// @brief Updates the figures when a frame is complete, and on a new frame
  /// the frame intervals.
  ///
  /// @param record The frame.
  /// @param now_us The current time, in monotonic microseconds.
  /// @param new_frame Whether this is the frame's first unit.
  /// @return Whether the frame is complete.
  [[nodiscard]] bool finalize(Record& record, std::uint64_t now_us,
                              bool new_frame) noexcept;
  /// @brief Estimates an incomplete frame's timing as it is displaced.
  ///
  /// @param record The frame.
  /// @return Whether an estimate was made.
  [[nodiscard]] bool estimate(Record& record) noexcept;
  /// @brief Marks frames up to one as finished and frees their slots.
  ///
  /// @param frame The unwrapped frame number.
  void retire(std::uint32_t frame) noexcept;

  /// @brief The two frames being tracked.
  std::array<Record, 2> slots_{};
  /// @brief Highest frame finished with.
  std::uint32_t finalized_{};
  /// @brief The latest figures.
  VideoFrameSnapshot snapshot_{};
  /// @brief Whether a first frame set the baseline.
  bool baseline_{};
  /// @brief Frame the intervals are measured from.
  std::uint32_t baseline_frame_{};
  /// @brief Its first sender timestamp.
  std::uint64_t baseline_sender_us_{};
  /// @brief Its first arrival.
  std::uint64_t baseline_arrival_us_{};
  /// @brief Unwraps frame indices.
  FrameNumberExpander frames_;
  /// @brief Unwraps sender timestamps.
  SenderTimestampExpander timestamps_;
  /// @brief Unwraps packet indices.
  PacketIndexExpander packets_;
  /// @brief Throughput and loss.
  MetricWindows windows_;
  /// @brief Whether a new frame started since last asked.
  bool signalled_{};
};

/// @brief Decides when to send a video frame metrics report and numbers them.
///
/// A report is due when a new frame starts or when 1.25 times the baseline
/// frame interval has passed since the last one; the baseline starts at
/// 500 ms and becomes the measured sender interval whenever consecutive
/// frames are seen. Owned by the client; used under its lock.
class VideoFrameReportWorker {
 public:
  /// @brief Starts with the first deadline one timeout from now.
  ///
  /// @param now_us The current time, in monotonic microseconds.
  explicit VideoFrameReportWorker(std::uint64_t now_us) noexcept
      : deadline_us_(now_us + timeout_us()) {}

  /// @brief Reads the report timeout.
  ///
  /// @return 1.25 x the baseline, in microseconds.
  std::uint32_t timeout_us() const noexcept {
    return static_cast<std::uint32_t>(baseline_us_ + (baseline_us_ >> 2U));
  }
  /// @brief Reads when a report is next due without a new frame.
  ///
  /// @return Monotonic microseconds.
  std::uint64_t deadline_us() const noexcept { return deadline_us_; }
  /// @brief Reads the next report number.
  ///
  /// @return The number.
  std::uint32_t sequence() const noexcept { return sequence_; }
  /// @brief Reads the baseline frame interval.
  ///
  /// @return Microseconds.
  std::uint32_t baseline_us() const noexcept { return baseline_us_; }

  /// @brief Tells whether a report is due.
  ///
  /// @param now_us The current time, in monotonic microseconds.
  /// @param signalled Whether a new frame started.
  /// @return Whether one is.
  bool due(std::uint64_t now_us, bool signalled) const noexcept {
    return signalled || now_us >= deadline_us_;
  }

  /// @brief Builds the report from the latest figures.
  ///
  /// @param snapshot The figures.
  /// @param now_us The current time, in monotonic microseconds.
  /// @return The report.
  VideoFrameMetricsReport report(const VideoFrameSnapshot& snapshot,
                                 std::uint64_t now_us) const noexcept;

  /// @brief Records that a report was sent: numbers the next, updates the
  /// baseline and moves the deadline.
  ///
  /// @param snapshot The figures reported.
  /// @param now_us The current time, in monotonic microseconds.
  void complete(const VideoFrameSnapshot& snapshot,
                std::uint64_t now_us) noexcept;

 private:
  /// @brief Baseline frame interval, in microseconds.
  std::uint32_t baseline_us_{video_frame_report_initial_baseline_us};
  /// @brief Next report number.
  std::uint32_t sequence_{};
  /// @brief When a report is next due without a new frame.
  std::uint64_t deadline_us_{};
};

}  // namespace libchiaking::net::protocol::takion
