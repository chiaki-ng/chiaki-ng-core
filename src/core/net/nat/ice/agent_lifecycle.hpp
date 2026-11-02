/// @file
/// @brief Declares AgentLifecycle, how far an ICE agent has got, whether it
/// failed or was closed, whether it holds checks from before connect(), and
/// whether consent expired, with the events it has to report.
#pragma once

#include "src/core/net/nat/ice/agent_event.hpp"

#include <boost/sml.hpp>

#include <deque>
#include <optional>
#include <utility>

namespace libchiaking::net::nat::ice::agent_lifecycle {

/// @brief Boost.SML, which the machine is written in.
namespace sml = boost::sml;

/// @brief Progress state: candidates are being gathered. The initial progress
/// state.
struct Gathering {};
/// @brief Progress state: gathering finished and the local parameters were
/// reported.
struct Gathered {};
/// @brief Progress state: connect() succeeded and checks run.
struct Connected {};
/// @brief Health state: the agent works. The initial health state.
struct Healthy {};
/// @brief Health state: the agent failed and does nothing more.
struct Broken {};
/// @brief Closure state: close() has not run. The initial closure state.
struct Open {};
/// @brief Closure state: close() ran; final.
struct Shut {};
/// @brief Early state: no checks from before connect() await processing. The
/// initial early state.
struct NoneHeld {};
/// @brief Early state: checks from before connect() await processing.
struct Held {};
/// @brief Consent state: consent has not expired. The initial consent state.
struct ConsentKept {};
/// @brief Consent state: consent expired.
struct ConsentLost {};

/// @brief Event: gathering finished.
struct FinishGathering {};
/// @brief Event: connect() succeeded.
struct Connect {};
/// @brief Event: the agent failed.
struct Fail {};
/// @brief Event: close() ran.
struct Close {};
/// @brief Event: checks from before connect() await processing.
struct Hold {};
/// @brief Event: the held checks were processed or dropped.
struct Release {};
/// @brief Event: consent expired.
struct ExpireConsent {};

/// @brief The transition table of an ICE agent.
///
/// Five orthogonal regions:
///
/// - progress: Gathering, Gathered, Connected, each once;
/// - health: Healthy to Broken on Fail;
/// - closure: Open to Shut on Close; a Close once shut is accepted and
///   changes nothing;
/// - early: NoneHeld and Held; Release is accepted while none are held;
/// - consent: ConsentKept to ConsentLost, once.
struct AgentFsm {
  /// @brief Builds the transition table for Boost.SML.
  /// @return The table of all five regions.
  auto operator()() const noexcept {
    using namespace sml;
    const auto unchanged = [] {};
    return make_transition_table(
        *state<Gathering> + sml::event<FinishGathering> = state<Gathered>,
        state<Gathered> + sml::event<Connect> = state<Connected>,

        *state<Healthy> + sml::event<Fail> = state<Broken>,

        *state<Open> + sml::event<Close> = state<Shut>,
        state<Shut> + sml::event<Close> / unchanged,

        *state<NoneHeld> + sml::event<Hold> = state<Held>,
        state<NoneHeld> + sml::event<Release> / unchanged,
        state<Held> + sml::event<Release> = state<NoneHeld>,

        *state<ConsentKept> + sml::event<ExpireConsent> = state<ConsentLost>);
  }
};

}  // namespace libchiaking::net::nat::ice::agent_lifecycle

namespace libchiaking::net::nat::ice {

/// @brief Where an ICE agent stands, and the events it has queued for its
/// owner.
///
/// Owned by one Agent; not thread-safe. Every event reaches the queue through
/// emit(), fail() included.
class AgentLifecycle {
 public:
  /// @brief Starts gathering, healthy, open, holding nothing and with consent
  /// kept.
  AgentLifecycle() = default;

  /// @brief Queues an event for the owner.
  /// @param event The event.
  void emit(Event event) { events_.push_back(std::move(event)); }
  /// @brief Takes the oldest queued event.
  /// @return The event, or nothing when none is queued.
  auto take() -> std::optional<Event> {
    if (events_.empty()) return std::nullopt;
    auto event = std::move(events_.front());
    events_.pop_front();
    return event;
  }

  /// @brief Records that gathering finished.
  void finish_gathering() { apply(agent_lifecycle::FinishGathering{}); }
  /// @brief Records that connect() succeeded.
  /// @return False unless gathering finished and connect() had not succeeded
  ///     before.
  auto connect() -> bool {
    return machine_.process_event(agent_lifecycle::Connect{});
  }
  /// @brief Records that the agent failed, queuing event::Failed the first
  /// time.
  /// @param error Why.
  void fail(Error error) {
    if (machine_.process_event(agent_lifecycle::Fail{}))
      emit(event::Failed{error});
  }
  /// @brief Records that close() ran, dropping the queued events.
  void close() {
    apply(agent_lifecycle::Close{});
    events_.clear();
  }
  /// @brief Records that checks from before connect() await processing.
  void hold() { apply(agent_lifecycle::Hold{}); }
  /// @brief Records that no held checks await processing.
  void release() { apply(agent_lifecycle::Release{}); }
  /// @brief Records that consent expired.
  void expire_consent() { apply(agent_lifecycle::ExpireConsent{}); }

  /// @brief Tells whether gathering finished.
  /// @return True once gathered, connected or not.
  auto gathered() const noexcept -> bool {
    return !machine_.is(
        agent_lifecycle::sml::state<agent_lifecycle::Gathering>);
  }
  /// @brief Tells whether connect() succeeded.
  /// @return True once connected.
  auto connected() const noexcept -> bool {
    return machine_.is(agent_lifecycle::sml::state<agent_lifecycle::Connected>);
  }
  /// @brief Tells whether the agent failed.
  /// @return True once failed.
  auto failed() const noexcept -> bool {
    return machine_.is(agent_lifecycle::sml::state<agent_lifecycle::Broken>);
  }
  /// @brief Tells whether close() ran.
  /// @return True once closed.
  auto closed() const noexcept -> bool {
    return machine_.is(agent_lifecycle::sml::state<agent_lifecycle::Shut>);
  }
  /// @brief Tells whether checks from before connect() await processing.
  /// @return True while held.
  auto holding() const noexcept -> bool {
    return machine_.is(agent_lifecycle::sml::state<agent_lifecycle::Held>);
  }
  /// @brief Tells whether consent expired.
  /// @return True once expired.
  auto consent_expired() const noexcept -> bool {
    return machine_.is(
        agent_lifecycle::sml::state<agent_lifecycle::ConsentLost>);
  }

 private:
  /// @brief Feeds an event; one the table does not allow changes nothing.
  /// @tparam Change The event type.
  /// @param change The event.
  template <class Change>
  void apply(const Change& change) {
    static_cast<void>(machine_.process_event(change));
  }

  /// @brief The events queued for the owner.
  std::deque<Event> events_;
  /// @brief The SML machine.
  agent_lifecycle::sml::sm<agent_lifecycle::AgentFsm> machine_;
};

}  // namespace libchiaking::net::nat::ice
