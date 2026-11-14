/// @file
/// @brief Declares the stream preferences a frontend passes when starting a
/// session.
#pragma once

#include "libchiaking/detail/catalog/platform.hpp"
#include "libchiaking/detail/consoles/firmware.hpp"
#include "libchiaking/detail/consoles/home_network.hpp"
#include "libchiaking/detail/media/video_codec.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace libchiaking::play {

/// @brief How a PS5 cloud service is asked to balance picture quality against
/// steady performance.
enum class CloudQuality : std::uint8_t {
  /// @brief Higher picture quality, which uses more data. The default.
  high,
  /// @brief Steadier performance, at the cost of picture quality.
  stable,
};

/// @brief Describes the picture one console or cloud service is asked to send.
///
/// A Picture is what is left of a PictureRequest after it has been fitted to a
/// target. It can only be obtained from PictureRequest::on_console or
/// PictureRequest::on_cloud, so it always names a combination that target
/// supports.
class Picture {
 public:
  /// @brief Returns the picture height in pixels.
  ///
  /// @return The picture height in pixels.
  constexpr auto height() const noexcept -> std::uint32_t { return height_; }
  /// @brief Returns the picture width in pixels, derived from the height at
  /// 16:9.
  ///
  /// @return The picture width in pixels, derived from the height at 16:9.
  constexpr auto width() const noexcept -> std::uint32_t {
    return height_ * 16 / 9;
  }
  /// @brief Returns the frame rate in frames per second.
  ///
  /// @return The frame rate in frames per second.
  constexpr auto fps() const noexcept -> std::uint32_t { return fps_; }
  /// @brief Returns the target video bitrate in kilobits per second.
  ///
  /// @return The target video bitrate in kilobits per second.
  constexpr auto bitrate_kbps() const noexcept -> std::uint32_t {
    return bitrate_kbps_;
  }
  /// @brief Reports whether a cloud session measures the connection before it
  /// asks for a stream.
  ///
  /// @return `true` only for a cloud picture fitted from a request made with
  ///     PictureRequest::measuring_bandwidth.
  constexpr auto measures_bandwidth() const noexcept -> bool {
    return measure_bandwidth_;
  }
  /// @brief Returns the codec the stream is asked to use.
  ///
  /// @return The codec the stream is asked to use.
  constexpr auto codec() const noexcept -> media::VideoCodec { return codec_; }
  /// @brief Returns the quality balance a PS5 cloud service is asked for.
  ///
  /// @return The balance of the request for a PS5 cloud picture, and
  ///     CloudQuality::high for every other target, which ignores it.
  constexpr auto cloud_quality() const noexcept -> CloudQuality {
    return cloud_quality_;
  }

  /// @brief Compares height, frame rate, bitrate, codec, bandwidth measuring
  /// and cloud quality.
  friend constexpr auto operator==(const Picture&, const Picture&) noexcept
      -> bool = default;

 private:
  friend class PictureRequest;

  constexpr Picture(std::uint32_t height, std::uint32_t fps,
                    std::uint32_t bitrate_kbps, media::VideoCodec codec,
                    bool measure_bandwidth = false,
                    CloudQuality cloud_quality = CloudQuality::high) noexcept
      : height_(height),
        fps_(fps),
        bitrate_kbps_(bitrate_kbps),
        codec_(codec),
        measure_bandwidth_(measure_bandwidth),
        cloud_quality_(cloud_quality) {}

  std::uint32_t height_;
  std::uint32_t fps_;
  std::uint32_t bitrate_kbps_;
  media::VideoCodec codec_;
  bool measure_bandwidth_;
  CloudQuality cloud_quality_;
};

/// @brief Holds the picture the player would like, before it is fitted to a
/// target.
///
/// A request is valid from construction: PictureRequest::create refuses any
/// combination outside the published heights, frame rates and bitrate range.
/// The request does not depend on where the session will run. Play fits it to
/// the console generation or cloud service when a session starts, and a
/// frontend can call PictureRequest::on_console or PictureRequest::on_cloud
/// itself to show the player what they will get.
class PictureRequest {
 public:
  /// @brief Lists the picture heights, in pixels, a request may name.
  static constexpr std::array<std::uint32_t, 6> heights{360,  540,  720,
                                                        1080, 1440, 2160};
  /// @brief Lists the frame rates, in frames per second, a request may name.
  static constexpr std::array<std::uint32_t, 2> frame_rates{30, 60};
  /// @brief Lists the heights, in pixels, the older cloud service streams at.
  static constexpr std::array<std::uint32_t, 2> old_service_heights{720, 1080};
  /// @brief Lists the heights, in pixels, PS5 cloud streaming streams at.
  static constexpr std::array<std::uint32_t, 4> ps5_cloud_heights{720, 1080,
                                                                  1440, 2160};
  /// @brief Tallest picture, in pixels, that may be requested with
  /// VideoCodec::h264.
  static constexpr std::uint32_t maximum_h264_height = 1080;
  /// @brief Tallest PS5 cloud picture, in pixels, that is always streamed in
  /// standard dynamic range.
  static constexpr std::uint32_t maximum_sdr_cloud_height = 1080;
  /// @brief Tallest picture, in pixels, a PS4 console is asked for.
  static constexpr std::uint32_t ps4_height = 720;
  /// @brief Lowest bitrate a request may name, in kilobits per second.
  static constexpr std::uint32_t minimum_bitrate_kbps = 2000;
  /// @brief Highest bitrate a request may name, in kilobits per second.
  static constexpr std::uint32_t maximum_bitrate_kbps = 100000;

