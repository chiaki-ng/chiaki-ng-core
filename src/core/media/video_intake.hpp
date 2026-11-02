/// @file
/// @brief Declares VideoIntake, which checks a session's video profiles and
/// frames and decides when to report losses and ask for a keyframe.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/media/media_sink.hpp"
#include "libchiaking/detail/play/outcome.hpp"
#include "libchiaking/detail/play/statistics.hpp"
#include "libchiaking/platform/byte_units.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "src/core/net/protocol/remote_play/stream/stream.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace libchiaking::media {

/// @brief Bounds a VideoIntake checks against. Plain data.
struct VideoLimits {
  /// @brief The most video profiles a stream may describe.
  std::size_t profiles{8};
  /// @brief The largest profile header accepted, in bytes.
  std::size_t header_bytes{64 * bytes_per_kib};
  /// @brief The largest frame accepted, in bytes.
  std::size_t frame_bytes{4 * bytes_per_kib * bytes_per_kib};
  /// @brief The largest width or height a profile may have, in pixels; empty
  /// for no limit.
  std::optional<std::uint32_t> dimension{};
  /// @brief Decoder rejections without a presented frame after which a
  /// keyframe is asked for.
  std::uint64_t idr_rejections{2};
  /// @brief The least time between two loss-driven keyframe requests.
  std::chrono::milliseconds idr_interval{1000};
};

/// @brief A run of frame indices the decoder rejected, to report to the
/// sender as lost. Indices wrap at 16 bits.
struct VideoLossRange {
  /// @brief The first rejected frame index.
  std::uint16_t first{};
  /// @brief The last rejected frame index.
  std::uint16_t last{};

  /// @brief Compares both ends.
  ///
  /// @return `true` when both ranges are the same.
  constexpr bool operator==(const VideoLossRange&) const noexcept = default;
};

/// @brief The video side of a session's media path.
///
/// Holds the profiles of the current stream description and checks each
/// frame against them. Counts units, bytes, losses, recoveries and decoder
/// rejections, collects rejected and dropped frames into one range for a loss
/// report, and decides when to ask for a keyframe: when one is forced, after a
/// dropped frame or an FEC failure, or after VideoLimits::idr_rejections
/// rejections, no sooner than VideoLimits::idr_interval after the last
/// request.
///
/// Takes no lock. The profiles belong to the thread that delivers media:
/// VideoIntake::configure, VideoIntake::admit, VideoIntake::settle and
/// VideoIntake::profile_count must be called by one thread at a time. Every
/// other member works on atomics, so the decoder thread, the session loop and
/// any reader of VideoIntake::statistics may call it from their own threads.
/// The counters are each exact but VideoIntake::statistics reads them one by
/// one, not as one snapshot. The clock must outlive it.
class VideoIntake {
 public:
  /// @brief Builds the intake with no profiles.
  ///
  /// @param clock The monotonic clock keyframe requests are paced by; must
  ///     outlive the intake.
  /// @param limits The bounds to check against.
  explicit VideoIntake(platform::ClockProvider& clock, VideoLimits limits = {});

  /// @brief Replaces the profiles with a new stream description's.
  ///
  /// @param info The stream description.
  /// @return Nothing when there is at least one profile, no more than
  ///     VideoLimits::profiles, each sized within VideoLimits::dimension with a
  ///     header of 1 to VideoLimits::header_bytes bytes;
  ///     MediaFailure::profile_validation otherwise, keeping the old profiles.
  auto configure(const net::protocol::remote_play::event::StreamInfoReady& info)
      -> Expected<void, MediaFailure>;
  /// @brief Turns a link's encoded unit into a video frame, taking its bytes.
  ///
  /// @param media The unit.
  /// @return The frame; empty when the unit is not video.
  static auto frame(net::protocol::remote_play::event::EncodedMedia&& media)
      -> std::optional<VideoFrame>;

