/// @file
/// @brief Declares MediaSender, the host side of Kakusan media forwarding to
/// one
/// guest, and its tuning and report.
#pragma once

#include "libchiaking/platform/byte_units.hpp"
#include "libchiaking/platform/crypto/provider.hpp"
#include "src/core/net/kakusan/congestion.hpp"
#include "src/core/net/kakusan/result.hpp"
#include "src/core/net/kakusan/transport_frame.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

namespace libchiaking::net::kakusan {

/// @brief Shortest the send queue's age bound may be, in microseconds: 100 ms.
inline constexpr std::uint64_t media_queue_age_us = 100000;
/// @brief Longest the age bound may be, and its value before any RTT sample:
/// 250 ms.
inline constexpr std::uint64_t media_queue_age_limit_us = 250000;
/// @brief Drops in a spoke's first 5 seconds do not count towards removal.
inline constexpr std::uint64_t media_startup_grace_us = 5000000;
/// @brief Largest send queue, in bytes: 4 MiB.
inline constexpr std::size_t media_queue_bytes = std::size_t{4} * bytes_per_mib;
/// @brief Most source datagrams queued.
inline constexpr std::size_t media_queue_packets = 8192;
/// @brief Time with datagrams in flight and no new acknowledgement before the
/// spoke is removed: 3 seconds.
inline constexpr std::uint64_t media_progress_timeout_us = 3000000;
/// @brief Length of a drop window: 1 second.
inline constexpr std::uint64_t media_drop_window_us = 1000000;
/// @brief Consecutive lossy windows that remove the guest.
inline constexpr std::uint64_t media_drop_windows_to_fail = 3;
/// @brief Bytes of a probe datagram: 76.
inline constexpr std::size_t media_probe_size =
    media_header_size + media_tag_size;
/// @brief The age bound is this many smoothed round trips, clamped.
inline constexpr std::uint32_t media_queue_age_round_trips = 2;
/// @brief Least age bound a tuning may set, in microseconds.
inline constexpr std::uint64_t media_queue_age_floor_us = 1000;
/// @brief Least progress timeout a tuning may set, in microseconds.
inline constexpr std::uint64_t media_progress_timeout_floor_us = 100000;
/// @brief Most loss timers run in one poll.
inline constexpr int media_max_timer_rounds = 4;
/// @brief Interval of the offered-rate average: 100 ms.
inline constexpr std::uint64_t media_rate_window_us = 100000;
/// @brief Most idle intervals decayed at once.
inline constexpr std::uint64_t media_rate_idle_windows = 32;
/// @brief Weight kept of the offered rate per interval.
inline constexpr double media_rate_retain = 0.75;
/// @brief The pacing rate is at least this times the offered rate.
inline constexpr double media_pace_headroom = 2.0;
/// @brief Least pacing rate, in bytes per microsecond.
inline constexpr double media_pace_floor = 0.5;
/// @brief The burst must drain within this fraction of the age bound.
inline constexpr double media_drain_fraction = 0.5;

/// @brief Host-local media sending parameters; the defaults are the
/// interoperable ones, and changing them does not affect the wire.
struct MediaTuning {
  /// @brief Shortest age bound, in microseconds.
  std::uint64_t queue_age_min_us{media_queue_age_us};
  /// @brief Longest age bound, in microseconds.
  std::uint64_t queue_age_limit_us{media_queue_age_limit_us};
  /// @brief Round trips the age bound spans.
  std::uint32_t queue_age_round_trips{media_queue_age_round_trips};
  /// @brief Time at the start when drops do not count, in microseconds.
  std::uint64_t startup_grace_us{media_startup_grace_us};
  /// @brief Consecutive lossy windows that remove the guest.
  std::uint64_t drop_windows_to_fail{media_drop_windows_to_fail};
  /// @brief Time without acknowledgement progress before removal, in
  /// microseconds.
  std::uint64_t progress_timeout_us{media_progress_timeout_us};
  /// @brief Largest queue, in bytes.
  std::size_t queue_bytes{media_queue_bytes};
  /// @brief Most datagrams queued.
  std::size_t queue_packets{media_queue_packets};

  /// @brief The tuning with every value raised to its floor.
  ///
  /// @return The bounded copy.
  auto bounded() const noexcept -> MediaTuning;

