/// @file
/// @brief Declares LinkPreferences, which turns the player's Preferences into
/// the settings a console or cloud link is configured with.
#pragma once

#include "libchiaking/detail/play/preferences.hpp"
#include "src/core/play/adapters/session/session_settings.hpp"
#include "src/core/play/adapters/start/link_request.hpp"

#include <chrono>
#include <optional>

namespace libchiaking::play {

/// @brief Resolves the player's Preferences for one kind of link.
///
/// The public Preferences say what the player wants; a link needs concrete
/// wire values that depend on the console generation or cloud service. These
/// static members make that translation and refuse a picture the link cannot
/// carry. Stateless; every member may be called from any thread.
class LinkPreferences {
 public:
  /// @brief The resolved settings for a console link, at home or over the
  /// internet.
  struct OnConsole {
    /// @brief The picture to ask the console for.
    VideoRequest video;
    /// @brief Controller, feedback, keyboard, keyframe and tuning settings.
    ConsolePreferences console;
    /// @brief How an internet start guesses the console's NAT ports.
    PortGuessing port_guessing;
    /// @brief How long an internet start may take before it gives up, in
    /// seconds; zero until resolved.
    std::chrono::seconds start_timeout{};
  };

  /// @brief The resolved settings for a cloud link.
  struct OnCloud {
    /// @brief The picture to ask the cloud server for.
    VideoRequest video;
    /// @brief Whether to measure the bandwidth to the server before streaming.
    bool measure_bandwidth{};
    /// @brief The controller the cloud server emulates: DualSense when the
    /// title uses one on its service, otherwise DualShock 4.
    net::protocol::remote_play::ControllerPreference controller{
        net::protocol::remote_play::ControllerPreference::dualsense};
    /// @brief How controller feedback is delivered.
    net::protocol::remote_play::FeedbackMode feedback{
        net::protocol::remote_play::FeedbackMode::standard};
    /// @brief Whether to ask for a keyframe when a frame cannot be repaired
    /// from its forward error correction.
    bool idr_on_fec_failure{};
    /// @brief Developer tuning and the audio channels requested.
    net::protocol::remote_play::SessionTuning tuning{};
    /// @brief The player's screen reader and contrast choices for a cloud
    /// console.
    AccessibilityRequest accessibility{};
    /// @brief The balance PS5 cloud is asked for between picture quality and
    /// steady performance.
    CloudQuality quality{CloudQuality::high};
  };

  /// @brief Whether a console link asks for a keyframe after a lost frame when
  /// the developer settings do not say.
  static constexpr bool console_keyframe_on_lost_frame = true;
  /// @brief Whether a cloud link asks for a keyframe after a lost frame when
  /// the developer settings do not say.
  static constexpr bool cloud_keyframe_on_lost_frame = false;

  /// @brief Resolves the preferences for a console of one generation.
  ///
  /// @param preferences The player's preferences.
  /// @param generation The console's generation, which caps the picture and
  ///     sound.
  /// @return The settings; empty when the fitted picture is not a valid
  ///     VideoRequest.
  [[nodiscard]] static auto on_console(const Preferences& preferences,
                                       consoles::Generation generation)
      -> std::optional<OnConsole>;
  /// @brief Resolves the preferences for one cloud service.
  ///
  /// @param preferences The player's preferences.
  /// @param service The service carrying the stream, which caps the picture,
  ///     sound and controller.
  /// @return The settings; empty when the fitted picture is not a valid
  ///     VideoRequest.
  [[nodiscard]] static auto on_cloud(const Preferences& preferences,
                                     catalog::ServiceGeneration service)
      -> std::optional<OnCloud>;

  /// @brief Turns a picture preference into a video request.
  ///
  /// @param picture The fitted picture.
  /// @return The request; empty when VideoRequest::create refuses it.
  static auto video(const Picture& picture) -> std::optional<VideoRequest>;
  /// @brief Builds the session tuning from the developer settings and the
  /// requested sound.
  ///
  /// @param developer Audio and video switches, Takion version override and
  ///     negotiation, and the packet loss ceiling.
  /// @param sound The sound request, whose channel count is asked for.
  /// @return The tuning; a Takion version override of 0 means none.
  static auto tuning(const DeveloperTuning& developer,
                     SoundRequest sound) noexcept
      -> net::protocol::remote_play::SessionTuning;
  /// @brief Maps the feedback style to its wire mode.
  ///
  /// @param style The player's feedback style.
  /// @return FeedbackMode::switch_compatibility for that style, otherwise
  ///     FeedbackMode::standard.
  static auto feedback(FeedbackStyle style) noexcept
      -> net::protocol::remote_play::FeedbackMode;
  /// @brief Applies the player's output request to the session loop settings.
  ///
  /// @param output The output request.
  /// @param base The settings to start from.
  /// @return @p base with the video's largest picture side replaced when the
  ///     output names one, and with audio failure ending the run when either
  ///     asks for it.
  static auto engine(const OutputRequest& output, SessionSettings base)
      -> SessionSettings;
};

}  // namespace libchiaking::play