  /// @brief Requests 720 pixels of height at 60 fps and 10000 kbps in
  /// VideoCodec::h264.
  constexpr PictureRequest() noexcept = default;

  /// @brief Builds a request from the player's settings, or refuses an
  /// invalid one.
  ///
  /// @param height Picture height in pixels; must be one of
  ///     PictureRequest::heights.
  /// @param fps Frames per second; must be one of
  ///     PictureRequest::frame_rates.
  /// @param bitrate_kbps Target bitrate in kilobits per second, from
  ///     PictureRequest::minimum_bitrate_kbps to
  ///     PictureRequest::maximum_bitrate_kbps inclusive.
  /// @param codec Codec the player prefers. VideoCodec::h264 is refused above
  ///     PictureRequest::maximum_h264_height.
  /// @return The request, or `std::nullopt` when any argument is outside
  ///     those rules.
  [[nodiscard]] static auto create(std::uint32_t height, std::uint32_t fps,
                                   std::uint32_t bitrate_kbps,
                                   media::VideoCodec codec) noexcept
      -> std::optional<PictureRequest>;
  /// @brief Returns the request suggested for cloud streaming.
  ///
  /// @return 1080 pixels of height at 60 fps and 15000 kbps in
  ///     VideoCodec::h265.
  static auto cloud_default() noexcept -> PictureRequest;

  /// @brief Returns the requested picture height in pixels.
  ///
  /// @return The requested picture height in pixels.
  constexpr auto height() const noexcept -> std::uint32_t { return height_; }
  /// @brief Returns the requested width in pixels, derived from the height at
  /// 16:9.
  ///
  /// @return The requested width in pixels, derived from the height at 16:9.
  constexpr auto width() const noexcept -> std::uint32_t {
    return height_ * 16 / 9;
  }
  /// @brief Returns the requested frame rate in frames per second.
  ///
  /// @return The requested frame rate in frames per second.
  constexpr auto fps() const noexcept -> std::uint32_t { return fps_; }
  /// @brief Returns the requested bitrate in kilobits per second.
  ///
  /// @return The requested bitrate in kilobits per second.
  constexpr auto bitrate_kbps() const noexcept -> std::uint32_t {
    return bitrate_kbps_;
  }
  /// @brief Returns the codec the player prefers.
  ///
  /// @return The codec the player prefers.
  constexpr auto codec() const noexcept -> media::VideoCodec { return codec_; }
  /// @brief Returns a copy that asks cloud sessions to measure the
  /// connection.
  ///
  /// A PS5 cloud session then runs a bandwidth test against the datacenter
  /// it chose and tells the service the measured bandwidth, which also
  /// decides how far up the service's resolutions the stream climbs.
  /// PictureRequest::bitrate_kbps is told instead when the test fails.
  /// Console sessions never measure.
  ///
  /// @return This request with bandwidth measuring turned on.
  constexpr auto measuring_bandwidth() const noexcept -> PictureRequest {
    auto copy = *this;
    copy.measure_bandwidth_ = true;
    return copy;
  }
  /// @brief Reports whether cloud sessions measure the connection.
  ///
  /// @return `true` when cloud sessions measure the connection.
  constexpr auto measures_bandwidth() const noexcept -> bool {
    return measure_bandwidth_;
  }
  /// @brief Returns a copy that asks PS5 cloud sessions for the given
  /// quality balance.
  ///
  /// The service decides what the balance changes. Console sessions and the
  /// older cloud service ignore it.
  ///
  /// @param quality Balance between picture quality and steady performance.
  /// @return This request with that balance.
  constexpr auto favouring(CloudQuality quality) const noexcept
      -> PictureRequest {
    auto copy = *this;
    copy.cloud_quality_ = quality;
    return copy;
  }
  /// @brief Returns the quality balance PS5 cloud sessions are asked for.
  ///
  /// @return The balance between picture quality and steady performance.
  constexpr auto cloud_quality() const noexcept -> CloudQuality {
    return cloud_quality_;
  }