  /// @brief Compares every field.
  ///
  /// @return True when equal.
  constexpr bool operator==(const MediaTuning&) const noexcept = default;
};

/// @brief Why media to a guest stopped.
enum class MediaStop : std::uint8_t {
  /// @brief It has not.
  none,
  /// @brief Too many consecutive lossy drop windows.
  drop_windows,
  /// @brief No acknowledgement progress for the progress timeout.
  ack_timeout,
};

/// @brief Whether video is forwarded to a guest.
enum class VideoFlow : std::uint8_t {
  /// @brief The path is clear; video is sent.
  forwarding,
  /// @brief The path is congested; video is dropped, audio continues.
  shedding,
};

/// @brief A snapshot of one guest's media sending, for diagnostics.
struct MediaSenderReport {
  /// @brief Why sending stopped, if it did.
  MediaStop stop{MediaStop::none};
  /// @brief Smoothed RTT, microseconds.
  std::uint64_t smoothed_rtt_us{};
  /// @brief Delay floor, microseconds.
  std::uint64_t floor_rtt_us{};
  /// @brief Bytes unsettled.
  std::uint64_t bytes_in_flight{};
  /// @brief Datagrams declared lost.
  std::uint64_t lost_datagrams{};
  /// @brief The current age bound, microseconds.
  std::uint64_t queue_age_us{};
  /// @brief Datagrams sent, probes included.
  std::uint64_t datagrams_sent{};
  /// @brief Acknowledgements that made progress.
  std::uint64_t acks{};
  /// @brief Source datagrams admitted, shed ones included.
  std::uint64_t source_packets{};
  /// @brief Source datagrams dropped or shed.
  std::uint64_t dropped_packets{};
  /// @brief Of those, video.
  std::uint64_t dropped_video{};
  /// @brief Whether an RTT has been sampled.
  bool rtt_sampled{};
};

/// @brief Forwards the source's encrypted A/V datagrams to one guest as
/// authenticated RCPM fragments, paced and judged by the congestion controller
/// (RCP TRANSPORT sections 2 and 5).
///
/// Source datagrams queue FIFO, bounded by age (twice the smoothed RTT within
/// 100 to 250 ms), 4 MiB and 8192 datagrams; expired or evicted ones are
/// dropped whole. While the path is congested queued and new video is shed.
/// After the 5-second grace, three consecutive lossy 1-second windows, or 3
/// seconds with datagrams in flight and no new acknowledgement, stop the
/// sender with Error::congestion. A dropped or resumed video stream asks for an
/// IDR. Holds the media key; not copyable or movable; one thread.
class MediaSender {
 public:
  /// @brief Starts a sender for one spoke.
  ///
  /// @param crypto Computes the tags; must outlive the sender.
  /// @param key The spoke's media key.
  /// @param connection The spoke's connection id.
  /// @param now_us The current monotonic time.
  /// @param tuning Local parameters; raised to their floors.
  MediaSender(platform::CryptoProvider& crypto, MediaKey key,
              const ConnectionId& connection, std::uint64_t now_us,
              const MediaTuning& tuning = {});

  /// @brief Not copyable.
  MediaSender(const MediaSender&) = delete;
  /// @brief Not movable.
  MediaSender(MediaSender&&) = delete;
  /// @brief Not copyable.
  ///
  /// @return Never returns.
  auto operator=(const MediaSender&) -> MediaSender& = delete;
  /// @brief Not movable.
  ///
  /// @return Never returns.
  auto operator=(MediaSender&&) -> MediaSender& = delete;

  /// @brief Queues an authenticated source datagram, or sheds it.
  ///
  /// @param kind Video or audio.
  /// @param key_position Its full 64-bit source key position.
  /// @param source_datagram The original datagram, 1 to 8168 bytes.
  /// @param now_us The current monotonic time.
  /// @return Nothing; also nothing once stopped.
  /// @retval Error::protocol A probe, an empty or oversized datagram, or the
  ///     sequence space ran out.
  /// @retval Error::congestion Too many lossy windows.
  [[nodiscard]] auto submit(MediaKind kind, std::uint64_t key_position,
                            std::span<const std::byte> source_datagram,
                            std::uint64_t now_us) -> Result<void>;
  /// @brief Runs timers, follows the path, expires the queue and sends what
  /// the
  /// pacer allows, a pending probe first.
  ///
  /// @param now_us The current monotonic time.
  /// @param datagrams Receives the RCPM datagrams to send.
  /// @return Nothing on success.
  /// @retval Error::congestion No acknowledgement progress, or too many lossy
  ///     windows.
  /// @retval Error::protocol The sequence space ran out.
  /// @retval Error A tag could not be computed.
  [[nodiscard]] auto poll(std::uint64_t now_us,
                          std::vector<std::vector<std::byte>>& datagrams)
      -> Result<void>;
  /// @brief Feeds a guest's RCA1 record.
  ///
  /// @param record The record.
  /// @param now_us The current monotonic time.
  /// @return Nothing; also nothing once stopped.
  /// @retval Error::protocol The record is malformed or names unsent
  ///     datagrams.
  /// @retval Error::congestion No acknowledgement progress in time.
  [[nodiscard]] auto on_ack(std::span<const std::byte> record,
                            std::uint64_t now_us) -> Result<void>;
  /// @brief Takes the pending keyframe request.
  ///
  /// @return Whether one was pending; the caller coalesces them to one per
  ///     second.
  [[nodiscard]] auto take_idr_request() noexcept -> bool;
  /// @brief When poll() must next run.
  ///
  /// @return The earliest of the progress, loss, probe, expiry and pacing
  ///     times; nothing once stopped.
  auto next_deadline_us() const noexcept -> std::optional<std::uint64_t>;

