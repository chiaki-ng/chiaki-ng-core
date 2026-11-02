/// @file
/// @brief Declares Route and Starting, the port through which a Console starts
/// a play session.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/users/ps_user.hpp"
#include "libchiaking/platform/runtime/stop_token.hpp"

#include <cstdint>
#include <memory>

namespace libchiaking::media {
class MediaSink;
}

namespace libchiaking::play {
struct Preferences;
class Session;
class SessionEvents;
class StartRefusal;
}  // namespace libchiaking::play

namespace libchiaking::consoles {

class Console;

/// @brief Which way a play session reaches a console.
enum class Route : std::uint8_t {
  /// @brief Let the library choose from the console's presence and pairings.
  automatic,
  /// @brief Connect directly on the home network. Needs a pairing and a known
  /// address.
  home,
  /// @brief Connect through PSN from anywhere. Needs a PSN sign-in.
  internet,
};

/// @brief Port through which the consoles context starts play sessions.
///
/// Starting a session belongs to the play context, which implements this
/// interface. The application attaches the implementation with
/// KnownConsoles::start_through, after which Console::start and
/// Console::route_for forward here.
class Starting {
 public:
  /// @brief Allows deletion through the interface.
  virtual ~Starting() = default;

  /// @brief Starts a Remote Play session with a console.
  ///
  /// @param player The user to play as. Shared with the session.
  /// @param console The console to play on. It must be the object its
  ///     KnownConsoles currently holds.
  /// @param preferences Video, audio and input preferences for the session.
  /// @param media Receives the session's video and audio.
  /// @param events Receives the session's events.
  /// @param route Which way to reach the console.
  /// @param stop Cancels the start while the route is being worked out.
  /// @return The session, or the reason none was created.
  /// @see Console::start
  [[nodiscard]] virtual auto start(std::shared_ptr<users::PsUser> player,
                                   Console& console,
                                   const play::Preferences& preferences,
                                   media::MediaSink& media,
                                   play::SessionEvents& events, Route route,
                                   const platform::StopToken& stop)
      -> Expected<std::shared_ptr<play::Session>, play::StartRefusal> = 0;
  /// @brief Works out which route Route::automatic would take for a console
  /// now.
  ///
  /// @param player The user who would play.
  /// @param console The console to play on.
  /// @param stop Cancels any lookups.
  /// @return Route::home or Route::internet, or the reason the route could
  ///     not be worked out.
  /// @see Console::route_for
  [[nodiscard]] virtual auto route_for(users::PsUser& player, Console& console,
                                       const platform::StopToken& stop)
      -> Expected<Route, play::StartRefusal> = 0;
};

}  // namespace libchiaking::consoles
