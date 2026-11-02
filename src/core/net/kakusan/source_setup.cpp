#include "src/core/net/kakusan/source_setup.hpp"

#include "src/core/utils/bytes/endian.hpp"
#include "src/core/utils/memory/constant_time.hpp"

#include <algorithm>
#include <optional>
#include <utility>

namespace libchiaking::net::kakusan::control {

class SourceHeaders {
 public:
  static constexpr std::uint32_t sample_rate = 48000;
  static constexpr std::uint8_t sample_bits = 16;
  static constexpr std::uint8_t profile_baseline = 66;
  static constexpr std::uint8_t profile_high = 100;
  static constexpr std::uint8_t nal_type_mask = 0x1f;
  static constexpr std::uint8_t nal_type_sps = 7;
  static constexpr std::uint8_t nal_forbidden_bit = 0x80;
  static constexpr std::size_t sps_prefix_size = 4;
  static constexpr std::uint8_t h265_nal_type_mask = 0x3f;
  static constexpr std::uint8_t h265_nal_type_sps = 33;
  static constexpr std::uint8_t h265_layer_bit = 0x01;
  static constexpr std::size_t h265_nal_header_size = 2;
  static constexpr std::size_t h265_profile_prefix_size = 13;
  static constexpr std::size_t h265_profile_offset = 1;
  static constexpr std::size_t h265_level_offset = 12;
  static constexpr std::uint8_t h265_profile_space_mask = 0xc0;
  static constexpr std::uint8_t h265_profile_idc_mask = 0x1f;
  static constexpr std::uint8_t h265_profile_main = 1;
  static constexpr std::uint8_t h265_profile_main10 = 2;
  static constexpr std::uint8_t emulation_prevention = 3;
  static constexpr std::array<std::uint32_t, 6> opus_frame_sizes{
      120, 240, 480, 960, 1920, 2880};

  static auto byte_at(std::span<const std::byte> bytes,
                      std::size_t offset) noexcept -> std::uint8_t {
    return std::to_integer<std::uint8_t>(bytes[offset]);
  }

  static auto starts_unit(std::span<const std::byte> bytes,
                          std::size_t index) noexcept -> bool {
    return index + 2 < bytes.size() && byte_at(bytes, index) == 0 &&
           byte_at(bytes, index + 1) == 0 && byte_at(bytes, index + 2) == 1;
  }

  static auto is_h264_sps(std::uint8_t unit_header) noexcept -> bool {
    return (unit_header & nal_forbidden_bit) == 0 &&
           (unit_header & nal_type_mask) == nal_type_sps;
  }

  static auto is_h265_sps(std::uint8_t unit_header) noexcept -> bool {
    return (unit_header & nal_forbidden_bit) == 0 &&
           (unit_header & h265_layer_bit) == 0 &&
           ((unit_header >> 1U) & h265_nal_type_mask) == h265_nal_type_sps;
  }

  [[nodiscard]] static auto h265_profile_prefix(
      std::span<const std::byte> header, std::size_t offset)
      -> std::optional<std::array<std::uint8_t, h265_profile_prefix_size>> {
    std::array<std::uint8_t, h265_profile_prefix_size> prefix{};
    std::size_t filled = 0;
    std::size_t zeros = 0;
    for (std::size_t index = offset;
         index < header.size() && filled < prefix.size(); ++index) {
      const auto value = byte_at(header, index);
      if (zeros >= 2 && value == 1) return std::nullopt;
      if (zeros >= 2 && value == emulation_prevention) {
        zeros = 0;
        continue;
      }
      zeros = value == 0 ? zeros + 1 : 0;
      prefix[filled++] = value;
    }
    if (filled != prefix.size()) return std::nullopt;
    return prefix;
  }