  /// @brief Checks a frame and builds the unit the sink decodes.
  ///
  /// Counts the frame and the losses and recovery it reports.
  ///
  /// @param frame The frame.
  /// @return The unit, with its profile's header and size, whether it is HEVC
  ///     (from the profile header, else the frame's codec) and whether the
  ///     profile changed since the last settled frame;
  ///     MediaFailure::frame_validation for an empty or oversized frame or one
  ///     naming no profile.
  auto admit(VideoFrame frame) -> Expected<VideoUnit, MediaFailure>;
  /// @brief Settles the sink's result for a unit.
  ///
  /// Remembers the unit's profile as current. A presented frame clears the
  /// rejection count and pending losses. A frame to recover is recorded as
  /// rejected. A dropped frame is counted as lost, handed to the next
  /// presented frame, added to the loss range and makes a keyframe due, since
  /// the frames after it may refer to it.
  ///
  /// @param unit The unit given to the sink.
  /// @param result The sink's verdict.
  /// @return Nothing; MediaFailure::decoder_submission when the decoder could
  ///     not be set up or refused the frame.
  auto settle(const VideoUnit& unit, VideoResult result)
      -> Expected<void, MediaFailure>;
  /// @brief Records that a frame could not be repaired by forward error
  /// correction.
  ///
  /// @param idr_already_requested Whether the link already asked for a
  ///     keyframe; then only the request time is noted, otherwise a keyframe
  ///     becomes due.
  void fec_failed(bool idr_already_requested) noexcept;
  /// @brief Records a decoder rejection and widens the loss range to include
  /// the frame.
  ///
  /// @param frame The frame index.
  /// @param cause The decoder's cause, kept as the last decoder error.
  void rejected(std::uint16_t frame, int cause) noexcept;
  /// @brief Records that a frame was presented.
  ///
  /// @return The frames lost and whether one was recovered since the last
  ///     presentation, both reset.
  auto presented() noexcept -> PresentationLoss;
  /// @brief Forces a keyframe request on the next check.
  void request_idr() noexcept;
  /// @brief Asks for the next video frame to be discarded on the next check.
  void request_drop() noexcept;
  /// @brief Takes a pending request to discard a video frame.
  ///
  /// @return `true` once per request.
  auto drop_due() noexcept -> bool;

  /// @brief Takes the range of rejected frames to report.
  ///
  /// @return The range, cleared; empty when nothing was rejected since the
  ///     last report.
  auto take_loss_report() noexcept -> std::optional<VideoLossRange>;
  /// @brief Decides whether to ask for a keyframe now, and if so records the
  /// request.
  ///
  /// @return `true` when one was forced, or when losses call for one and the
  ///     last request is at least VideoLimits::idr_interval old.
  auto idr_due() noexcept -> bool;
  /// @brief Reports whether a loss report or forced keyframe is waiting, so the
  /// session loop does not sleep.
  ///
  /// @return `true` when either is waiting.
  auto work_pending() const noexcept -> bool;
  /// @brief Reads the number of profiles held.
  ///
  /// @return The count from the last valid description.
  auto profile_count() const noexcept -> std::size_t;
  /// @brief Reads the video counters.
  ///
  /// @return A copy of the counters, each read on its own.
  auto statistics() const noexcept -> VideoStatistics;
  /// @brief Reads how many frames the decoder rejected.
  ///
  /// @return The count since the intake was built.
  auto decoder_rejections() const noexcept -> std::uint64_t;
  /// @brief Reads how many frames were lost.
  ///
  /// @return The frames the stream reported lost plus the frames dropped.
  auto lost() const noexcept -> std::uint64_t;

 private:
  /// @brief Half the 16-bit index space; a frame within it after the range's
  /// end extends the end, otherwise one within it before the start extends the
  /// start.
  static constexpr std::uint16_t forward_window = 0x8000;

  /// @brief One video profile of the stream description.
  struct Profile {
    /// @brief Width in pixels.
    std::uint32_t width{};
    /// @brief Height in pixels.
    std::uint32_t height{};
    /// @brief The codec header the decoder is set up with.
    std::vector<std::byte> header;
    /// @brief Whether the header is HEVC, from its first NAL unit; empty when
    /// it could not be told.
    std::optional<bool> hevc;
  };

  /// @brief Tells HEVC from H.264 by the first Annex B NAL unit of a header.
  ///
  /// @param bytes The header.
  /// @return `false` for an H.264 SPS, `true` for an HEVC VPS, empty for
  ///     anything else or no start code.
  static auto sniff_hevc(std::span<const std::byte> bytes) noexcept
      -> std::optional<bool>;

