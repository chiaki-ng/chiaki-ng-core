/// @file
/// @brief Declares CommandRoute, the path the session's prompts and commands
/// take to the link, with the rules for when they are accepted.
#pragma once

#include "libchiaking/detail/play/input_refusal.hpp"

#include "libchiaking/platform/diagnostics/provider.hpp"
#include "src/core/net/protocol/remote_play/stream/stream.hpp"
#include "src/core/play/model/venue.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace libchiaking::play {

class LinkDriver;
class LockedSessionLifecycle;

/// @brief Where player commands go and whether they are accepted now.
///
/// Bundles the session's link driver, lifecycle, log and venue for the
/// prompts, seats and commands of one session. Commands are accepted only
/// while the session runs and nobody asked it to end; some only for a
/// console. Holds references; the session core owns everything referred to.
/// The const members may be called from any thread.
class CommandRoute {
 public:
  /// @brief Builds the route.
  ///
  /// @param driver The link driver; must outlive the route.
  /// @param lifecycle The session's lifecycle; must outlive the route.
  /// @param log Receives log lines; must outlive the route.
  /// @param venue Where the session runs; empty when unknown.
  CommandRoute(LinkDriver& driver, const LockedSessionLifecycle& lifecycle,
               platform::DiagnosticsProvider& log,
               std::optional<Venue> venue) noexcept;

  /// @brief Reports whether commands are accepted.
  ///
  /// @return `true` while the stream runs and nobody asked the session to end.
  auto accepting() const noexcept -> bool;
  /// @brief Reports whether console-only commands are accepted.
  ///
  /// @return `true` when CommandRoute::console_refusal is empty.
  auto console() const noexcept -> bool;
  /// @brief Gives the reason a command is refused now.
  ///
  /// @return InputRefusal::not_running when commands are not accepted; empty
  ///     otherwise.
  auto refusal() const noexcept -> std::optional<InputRefusal>;
  /// @brief Gives the reason a console-only command is refused now.
  ///
  /// @return InputRefusal::not_running when commands are not accepted;
  ///     InputRefusal::wrong_route for a cloud session or an unknown venue;
  ///     empty otherwise.
  auto console_refusal() const noexcept -> std::optional<InputRefusal>;
  /// @brief Gives the reason a microphone request is refused now.
  ///
  /// @return InputRefusal::not_running when commands are not accepted;
  ///     InputRefusal::wrong_route for an unknown venue; empty otherwise,
  ///     since consoles, PS5 cloud and LegacyCloudStream all take a microphone.
  auto microphone_refusal() const noexcept -> std::optional<InputRefusal>;
  /// @brief Gives the reason 3D audio cannot be switched.
  ///
  /// @return InputRefusal::wrong_route on LegacyCloudStream or an unknown
  /// venue; empty
  ///     otherwise, whether or not the session runs.
  auto audio_3d_refusal() const noexcept -> std::optional<InputRefusal>;
  /// @brief Gives the reason a PS5 cloud request is refused now.
  ///
  /// @return InputRefusal::wrong_route unless the session runs on PS5 cloud;
  ///     empty otherwise, whether or not the session runs.
  auto cloud_refusal() const noexcept -> std::optional<InputRefusal>;
  /// @brief Gives the reason a game invitation cannot be joined.
  ///
  /// @return InputRefusal::wrong_route unless the session runs in the cloud,
  ///     on PS5 cloud or LegacyCloudStream; empty otherwise, whether or not the
  ///     session runs.
  auto invitation_refusal() const noexcept -> std::optional<InputRefusal>;
  /// @brief Gives the reason a LegacyCloudStream request is refused now.
  ///
  /// @return InputRefusal::wrong_route unless the session runs on
  /// LegacyCloudStream;
  ///     empty otherwise, whether or not the session runs.
  auto old_service_refusal() const noexcept -> std::optional<InputRefusal>;
  /// @brief Reads the venue.
  ///
  /// @return Where the session runs; empty when unknown.
  auto venue() const noexcept -> std::optional<Venue> { return venue_; }
  /// @brief Names the venue for logs.
  ///
  /// @return The venue's name, or "unknown".
  auto name() const noexcept -> std::string_view;
  /// @brief Gives the link driver.
  ///
  /// @return The driver commands are sent through.
  auto driver() const noexcept -> LinkDriver& { return driver_; }
  /// @brief Wakes the session thread so it sends what was queued.
  void wake() const noexcept;
  /// @brief Writes one log line.
  ///
  /// @param severity The line's severity.
  /// @param message The line.
  void log(platform::LogSeverity severity,
           std::string_view message) const noexcept;
  /// @brief Logs a command's result: info on success, warning with the error's
  /// name otherwise. Never throws.
  ///
  /// @param what The command named in the line.
  /// @param result The link's result.
  void report(std::string_view what,
              const Expected<void, net::protocol::remote_play::StreamError>&
                  result) const noexcept;

  /// @brief Names a stream error for logs.
  ///
  /// @param error The error.
  /// @return The enumerator's name.
  static auto result_name(
      net::protocol::remote_play::StreamError error) noexcept
      -> std::string_view;

 private:
  /// @brief The link driver.
  LinkDriver& driver_;
  /// @brief The session's lifecycle.
  const LockedSessionLifecycle& lifecycle_;
  /// @brief Receives log lines.
  platform::DiagnosticsProvider& log_;
  /// @brief Where the session runs; empty when unknown.
  std::optional<Venue> venue_;
};

}  // namespace libchiaking::play