  /// @brief Why the sender stopped.
  ///
  /// @return The error; nothing while running.
  auto failure() const noexcept -> std::optional<Error> { return failure_; }
  /// @brief Source datagrams queued.
  ///
  /// @return The count.
  auto queued_packets() const noexcept -> std::size_t { return queue_.size(); }
  /// @brief Bytes queued.
  ///
  /// @return The count.
  auto queued_bytes() const noexcept -> std::size_t { return queued_bytes_; }
  /// @brief The packet sequence the next source datagram gets.
  ///
  /// @return The number.
  auto next_packet_sequence() const noexcept -> std::uint64_t {
    return next_packet_sequence_;
  }
  /// @brief When the oldest queued datagram was queued.
  ///
  /// @return The instant; nothing when empty.
  auto oldest_queued_us() const noexcept -> std::optional<std::uint64_t>;
  /// @brief When missing acknowledgement progress stops the sender.
  ///
  /// @return The instant; nothing with nothing in flight.
  auto progress_deadline_us() const noexcept -> std::optional<std::uint64_t>;
  /// @brief The current age bound.
  ///
  /// @return Round trips × smoothed RTT, clamped to the tuning's bounds.
  auto queue_age_us() const noexcept -> std::uint64_t;
  /// @brief A diagnostic snapshot.
  ///
  /// @return The report.
  auto report() const noexcept -> MediaSenderReport;
  /// @brief The bounded tuning in use.
  ///
  /// @return The tuning.
  auto tuning() const noexcept -> const MediaTuning& { return tuning_; }
  /// @brief Whether video is forwarded.
  ///
  /// @return The flow.
  auto flow() const noexcept -> VideoFlow { return flow_; }
  /// @brief The congestion controller.
  ///
  /// @return It.
  auto controller() const noexcept -> const CongestionController& {
    return controller_;
  }
  /// @brief The pacer.
  ///
  /// @return It.
  auto pacer() const noexcept -> const Pacer& { return pacer_; }

 private:
  /// @brief A queued source datagram.
  struct Queued {
    /// @brief Video or audio.
    MediaKind kind{MediaKind::video};
    /// @brief Its packet sequence.
    std::uint64_t packet_sequence{};
    /// @brief Its key position.
    std::uint64_t key_position{};
    /// @brief When it was queued.
    std::uint64_t queued_us{};
    /// @brief Bytes already sent as fragments.
    std::size_t sent{};
    /// @brief The datagram.
    std::vector<std::byte> bytes;
  };

  /// @brief Advances the sender's clock.
  ///
  /// @param now_us The current time.
  /// @return The clock, never moving backwards.
  auto observe(std::uint64_t now_us) noexcept -> std::uint64_t;
  /// @brief Whether the sender stopped.
  ///
  /// @return True after a failure.
  auto stopped() const noexcept -> bool { return failure_.has_value(); }
  /// @brief Whether acknowledgement progress is overdue.
  ///
  /// @param now_us The current time.
  /// @return True past the progress deadline.
  [[nodiscard]] auto progress_expired(std::uint64_t now_us) noexcept -> bool;
  /// @brief Encodes, records and emits one datagram.
  ///
  /// @param header The header.
  /// @param fragment The fragment.
  /// @param probe Whether it is the pending probe.
  /// @param now_us The current time.
  /// @param datagrams Receives it.
  /// @return Nothing on success.
  /// @retval Error It could not be encoded or tracked; the sender stops.
  [[nodiscard]] auto transmit(const MediaHeader& header,
                              std::span<const std::byte> fragment, bool probe,
                              std::uint64_t now_us,
                              std::vector<std::vector<std::byte>>& datagrams)
      -> Result<void>;
  /// @brief Stops the sender, keeping the first error, and empties the queue.
  ///
  /// @param error Why.
  /// @return The first error.
  [[nodiscard]] auto stop(Error error) noexcept -> Unexpected<Error>;
  /// @brief Runs due loss timers, at most four.
  ///
  /// @param now_us The current time.
  void run_timers(std::uint64_t now_us);
  /// @brief Drops queued datagrams older than the age bound.
  ///
  /// @param now_us The current time.
  void expire(std::uint64_t now_us);
  /// @brief Drops the oldest queued datagram as a queue drop.
  ///
  /// @param now_us The current time.
  void drop_front(std::uint64_t now_us);
  /// @brief One 1-second window's offered and dropped datagrams.
  struct DropWindow {
    /// @brief The window's index since start.
    std::uint64_t index{};
    /// @brief Source datagrams offered.
    std::uint64_t offered{};
    /// @brief Datagrams dropped by queue bounds.
    std::uint64_t dropped{};
    /// @brief Video shed because the path was congested.
    std::uint64_t shed{};