  /// @brief Fits this request to a console of the given generation.
  ///
  /// A PS5 is asked for exactly what was requested. Any other generation is
  /// asked for VideoCodec::h264 at no more than PictureRequest::ps4_height.
  /// The frame rate and bitrate are kept in both cases.
  ///
  /// @param generation Generation of the console the session will run on.
  /// @return The picture that console will be asked to send.
  auto on_console(consoles::Generation generation) const noexcept -> Picture;
  /// @brief Fits this request to a cloud streaming service.
  ///
  /// The height becomes the tallest one the service offers that does not
  /// exceed the request, or the service's smallest height when the request is
  /// below all of them. The older service always streams VideoCodec::h264.
  /// PS5 cloud streaming streams VideoCodec::h265, or VideoCodec::h265_hdr
  /// when that was requested and the fitted height is above
  /// PictureRequest::maximum_sdr_cloud_height. The frame rate and bitrate
  /// are kept.
  ///
  /// @param service Cloud service the title will be streamed from.
  /// @return The picture that service will be asked to send.
  /// @note PS5 cloud streaming sends HDR only above 1080 pixels of height, so
  ///     a VideoCodec::h265_hdr request at or below it becomes plain
  ///     VideoCodec::h265.
  auto on_cloud(catalog::ServiceGeneration service) const noexcept -> Picture;

  /// @brief Compares height, frame rate, bitrate, codec, bandwidth measuring
  /// and cloud quality.
  friend constexpr auto operator==(const PictureRequest&,
                                   const PictureRequest&) noexcept
      -> bool = default;

 private:
  constexpr PictureRequest(std::uint32_t height, std::uint32_t fps,
                           std::uint32_t bitrate_kbps,
                           media::VideoCodec codec) noexcept
      : height_(height),
        fps_(fps),
        bitrate_kbps_(bitrate_kbps),
        codec_(codec) {}

  std::uint32_t height_{720};
  std::uint32_t fps_{60};
  std::uint32_t bitrate_kbps_{10000};
  media::VideoCodec codec_{media::VideoCodec::h264};
  bool measure_bandwidth_{};
  CloudQuality cloud_quality_{CloudQuality::high};
};

/// @brief Holds the number of audio channels the player would like.
///
/// Zero channels means no layout is requested and the console or service
/// chooses. Any count above SoundRequest::stereo is a surround request, which
/// is preserved for PS5 consoles and PS5 cloud sessions.
class SoundRequest {
 public:
  /// @brief Lists the channel counts a request may name; 0 requests no
  /// layout.
  static constexpr std::array<std::uint8_t, 5> channel_counts{0, 2, 6, 8, 12};
  /// @brief Channel count of a stereo request.
  static constexpr std::uint8_t stereo = 2;

  /// @brief Requests no particular layout (0 channels).
  constexpr SoundRequest() noexcept = default;

  /// @brief Builds a request for a channel count, or refuses an unknown one.
  ///
  /// @param channels Number of channels; must be one of
  ///     SoundRequest::channel_counts.
  /// @return The request, or `std::nullopt` for any other count.
  [[nodiscard]] static constexpr auto create(std::uint32_t channels) noexcept
      -> std::optional<SoundRequest> {
    for (const auto known : channel_counts)
      if (channels == known) return SoundRequest{known};
    return std::nullopt;
  }

  /// @brief Returns the requested channel count; 0 means no layout was
  /// requested.
  ///
  /// @return The requested channel count; 0 means no layout was requested.
  constexpr auto channels() const noexcept -> std::uint8_t { return channels_; }

  /// @brief Builds a copy that also caps the widest layout the service may
  /// send, overriding the service default ceiling.
  ///
  /// @param channels Ceiling channel count; must be one of
  ///     SoundRequest::channel_counts, and 0 restores the default ceiling.
  /// @return The capped request, or `std::nullopt` for any other count.
  [[nodiscard]] constexpr auto capped_at(std::uint32_t channels) const noexcept
      -> std::optional<SoundRequest> {
    for (const auto known : channel_counts)
      if (channels == known) {
        SoundRequest capped = *this;
        capped.ceiling_ = known;
        return capped;
      }
    return std::nullopt;
  }

  /// @brief Returns the ceiling channel count; 0 leaves the service default.
  ///
  /// @return The ceiling channel count, or 0 for the default.
  constexpr auto ceiling() const noexcept -> std::uint8_t { return ceiling_; }

  /// @brief Reports whether more than two channels were requested.
  ///
  /// @return `true` when more than two channels were requested.
  constexpr auto wants_surround() const noexcept -> bool {
    return channels_ > stereo;
  }

  /// @brief Builds a copy that also asks for the audio of the speaker built
  /// into each controller.
  ///
  /// Only PS5 cloud sessions can deliver it.
  ///
  /// @param enabled Whether controller speaker audio is wanted.
  /// @return This request with the controller speaker choice replaced.
  [[nodiscard]] constexpr auto with_controller_speaker(
      bool enabled) const noexcept -> SoundRequest {
    auto copy = *this;
    copy.controller_speaker_ = enabled;
    return copy;
  }

  /// @brief Reports whether controller speaker audio was requested.
  ///
  /// @return `true` when it was requested.
  constexpr auto wants_controller_speaker() const noexcept -> bool {
    return controller_speaker_;
  }

