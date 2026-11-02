/// @file
/// @brief Declares StagingLifecycle, whether the OpenSSL secure datagram
/// session routes the library's output into a staged retransmission copy,
/// and whether a new handshake flight started since the last step.
#pragma once

#include <boost/sml.hpp>

namespace libchiaking::platform::openssl::staging_lifecycle {

/// @brief Boost.SML, which the machine is written in.
namespace sml = boost::sml;

/// @brief Routing state: output goes straight out. The initial routing state.
struct Direct {};
/// @brief Routing state: output goes into the newest staged copy of the
/// flight.
struct Staging {};
/// @brief Flight state: no new flight since the last step. The initial flight
/// state.
struct Quiet {};
/// @brief Flight state: the library started a new flight.
struct Started {};

/// @brief Event: the library started a new flight.
struct BeginFlight {};
/// @brief Event: the library asked for another staged copy of the flight.
struct Stage {};
/// @brief Event: staging stops, at the end of a step or on release.
struct Unstage {};
/// @brief Event: the step took note of the new flight.
struct Land {};

/// @brief The transition table of the staging.
///
/// Two orthogonal regions. Routing goes from Direct to Staging on Stage and
/// back on Unstage or BeginFlight. The flight region goes from Quiet to
/// Started on BeginFlight and back on Land. Every event is accepted in every
/// state; the repeats change nothing.
struct StagingFsm {
  /// @brief Builds the transition table for Boost.SML.
  /// @return The table of both regions.
  auto operator()() const noexcept {
    using namespace sml;
    const auto unchanged = [] {};
    return make_transition_table(
        *state<Direct> + event<Stage> = state<Staging>,
        state<Direct> + event<Unstage> / unchanged,
        state<Direct> + event<BeginFlight> / unchanged,
        state<Staging> + event<Stage> / unchanged,
        state<Staging> + event<Unstage> = state<Direct>,
        state<Staging> + event<BeginFlight> = state<Direct>,

        *state<Quiet> + event<BeginFlight> = state<Started>,
        state<Quiet> + event<Land> / unchanged,
        state<Started> + event<BeginFlight> / unchanged,
        state<Started> + event<Land> = state<Quiet>);
  }
};

}  // namespace libchiaking::platform::openssl::staging_lifecycle

namespace libchiaking::platform::openssl {

/// @brief Where the session's retransmission staging stands.
///
/// Owned by one OpenSSL secure datagram session and driven from its step and
/// from the library's timer callback inside that step; not thread-safe.
class StagingLifecycle {
 public:
  /// @brief Records that the library started a new flight; output goes
  /// straight out again.
  void begin_flight() noexcept { apply(staging_lifecycle::BeginFlight{}); }
  /// @brief Routes output into the newest staged copy.
  void stage() noexcept { apply(staging_lifecycle::Stage{}); }
  /// @brief Routes output straight out.
  void unstage() noexcept { apply(staging_lifecycle::Unstage{}); }
  /// @brief Takes note of a new flight.
  /// @return Whether a flight started since the last call.
  auto take_flight() noexcept -> bool {
    const bool started =
        machine_.is(staging_lifecycle::sml::state<staging_lifecycle::Started>);
    apply(staging_lifecycle::Land{});
    return started;
  }
  /// @brief Tells whether output goes into a staged copy.
  /// @return True while staging.
  auto staging() const noexcept -> bool {
    return machine_.is(
        staging_lifecycle::sml::state<staging_lifecycle::Staging>);
  }

 private:
  /// @brief Feeds an event; the table accepts every event in every state.
  /// @tparam Event The event type.
  /// @param event The event.
  template <class Event>
  void apply(const Event& event) noexcept {
    static_cast<void>(machine_.process_event(event));
  }

  /// @brief The SML machine.
  staging_lifecycle::sml::sm<staging_lifecycle::StagingFsm> machine_;
};

}  // namespace libchiaking::platform::openssl