    /// @brief Whether the window counts towards removal.
    ///
    /// @return True when anything was shed or queue drops reached half the
    ///     offered.
    constexpr auto lossy() const noexcept -> bool {
      return shed > 0 || (dropped > 0 && dropped * 2 >= offered);
    }
  };

  /// @brief Why a datagram was dropped.
  enum class DropCause : std::uint8_t {
    /// @brief Its age or the queue's size.
    queue,
    /// @brief The path was congested.
    path,
  };

  /// @brief The exponentially weighted rate of source bytes queued.
  struct OfferedRate {
    /// @brief The current 100 ms interval.
    std::uint64_t window{};
    /// @brief Bytes queued in it.
    std::uint64_t bytes{};
    /// @brief The average, bytes per microsecond.
    double bytes_per_us{};

    /// @brief Counts bytes queued, folding finished intervals in.
    ///
    /// @param size The bytes.
    /// @param now_us The current time.
    void add(std::size_t size, std::uint64_t now_us) noexcept;
  };

  /// @brief The current drop window, judging the previous one when it
  /// changes.
  ///
  /// @param now_us The current time.
  /// @return The window; null during the start-up grace.
  auto drop_window_at(std::uint64_t now_us) noexcept -> DropWindow*;
  /// @brief Counts a drop and, for video while forwarding, asks for an IDR.
  ///
  /// @param kind The dropped datagram's kind.
  /// @param cause Why.
  /// @param now_us The current time.
  void note_drop(MediaKind kind, DropCause cause,
                 std::uint64_t now_us) noexcept;
  /// @brief Switches between forwarding and shedding with the path, shedding
  /// queued video on entry and asking for an IDR on recovery.
  ///
  /// @param now_us The current time.
  void follow_path(std::uint64_t now_us);
  /// @brief Sets the pacing rate: the largest of 0.5 bytes/µs, twice the
  /// offered rate, and the burst over half the age bound.
  ///
  /// @param now_us The current time.
  void pace(std::uint64_t now_us) noexcept;

  /// @brief Computes the tags.
  platform::CryptoProvider& crypto_;
  /// @brief The spoke's media key.
  MediaKey key_;
  /// @brief The spoke's connection id.
  ConnectionId connection_;
  /// @brief The bounded tuning.
  MediaTuning tuning_;
  /// @brief Loss and path judgement.
  CongestionController controller_;
  /// @brief Spaces the datagrams.
  Pacer pacer_;
  /// @brief Source datagrams waiting.
  std::deque<Queued> queue_;
  /// @brief Bytes waiting.
  std::size_t queued_bytes_{};
  /// @brief Bytes queued since the queue was last empty.
  std::size_t burst_bytes_{};
  /// @brief The offered rate.
  OfferedRate offered_;
  /// @brief Whether video is forwarded.
  VideoFlow flow_{VideoFlow::forwarding};
  /// @brief The next packet sequence.
  std::uint64_t next_packet_sequence_{};
  /// @brief When the sender started.
  std::uint64_t start_us_{};
  /// @brief The latest time seen.
  std::uint64_t now_us_{};
  /// @brief When acknowledgement progress was last made, or sending resumed
  /// from idle.
  std::optional<std::uint64_t> progress_anchor_us_;
  /// @brief The current drop window.
  std::optional<DropWindow> drop_window_;
  /// @brief Consecutive lossy windows.
  std::uint64_t lossy_windows_{};
  /// @brief Datagrams sent.
  std::uint64_t datagrams_sent_{};
  /// @brief Acknowledgements that made progress.
  std::uint64_t acks_{};
  /// @brief Source datagrams dropped.
  std::uint64_t dropped_packets_{};
  /// @brief Video datagrams dropped.
  std::uint64_t dropped_video_{};
  /// @brief Why sending stopped.
  MediaStop stop_{MediaStop::none};
  /// @brief The first failure.
  std::optional<Error> failure_;
  /// @brief Whether a keyframe request is pending.
  bool idr_requested_{};
};

}  // namespace libchiaking::net::kakusan