  /// @brief Builds a copy that sends or leaves out the PS5 console's audio
  /// profile list with a stereo request.
  ///
  /// A PS5 console given the list captures audio in its multichannel mode and
  /// selects the requested profile from it, which also lets the profile change
  /// during the session. Without the list it uses its default stereo path. A
  /// surround request always sends the list, so this only affects stereo.
  ///
  /// @param enabled Whether a stereo request sends the profile list.
  /// @return This request with the profile list choice replaced.
  [[nodiscard]] constexpr auto with_audio_profiles(bool enabled) const noexcept
      -> SoundRequest {
    auto copy = *this;
    copy.audio_profiles_ = enabled;
    return copy;
  }

  /// @brief Reports whether a stereo request sends the PS5 console's audio
  /// profile list.
  ///
  /// @return `true` unless with_audio_profiles() turned it off.
  constexpr auto wants_audio_profiles() const noexcept -> bool {
    return audio_profiles_;
  }

  /// @brief Fits this request to a console of the given generation.
  ///
  /// @param generation Generation of the console the session will run on.
  /// @return This request on a PS5, without controller speaker audio, which
  ///     consoles never deliver. On any other generation a surround request
  ///     is replaced by the default request (0 channels); other requests are
  ///     kept.
  constexpr auto on_console(consoles::Generation generation) const noexcept
      -> SoundRequest {
    auto fitted = wants_surround() && generation != consoles::Generation::ps5
                      ? SoundRequest{}
                      : *this;
    fitted.controller_speaker_ = false;
    return fitted;
  }

  /// @brief Fits this request to a cloud streaming service.
  ///
  /// @param service Generation of the cloud service the session will run on.
  /// @return This request unchanged on a PS5 cloud service. On other services
  ///     a surround request is replaced by the default request (0 channels),
  ///     controller speaker audio is dropped and other requests are kept. A
  ///     requested layout is subject to negotiation.
  constexpr auto on_cloud(catalog::ServiceGeneration service) const noexcept
      -> SoundRequest {
    if (service == catalog::ServiceGeneration::ps5_cloud) return *this;
    auto fitted = wants_surround() ? SoundRequest{} : *this;
    fitted.controller_speaker_ = false;
    return fitted;
  }

  /// @brief Reports whether a console of the given generation renders 3D audio.
  ///
  /// @param generation Generation of the console the session will run on.
  /// @return True on a PS5, which renders 3D audio as binaural stereo; false
  ///     on any other generation.
  static constexpr auto supports_3d_on_console(
      consoles::Generation generation) noexcept -> bool {
    return generation == consoles::Generation::ps5;
  }

  /// @brief Reports whether a cloud streaming service renders 3D audio.
  ///
  /// @param service Generation of the cloud service the session will run on.
  /// @return True on a PS5 cloud service; false on older services.
  static constexpr auto supports_3d_on_cloud(
      catalog::ServiceGeneration service) noexcept -> bool {
    return service == catalog::ServiceGeneration::ps5_cloud;
  }

  /// @brief Compares the requested channel counts, ceilings, controller
  /// speaker choices and audio profile choices.
  friend constexpr auto operator==(const SoundRequest&,
                                   const SoundRequest&) noexcept
      -> bool = default;

 private:
  constexpr explicit SoundRequest(std::uint8_t channels) noexcept
      : channels_(channels) {}

  std::uint8_t channels_{};
  std::uint8_t ceiling_{};
  bool controller_speaker_{};
  bool audio_profiles_{true};
};

/// @brief Names the controller the stream should present to the console or
/// service.
enum class ControllerChoice : std::uint8_t {
  /// @brief Uses the controller native to the target: DualSense on a PS5
  /// console and on both cloud services, DualShock 4 on a PS4 console.
  console_default,
  /// @brief Presents a DualShock 4 whatever the target is.
  dualshock4,
  /// @brief Presents a DualSense whatever the target is.
  dualsense
};

/// @brief Selects how controller feedback is requested from the console or
/// service.
enum class FeedbackStyle : std::uint8_t {
  /// @brief The default.
  standard,
  /// @brief The stream's alternative, compatibility feedback mode.
  switch_compatibility,
};

/// @brief Holds the player's controller and on-screen keyboard choices.
class ControlsRequest {
 public:
  /// @brief Chooses the target's native controller, standard feedback and no
  /// console keyboard.
  constexpr ControlsRequest() noexcept = default;
  /// @brief Builds a request from explicit choices; every combination is
  /// valid.
  ///
  /// @param controller Controller the stream should present.
  /// @param feedback How controller feedback is requested.
  /// @param console_keyboard Whether the console's on-screen keyboard should
  ///     be handled through this client.
  constexpr ControlsRequest(ControllerChoice controller, FeedbackStyle feedback,
                            bool console_keyboard) noexcept
      : controller_(controller),
        feedback_(feedback),
        console_keyboard_(console_keyboard) {}

