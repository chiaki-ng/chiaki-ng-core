/// @file
/// @brief Declares VideoBitstream and VideoDependencies, which read just
/// enough of each H.264 or H.265 frame to know what it references, so frames
/// that depend on a lost frame are dropped or, for H.265, repointed at an
/// older frame that did arrive.
///
/// Every frame begins with a 4-byte Annex B start code. The readers strip
/// emulation-prevention bytes and parse the SPS for the width of frame_num
/// (H.264) or of the picture order count LSB (H.265), then each slice header
/// for its type and its first reference.
#pragma once

#include "libchiaking/detail/expected.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace libchiaking::net::protocol::remote_play {

/// @brief The NAL unit types the readers act on.
enum class NalType : unsigned {
  /// @brief H.264 non-IDR slice.
  h264_slice = 1,
  /// @brief H.264 IDR slice.
  h264_idr = 5,
  /// @brief H.264 sequence parameter set.
  h264_sps = 7,
  /// @brief H.265 TRAIL_R slice (a referenced non-IRAP picture).
  hevc_trail_r = 1,
  /// @brief H.265 IDR_N_LP slice.
  hevc_idr_n_lp = 20,
  /// @brief H.265 video parameter set.
  hevc_vps = 32,
  /// @brief H.265 sequence parameter set.
  hevc_sps = 33,
  /// @brief H.265 picture parameter set.
  hevc_pps = 34,
};

/// @brief The NAL type bits of an H.264 NAL header byte.
inline constexpr unsigned h264_nal_type_mask = 31U;
/// @brief The NAL type bits of an H.265 NAL header byte after shifting right
/// by one.
inline constexpr unsigned hevc_nal_type_mask = 63U;
/// @brief VideoSlice::reference meaning the slice names no reference.
inline constexpr std::uint32_t video_slice_no_reference = 0xff;
/// @brief How many earlier frames are tracked, and the most short-term
/// references an H.265 slice may list.
inline constexpr std::size_t hevc_max_reference_slots = 16;
/// @brief How many unescaped header bytes the reader holds.
inline constexpr std::size_t rbsp_scratch_capacity = 512;
/// @brief How far into a frame the start code is looked for, in bytes.
inline constexpr std::size_t start_code_search_window = 64;
/// @brief The first H.265 NAL type that is not a slice.
inline constexpr unsigned hevc_first_non_slice_type = 32;
/// @brief The H.265 NAL types from BLA_W_LP to RSV_IRAP_VCL23, whose slice
/// headers carry no_output_of_prior_pics_flag.
inline constexpr unsigned hevc_first_irap_type = 16;
/// @brief The last H.265 IRAP NAL type.
inline constexpr unsigned hevc_last_irap_type = 23;
/// @brief The smallest width of frame_num or the POC LSB, in bits
/// (log2_max_*_minus4 is added to it).
inline constexpr unsigned frame_num_bits_base = 4;
/// @brief The largest log2_max_*_minus4 accepted.
inline constexpr std::uint32_t max_frame_num_bits_extra = 12;

/// @brief What a slice is, for dependency tracking.
enum class VideoSliceType : std::uint8_t { unknown, intra, predicted };
/// @brief The first slice of a frame, as far as dependencies go.
struct VideoSlice {
  /// @brief The slice type.
  VideoSliceType type{VideoSliceType::unknown};
  /// @brief Which earlier frame it references: 0 is the frame just before;
  /// video_slice_no_reference when none is named.
  std::uint32_t reference{video_slice_no_reference};
};

/// @brief Reads SPS and slice headers of one codec.
///
/// Remembers the frame number width of the last SPS. Not thread-safe.
class VideoBitstream {
  /// @brief A bit reader over a NAL unit with emulation-prevention bytes
  /// removed, remembering where each byte came from.
  class Reader {
   public:
    /// @brief Why a read failed.
    enum class Error : std::uint8_t {
      /// @brief The read runs past the unescaped bytes, or asks for more than
      /// 32 bits.
      past_end,
      /// @brief An Exp-Golomb code has more than 31 leading zeros.
      oversized_code,
    };

