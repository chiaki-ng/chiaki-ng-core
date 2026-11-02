/// @file
/// @brief Declares LinkLifecycle, whether a Kakusan link still runs and still
/// listens, and the events it has to report.
#pragma once

#include "src/core/net/kakusan/link_event.hpp"
#include "src/core/net/kakusan/result.hpp"

#include <boost/sml.hpp>

#include <span>
#include <utility>
#include <vector>

namespace libchiaking::net::kakusan::link_lifecycle {

/// @brief Boost.SML, which the machine is written in.
namespace sml = boost::sml;

/// @brief Run state: the link pumps and sends. The initial run state.
struct Running {};
/// @brief Run state: the link failed and reports nothing more.
struct Stopped {};
/// @brief Run state: close() ran; final.
struct Closed {};
/// @brief Hearing state: records are routed. The initial hearing state.
struct Hearing {};
/// @brief Hearing state: a malformed record arrived and later records are
/// dropped.
struct Deaf {};

/// @brief Event: the link failed.
struct Stop {};
/// @brief Event: a malformed record arrived.
struct Deafen {};
/// @brief Event: the link is closed.
struct Close {};

/// @brief The transition table of a link.
///
/// Two orthogonal regions. The run region goes from Running to Stopped on
/// Stop, and from Running or Stopped to Closed on Close; a Close once closed
/// is accepted and changes nothing. The hearing region goes from Hearing to
/// Deaf on Deafen.
struct LinkFsm {
  /// @brief Builds the transition table for Boost.SML.
  /// @return The table of both regions.
  auto operator()() const noexcept {
    using namespace sml;
    const auto unchanged = [] {};
    return make_transition_table(*state<Running> + event<Stop> = state<Stopped>,
                                 state<Running> + event<Close> = state<Closed>,
                                 state<Stopped> + event<Close> = state<Closed>,
                                 state<Closed> + event<Close> / unchanged,

                                 *state<Hearing> + event<Deafen> = state<Deaf>);
  }
};

}  // namespace libchiaking::net::kakusan::link_lifecycle

namespace libchiaking::net::kakusan {

/// @brief Whether a link runs and listens, and the events of the current
/// pump.
///
/// Owned by one Link; not thread-safe. Every event reaches the queue through
/// emit(), stop() and deafen() included; the queue keeps its capacity from
/// one pump to the next.
class LinkLifecycle {
 public:
  /// @brief Forgets the events of the previous pump, keeping the storage.
  void begin_pump() noexcept { events_.clear(); }
  /// @brief Queues an event for the owner.
  /// @param event The event.
  void emit(LinkEvent event) { events_.push_back(std::move(event)); }
  /// @brief Stops the link, queuing Failed when it was running.
  /// @param error Why.
  void stop(Error error) {
    if (machine_.process_event(link_lifecycle::Stop{}))
      emit(link_event::Failed{error});
  }
  /// @brief Stops routing records, queuing Malformed when it still heard.
  /// @param error Why.
  void deafen(Error error) {
    if (machine_.process_event(link_lifecycle::Deafen{}))
      emit(LinkEvent{std::in_place_type<link_event::Malformed>, error});
  }
  /// @brief Closes the link.
  void close() noexcept {
    static_cast<void>(machine_.process_event(link_lifecycle::Close{}));
  }
  /// @brief The events queued since begin_pump().
  /// @return A view valid until the next begin_pump().
  auto events() noexcept -> std::span<LinkEvent> { return events_; }

  /// @brief Tells whether the link still pumps and sends.
  /// @return False once stopped or closed.
  auto running() const noexcept -> bool {
    return machine_.is(link_lifecycle::sml::state<link_lifecycle::Running>);
  }
  /// @brief Tells whether close() ran.
  /// @return True once closed.
  auto closed() const noexcept -> bool {
    return machine_.is(link_lifecycle::sml::state<link_lifecycle::Closed>);
  }
  /// @brief Tells whether records are dropped after a malformed one.
  /// @return True once deaf.
  auto deaf() const noexcept -> bool {
    return machine_.is(link_lifecycle::sml::state<link_lifecycle::Deaf>);
  }

 private:
  /// @brief The events of the current pump.
  std::vector<LinkEvent> events_;
  /// @brief The SML machine.
  link_lifecycle::sml::sm<link_lifecycle::LinkFsm> machine_;
};

}  // namespace libchiaking::net::kakusan
