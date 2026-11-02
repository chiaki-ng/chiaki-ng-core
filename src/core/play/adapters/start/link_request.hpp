/// @file
/// @brief Declares the validated requests a start builds its links from: the
/// video request, port guessing, console preferences and the cloud link
/// request, and the errors that refuse them.
#pragma once

#include "libchiaking/detail/catalog/product_id.hpp"
#include "libchiaking/detail/consoles/firmware.hpp"
#include "libchiaking/detail/play/datacenter.hpp"
#include "libchiaking/detail/play/preferences.hpp"
#include "libchiaking/detail/secret_array.hpp"
#include "src/core/play/adapters/links/cloud/cloud_link_configuration.hpp"
#include "src/core/play/adapters/links/home_link_configuration.hpp"
#include "src/core/play/adapters/links/internet/internet_link_configuration.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace libchiaking::play {

/// @brief Why a link configuration could not be built from a start's request.
enum class ConfigurationError : std::uint8_t {
  /// @brief A home console has no known address.
  invalid_host,
  /// @brief The console is forgotten, or its firmware target is not one an
  /// internet start can register with.
  invalid_console,
  /// @brief The picture asked for is outside what VideoRequest allows.
  invalid_video,
  /// @brief The player has no PSN account, or a cloud request has no catalog.
  invalid_credential,
  /// @brief The device's time zone offset could not be read for a cloud
  /// launch, or no time zone was given.
  clock_unavailable,
  /// @brief The time zone is not one PS's launch accepts.
  time_zone_out_of_range,
  /// @brief The finished configuration failed its own validation, the
  /// controller preference was refused, or an internet start timeout is not
  /// positive.
  rejected,
  /// @brief The console is not paired with the player for home play.
  not_paired,
};

/// @brief The bounds VideoRequest::create checks a picture against.
struct VideoRequestLimits {
  /// @brief The smallest picture height allowed, in lines.
  std::uint32_t minimum_height{360};
  /// @brief The largest picture height allowed for H.264, in lines.
  std::uint32_t maximum_height{1080};
  /// @brief The largest picture height allowed for HEVC, in lines.
  std::uint32_t maximum_hevc_height{2160};
  /// @brief The lowest bitrate allowed, in kilobits per second.
  std::uint32_t minimum_bitrate_kbps{2000};
  /// @brief The highest bitrate allowed, in kilobits per second.
  std::uint32_t maximum_bitrate_kbps{100000};
};

/// @brief A picture a link may ask for: height, frame rate, bitrate and codec,
/// already checked against VideoRequestLimits.
///
/// Valid from construction: the default is the 720p, 60 fps H.264 preset and
/// any other value comes from VideoRequest::create. The width is always the
/// 16:9 width of the height. Immutable value; safe to share.
class VideoRequest {
 public:
  /// @brief The picture heights a request may name, in lines.
  static constexpr std::array<std::uint32_t, 6> heights{360,  540,  720,
                                                        1080, 1440, 2160};

  /// @brief Builds the default request: 720p, 60 fps, the preset's bitrate,
  /// H.264.
  VideoRequest() noexcept = default;

  /// @brief Checks a picture and builds the request.
  ///
  /// @param height Picture height in lines; one of VideoRequest::heights.
  /// @param fps Frames per second; 30 or 60.
  /// @param bitrate_kbps Bitrate in kilobits per second.
  /// @param codec H.264, HEVC or HEVC HDR.
  /// @param limits The bounds to apply.
  /// @return The request; ConfigurationError::invalid_video when the codec or
  ///     height is unknown, the height is outside the limits (HEVC may reach
  ///     VideoRequestLimits::maximum_hevc_height), the frame rate is not 30 or
  ///     60, or the bitrate is out of range.
  [[nodiscard]] static auto create(std::uint32_t height, std::uint32_t fps,
                                   std::uint32_t bitrate_kbps,
                                   net::protocol::remote_play::VideoCodec codec,
                                   const VideoRequestLimits& limits = {})
      -> Expected<VideoRequest, ConfigurationError>;

