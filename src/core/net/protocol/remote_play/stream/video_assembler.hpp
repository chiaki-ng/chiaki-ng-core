/// @file
/// @brief Declares VideoAssembler, which joins a frame's video units back
/// into one frame and repairs lost units from the frame's parity units.
///
/// Each Takion video unit carries the frame index, the unit's index, and the
/// frame's unit total and FEC count. A source unit's payload starts with a
/// big-endian 16-bit count of the zero bytes it would need to reach the
/// common unit size; parity units are that size. Newer Takion versions also
/// state a "video word", the number of picture blocks a unit covers, and
/// may send counts that are not to be trusted; then the frame is complete
/// when the covered blocks reach the picture's block count.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "src/core/net/protocol/remote_play/stream/cauchy_fec8.hpp"
#include "src/core/net/protocol/remote_play/stream/limits.hpp"
#include "src/core/net/protocol/takion/detail/packet_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace libchiaking::net::protocol::remote_play {

/// @brief Why a unit was refused.
enum class VideoAssemblerError : std::uint8_t {
  /// @brief The packet is not video.
  invalid_family,
  /// @brief The unit total or FEC count is impossible, without a video word to
  /// go by.
  invalid_count,
  /// @brief The unit is empty, its index is out of range, or recovery found
  /// the units inconsistent.
  invalid_unit,
  /// @brief A source unit is shorter than its 2-byte padding count.
  short_source,
  /// @brief The unit belongs to a frame already finished or given up.
  stale_frame,
  /// @brief The unit disagrees with its frame. Not produced by this assembler
  /// today.
  metadata_mismatch,
  /// @brief The unit arrived twice.
  duplicate_unit,
  /// @brief The frame would exceed max_frame_bytes.
  frame_too_large,
};

/// @brief A run of frames that will not be delivered.
struct VideoFrameLoss {
  /// @brief The first lost frame index.
  std::uint16_t first;
  /// @brief The last lost frame index.
  std::uint16_t last;
  /// @brief Whether a frame was received in part but could not be completed,
  /// rather than skipped entirely.
  bool fec_failure{};
};

/// @brief A frame joined from its units.
struct AssembledVideoFrame {
  /// @brief The frame index.
  std::uint16_t frame_index{};
  /// @brief The Takion codec byte.
  std::uint8_t codec{};
  /// @brief The picture size index.
  std::uint8_t adaptive_stream_index{};
  /// @brief The frame's bytes, the padding counts removed.
  std::vector<std::byte> bytes;
  /// @brief The source units recovered from parity.
  std::vector<std::uint16_t> restored_units;
  /// @brief Source plus parity units.
  std::uint16_t units_total{};
  /// @brief Parity units.
  std::uint16_t units_fec{};
  /// @brief Units received.
  std::uint16_t units_received{};
  /// @brief Whether units' counts were ignored in favour of block coverage.
  bool counts_ignored{};
  /// @brief Whether any unit set the dynamic range flag.
  bool dynamic_range_flag{};
  /// @brief Whether any unit asked that the frame not be shown.
  bool suppress_presentation{};
};

/// @brief Joins video units into frames, one frame at a time.
///
/// A unit of a newer frame abandons the frame in progress (reported as a
/// loss); units of older frames are refused. Frames skipped between the last
/// completed one and a new one are reported as losses too, at most eight
/// waiting. Not thread-safe.
class VideoAssembler {
 public:
  /// @brief The most units a frame may have.
  static constexpr std::size_t max_units = remote_play::max_units;
  /// @brief The largest frame, in bytes.
  static constexpr std::size_t max_frame_bytes = remote_play::max_frame_bytes;
  /// @brief The most losses kept until taken.
  static constexpr std::size_t max_pending_losses = 8;
  /// @brief Spare capacity reserved after a frame's bytes.
  static constexpr std::size_t buffer_padding = 64;

  /// @brief Adds a unit.
  ///
  /// The frame completes when every source unit is there, when enough units
  /// (with at least one parity unit) arrived to recover the rest, or, with
  /// video words and no usable counts, when the covered blocks reach
  /// `expected_blocks`.
  ///
  /// @param unit The unit; its data moves into the frame's slot.
  /// @param expected_blocks The picture's block count (16-pixel blocks for
  ///     H.264, 64-pixel for H.265), or 0 when unknown.
  /// @return The frame when complete, or nothing while it is not.
  /// @retval VideoAssemblerError::stale_frame The unit's frame is stale(), or
  ///     its data was left encrypted.
  /// @retval VideoAssemblerError Why the unit was refused, or why recovery
  ///     failed (the frame is then given up).
  [[nodiscard]] auto push(takion::AvPacketValues unit,
                          std::uint32_t expected_blocks = 0)
      -> Expected<std::optional<AssembledVideoFrame>, VideoAssemblerError>;

  /// @brief Takes the oldest reported loss.
  ///
  /// @return The loss, or nothing.
  [[nodiscard]] auto take_loss() noexcept -> std::optional<VideoFrameLoss>;

  /// @brief Records that a frame was delivered, so skipped frames are counted
  /// from it.
  ///
  /// @param frame_index The frame delivered.
  void mark_complete(std::uint16_t frame_index) noexcept;

