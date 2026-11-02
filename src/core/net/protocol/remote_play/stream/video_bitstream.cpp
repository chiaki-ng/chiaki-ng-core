#include "src/core/net/protocol/remote_play/stream/video_bitstream.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <span>
#include <vector>

namespace libchiaking::net::protocol::remote_play {

VideoBitstream::Reader::Reader(std::span<const std::byte> data,
                               std::size_t offset) {
  unsigned zeros = 0;
  for (std::size_t index = offset; index < data.size() && size_ < bytes_.size();
       ++index) {
    const auto value = std::to_integer<std::uint8_t>(data[index]);
    if (zeros >= 2 && value == 3) {
      zeros = 0;
      continue;
    }
    bytes_[size_] = value;
    offsets_[size_++] = index;
    zeros = value == 0 ? zeros + 1 : 0;
  }
}

auto VideoBitstream::Reader::bits(unsigned count)
    -> Expected<std::uint32_t, Error> {
  if (count > 32 || bit_ + count > size_ * 8)
    return unexpected(Error::past_end);
  std::uint32_t value = 0;
  for (unsigned index = 0; index < count; ++index, ++bit_)
    value = (value << 1U) | ((bytes_[bit_ / 8] >> (7U - bit_ % 8)) & 1U);
  return value;
}

auto VideoBitstream::Reader::flag() -> Expected<bool, Error> {
  return bits(1).transform([](std::uint32_t bit) { return bit != 0; });
}

auto VideoBitstream::Reader::skip(unsigned count) -> Expected<void, Error> {
  if (bit_ + count > size_ * 8) return unexpected(Error::past_end);
  bit_ += count;
  return {};
}

auto VideoBitstream::Reader::ue() -> Expected<std::uint32_t, Error> {
  unsigned zeros = 0;
  for (;;) {
    const auto bit = bits(1);
    if (!bit) return unexpected(bit.error());
    if (bit.value() != 0) break;
    if (++zeros > 31) return unexpected(Error::oversized_code);
  }
  const auto suffix = bits(zeros);
  if (!suffix) return unexpected(suffix.error());
  return ((std::uint32_t{1} << zeros) - 1U) + suffix.value();
}

auto VideoBitstream::Reader::skip_ue(unsigned count) -> Expected<void, Error> {
  for (unsigned index = 0; index < count; ++index)
    if (const auto value = ue(); !value) return unexpected(value.error());
  return {};
}

auto VideoBitstream::Reader::position() const -> std::optional<std::size_t> {
  if (bit_ >= size_ * 8) return std::nullopt;
  return offsets_[bit_ / 8] * 8 + bit_ % 8;
}

VideoBitstream::VideoBitstream(bool hevc) noexcept : hevc_(hevc) {}

auto VideoBitstream::header(std::span<const std::byte> bytes) -> bool {
  frame_bits_ = frame_num_bits_base;
  address_bits_ = 0;
  dependent_slices_ = false;
  output_flag_ = false;
  extra_slice_bits_ = 0;
  auto offset = start_code(bytes, 0);
  if (!offset) return false;
  if (hevc_) {
    for (unsigned count = 0; count < 8 && *offset + 2 <= bytes.size();
         ++count) {
      const auto type = static_cast<NalType>(
          (std::to_integer<unsigned>(bytes[*offset]) >> 1U) &
          hevc_nal_type_mask);
      if (type == NalType::hevc_vps) {
        offset = start_code(bytes, *offset + 2);
        if (!offset) return false;
        continue;
      }
      if (type != NalType::hevc_sps) return false;
      Reader reader{bytes, *offset + 2};
      if (!read_hevc_sps(reader)) return false;
      for (auto next = next_unit(bytes, *offset + 2);
           next && *next + 2 <= bytes.size();
           next = next_unit(bytes, *next + 2)) {
        if (static_cast<NalType>(
                (std::to_integer<unsigned>(bytes[*next]) >> 1U) &
                hevc_nal_type_mask) == NalType::hevc_pps) {
          read_pps(bytes, *next);
          break;
        }
      }
      return true;
    }
    return false;
  }
  if (*offset >= bytes.size() ||
      static_cast<NalType>(std::to_integer<unsigned>(bytes[*offset]) &
                           h264_nal_type_mask) != NalType::h264_sps)
    return false;
  Reader reader{bytes, *offset + 1};
  return read_h264_sps(reader);
}

auto VideoBitstream::read_hevc_sps(Reader& reader) -> bool {
  if (!reader.skip(4)) return false;
  const auto sub_layers = reader.bits(3);
  if (!sub_layers || !reader.skip(1) || !reader.skip(96) || !reader.ue())
    return false;
  const auto chroma = reader.ue();
  if (!chroma || (chroma.value() == 3 && !reader.skip(1))) return false;
  const auto width = reader.ue();
  const auto height = reader.ue();
  const auto cropped = reader.flag();
  if (!width || !height || !cropped) return false;
  if (cropped.value() && !reader.skip_ue(4)) return false;
  if (!reader.skip_ue(2) || !set_frame_bits(reader)) return false;
  set_address_bits(reader, sub_layers.value(), width.value(), height.value());
  return true;
}

auto VideoBitstream::read_h264_sps(Reader& reader) -> bool {
  const auto profile = reader.bits(8);
  if (!profile || !reader.skip(16) || !reader.ue()) return false;
  constexpr std::array high_profiles{100U, 110U, 122U, 244U, 44U,  83U, 86U,
                                     118U, 128U, 138U, 139U, 134U, 135U};
  if (std::find(high_profiles.begin(), high_profiles.end(), profile.value()) !=
      high_profiles.end()) {
    const auto chroma = reader.ue();
    if (!chroma || (chroma.value() == 3 && !reader.skip(1))) return false;
    if (!reader.skip_ue(2) || !reader.skip(1)) return false;
    const auto separate_planes = reader.flag();
    if (!separate_planes || separate_planes.value()) return false;
  }
  return set_frame_bits(reader);
}

auto VideoBitstream::slice(std::span<const std::byte> bytes) const
    -> std::optional<VideoSlice> {
  const auto offset = start_code(bytes, 0);
  if (!offset) return std::nullopt;
  if (hevc_) return hevc_slice(bytes, *offset);
  const auto type = static_cast<NalType>(
      std::to_integer<unsigned>(bytes[*offset]) & h264_nal_type_mask);
  if (type != NalType::h264_slice && type != NalType::h264_idr)
    return std::nullopt;
  Reader reader{bytes, *offset + 1};
  if (!reader.ue()) return std::nullopt;
  const auto slice_type = reader.ue();
  if (!slice_type) return std::nullopt;
  const VideoSlice result{
      slice_type.value() == 0 || slice_type.value() == 5
          ? VideoSliceType::predicted
      : slice_type.value() == 2 || slice_type.value() == 7
          ? VideoSliceType::intra
          : VideoSliceType::unknown,
      0};
  if (type != NalType::h264_slice) return result;
  return h264_reference(reader, result);
}

auto VideoBitstream::h264_reference(Reader& reader, VideoSlice result) const
    -> std::optional<VideoSlice> {
  if (!reader.ue() || !reader.skip(frame_bits_)) return std::nullopt;
  const auto overridden = reader.flag();
  if (!overridden) return std::nullopt;
  if (overridden.value()) {
    const auto counted = reader.flag();
    if (!counted || (counted.value() && !reader.ue())) return std::nullopt;
  }
  const auto modified = reader.flag();
  if (!modified) return std::nullopt;
  if (!modified.value()) return result;
  for (unsigned index = 0; index < 3; ++index) {
    const auto modification = reader.ue();
    if (!modification || modification.value() > 3) return std::nullopt;
    if (modification.value() == 3) return result;
    const auto reference = reader.ue();
    if (!reference) return std::nullopt;
    if (modification.value() == 0) result.reference = reference.value();
  }
  return std::nullopt;
}

auto VideoBitstream::set_reference(std::span<std::byte> bytes,
                                   unsigned reference) const -> bool {
  if (!hevc_ || reference >= hevc_max_reference_slots) return false;
  std::vector<std::size_t> flags;
  for (auto offset = start_code(bytes, 0);
       offset && *offset + 2 <= bytes.size();
       offset = next_unit(bytes, *offset + 2)) {
    const auto value =
        (std::to_integer<unsigned>(bytes[*offset]) >> 1U) & hevc_nal_type_mask;
    if (value >= hevc_first_non_slice_type) continue;
    const auto type = static_cast<NalType>(value);
    if (type != NalType::hevc_trail_r) return false;
    Reader reader{bytes, *offset + 2};
    const auto start = hevc_slice_start(reader, type);
    if (!start) return false;
    if (start->dependent) continue;
    const auto slice = reference_flags(reader, reference);
    if (!slice) return false;
    flags.insert(flags.end(), slice->begin(), slice->end());
  }
  if (flags.empty()) return false;
  for (std::size_t index = 0; index < flags.size(); ++index) {
    const auto mask = static_cast<std::byte>(1U << (7U - flags[index] % 8));
    if (index % (reference + 1) == reference)
      bytes[flags[index] / 8] |= mask;
    else
      bytes[flags[index] / 8] &= ~mask;
  }
  return true;
}

auto VideoBitstream::reference_flags(Reader& reader, unsigned reference) const
    -> std::optional<std::vector<std::size_t>> {
  if (!reader.skip(frame_bits_)) return std::nullopt;
  const auto from_sps = reader.flag();
  if (!from_sps || from_sps.value()) return std::nullopt;
  const auto negatives = reader.ue();
  if (!negatives || negatives.value() > hevc_max_reference_slots ||
      reference >= negatives.value() || !reader.ue())
    return std::nullopt;
  std::vector<std::size_t> flags;
  for (unsigned index = 0; index <= reference; ++index) {
    if (!reader.ue()) return std::nullopt;
    const auto position = reader.position();
    if (!position) return std::nullopt;
    flags.push_back(*position);
    if (!reader.skip(1)) return std::nullopt;
  }
  return flags;
}

auto VideoBitstream::start_code(std::span<const std::byte> bytes,
                                std::size_t begin)
    -> std::optional<std::size_t> {
  constexpr std::array<std::byte, 4> marker{std::byte{0}, std::byte{0},
                                            std::byte{0}, std::byte{1}};
  constexpr std::size_t trailing = 1;
  if (bytes.size() < begin + marker.size() + trailing) return std::nullopt;
  const auto last_start =
      std::min(begin + start_code_search_window, bytes.size() - marker.size());
  const auto window =
      bytes.subspan(begin, last_start - begin + marker.size() - 1);
  const auto found = std::ranges::search(window, marker);
  if (found.empty()) return std::nullopt;
  return begin + static_cast<std::size_t>(found.begin() - window.begin()) +
         marker.size();
}

auto VideoBitstream::next_unit(std::span<const std::byte> bytes,
                               std::size_t begin)
    -> std::optional<std::size_t> {
  constexpr std::array<std::byte, 3> marker{std::byte{0}, std::byte{0},
                                            std::byte{1}};
  if (begin >= bytes.size()) return std::nullopt;
  const auto found = std::ranges::search(bytes.subspan(begin), marker);
  if (found.empty()) return std::nullopt;
  return static_cast<std::size_t>(found.end() - bytes.begin());
}

void VideoBitstream::set_address_bits(Reader& reader, std::uint32_t sub_layers,
                                      std::uint32_t width,
                                      std::uint32_t height) {
  if (sub_layers != 0) return;
  const auto ctb = ctb_log2(reader);
  if (!ctb || ctb.value() > 6 || width == 0 || height == 0) return;
  const auto size = 1U << ctb.value();
  const auto blocks = ((width + size - 1U) >> ctb.value()) *
                      ((height + size - 1U) >> ctb.value());
  address_bits_ =
      blocks > 1 ? static_cast<unsigned>(std::bit_width(blocks - 1U)) : 1U;
}

auto VideoBitstream::ctb_log2(Reader& reader)
    -> Expected<std::uint32_t, Reader::Error> {
  if (const auto skipped = reader.skip(1); !skipped)
    return unexpected(skipped.error());
  if (const auto skipped = reader.skip_ue(3); !skipped)
    return unexpected(skipped.error());
  const auto minimum = reader.ue();
  if (!minimum) return unexpected(minimum.error());
  const auto difference = reader.ue();
  if (!difference) return unexpected(difference.error());
  return minimum.value() + 3U + difference.value();
}

void VideoBitstream::read_pps(std::span<const std::byte> bytes,
                              std::size_t offset) {
  Reader reader{bytes, offset + 2};
  if (!reader.skip_ue(2)) return;
  const auto dependent = reader.flag();
  const auto output = reader.flag();
  const auto extra = reader.bits(3);
  if (!dependent || !output || !extra) return;
  dependent_slices_ = dependent.value();
  output_flag_ = output.value();
  extra_slice_bits_ = extra.value();
}

auto VideoBitstream::hevc_slice_start(Reader& reader, NalType type) const
    -> std::optional<HevcSliceStart> {
  const auto value = static_cast<unsigned>(type);
  const auto first = reader.flag();
  if (!first) return std::nullopt;
  if (value >= hevc_first_irap_type && value <= hevc_last_irap_type &&
      !reader.skip(1))
    return std::nullopt;
  if (!reader.ue()) return std::nullopt;
  if (!first.value()) {
    if (address_bits_ == 0) return std::nullopt;
    bool dependent = false;
    if (dependent_slices_) {
      const auto flagged = reader.flag();
      if (!flagged) return std::nullopt;
      dependent = flagged.value();
    }
    if (!reader.skip(address_bits_)) return std::nullopt;
    if (dependent) return HevcSliceStart{true, 0};
  }
  if (!reader.skip(extra_slice_bits_)) return std::nullopt;
  const auto slice_type = reader.ue();
  if (!slice_type || (output_flag_ && !reader.skip(1))) return std::nullopt;
  return HevcSliceStart{false, slice_type.value()};
}

auto VideoBitstream::set_frame_bits(Reader& reader) -> bool {
  const auto bits = reader.ue();
  if (!bits || bits.value() > max_frame_num_bits_extra) return false;
  frame_bits_ = bits.value() + frame_num_bits_base;
  return true;
}

auto VideoBitstream::hevc_slice(std::span<const std::byte> bytes,
                                std::size_t offset) const
    -> std::optional<VideoSlice> {
  if (offset + 2 > bytes.size()) return std::nullopt;
  const auto type = static_cast<NalType>(
      (std::to_integer<unsigned>(bytes[offset]) >> 1U) & hevc_nal_type_mask);
  if (type != NalType::hevc_trail_r && type != NalType::hevc_idr_n_lp)
    return std::nullopt;
  Reader reader{bytes, offset + 2};
  const auto start = hevc_slice_start(reader, type);
  if (!start || start->dependent) return std::nullopt;
  const VideoSlice result{start->slice_type == 1   ? VideoSliceType::predicted
                          : start->slice_type == 2 ? VideoSliceType::intra
                                                   : VideoSliceType::unknown,
                          video_slice_no_reference};
  if (type != NalType::hevc_trail_r) return result;
  return hevc_reference(reader, result);
}

auto VideoBitstream::hevc_reference(Reader& reader, VideoSlice result) const
    -> std::optional<VideoSlice> {
  if (!reader.skip(frame_bits_)) return std::nullopt;
  const auto from_sps = reader.flag();
  if (!from_sps) return std::nullopt;
  if (from_sps.value()) return result;
  const auto negatives = reader.ue();
  if (!negatives || negatives.value() > hevc_max_reference_slots ||
      !reader.ue())
    return std::nullopt;
  for (unsigned index = 0; index < negatives.value(); ++index) {
    if (!reader.ue()) return std::nullopt;
    const auto used = reader.flag();
    if (!used) return std::nullopt;
    if (used.value()) {
      result.reference = index;
      break;
    }
  }
  return result;
}

auto VideoDependencies::request_idr() noexcept -> bool {
  if (waiting_for_idr_) return false;
  waiting_for_idr_ = true;
  return true;
}

void VideoDependencies::invalidate(std::uint16_t first,
                                   std::uint16_t last) noexcept {
  const auto distance = static_cast<std::uint16_t>(last - first);
  for (auto& frame : frames_)
    if (frame && static_cast<std::uint16_t>(*frame - first) <= distance)
      frame.reset();
}

auto VideoDependencies::accept(std::uint16_t index, std::span<std::byte> bytes)
    -> VideoAcceptance {
  bool repaired = false;
  const auto slice = bitstream_.slice(bytes);
  if (slice) {
    if (waiting_for_idr_) {
      if (slice->type != VideoSliceType::intra) return {};
      waiting_for_idr_ = false;
    }
    if (slice->type == VideoSliceType::predicted &&
        slice->reference != video_slice_no_reference &&
        !contains(static_cast<std::uint16_t>(index - slice->reference - 1U))) {
      for (unsigned reference = slice->reference + 1;
           reference < hevc_max_reference_slots; ++reference) {
        if (contains(static_cast<std::uint16_t>(index - reference - 1U))) {
          repaired = bitstream_.set_reference(bytes, reference);
          break;
        }
      }
      if (!repaired) return {};
    }
  }
  frames_[next_] = index;
  next_ = (next_ + 1) % frames_.size();
  return {true, repaired};
}

}  // namespace libchiaking::net::protocol::remote_play
