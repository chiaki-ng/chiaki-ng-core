/// @file
/// @brief Declares the media congestion controller and pacer a Kakusan host
/// runs
/// for each guest.
#pragma once

#include "src/core/net/kakusan/result.hpp"
#include "src/core/net/kakusan/transport_frame.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <vector>

namespace libchiaking::net::kakusan {

/// @brief max_datagram_size in RFC 9002 terms: 1200 bytes.
inline constexpr std::size_t congestion_datagram_size = max_datagram_size;
/// @brief Round-trip estimate before the first sample, in microseconds: 333 ms.
inline constexpr double initial_rtt_us = 333000.0;
/// @brief Timer granularity, in microseconds: 1 ms.
inline constexpr double timer_granularity_us = 1000.0;
/// @brief max_ack_delay: the largest ACK delay credited, in microseconds: 25
/// ms.
inline constexpr double peer_ack_delay_limit_us = 25000.0;
/// @brief A datagram this many below the largest acknowledged is lost.
inline constexpr std::uint64_t packet_threshold = 3;
/// @brief A datagram older than this many round trips is lost: 9/8.
inline constexpr double time_threshold = 9.0 / 8.0;
/// @brief Most unsettled datagrams; nothing more is sent at the limit.
inline constexpr std::size_t max_tracked_packets = 16384;
/// @brief Cap on the probe timeout's doubling exponent.
inline constexpr std::size_t max_pto_backoff_exponent = 30;
/// @brief Multiplier of the RTT variance in the probe timeout.
inline constexpr double pto_variance_multiplier = 4.0;
/// @brief Weight kept of the RTT variance per sample.
inline constexpr double rttvar_retain_weight = 0.75;
/// @brief Weight of the new deviation per sample.
inline constexpr double rttvar_sample_weight = 0.25;
/// @brief Weight kept of the smoothed RTT per sample.
inline constexpr double srtt_retain_weight = 0.875;
/// @brief Weight of the new sample in the smoothed RTT.
inline constexpr double srtt_sample_weight = 0.125;
/// @brief The first RTT variance is the first sample divided by this.
inline constexpr double initial_rttvar_divisor = 2.0;
/// @brief Least delay margin above the floor, in microseconds: 200 ms.
inline constexpr double delay_margin_floor_us = 200000.0;
/// @brief The delay signal clears below the floor plus this fraction of the
/// margin.
inline constexpr double delay_release_fraction = 0.5;
/// @brief Length of a delay-floor interval, in microseconds: 10 seconds.
inline constexpr std::uint64_t delay_floor_window_us = 10000000;
/// @brief Length of a loss interval, in microseconds: 500 ms.
inline constexpr std::uint64_t loss_window_us = 500000;
/// @brief Settled datagrams an interval needs to be judged.
inline constexpr std::uint64_t loss_window_packets = 40;
/// @brief Loss above which the loss signal becomes congested.
inline constexpr std::uint64_t loss_limit_percent = 10;
/// @brief Loss below which the loss signal clears.
inline constexpr std::uint64_t loss_release_percent = 5;

/// @brief Least pacer burst while sending, in bytes.
inline constexpr double pacer_capacity = 2400.0;
/// @brief Pacer burst while idle, in bytes.
inline constexpr double pacer_initial_tokens = 1200.0;
/// @brief Pacer rate before the sender sets one, in bytes per microsecond.
inline constexpr double pacer_initial_rate = 12000.0 / initial_rtt_us;
/// @brief The pacer burst while sending covers this many microseconds at its
/// rate: 10 ms.
inline constexpr double pacer_catch_up_us = 10000.0;

/// @brief What a loss-detection timer firing did.
enum class TimeoutEvent : std::uint8_t {
  /// @brief Nothing was due.
  none,
  /// @brief Datagrams were declared lost by time.
  loss,
  /// @brief The probe timeout fired; one probe must be sent.
  probe,
};

/// @brief Whether a congestion signal says the path can carry video.
enum class PathState : std::uint8_t {
  /// @brief Video is forwarded.
  clear,
  /// @brief Video is shed; audio and probes continue.
  congested,
};

/// @brief What one acknowledgement settled.
struct AckOutcome {
  /// @brief Datagrams acknowledged for the first time.
  std::size_t newly_acked{};
  /// @brief Datagrams it caused to be declared lost.
  std::size_t lost{};
  /// @brief Whether it produced a round-trip sample.
  bool rtt_sampled{};
};

/// @brief Floating-point helpers of the congestion code. Stateless.
class CongestionArithmetic {
 public:
  /// @brief Slack in floating-point comparisons.
  static constexpr double tolerance = 1e-6;
  /// @brief Times at or above this saturate to the largest u64.
  static constexpr double max_time = 1.8e19;