    /// @brief Unescapes up to rbsp_scratch_capacity bytes.
    ///
    /// @param data The frame.
    /// @param offset Where the NAL payload starts.
    explicit Reader(std::span<const std::byte> data, std::size_t offset);
    /// @brief Reads bits, most significant first.
    ///
    /// @param count At most 32.
    /// @return The value.
    /// @retval Error::past_end Too few bits remain, or @p count is above 32.
    [[nodiscard]] auto bits(unsigned count) -> Expected<std::uint32_t, Error>;
    /// @brief Reads one bit as a flag.
    ///
    /// @return Whether the bit is set.
    /// @retval Error::past_end No bit remains.
    [[nodiscard]] auto flag() -> Expected<bool, Error>;
    /// @brief Skips bits.
    ///
    /// @param count Any number.
    /// @return Nothing.
    /// @retval Error::past_end Too few bits remain.
    [[nodiscard]] auto skip(unsigned count) -> Expected<void, Error>;
    /// @brief Reads an Exp-Golomb unsigned value.
    ///
    /// @return The value.
    /// @retval Error::past_end The code runs past the end.
    /// @retval Error::oversized_code The code has too many leading zeros.
    [[nodiscard]] auto ue() -> Expected<std::uint32_t, Error>;
    /// @brief Skips Exp-Golomb unsigned values.
    ///
    /// @param count How many.
    /// @return Nothing.
    /// @retval Error Why a value could not be read.
    [[nodiscard]] auto skip_ue(unsigned count) -> Expected<void, Error>;
    /// @brief Where the next bit lies in the original frame.
    ///
    /// @return The bit offset into the frame, or nothing past the end.
    auto position() const -> std::optional<std::size_t>;

   private:
    /// @brief The unescaped bytes.
    std::array<std::uint8_t, rbsp_scratch_capacity> bytes_{};
    /// @brief Each unescaped byte's offset in the frame.
    std::array<std::size_t, rbsp_scratch_capacity> offsets_{};
    /// @brief How many bytes were unescaped.
    std::size_t size_{};
    /// @brief The next bit to read.
    std::size_t bit_{};
  };

 public:
  /// @brief Makes a reader for one codec.
  ///
  /// @param hevc True for H.265, false for H.264.
  explicit VideoBitstream(bool hevc = false) noexcept;

  /// @brief Reads the frame number width from a codec header.
  ///
  /// For H.265 a leading VPS is skipped, and the picture size in coding blocks
  /// and a following PPS are read too, so that slices after the first in a
  /// frame can be parsed. An H.264 SPS of a high profile with separate colour
  /// planes is refused.
  ///
  /// @param bytes The codec header.
  /// @return True when an SPS was read; the width falls back to 4 bits
  ///     otherwise.
  auto header(std::span<const std::byte> bytes) -> bool;

  /// @brief Reads a frame's first slice header.
  ///
  /// @param bytes The frame.
  /// @return The slice, or nothing when it is not a recognised slice or its
  ///     header is malformed.
  auto slice(std::span<const std::byte> bytes) const
      -> std::optional<VideoSlice>;

  /// @brief Repoints an H.265 TRAIL_R frame at an older reference by rewriting
  /// the used_by_curr_pic flags of the short-term reference set of every
  /// slice in the frame, in place.
  ///
  /// Dependent slice segments carry no reference set and are left alone. The
  /// frame is changed only when every other slice can be rewritten.
  ///
  /// @param bytes The frame, changed in place.
  /// @param reference The reference to use instead, 0 to 15.
  /// @return True when rewritten; false for H.264 or a frame that does not
  ///     allow it, which is then unchanged.
  auto set_reference(std::span<std::byte> bytes, unsigned reference) const
      -> bool;