  /// @brief Returns the controller the stream should present.
  ///
  /// @return The controller the stream should present.
  constexpr auto controller() const noexcept -> ControllerChoice {
    return controller_;
  }
  /// @brief Returns how controller feedback is requested.
  ///
  /// @return How controller feedback is requested.
  constexpr auto feedback() const noexcept -> FeedbackStyle {
    return feedback_;
  }
  /// @brief Reports whether the console's on-screen keyboard is handled by
  /// this client.
  ///
  /// It applies to sessions on a console, at home or over the internet. Cloud
  /// sessions do not use it.
  ///
  /// @see ConsoleKeyboard
  ///
  /// @return `true` when the console's on-screen keyboard is handled by this
  ///     client.
  constexpr auto console_keyboard() const noexcept -> bool {
    return console_keyboard_;
  }

  /// @brief Reports whether a console of the given generation is presented a
  /// DualSense.
  ///
  /// @param generation Generation of the console the session will run on.
  /// @return `true` for ControllerChoice::dualsense, and for
  ///     ControllerChoice::console_default on a PS5.
  constexpr auto uses_dualsense_on(
      consoles::Generation generation) const noexcept -> bool {
    return controller_ == ControllerChoice::dualsense ||
           (controller_ == ControllerChoice::console_default &&
            generation == consoles::Generation::ps5);
  }

  /// @brief Reports whether a cloud service is presented a DualSense.
  ///
  /// @return `true` unless ControllerChoice::dualshock4 was chosen, on either
  ///     service.
  constexpr auto uses_dualsense_on(catalog::ServiceGeneration) const noexcept
      -> bool {
    return controller_ != ControllerChoice::dualshock4;
  }

  /// @brief Compares controller, feedback style and keyboard choice.
  friend constexpr auto operator==(const ControlsRequest&,
                                   const ControlsRequest&) noexcept
      -> bool = default;

 private:
  ControllerChoice controller_{ControllerChoice::console_default};
  FeedbackStyle feedback_{FeedbackStyle::standard};
  bool console_keyboard_{};
};

/// @brief Holds the options that only apply to remote play over the internet.
///
/// They control how hard the client tries to open a direct path through the
/// player's router to the console, and how long a start may take.
class InternetOptions {
 public:
  /// @brief Smallest number of port guesses; lower values are raised to it.
  static constexpr std::size_t minimum_port_guesses = 5;
  /// @brief Number of port guesses used unless the player chooses otherwise.
  static constexpr std::size_t default_port_guesses = 75;
  /// @brief Largest number of port guesses; higher values are lowered to it.
  static constexpr std::size_t maximum_port_guesses = 80;
  /// @brief Time an internet start is given unless the player chooses
  /// otherwise.
  static constexpr std::chrono::seconds default_start_timeout{60};

  /// @brief Uses default port guessing and
  /// InternetOptions::default_start_timeout.
  constexpr InternetOptions() noexcept = default;

  /// @brief Builds the options, clamping the guess count into its allowed
  /// range.
  ///
  /// @param force_port_guessing Whether port guessing is always used when
  ///     reaching the console.
  /// @param port_guesses Number of ports to guess. Values outside
  ///     InternetOptions::minimum_port_guesses to
  ///     InternetOptions::maximum_port_guesses are clamped, not refused.
  /// @param start_timeout Time the internet start is given.
  /// @return The options, or `std::nullopt` when `start_timeout` is zero or
  ///     negative.
  [[nodiscard]] static constexpr auto create(
      bool force_port_guessing, std::size_t port_guesses,
      std::chrono::seconds start_timeout = default_start_timeout) noexcept
      -> std::optional<InternetOptions> {
    if (start_timeout.count() <= 0) return std::nullopt;
    InternetOptions options;
    options.force_port_guessing_ = force_port_guessing;
    options.port_guesses_ =
        port_guesses < minimum_port_guesses   ? minimum_port_guesses
        : port_guesses > maximum_port_guesses ? maximum_port_guesses
                                              : port_guesses;
    options.start_timeout_ = start_timeout;
    return options;
  }

  /// @brief Reports whether port guessing is always used.
  ///
  /// @return `true` when port guessing is always used.
  constexpr auto forces_port_guessing() const noexcept -> bool {
    return force_port_guessing_;
  }
  /// @brief Returns the number of ports guessed, already clamped to the
  /// allowed range.
  ///
  /// @return The number of ports guessed, already clamped to the allowed
  ///     range.
  constexpr auto port_guesses() const noexcept -> std::size_t {
    return port_guesses_;
  }
  /// @brief Returns the time an internet start is given.
  ///
  /// @return The time an internet start is given.
  constexpr auto start_timeout() const noexcept -> std::chrono::seconds {
    return start_timeout_;
  }

  /// @brief Compares every option.
  friend constexpr auto operator==(const InternetOptions&,
                                   const InternetOptions&) noexcept
      -> bool = default;

 private:
  bool force_port_guessing_{};
  std::size_t port_guesses_{default_port_guesses};
  std::chrono::seconds start_timeout_{default_start_timeout};
};

/// @brief Holds the options that only apply to a session with a console on
/// the local network: the network provider the stream runs over.
///
/// It is consoles::HomeNetwork. A stream started with a provider shares
/// ownership of it until the session has ended and been released, and every
/// socket, poller and name lookup of the stream goes through it, as do the
/// wakes the start sends and the check consoles::Console::start makes when
/// the console's presence is unknown. It belongs to that start only: the
/// console's saved address, network searches and every other session keep
/// the platform's provider unless they are given a consoles::HomeNetwork of
/// their own.
using HomeOptions = consoles::HomeNetwork;

