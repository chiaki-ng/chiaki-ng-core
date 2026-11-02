#include "libchiaking/detail/play/preferences.hpp"

#include <algorithm>
#include <span>

namespace libchiaking::play {

auto PictureRequest::create(std::uint32_t height, std::uint32_t fps,
                            std::uint32_t bitrate_kbps,
                            media::VideoCodec codec) noexcept
    -> std::optional<PictureRequest> {
  const bool hevc =
      codec == media::VideoCodec::h265 || codec == media::VideoCodec::h265_hdr;
  const bool known_codec = hevc || codec == media::VideoCodec::h264;
  const bool known_height = std::ranges::find(heights, height) != heights.end();
  const bool allowed_height = hevc || height <= maximum_h264_height;
  const bool known_rate =
      std::ranges::find(frame_rates, fps) != frame_rates.end();
  if (!known_codec || !known_height || !allowed_height || !known_rate ||
      bitrate_kbps < minimum_bitrate_kbps ||
      bitrate_kbps > maximum_bitrate_kbps)
    return std::nullopt;
  return PictureRequest{height, fps, bitrate_kbps, codec};
}

auto PictureRequest::cloud_default() noexcept -> PictureRequest {
  return PictureRequest{1080, 60, 15000, media::VideoCodec::h265};
}

auto PictureRequest::on_console(consoles::Generation generation) const noexcept
    -> Picture {
  if (generation == consoles::Generation::ps5)
    return Picture{height_, fps_, bitrate_kbps_, codec_};
  return Picture{std::min(height_, ps4_height), fps_, bitrate_kbps_,
                 media::VideoCodec::h264};
}

auto PictureRequest::on_cloud(catalog::ServiceGeneration service) const noexcept
    -> Picture {
  const bool ps5 = service == catalog::ServiceGeneration::ps5_cloud;
  const auto allowed =
      ps5 ? std::span<const std::uint32_t>{ps5_cloud_heights}
          : std::span<const std::uint32_t>{old_service_heights};
  auto height = allowed.front();
  for (const auto candidate : allowed)
    if (candidate <= height_) height = candidate;
  if (!ps5)
    return Picture{height, fps_, bitrate_kbps_, media::VideoCodec::h264};
  const bool hdr = codec_ == media::VideoCodec::h265_hdr &&
                   height > maximum_sdr_cloud_height;
  return Picture{height,
                 fps_,
                 bitrate_kbps_,
                 hdr ? media::VideoCodec::h265_hdr : media::VideoCodec::h265,
                 measure_bandwidth_,
                 cloud_quality_};
}

}  // namespace libchiaking::play