  /// @brief Reads the height.
  ///
  /// @return The picture height in lines.
  auto height() const noexcept -> std::uint32_t { return height_; }
  /// @brief Reads the width.
  ///
  /// @return The 16:9 width for the height, in pixels.
  auto width() const noexcept -> std::uint32_t { return height_ * 16 / 9; }
  /// @brief Reads the frame rate.
  ///
  /// @return Frames per second.
  auto fps() const noexcept -> std::uint32_t { return fps_; }
  /// @brief Reads the bitrate.
  ///
  /// @return Kilobits per second.
  auto bitrate_kbps() const noexcept -> std::uint32_t { return bitrate_kbps_; }
  /// @brief Reads the codec asked for.
  ///
  /// @return The codec.
  auto codec() const noexcept -> net::protocol::remote_play::VideoCodec {
    return codec_;
  }
  /// @brief Gives the codec a console of one generation can stream.
  ///
  /// @param generation The console's generation.
  /// @return The asked codec for a PS5; H.264 for a PS4.
  auto codec_for(consoles::Generation generation) const noexcept
      -> net::protocol::remote_play::VideoCodec {
    return generation == consoles::Generation::ps5
               ? codec_
               : net::protocol::remote_play::VideoCodec::h264;
  }
  /// @brief Gives the video settings a console session request carries.
  ///
  /// @return Width, height, maximum frame rate and bitrate.
  auto settings() const noexcept
      -> net::protocol::remote_play::LocalVideoSettings {
    return {.width = width(),
            .height = height_,
            .max_fps = fps_,
            .bitrate_kbps = bitrate_kbps_};
  }

 private:
  /// @brief Builds a request already checked by VideoRequest::create.
  ///
  /// @param height Picture height in lines.
  /// @param fps Frames per second.
  /// @param bitrate_kbps Bitrate in kilobits per second.
  /// @param codec The codec.
  VideoRequest(std::uint32_t height, std::uint32_t fps,
               std::uint32_t bitrate_kbps,
               net::protocol::remote_play::VideoCodec codec) noexcept
      : height_(height),
        fps_(fps),
        bitrate_kbps_(bitrate_kbps),
        codec_(codec) {}

  /// @brief Picture height in lines.
  std::uint32_t height_{default_video_preset.height};
  /// @brief Frames per second.
  std::uint32_t fps_{default_video_fps};
  /// @brief Bitrate in kilobits per second.
  std::uint32_t bitrate_kbps_{default_video_preset.bitrate_kbps};
  /// @brief The codec asked for.
  net::protocol::remote_play::VideoCodec codec_{
      net::protocol::remote_play::VideoCodec::h264};
};

/// @brief Whether an internet start guesses NAT ports, and how many.
///
/// Valid from construction: the count is clamped to
/// [PortGuessing::minimum_count, PortGuessing::maximum_count]. Immutable value.
class PortGuessing {
 public:
  /// @brief The fewest ports guessed.
  static constexpr std::size_t minimum_count = 5;
  /// @brief The ports guessed when the player did not say.
  static constexpr std::size_t default_count =
      net::nat::holepunch::default_port_guess_count;
  /// @brief The most ports guessed; the internet link's limit.
  static constexpr std::size_t maximum_count =
      InternetLinkConfiguration::port_guess_limit;

  /// @brief Guesses only when the NAT needs it, with the default count.
  constexpr PortGuessing() noexcept = default;
  /// @brief Builds the setting from the player's preferences.
  ///
  /// @param forced Whether to guess ports even when the NAT does not need it.
  /// @param count How many ports to guess; clamped into range.
  constexpr PortGuessing(bool forced, std::size_t count) noexcept
      : forced_(forced),
        count_(count < minimum_count   ? minimum_count
               : count > maximum_count ? maximum_count
                                       : count) {}

