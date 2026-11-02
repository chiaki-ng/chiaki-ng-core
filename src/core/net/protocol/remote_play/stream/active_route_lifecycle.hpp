/// @file
/// @brief Declares ActiveRouteLifecycle, whether an active session's route
/// still runs, whether Takion was lost before it connected, and whether the
/// queued media outlives the stop.
#pragma once

#include <boost/sml.hpp>

#include <cstddef>

namespace libchiaking::net::protocol::remote_play::active_route_lifecycle {

/// @brief Boost.SML, which the machine is written in.
namespace sml = boost::sml;

/// @brief Route state: the session pumps and posts. The initial route state.
struct Live {};
/// @brief Route state: the session stopped; final.
struct Halted {};
/// @brief Takion state: Takion is in use. The initial Takion state.
struct TakionPresent {};
/// @brief Takion state: Takion failed before it connected; the handshake
/// window runs out without it.
struct TakionLost {};
/// @brief Media state: a stop drops the queued media. The initial media
/// state.
struct DropMedia {};
/// @brief Media state: a stop keeps the queued media, because the console
/// disconnected while streaming.
struct KeepMedia {};

/// @brief Event: the session stops.
struct Halt {};
/// @brief Event: Takion failed before it connected.
struct LoseTakion {};
/// @brief Event: the console disconnected while streaming.
struct Keep {};

/// @brief The transition table of an active session's route.
///
/// Three orthogonal regions, each moving once from its initial state; a
/// repeated event is accepted and changes nothing: Live to Halted on Halt,
/// TakionPresent to TakionLost on LoseTakion, DropMedia to KeepMedia on Keep.
struct RouteFsm {
  /// @brief Builds the transition table for Boost.SML.
  /// @return The table of all three regions.
  auto operator()() const noexcept {
    using namespace sml;
    const auto unchanged = [] {};
    return make_transition_table(
        *state<Live> + sml::event<Halt> = state<Halted>,
        state<Halted> + sml::event<Halt> / unchanged,

        *state<TakionPresent> + sml::event<LoseTakion> = state<TakionLost>,
        state<TakionLost> + sml::event<LoseTakion> / unchanged,

        *state<DropMedia> + sml::event<Keep> = state<KeepMedia>,
        state<KeepMedia> + sml::event<Keep> / unchanged);
  }
};

}  // namespace libchiaking::net::protocol::remote_play::active_route_lifecycle

namespace libchiaking::net::protocol::remote_play {

/// @brief Where an active session's route stands.
///
/// Owned by one ActiveSession and driven from its pump thread; not
/// thread-safe.
class ActiveRouteLifecycle {
 public:
  /// @brief Records that the session stopped.
  void halt() noexcept {
    static_cast<void>(machine_.process_event(active_route_lifecycle::Halt{}));
  }
  /// @brief Records that Takion failed before it connected.
  void lose_takion() noexcept {
    static_cast<void>(
        machine_.process_event(active_route_lifecycle::LoseTakion{}));
  }
  /// @brief Records that the queued media must outlive the stop.
  void keep_media() noexcept {
    static_cast<void>(machine_.process_event(active_route_lifecycle::Keep{}));
  }

  /// @brief Tells whether the session stopped.
  /// @return True once halted.
  auto stopped() const noexcept -> bool {
    return machine_.is(
        active_route_lifecycle::sml::state<active_route_lifecycle::Halted>);
  }
  /// @brief Tells whether Takion was lost before it connected.
  /// @return True once lost.
  auto takion_lost() const noexcept -> bool {
    return machine_.is(
        active_route_lifecycle::sml::state<active_route_lifecycle::TakionLost>);
  }
  /// @brief Tells whether a stop keeps the queued media.
  /// @return True once the console disconnected while streaming.
  auto keeps_media() const noexcept -> bool {
    return machine_.is(
        active_route_lifecycle::sml::state<active_route_lifecycle::KeepMedia>);
  }

 private:
  /// @brief The SML machine.
  active_route_lifecycle::sml::sm<active_route_lifecycle::RouteFsm> machine_;
};

}  // namespace libchiaking::net::protocol::remote_play