  static constexpr auto valid_frame_size(std::uint32_t frame_size) noexcept
      -> bool {
    return std::ranges::find(opus_frame_sizes, frame_size) !=
           opus_frame_sizes.end();
  }
};

SourceKey::SourceKey(const std::array<std::byte, extent>& bytes) noexcept {
  std::ranges::copy(bytes, bytes_.begin());
}

SourceKey::SourceKey(SecretArray<extent> bytes) noexcept
    : bytes_(std::move(bytes)) {}

auto SourceKey::operator==(const SourceKey& other) const noexcept -> bool {
  return memory::constant_time_equal(std::span<const std::byte>{bytes_},
                                     std::span<const std::byte>{other.bytes_});
}

auto SourceSetup::validate_audio_header(std::span<const std::byte> header)
    -> Result<void> {
  if (header.size() != audio_header_size) return fail(Error::codec);
  const auto channels = SourceHeaders::byte_at(header, 0);
  if ((channels != 1 && channels != 2) ||
      SourceHeaders::byte_at(header, 1) != SourceHeaders::sample_bits ||
      bytes::load_be<std::uint32_t>(header.subspan(2)) !=
          SourceHeaders::sample_rate ||
      !SourceHeaders::valid_frame_size(
          bytes::load_be<std::uint32_t>(header.subspan(6))))
    return fail(Error::codec);
  return {};
}

auto SourceSetup::validate_h264_profile_header(
    std::span<const std::byte> header) -> Result<void> {
  for (std::size_t index = 0; index + 2 < header.size(); ++index) {
    if (!SourceHeaders::starts_unit(header, index)) continue;
    const auto nal = index + 3;
    if (nal >= header.size()) return fail(Error::codec);
    if (!SourceHeaders::is_h264_sps(SourceHeaders::byte_at(header, nal)))
      continue;
    if (header.size() - nal < SourceHeaders::sps_prefix_size)
      return fail(Error::codec);
    const auto profile = SourceHeaders::byte_at(header, nal + 1);
    const auto level = SourceHeaders::byte_at(header, nal + 3);
    if ((profile != SourceHeaders::profile_baseline &&
         profile != SourceHeaders::profile_high) ||
        level == 0 || level > max_h264_level_idc)
      return fail(Error::codec);
    return {};
  }
  return fail(Error::codec);
}

auto SourceSetup::validate_h265_profile_header(
    std::span<const std::byte> header) -> Result<void> {
  for (std::size_t index = 0; index + 2 < header.size(); ++index) {
    if (!SourceHeaders::starts_unit(header, index)) continue;
    const auto nal = index + 3;
    if (nal >= header.size()) return fail(Error::codec);
    if (!SourceHeaders::is_h265_sps(SourceHeaders::byte_at(header, nal)))
      continue;
    const auto prefix = SourceHeaders::h265_profile_prefix(
        header, nal + SourceHeaders::h265_nal_header_size);
    if (!prefix) return fail(Error::codec);
    const auto profile = (*prefix)[SourceHeaders::h265_profile_offset];
    const auto profile_idc = static_cast<std::uint8_t>(
        profile & SourceHeaders::h265_profile_idc_mask);
    const auto level = (*prefix)[SourceHeaders::h265_level_offset];
    if ((profile & SourceHeaders::h265_profile_space_mask) != 0 ||
        (profile_idc != SourceHeaders::h265_profile_main &&
         profile_idc != SourceHeaders::h265_profile_main10) ||
        level == 0 || level > max_h265_level_idc)
      return fail(Error::codec);
    return {};
  }
  return fail(Error::codec);
}

auto SourceSetup::detect_codec(std::span<const std::byte> header)
    -> Result<SourceCodec> {
  for (std::size_t index = 0; index + 3 < header.size(); ++index) {
    if (!SourceHeaders::starts_unit(header, index)) continue;
    const auto unit_header = SourceHeaders::byte_at(header, index + 3);
    if (SourceHeaders::is_h264_sps(unit_header)) return SourceCodec::h264;
    if (SourceHeaders::is_h265_sps(unit_header)) return SourceCodec::h265;
  }
  return fail(Error::codec);
}

auto SourceSetup::validate_profile_header(SourceCodec codec,
                                          std::span<const std::byte> header)
    -> Result<void> {
  return codec == SourceCodec::h265 ? validate_h265_profile_header(header)
                                    : validate_h264_profile_header(header);
}

auto SourceSetup::valid_fps(std::uint32_t fps) noexcept -> bool {
  return fps >= 1 && fps <= max_video_fps;
}

auto SourceSetup::valid_geometry(std::uint32_t width, std::uint32_t height,
                                 std::size_t header_size) noexcept -> bool {
  return width >= 1 && width <= max_video_width && height >= 1 &&
         height <= max_video_height && header_size >= 1 &&
         header_size <= max_profile_header_size;
}

auto SourceSetup::well_formed() const noexcept -> bool {
  return supported_protocol(source_protocol) && valid_fps(video_fps) &&
         !video_profiles.empty() &&
         video_profiles.size() <= max_video_profiles &&
         std::ranges::all_of(video_profiles, [](const VideoProfile& profile) {
           return valid_geometry(profile.width, profile.height,
                                 profile.header.size());
         });
}

auto SourceSetup::validate() const -> Result<void> {
  if (!well_formed()) return fail(Error::codec);
  return validate_audio_header(audio_header);
}

auto SourceSetup::validate_profile_headers() const -> Result<void> {
  for (const auto& profile : video_profiles) {
    if (const auto header =
            validate_profile_header(source_codec, profile.header);
        !header)
      return fail(header.error());
  }
  return {};
}

auto SourceSetup::clone() const -> SourceSetup {
  return SourceSetup{source_session_id,  source_protocol, receive_key.clone(),
                     receive_iv.clone(), video_fps,       video_profiles,
                     audio_header,       source_codec};
}

}  // namespace libchiaking::net::kakusan::control