/// @brief Holds diagnostic overrides meant for developers, not for players.
///
/// The defaults leave the stream untouched. Frontends normally expose these
/// only in a developer or debug menu.
class DeveloperTuning {
 public:
  /// @brief Default for Facts::packet_loss_max, as a fraction of packets.
  static constexpr double default_packet_loss_max = 0.05;
  /// @brief Default for Facts::nat_probe_sockets.
  static constexpr std::size_t default_nat_probe_sockets = 250;
  /// @brief Largest value DeveloperTuning::create accepts for
  /// Facts::nat_probe_sockets.
  static constexpr std::size_t maximum_nat_probe_sockets = 4096;

  /// @brief Plain values DeveloperTuning::create validates.
  struct Facts {
    /// @brief Discards received audio instead of delivering it.
    bool disable_audio{};
    /// @brief Discards received video instead of delivering it.
    bool disable_video{};
    /// @brief Stream protocol version to use in place of the built-in one;
    /// must not be 0 when set.
    std::optional<std::uint32_t> protocol_version{};
    /// @brief Lets the stream protocol version be negotiated with the peer.
    bool negotiate_protocol_version{};
    /// @brief Packet loss the stream tolerates, as a fraction from 0.0 to
    /// 1.0.
    double packet_loss_max{default_packet_loss_max};
    /// @brief Whether a keyframe is requested when a frame cannot be
    /// recovered; unset uses the default, which is on for consoles and off
    /// for cloud.
    std::optional<bool> keyframe_on_lost_frame{};
    /// @brief Number of sockets opened to probe the router during an
    /// internet start.
    std::size_t nat_probe_sockets{default_nat_probe_sockets};

    /// @brief Compares every field.
    friend constexpr auto operator==(const Facts&, const Facts&) noexcept
        -> bool = default;
  };

  /// @brief Applies no overrides.
  constexpr DeveloperTuning() noexcept = default;

  /// @brief Returns the stream protocol version this build uses for a
  /// console.
  ///
  /// @param generation Generation of the console.
  /// @return The built-in version, for showing next to an override.
  static auto built_protocol_version(consoles::Generation generation) noexcept
      -> std::uint32_t;
  /// @brief Returns the stream protocol version this build uses for a cloud
  /// service.
  ///
  /// @param service Cloud service.
  /// @return The built-in version, for showing next to an override.
  static auto built_protocol_version(
      catalog::ServiceGeneration service) noexcept -> std::uint32_t;
  /// @brief Lists the stream protocol versions this build implements for a
  /// console, for a developer screen to offer as Facts::protocol_version.
  ///
  /// PS4 and PS5 consoles are offered the same versions; those reserved for
  /// PS5 cloud streaming are left out. An override outside the list is
  /// ignored by a console session, which keeps its built-in version.
  ///
  /// @param generation Generation of the console.
  /// @return The versions, ascending; the one built_protocol_version names
  ///     is among them.
  static auto implemented_protocol_versions(consoles::Generation generation)
      -> std::vector<std::uint32_t>;
  /// @brief Lists the stream protocol versions this build implements for a
  /// cloud service, for a developer screen to offer as
  /// Facts::protocol_version.
  ///
  /// PS5 cloud streaming is offered every implemented version; the older
  /// service is offered the ones consoles use too. An override outside the
  /// list is ignored by a cloud session, which keeps its built-in version.
  ///
  /// @param service Cloud service.
  /// @return The versions, ascending; the one built_protocol_version names
  ///     is among them.
  static auto implemented_protocol_versions(catalog::ServiceGeneration service)
      -> std::vector<std::uint32_t>;
  /// @brief Returns the protocol version override compiled into this build,
  /// if any.
  ///
  /// @return The override, or `std::nullopt` when the build sets none.
  static auto build_protocol_override() noexcept
      -> std::optional<std::uint32_t>;

  /// @brief Builds the tuning from plain values, or refuses invalid ones.
  ///
  /// @param facts Values to validate.
  /// @return The tuning, or `std::nullopt` when Facts::packet_loss_max is
  ///     outside 0.0 to 1.0, Facts::protocol_version is set to 0, or
  ///     Facts::nat_probe_sockets exceeds
  ///     DeveloperTuning::maximum_nat_probe_sockets.
  [[nodiscard]] static constexpr auto create(Facts facts) noexcept
      -> std::optional<DeveloperTuning> {
    const bool loss_valid =
        facts.packet_loss_max >= 0.0 && facts.packet_loss_max <= 1.0;
    const bool version_valid =
        !facts.protocol_version || *facts.protocol_version != 0;
    if (!loss_valid || !version_valid ||
        facts.nat_probe_sockets > maximum_nat_probe_sockets)
      return std::nullopt;
    DeveloperTuning tuning;
    tuning.facts_ = facts;
    return tuning;
  }