 private:
  /// @brief Finds a 4-byte start code within start_code_search_window bytes.
  ///
  /// @param bytes The frame.
  /// @param begin Where to start looking.
  /// @return The offset just after the start code, or nothing.
  static auto start_code(std::span<const std::byte> bytes, std::size_t begin)
      -> std::optional<std::size_t>;
  /// @brief Finds the next 3- or 4-byte start code anywhere after a point.
  ///
  /// @param bytes The frame.
  /// @param begin Where to start looking.
  /// @return The offset just after the start code, or nothing.
  static auto next_unit(std::span<const std::byte> bytes, std::size_t begin)
      -> std::optional<std::size_t>;
  /// @brief Reads the H.265 SPS fields after the POC LSB width that give the
  /// picture size in coding tree blocks, and sets the slice address width.
  ///
  /// Leaves the width unknown when the fields cannot be read.
  ///
  /// @param reader The reader, positioned after log2_max_pic_order_cnt_lsb.
  /// @param sub_layers sps_max_sub_layers_minus1.
  /// @param width pic_width_in_luma_samples.
  /// @param height pic_height_in_luma_samples.
  void set_address_bits(Reader& reader, std::uint32_t sub_layers,
                        std::uint32_t width, std::uint32_t height);
  /// @brief Reads the picture size in coding tree blocks.
  ///
  /// @param reader The reader, positioned after log2_max_pic_order_cnt_lsb.
  /// @return log2 of the coding tree block size.
  /// @retval Reader::Error Why a field could not be read.
  static auto ctb_log2(Reader& reader) -> Expected<std::uint32_t, Reader::Error>;
  /// @brief Reads the H.265 SPS from its first field to the frame number
  /// width, and sets it and the slice address width.
  ///
  /// @param reader The reader, positioned after the NAL header.
  /// @return False when the SPS is malformed.
  auto read_hevc_sps(Reader& reader) -> bool;
  /// @brief Reads the H.264 SPS and sets the frame number width.
  ///
  /// @param reader The reader, positioned after the NAL header.
  /// @return False when the SPS is malformed or has separate colour planes.
  auto read_h264_sps(Reader& reader) -> bool;
  /// @brief Reads an H.264 P slice's reference list modification.
  ///
  /// @param reader The reader, positioned after the slice's frame number.
  /// @param result The slice so far.
  /// @return The slice with its reference, or nothing when malformed.
  auto h264_reference(Reader& reader, VideoSlice result) const
      -> std::optional<VideoSlice>;
  /// @brief Reads an H.265 TRAIL_R slice's short-term reference set.
  ///
  /// @param reader The reader, positioned after the slice type.
  /// @param result The slice so far.
  /// @return The slice with its reference, or nothing when malformed.
  auto hevc_reference(Reader& reader, VideoSlice result) const
      -> std::optional<VideoSlice>;
  /// @brief Finds the used_by_curr_pic flags of one slice to rewrite.
  ///
  /// @param reader The reader, positioned after the slice type.
  /// @param reference The reference to use instead.
  /// @return The flags' bit positions, or nothing when the slice does not
  ///     allow it.
  auto reference_flags(Reader& reader, unsigned reference) const
      -> std::optional<std::vector<std::size_t>>;
  /// @brief Reads the H.265 PPS fields that shape slice headers.
  ///
  /// @param bytes The codec header.
  /// @param offset Where the PPS NAL header starts.
  void read_pps(std::span<const std::byte> bytes, std::size_t offset);
  /// @brief The start of an H.265 slice segment header.
  struct HevcSliceStart {
    /// @brief Whether it is a dependent slice segment, whose header ends at
    /// its address.
    bool dependent{};
    /// @brief The slice type; meaningless for a dependent segment.
    std::uint32_t slice_type{};
  };
  /// @brief Reads an H.265 slice header up to its slice type.
  ///
  /// @param reader The reader, positioned after the NAL header.
  /// @param type The NAL type.
  /// @return The header start, or nothing when it is malformed or follows the
  ///     first slice while the address width is unknown.
  auto hevc_slice_start(Reader& reader, NalType type) const
      -> std::optional<HevcSliceStart>;
  /// @brief Reads log2_max_*_minus4 and sets the frame number width.
  ///
  /// @param reader The reader, positioned at the field.
  /// @return False when the value is malformed or too large.
  auto set_frame_bits(Reader& reader) -> bool;
  /// @brief Reads an H.265 slice header.
  ///
  /// @param bytes The frame.
  /// @param offset Where its NAL header starts.
  /// @return The slice, or nothing.
  auto hevc_slice(std::span<const std::byte> bytes, std::size_t offset) const
      -> std::optional<VideoSlice>;
  /// @brief Whether the codec is H.265.
  bool hevc_{};
  /// @brief The frame number (or POC LSB) width, in bits.
  unsigned frame_bits_{frame_num_bits_base};
  /// @brief The H.265 slice_segment_address width, in bits; 0 while unknown,
  /// so slices after the first cannot be read.
  unsigned address_bits_{};
  /// @brief Whether the H.265 PPS enables dependent slice segments.
  bool dependent_slices_{};
  /// @brief Whether H.265 slice headers carry pic_output_flag.
  bool output_flag_{};
  /// @brief The H.265 num_extra_slice_header_bits.
  unsigned extra_slice_bits_{};
};