  /// @brief Packs a loss range into the word VideoIntake::loss_ holds.
  ///
  /// @param range The range.
  /// @return The range's first index in the low 16 bits, its last in the next
  ///     16, and VideoIntake::loss_present set.
  static constexpr auto pack(VideoLossRange range) noexcept -> std::uint64_t {
    return loss_present | (static_cast<std::uint64_t>(range.last) << 16U) |
           range.first;
  }
  /// @brief Unpacks the word VideoIntake::loss_ holds.
  ///
  /// @param packed The word.
  /// @return The range; empty when VideoIntake::loss_present is clear.
  static constexpr auto unpack(std::uint64_t packed) noexcept
      -> std::optional<VideoLossRange> {
    if ((packed & loss_present) == 0) {
      return std::nullopt;
    }
    return VideoLossRange{static_cast<std::uint16_t>(packed),
                          static_cast<std::uint16_t>(packed >> 16U)};
  }
  /// @brief Widens a packed range to include a frame, by the
  /// VideoIntake::forward_window rule.
  ///
  /// @param packed The packed range, possibly empty.
  /// @param frame The frame index.
  /// @return The packed widened range.
  static constexpr auto widened(std::uint64_t packed,
                                std::uint16_t frame) noexcept -> std::uint64_t {
    auto range = unpack(packed);
    if (!range) {
      return pack({frame, frame});
    }
    if (static_cast<std::uint16_t>(frame - range->last) < forward_window) {
      range->last = frame;
    } else if (static_cast<std::uint16_t>(range->first - frame) <
               forward_window) {
      range->first = frame;
    }
    return pack(*range);
  }
  /// @brief Adds a frame to the loss range waiting to be reported.
  ///
  /// @param frame The frame index.
  void lose(std::uint16_t frame) noexcept;

  /// @brief The bit of VideoIntake::loss_ that says a range is held.
  static constexpr std::uint64_t loss_present = std::uint64_t{1} << 32U;

  /// @brief Paces keyframe requests.
  platform::ClockProvider& clock_;
  /// @brief The bounds checked.
  VideoLimits limits_;
  /// @brief The current profiles; owned by the thread that delivers media.
  std::vector<Profile> profiles_;
  /// @brief The profile of the last settled frame; empty after a new
  /// description. Owned by the thread that delivers media.
  std::optional<std::size_t> selected_;
  /// @brief Video frames accepted from the stream.
  std::atomic<std::uint64_t> units_{};
  /// @brief Total size of the accepted frames, in bytes.
  std::atomic<std::uint64_t> bytes_{};
  /// @brief Frames the stream reported lost plus frames dropped.
  std::atomic<std::uint64_t> lost_{};
  /// @brief Frames delivered after being pointed at an older reference.
  std::atomic<std::uint64_t> recovered_{};
  /// @brief Frames the decoder rejected.
  std::atomic<std::uint64_t> decoder_rejections_{};
  /// @brief Keyframes asked for.
  std::atomic<std::uint64_t> keyframe_requests_{};
  /// @brief The cause given with the latest decoder rejection.
  std::atomic<int> last_decoder_error_{};
  /// @brief Frames lost since the last presentation.
  std::atomic<std::int32_t> pending_lost_{};
  /// @brief Whether a frame was recovered since the last presentation.
  std::atomic<bool> pending_recovered_{};
  /// @brief Decoder rejections since the last presented frame.
  std::atomic<std::uint64_t> rejections_since_output_{};
  /// @brief Rejected and dropped frames not yet reported, packed by
  /// VideoIntake::pack; zero when there are none.
  std::atomic<std::uint64_t> loss_{};
  /// @brief Whether an FEC failure or a dropped frame calls for a keyframe.
  std::atomic<bool> loss_signalled_{};
  /// @brief Whether a keyframe request was forced.
  std::atomic<bool> idr_forced_{};
  /// @brief Whether a video frame is to be discarded on request.
  std::atomic<bool> drop_forced_{};
  /// @brief Whether a keyframe was ever requested; set after
  /// VideoIntake::idr_requested_us_ is written.
  std::atomic<bool> idr_requested_{};
  /// @brief When the last keyframe was requested, in monotonic microseconds.
  std::atomic<std::uint64_t> idr_requested_us_{};
};

}  // namespace libchiaking::media