  /// @brief Reports whether received audio is discarded.
  ///
  /// @return `true` when received audio is discarded.
  constexpr auto disables_audio() const noexcept -> bool {
    return facts_.disable_audio;
  }
  /// @brief Reports whether received video is discarded.
  ///
  /// @return `true` when received video is discarded.
  constexpr auto disables_video() const noexcept -> bool {
    return facts_.disable_video;
  }
  /// @brief Returns the protocol version override, or `std::nullopt` for the
  /// built-in version.
  ///
  /// @return The protocol version override, or `std::nullopt` for the
  ///     built-in version.
  constexpr auto protocol_version() const noexcept
      -> std::optional<std::uint32_t> {
    return facts_.protocol_version;
  }
  /// @brief Reports whether the protocol version is negotiated with the peer.
  ///
  /// @return `true` when the protocol version is negotiated with the peer.
  constexpr auto negotiates_protocol_version() const noexcept -> bool {
    return facts_.negotiate_protocol_version;
  }
  /// @brief Returns the tolerated packet loss as a fraction from 0.0 to 1.0.
  ///
  /// @return The tolerated packet loss as a fraction from 0.0 to 1.0.
  constexpr auto packet_loss_max() const noexcept -> double {
    return facts_.packet_loss_max;
  }
  /// @brief Returns the keyframe-on-lost-frame override, or `std::nullopt`
  /// for the target's default.
  ///
  /// @return The keyframe-on-lost-frame override, or `std::nullopt` for the
  ///     target's default.
  constexpr auto keyframe_on_lost_frame() const noexcept
      -> std::optional<bool> {
    return facts_.keyframe_on_lost_frame;
  }
  /// @brief Returns the number of router probe sockets used by an internet
  /// start.
  ///
  /// @return The number of router probe sockets used by an internet start.
  constexpr auto nat_probe_sockets() const noexcept -> std::size_t {
    return facts_.nat_probe_sockets;
  }

  /// @brief Compares every value.
  friend constexpr auto operator==(const DeveloperTuning&,
                                   const DeveloperTuning&) noexcept
      -> bool = default;

 private:
  Facts facts_{};
};

/// @brief Describes limits of the frontend's own picture and sound output.
///
/// Unlike the other requests this is not sent to the console or service. It
/// tells the session what the frontend can present and how strictly to treat
/// audio output failures.
class OutputRequest {
 public:
  /// @brief Sets no picture size limit and lets a session survive audio
  /// failures.
  constexpr OutputRequest() noexcept = default;

  /// @brief Builds the request, or refuses a zero picture limit.
  ///
  /// @param largest_picture_side Longest width or height, in pixels, the
  ///     frontend's decoder accepts; `std::nullopt` for no limit.
  /// @param audio_failure_ends_session Whether a failure of the frontend's
  ///     audio output ends the session instead of leaving it running without
  ///     sound.
  /// @return The request, or `std::nullopt` when `largest_picture_side` is 0.
  [[nodiscard]] static constexpr auto create(
      std::optional<std::uint32_t> largest_picture_side,
      bool audio_failure_ends_session) noexcept
      -> std::optional<OutputRequest> {
    if (largest_picture_side && *largest_picture_side == 0) return std::nullopt;
    OutputRequest request;
    request.largest_picture_side_ = largest_picture_side;
    request.audio_failure_ends_session_ = audio_failure_ends_session;
    return request;
  }

  /// @brief Returns the longest picture side, in pixels, the frontend
  /// accepts.
  ///
  /// A stream whose width or height exceeds it is rejected as a media
  /// failure.
  ///
  /// @return The limit, or `std::nullopt` for no limit.
  /// @see MediaFailure::profile_validation
  constexpr auto largest_picture_side() const noexcept
      -> std::optional<std::uint32_t> {
    return largest_picture_side_;
  }
  /// @brief Reports whether an audio output failure ends the session.
  ///
  /// @return `true` when an audio output failure ends the session.
  constexpr auto audio_failure_ends_session() const noexcept -> bool {
    return audio_failure_ends_session_;
  }

  /// @brief Compares the picture limit and the audio failure choice.
  friend constexpr auto operator==(const OutputRequest&,
                                   const OutputRequest&) noexcept
      -> bool = default;

 private:
  std::optional<std::uint32_t> largest_picture_side_{};
  bool audio_failure_ends_session_{};
};

/// @brief Says whether a console start follows straight on from this client's
/// own session with the same console.
enum class ConsoleHandover : std::uint8_t {
  /// @brief No recent session from this client. A console that reports
  /// Remote Play as already in use refuses the start at once.
  fresh,
  /// @brief This client ended a session with the console moments ago. A
  /// console keeps refusing new sessions as in use for a few seconds while
  /// it closes the last one, so the start tries again for a short while
  /// before it gives up.
  just_left,
};