/// @brief Whether a frame may be passed on.
struct VideoAcceptance {
  /// @brief Whether the frame is accepted.
  bool accepted{};
  /// @brief Whether it was accepted after being repointed at an older
  /// reference.
  bool repaired{};
  /// @brief Whether the frame is accepted.
  ///
  /// @return accepted.
  explicit operator bool() const noexcept { return accepted; }
};

/// @brief Tracks which recent frames arrived and drops frames whose
/// reference did not.
///
/// After a loss the stream asks the console for a keyframe; until an intra
/// frame arrives every frame is refused. Not thread-safe.
class VideoDependencies {
 public:
  /// @brief Makes a tracker for one codec.
  ///
  /// @param hevc True for H.265.
  explicit VideoDependencies(bool hevc = false) noexcept : bitstream_(hevc) {}
  /// @brief Reads a new codec header.
  ///
  /// @param bytes The header.
  void header(std::span<const std::byte> bytes) {
    (void)bitstream_.header(bytes);
  }
  /// @brief Starts waiting for a keyframe.
  ///
  /// @return True when it was not waiting already, so a request should be
  ///     sent.
  auto request_idr() noexcept -> bool;
  /// @brief Stops waiting for a keyframe.
  void cancel_idr() noexcept { waiting_for_idr_ = false; }
  /// @brief Whether it is waiting for a keyframe.
  ///
  /// @return True while waiting.
  auto waiting_for_idr() const noexcept -> bool { return waiting_for_idr_; }
  /// @brief Forgets the frames in an index range, which wraps.
  ///
  /// @param first The first lost index.
  /// @param last The last lost index.
  void invalidate(std::uint16_t first, std::uint16_t last) noexcept;
  /// @brief Forgets every frame.
  void invalidate_all() noexcept { frames_.fill(std::nullopt); }
  /// @brief Decides whether a frame may be passed on, and records it.
  ///
  /// A predicted frame whose reference is missing is repointed at the nearest
  /// older frame that arrived (H.265 only) or refused. Frames that cannot be
  /// parsed are accepted.
  ///
  /// @param index The frame's index.
  /// @param bytes The frame, which may be rewritten in place.
  /// @return Whether it is accepted and whether it was repaired.
  auto accept(std::uint16_t index, std::span<std::byte> bytes)
      -> VideoAcceptance;

 private:
  /// @brief Whether a frame index arrived recently.
  ///
  /// @param index The index.
  /// @return True when tracked.
  auto contains(std::uint16_t index) const noexcept -> bool {
    return std::find(frames_.begin(), frames_.end(), std::optional{index}) !=
           frames_.end();
  }
  /// @brief The slice reader.
  VideoBitstream bitstream_;
  /// @brief The indexes of the last frames accepted, as a ring.
  std::array<std::optional<std::uint16_t>, hevc_max_reference_slots> frames_{};
  /// @brief The next ring slot.
  std::size_t next_{};
  /// @brief Whether frames are refused until a keyframe.
  bool waiting_for_idr_{};
};

/// @var VideoSliceType::unknown
/// @brief The slice type could not be classified.
/// @var VideoSliceType::intra
/// @brief An I slice, which depends on no earlier frame.
/// @var VideoSliceType::predicted
/// @brief A P slice, which depends on an earlier frame.
}  // namespace libchiaking::net::protocol::remote_play