  /// @brief Reports whether guessing is forced.
  ///
  /// @return `true` when ports are guessed whatever the NAT.
  constexpr auto forced() const noexcept -> bool { return forced_; }
  /// @brief Reads how many ports are guessed.
  ///
  /// @return The clamped count.
  constexpr auto count() const noexcept -> std::size_t { return count_; }

 private:
  /// @brief Whether guessing is forced.
  bool forced_{};
  /// @brief How many ports are guessed.
  std::size_t count_{default_count};
};

/// @brief The resolved controller, feedback and tuning settings of a console
/// link. Plain data.
struct ConsolePreferences {
  /// @brief How controller feedback is delivered; the Switch-compatible mode
  /// by default.
  net::protocol::remote_play::FeedbackMode feedback{
      net::protocol::remote_play::FeedbackMode::switch_compatibility};
  /// @brief Which controller the console sees; the console's default unless
  /// the player chose one.
  net::protocol::remote_play::ControllerPreference controller{
      net::protocol::remote_play::ControllerPreference::console_default};
  /// @brief Whether this client handles the console's on-screen keyboard, so
  /// the console's text prompts reach the player.
  bool keyboard{};
  /// @brief Whether to ask for a keyframe when a frame cannot be repaired from
  /// its forward error correction.
  bool idr_on_fec_failure{true};
  /// @brief Developer tuning, the audio channels requested and the share tap.
  net::protocol::remote_play::SessionTuning tuning{};
};

/// @brief Everything a cloud start knows before its link is configured.
///
/// Plain data built by the start use case and checked by
/// CloudLinkConfiguration::create. The catalog it points at must outlive the
/// link.
struct CloudLinkRequest {
  /// @brief Builds a request for one title.
  ///
  /// @param player The user to play as.
  /// @param titles The catalog that clears the title.
  /// @param title The title to stream.
  CloudLinkRequest(std::shared_ptr<users::PsUser> player,
                   catalog::Catalog& titles, catalog::ProductId title) noexcept
      : user(std::move(player)), catalog(&titles), product(std::move(title)) {}

  /// @brief The user to play as.
  std::shared_ptr<users::PsUser> user;
  /// @brief The catalog that clears the title; never null when built by the
  /// constructor.
  catalog::Catalog* catalog;
  /// @brief The title to stream.
  catalog::ProductId product;
  /// @brief Which datacenter to use, or how to choose one.
  DatacenterPreference datacenter;
  /// @brief Where measured datacenters are kept; null to keep none.
  play::DatacenterStore* datacenters{};
  /// @brief Datacenter round trips measured before this launch, given to the
  /// launch as prior measurements; empty normally.
  std::vector<cloud::DatacenterPing> prior_datacenters;
  /// @brief The picture to ask for.
  VideoRequest video;
  /// @brief Whether to measure the bandwidth to the server before streaming.
  bool measure_bandwidth{};
  /// @brief The controller the cloud server emulates.
  net::protocol::remote_play::ControllerPreference controller{
      net::protocol::remote_play::ControllerPreference::dualsense};
  /// @brief How controller feedback is delivered.
  net::protocol::remote_play::FeedbackMode feedback{
      net::protocol::remote_play::FeedbackMode::standard};
  /// @brief Whether to ask for a keyframe after a frame that forward error
  /// correction cannot repair.
  bool idr_on_fec_failure{};
  /// @brief Developer tuning and the audio channels requested.
  net::protocol::remote_play::SessionTuning tuning{};
  /// @brief The time zone name sent with the launch; empty to derive it from
  /// the device's UTC offset.
  std::optional<std::string> time_zone{};
  /// @brief What an earlier catalog check cleared, so the launch does not
  /// check again; empty to check during the launch.
  std::optional<catalog::ClearedLaunch> cleared{};
  /// @brief The player's screen reader and contrast choices for a cloud
  /// console.
  AccessibilityRequest accessibility{};
  /// @brief The balance PS5 cloud is asked for between picture quality and
  /// steady performance.
  CloudQuality quality{CloudQuality::high};
};

}  // namespace libchiaking::play