/// @brief Holds the accessibility settings a cloud session hands to the
/// console it streams from.
///
/// A PS5 cloud console uses them to turn on its own screen reader and higher
/// contrast for the session. Console sessions keep the console's own
/// accessibility settings and ignore these.
class AccessibilityRequest {
 public:
  /// @brief Asks for neither the screen reader nor higher contrast.
  constexpr AccessibilityRequest() noexcept = default;
  /// @brief Builds a request from the player's settings.
  ///
  /// @param screen_reader Whether the cloud console reads its screen aloud.
  /// @param higher_contrast Whether the cloud console raises its contrast.
  constexpr AccessibilityRequest(bool screen_reader,
                                 bool higher_contrast) noexcept
      : screen_reader_(screen_reader), higher_contrast_(higher_contrast) {}

  /// @brief Builds a cloud request with text and speech options.
  /// @param screen_reader Whether spoken guidance is enabled.
  /// @param screen_reader_speed Speech speed from 0 to 8.
  /// @param screen_reader_volume Speech volume from 0 to 24.
  /// @param text_size Text size from -1 (small) to 2 (very large); 0 is
  /// standard.
  /// @param bold_text Whether bold text is enabled.
  /// @param higher_contrast Whether higher contrast is enabled.
  constexpr AccessibilityRequest(bool screen_reader, int screen_reader_speed,
                                 int screen_reader_volume, int text_size,
                                 bool bold_text, bool higher_contrast) noexcept
      : screen_reader_(screen_reader),
        screen_reader_speed_(screen_reader_speed >= 0 &&
                                     screen_reader_speed <= 8
                                 ? screen_reader_speed
                                 : 3),
        screen_reader_volume_(screen_reader_volume >= 0 &&
                                      screen_reader_volume <= 24
                                  ? screen_reader_volume
                                  : 12),
        text_size_(text_size >= -1 && text_size <= 2 ? text_size : 0),
        bold_text_(bold_text),
        higher_contrast_(higher_contrast) {}

  /// @brief Reports whether the cloud console reads its screen aloud.
  ///
  /// @return True when the cloud console's screen reader is asked for.
  constexpr auto screen_reader() const noexcept -> bool {
    return screen_reader_;
  }
  /// @brief Reports the requested speech speed.
  /// @return A speed from 0 to 8.
  constexpr auto screen_reader_speed() const noexcept -> int {
    return screen_reader_speed_;
  }
  /// @brief Reports the requested speech volume.
  /// @return A volume from 0 to 24.
  constexpr auto screen_reader_volume() const noexcept -> int {
    return screen_reader_volume_;
  }
  /// @brief Reports the requested cloud text size.
  /// @return A size from -1 (small) to 2 (very large); 0 is standard.
  constexpr auto text_size() const noexcept -> int { return text_size_; }
  /// @brief Reports whether bold text is requested.
  /// @return True when bold text is requested.
  constexpr auto bold_text() const noexcept -> bool { return bold_text_; }
  /// @brief Reports whether the cloud console raises its contrast.
  ///
  /// @return True when the cloud console's higher contrast is asked for.
  constexpr auto higher_contrast() const noexcept -> bool {
    return higher_contrast_;
  }

  /// @brief Compares both settings.
  friend constexpr auto operator==(const AccessibilityRequest&,
                                   const AccessibilityRequest&) noexcept
      -> bool = default;

 private:
  bool screen_reader_{};
  int screen_reader_speed_{3};
  int screen_reader_volume_{12};
  int text_size_{};
  bool bold_text_{};
  bool higher_contrast_{};
};

/// @brief Gathers everything the player chose about how a session should
/// stream.
///
/// One value is passed to Play::start_on_console, Play::start_over_internet or
/// Play::start_on_cloud. Each part is already valid on its own, and each start
/// uses only the parts that apply to it.
struct Preferences {
  /// @brief Resolution, frame rate, bitrate and codec the player would like.
  PictureRequest picture{};
  /// @brief Audio channel layout the player would like.
  SoundRequest sound{};
  /// @brief Controller, feedback and console keyboard choices.
  ControlsRequest controls{};
  /// @brief Options used only by Play::start_on_console, and by
  /// consoles::Console::start when it plays at home.
  HomeOptions home{};
  /// @brief Options used only by Play::start_over_internet.
  InternetOptions internet{};
  /// @brief Developer overrides; leave defaulted for players.
  DeveloperTuning developer{};
  /// @brief Limits of the frontend's own picture and sound output.
  OutputRequest output{};
  /// @brief Accessibility settings handed to cloud consoles.
  AccessibilityRequest accessibility{};
  /// @brief Whether a console start follows this client's own session with
  /// the same console; used by Play::start_on_console and
  /// Play::start_over_internet.
  ConsoleHandover handover{ConsoleHandover::fresh};
  /// @brief Whether a home or internet stream that drops after playing is
  /// connected again automatically, up to three times, instead of ending;
  /// used by Play::start_on_console and Play::start_over_internet.
  bool reconnect_after_drop{true};

  /// @brief Compares every part.
  friend auto operator==(const Preferences&, const Preferences&) noexcept
      -> bool = default;
};

}  // namespace libchiaking::play