  /// @brief Converts a count or time.
  ///
  /// @param value The value.
  /// @return It as a double.
  static auto as_double(std::uint64_t value) noexcept -> double;
  /// @brief Rounds a time up to a whole microsecond.
  ///
  /// @param value Microseconds.
  /// @return 0 for non-positive or NaN, the largest u64 when too large.
  static auto ceil_time(double value) noexcept -> std::uint64_t;
};

/// @brief Per-spoke loss detection, RTT estimation and path judgement for
/// forwarded media (RCP TRANSPORT section 5).
///
/// Loss and probe timing follow RFC 9002 sections 5 and 6 with the table's
/// constants, but there is no congestion window: the path is judged by two
/// signals. Delay is congested when the smoothed RTT exceeds the floor (the
/// smallest sample in this and the previous 10-second interval) plus the larger
/// of 200 ms and the floor, and clears below the floor plus half that margin.
/// Loss is judged per 500 ms interval of at least 40 settled datagrams:
/// congested above 10 % lost, clear below 5 %. Every datagram, probes included,
/// is tracked by its datagram sequence. Not thread-safe.
class CongestionController {
 public:
  /// @brief The datagram sequence the next send gets.
  ///
  /// @return The number, from 0.
  auto next_packet_number() const noexcept -> std::uint64_t { return next_; }
  /// @brief Whether the sequence space is used up.
  ///
  /// @return True at the largest u64.
  auto exhausted() const noexcept -> bool;
  /// @brief Whether another datagram may be sent.
  ///
  /// @return False when exhausted or 16384 datagrams are unsettled.
  auto can_track() const noexcept -> bool;
  /// @brief Records a datagram sent.
  ///
  /// @param bytes Its UDP payload size, 1 to 1200.
  /// @param probe Whether it answers a pending probe timeout.
  /// @param now_us When it was sent.
  /// @return Its datagram sequence.
  /// @retval Error::protocol Exhausted or the size is out of range.
  [[nodiscard]] auto on_packet_sent(std::size_t bytes, bool probe,
                                    std::uint64_t now_us)
      -> Result<std::uint64_t>;
  /// @brief Settles the datagrams an acknowledgement covers, samples the RTT
  /// when its largest is newly acknowledged, and detects losses.
  ///
  /// @param ack The acknowledgement.
  /// @param now_us When it arrived.
  /// @return What it settled.
  /// @retval Error::protocol It is malformed or names a datagram never sent.
  [[nodiscard]] auto on_ack(const MediaAck& ack, std::uint64_t now_us)
      -> Result<AckOutcome>;
  /// @brief When the loss-detection timer fires.
  ///
  /// @return The time-threshold loss time if armed, otherwise the probe
  ///     timeout; nothing with nothing in flight.
  auto loss_detection_deadline() const noexcept -> std::optional<std::uint64_t>;
  /// @brief Runs the loss-detection timer if due.
  ///
  /// @param now_us The current time.
  /// @return What it did.
  auto on_timeout(std::uint64_t now_us) -> TimeoutEvent;
  /// @brief Forgets everything, as for a new spoke.
  void reset() noexcept;

  /// @brief Whether a probe must be sent.
  ///
  /// @return True after a probe timeout until the probe is sent.
  auto probe_pending() const noexcept -> bool { return probe_pending_; }
  /// @brief The path's state.
  ///
  /// @return congested while either signal is.
  auto path() const noexcept -> PathState;
  /// @brief The delay signal.
  ///
  /// @return Its state.
  auto delay() const noexcept -> PathState { return delay_; }
  /// @brief The loss signal.
  ///
  /// @return Its state.
  auto loss() const noexcept -> PathState { return loss_; }
  /// @brief The delay floor.
  ///
  /// @return The smallest sample of this and the previous interval, in
  ///     microseconds; 0 before any.
  auto delay_floor_us() const noexcept -> double;
  /// @brief The smoothed RTT above which delay becomes congested.
  ///
  /// @return Floor plus max(200 ms, floor).
  auto delay_limit_us() const noexcept -> double;
  /// @brief The smoothed RTT below which delay clears.
  ///
  /// @return Floor plus half of max(200 ms, floor).
  auto delay_release_us() const noexcept -> double;
  /// @brief Bytes sent and not yet settled.
  ///
  /// @return The count.
  auto bytes_in_flight() const noexcept -> std::uint64_t { return in_flight_; }
  /// @brief Datagrams declared lost so far.
  ///
  /// @return The count.
  auto lost_packets() const noexcept -> std::uint64_t { return lost_packets_; }
  /// @brief Whether any RTT was sampled.
  ///
  /// @return True after the first.
  auto has_rtt_sample() const noexcept -> bool {
    return first_sample_.has_value();
  }
  /// @brief The smoothed RTT.
  ///
  /// @return Microseconds; 333 ms before the first sample.
  auto smoothed_rtt_us() const noexcept -> double { return smoothed_rtt_; }
  /// @brief The RTT variance.
  ///
  /// @return Microseconds.
  auto rtt_variance_us() const noexcept -> double { return rtt_variance_; }
  /// @brief The smallest RTT seen.
  ///
  /// @return Microseconds; 0 before the first sample.
  auto min_rtt_us() const noexcept -> double { return min_rtt_; }
  /// @brief The latest RTT sample.
  ///
  /// @return Microseconds.
  auto latest_rtt_us() const noexcept -> double { return latest_rtt_; }
  /// @brief Probe timeouts since the last acknowledgement.
  ///
  /// @return The count.
  auto pto_count() const noexcept -> std::size_t { return pto_count_; }
  /// @brief The largest datagram sequence acknowledged.
  ///
  /// @return It; nothing before any acknowledgement.
  auto largest_acked() const noexcept -> std::optional<std::uint64_t> {
    return largest_acked_;
  }
  /// @brief Datagrams tracked, from the oldest unsettled.
  ///
  /// @return The count.
  auto tracked_packets() const noexcept -> std::size_t { return sent_.size(); }