  /// @brief Moves the last delivered frame back to just before a lost one.
  ///
  /// @param first_lost The first frame lost downstream.
  void rewind_complete(std::uint16_t first_lost) noexcept;

  /// @brief The last frame delivered.
  ///
  /// @return Its index.
  auto last_complete() const noexcept -> std::uint16_t {
    return last_complete_;
  }

  /// @brief The frame seen last.
  ///
  /// @return Its index, or nothing before the first unit.
  auto last_frame_index() const noexcept -> std::optional<std::uint16_t> {
    return last_frame_index_;
  }

  /// @brief Tells whether push() would refuse a unit of a frame as stale: the
  /// frame is older than the one seen last, or is that one and it finished.
  ///
  /// Once true for a frame it stays true until reset(), while frames advance
  /// by less than half the index range.
  ///
  /// @param frame_index The frame.
  /// @return True when the frame is stale.
  auto stale(std::uint16_t frame_index) const noexcept -> bool;

  /// @brief Forgets everything, losses included.
  void reset() noexcept;

 private:
  /// @brief One received unit of the frame in progress.
  struct Slot {
    /// @brief The payload.
    std::vector<std::byte> bytes;
    /// @brief The video word: picture blocks the unit covers.
    std::uint16_t word{};
    /// @brief Whether it is a source unit.
    bool source{};
    /// @brief Whether its blocks were counted.
    bool counted{};
    /// @brief The slice it belongs to, when stated.
    std::optional<takion::VideoSliceExtension> slice;
  };

  /// @brief Recovers the missing source units with the Cauchy decoder.
  ///
  /// @return The frame.
  /// @retval VideoAssemblerError::invalid_unit The parity units differ in size,
  ///     a source's padding count is wrong, or decoding failed.
  /// @retval VideoAssemblerError::frame_too_large The frame would be too large.
  [[nodiscard]] auto try_recover()
      -> Expected<AssembledVideoFrame, VideoAssemblerError>;

  /// @brief Joins the source units, dropping their padding counts, and ends
  /// the frame.
  ///
  /// @return The frame.
  auto concatenate() -> AssembledVideoFrame;

  /// @brief Whether every source unit has arrived.
  ///
  /// @return True when complete.
  auto sources_complete() const noexcept -> bool;

  /// @brief Counts the stored source units below the source count, once
  /// when the count becomes known; later units are counted as they arrive.
  void count_sources_in_place() noexcept;

  /// @brief Adds a source unit's blocks to the coverage, once its whole slice
  /// has arrived.
  ///
  /// @param index The unit's index.
  void account_coverage(std::size_t index);

  /// @brief Refuses a unit, giving up the frame when it was the last.
  ///
  /// @param unit The unit.
  /// @param error Why.
  /// @return The error.
  [[nodiscard]] auto reject_unit(const takion::AvPacketValues& unit,
                                 VideoAssemblerError error)
      -> Expected<std::optional<AssembledVideoFrame>, VideoAssemblerError>;

  /// @brief Reports the frame in progress, and any before it, as lost.
  void report_failed_frame() noexcept;

  /// @brief Queues a loss, dropping the oldest when full.
  ///
  /// @param loss The loss.
  void push_loss(VideoFrameLoss loss);

  /// @brief Starts a new frame.
  ///
  /// @param unit Its first unit.
  /// @param expected_blocks The picture's block count.
  void start(const takion::AvPacketValues& unit, std::uint32_t expected_blocks);

  /// @brief Ends the frame in progress.
  void clear_active() noexcept;

  /// @brief Recovers lost units from parity units.
  CauchyFec8Decoder decoder_;
  /// @brief The frame seen last.
  std::optional<std::uint16_t> last_frame_index_;
  /// @brief The frame delivered last.
  std::uint16_t last_complete_{};
  /// @brief Losses not yet taken.
  std::deque<VideoFrameLoss> losses_;
  /// @brief The units of the frame in progress, by unit index.
  std::vector<std::optional<Slot>> slots_;
  /// @brief Source units received.
  std::size_t source_received_{};
  /// @brief Parity units received.
  std::size_t parity_received_{};
  /// @brief Bytes held.
  std::size_t stored_bytes_{};
  /// @brief The number of source units, once known.
  std::optional<std::size_t> source_count_;
  /// @brief Stored source units whose index is below the source count.
  std::size_t sources_in_place_{};
  /// @brief The number of parity units.
  std::size_t parity_count_{};
  /// @brief Picture blocks covered so far.
  std::uint32_t coverage_{};
  /// @brief The picture's block count, or 0.
  std::uint32_t expected_blocks_{};
  /// @brief The frame's codec byte.
  std::uint8_t codec_{};
  /// @brief The frame's picture size index.
  std::uint8_t adaptive_stream_index_{};
  /// @brief Whether counts were ignored.
  bool counts_ignored_{};
  /// @brief Whether a unit set the dynamic range flag.
  bool dynamic_range_flag_{};
  /// @brief Whether a unit asked that the frame not be shown.
  bool suppress_presentation_{};
  /// @brief Whether a frame is in progress.
  bool active_{};
};

}  // namespace libchiaking::net::protocol::remote_play