 private:
  /// @brief Where a sent datagram stands.
  enum class PacketState : std::uint8_t {
    /// @brief Neither acknowledged nor lost.
    outstanding,
    /// @brief Acknowledged.
    acked,
    /// @brief Declared lost; a late acknowledgement changes nothing.
    lost,
  };

  /// @brief A sent datagram.
  struct SentPacket {
    /// @brief When it was sent.
    std::uint64_t time_sent{};
    /// @brief Its size.
    std::uint32_t bytes{};
    /// @brief Its state.
    PacketState state{PacketState::outstanding};
  };

  /// @brief A datagram just acknowledged or lost.
  struct Settled {
    /// @brief Its sequence.
    std::uint64_t number{};
    /// @brief When it was sent.
    std::uint64_t time_sent{};
    /// @brief Its size.
    std::uint32_t bytes{};
  };

  /// @brief The smallest RTT sample of one 10-second interval.
  struct DelayFloor {
    /// @brief The interval's index.
    std::uint64_t window{};
    /// @brief The sample, in microseconds.
    double rtt_us{};
  };

  /// @brief One 500 ms interval's settled datagrams.
  struct LossWindow {
    /// @brief The interval's index.
    std::uint64_t window{};
    /// @brief Datagrams acknowledged.
    std::uint64_t delivered{};
    /// @brief Datagrams lost.
    std::uint64_t lost{};
  };

  /// @brief The record of a tracked datagram.
  ///
  /// @param number Its sequence, at least the oldest tracked.
  /// @return The record.
  auto entry(std::uint64_t number) noexcept -> SentPacket&;
  /// @brief The record of a tracked datagram.
  ///
  /// @param number Its sequence, at least the oldest tracked.
  /// @return The record.
  auto entry(std::uint64_t number) const noexcept -> const SentPacket&;
  /// @brief The probe timeout: the last send plus (smoothed RTT + max(4 ×
  /// variance, 1 ms) + 25 ms) doubled per timeout.
  ///
  /// @return The instant; nothing with nothing in flight.
  auto pto_deadline() const noexcept -> std::optional<std::uint64_t>;
  /// @brief Declares lost the outstanding datagrams the packet or time
  /// threshold condemns, and arms the loss timer for the rest.
  ///
  /// @param now_us The current time.
  /// @return The datagrams declared lost.
  auto detect_lost(std::uint64_t now_us) -> std::vector<Settled>;
  /// @brief Removes lost datagrams from the bytes in flight and counts them.
  ///
  /// @param lost The datagrams.
  void on_packets_lost(const std::vector<Settled>& lost) noexcept;
  /// @brief Folds the latest sample into the estimates (RFC 9002 section
  /// 5.3).
  ///
  /// @param ack_delay_us The peer's delay, already capped at 25 ms.
  /// @param now_us The current time.
  void update_rtt(double ack_delay_us, std::uint64_t now_us) noexcept;
  /// @brief Updates the delay floor and judges the delay signal.
  ///
  /// @param now_us The current time.
  void note_delay(std::uint64_t now_us) noexcept;
  /// @brief Counts settled datagrams in the current loss interval, judging
  /// the
  /// previous one first when the interval changed.
  ///
  /// @param delivered Datagrams acknowledged.
  /// @param lost Datagrams lost.
  /// @param now_us The current time.
  void note_settled(std::size_t delivered, std::size_t lost,
                    std::uint64_t now_us) noexcept;
  /// @brief Judges the loss signal from the current interval.
  void judge_loss() noexcept;
  /// @brief Drops settled datagrams from the front of the record.
  void trim() noexcept;

  /// @brief Tracked datagrams, from `base_` on.
  std::deque<SentPacket> sent_;
  /// @brief Sequence of the oldest tracked datagram.
  std::uint64_t base_{};
  /// @brief Sequence of the next datagram.
  std::uint64_t next_{};
  /// @brief Bytes outstanding.
  std::uint64_t in_flight_{};
  /// @brief Datagrams lost so far.
  std::uint64_t lost_packets_{};
  /// @brief The latest sample, microseconds.
  double latest_rtt_{};
  /// @brief The smoothed RTT, microseconds.
  double smoothed_rtt_{initial_rtt_us};
  /// @brief The RTT variance, microseconds.
  double rtt_variance_{initial_rtt_us / initial_rttvar_divisor};
  /// @brief The smallest sample, microseconds.
  double min_rtt_{};
  /// @brief When the first sample was taken.
  std::optional<std::uint64_t> first_sample_;
  /// @brief The largest sequence acknowledged.
  std::optional<std::uint64_t> largest_acked_;
  /// @brief When the last datagram was sent.
  std::optional<std::uint64_t> last_ack_eliciting_;
  /// @brief When the earliest outstanding datagram becomes lost by time.
  std::optional<std::uint64_t> loss_time_;
  /// @brief The current interval's floor.
  std::optional<DelayFloor> floor_;
  /// @brief The previous interval's floor, if it was the one just before.
  std::optional<DelayFloor> earlier_floor_;
  /// @brief The current loss interval.
  LossWindow settled_;
  /// @brief The delay signal.
  PathState delay_{PathState::clear};
  /// @brief The loss signal.
  PathState loss_{PathState::clear};
  /// @brief Probe timeouts since the last acknowledgement.
  std::size_t pto_count_{};
  /// @brief Whether a probe must be sent.
  bool probe_pending_{};
};

/// @brief A byte token bucket that spaces media datagrams.
///
/// Holds at most 1200 bytes while idle and at most max(2400, 10 ms at the
/// rate) while sending. A send may overdraw it; later refills repay the debt.
/// Not thread-safe.
class Pacer {
 public:
  /// @brief Starts idle with 1200 bytes.
  ///
  /// @param now_us The current time.
  explicit Pacer(std::uint64_t now_us) noexcept : updated_(now_us) {}

  /// @brief Refills at the old rate, then changes it.
  ///
  /// @param bytes_per_us The new rate.
  /// @param now_us The current time.
  void set_rate(double bytes_per_us, std::uint64_t now_us) noexcept;
  /// @brief Refills, then marks the queue idle or busy, which sets the
  /// ceiling.
  ///
  /// @param idle Whether the queue is empty.
  /// @param now_us The current time.
  void set_idle(bool idle, std::uint64_t now_us) noexcept;
  /// @brief Whether the queue is idle.
  ///
  /// @return True when idle.
  auto idle() const noexcept -> bool { return idle_; }
  /// @brief The bytes available.
  ///
  /// @param now_us The current time.
  /// @return Bytes, possibly negative after a probe.
  auto tokens(std::uint64_t now_us) const noexcept -> double;
  /// @brief Whether a datagram fits now.
  ///
  /// @param bytes Its size.
  /// @param now_us The current time.
  /// @return True when enough bytes are available.
  auto can_send(std::size_t bytes, std::uint64_t now_us) const noexcept -> bool;
  /// @brief Debits a datagram sent.
  ///
  /// @param bytes Its size.
  /// @param now_us When.
  void on_sent(std::size_t bytes, std::uint64_t now_us) noexcept;
  /// @brief When a datagram will fit.
  ///
  /// @param bytes Its size.
  /// @return The instant; the largest u64 at rate 0.
  auto next_send_time(std::size_t bytes) const noexcept -> std::uint64_t;
  /// @brief The refill rate.
  ///
  /// @return Bytes per microsecond.
  auto rate() const noexcept -> double { return rate_; }
  /// @brief Returns to the initial state.
  ///
  /// @param now_us The current time.
  void reset(std::uint64_t now_us) noexcept;

 private:
  /// @brief Adds the bytes earned, up to the ceiling.
  ///
  /// @param now_us The current time.
  void refill(std::uint64_t now_us) noexcept;
  /// @brief The most bytes held.
  ///
  /// @return 1200 idle, otherwise max(2400, 10 ms at the rate).
  auto ceiling() const noexcept -> double;
  /// @brief The bytes a refill would leave.
  ///
  /// @param now_us The current time.
  /// @return The bytes.
  auto refilled(std::uint64_t now_us) const noexcept -> double;

  /// @brief Bytes available.
  double tokens_{pacer_initial_tokens};
  /// @brief Refill rate, bytes per microsecond.
  double rate_{pacer_initial_rate};
  /// @brief When the bucket was last refilled.
  std::uint64_t updated_{};
  /// @brief Whether the queue is idle.
  bool idle_{true};
};

}  // namespace libchiaking::net::kakusan
