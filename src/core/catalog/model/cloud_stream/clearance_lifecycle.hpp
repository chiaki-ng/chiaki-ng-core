/// @file
/// @brief Declares ClearanceLifecycle, the order in which a cloud streaming
/// launch is cleared: standing, cloud streaming, the access route, ownership
/// when it is unknown, the entitlement, the selected title, exclusions and
/// the release.
#pragma once

#include <boost/sml.hpp>

#include <cstdint>

namespace libchiaking::catalog::cloud_stream {

/// @brief The check a launch clearance has reached.
enum class ClearanceStage : std::uint8_t {
  /// @brief The game's listing, visibility and remembered refusals.
  standing,
  /// @brief The account's cloud streaming access.
  cloud_streaming,
  /// @brief Which entitlement the launch goes through.
  access_route,
  /// @brief Whether the account owns the game, when the listing does not
  /// know.
  ownership,
  /// @brief The entitlement, read afresh.
  entitlement,
  /// @brief The cloud title the launch selects.
  selected_title,
  /// @brief The exclusion list.
  exclusions,
  /// @brief The release date.
  release,
};

}  // namespace libchiaking::catalog::cloud_stream

namespace libchiaking::catalog::cloud_stream::clearance_lifecycle {

/// @brief Boost.SML, which the machine is written in.
namespace sml = boost::sml;

/// @brief Checking the game's standing. The initial state.
struct StandingState {};
/// @brief Checking cloud streaming access.
struct CloudStreamingState {};
/// @brief Taking the access route.
struct AccessRouteState {};
/// @brief Rechecking ownership.
struct OwnershipState {};
/// @brief Rereading the entitlement.
struct EntitlementState {};
/// @brief Resolving the selected title.
struct SelectedTitleState {};
/// @brief Checking the exclusions.
struct ExclusionsState {};
/// @brief Checking the release; the last check.
struct ReleaseState {};

/// @brief Event: the current check passed; go on to the next one.
struct Pass {};
/// @brief Event: the access route needs ownership rechecked first.
struct NeedOwnership {};

/// @brief The transition table of a launch clearance.
///
/// Standing, cloud streaming, the access route, the entitlement, the
/// selected title, the exclusions and the release follow one another on
/// Pass. From the access route, NeedOwnership goes to the ownership recheck
/// instead, which goes on to the entitlement on Pass. A check that refuses
/// the launch feeds nothing, so the machine stays where the refusal came
/// from.
struct ClearanceFsm {
  /// @brief Builds the transition table for Boost.SML.
  /// @return The table.
  auto operator()() const noexcept {
    using namespace sml;
    return make_transition_table(
        *state<StandingState> + event<Pass> = state<CloudStreamingState>,
        state<CloudStreamingState> + event<Pass> = state<AccessRouteState>,
        state<AccessRouteState> + event<NeedOwnership> = state<OwnershipState>,
        state<AccessRouteState> + event<Pass> = state<EntitlementState>,
        state<OwnershipState> + event<Pass> = state<EntitlementState>,
        state<EntitlementState> + event<Pass> = state<SelectedTitleState>,
        state<SelectedTitleState> + event<Pass> = state<ExclusionsState>,
        state<ExclusionsState> + event<Pass> = state<ReleaseState>);
  }
};

}  // namespace libchiaking::catalog::cloud_stream::clearance_lifecycle

namespace libchiaking::catalog::cloud_stream {

/// @brief Which check a launch clearance runs next.
///
/// Owned by one clearance attempt; not thread-safe. Events the table does
/// not allow change nothing.
class ClearanceLifecycle {
 public:
  /// @brief Moves on from a passed check.
  void pass() { accept(clearance_lifecycle::Pass{}); }
  /// @brief Moves from the access route to the ownership recheck.
  void need_ownership() { accept(clearance_lifecycle::NeedOwnership{}); }
  /// @brief The check reached.
  /// @return The stage.
  auto stage() const noexcept -> ClearanceStage {
    namespace lifecycle = clearance_lifecycle;
    if (in<lifecycle::CloudStreamingState>())
      return ClearanceStage::cloud_streaming;
    if (in<lifecycle::AccessRouteState>()) return ClearanceStage::access_route;
    if (in<lifecycle::OwnershipState>()) return ClearanceStage::ownership;
    if (in<lifecycle::EntitlementState>()) return ClearanceStage::entitlement;
    if (in<lifecycle::SelectedTitleState>())
      return ClearanceStage::selected_title;
    if (in<lifecycle::ExclusionsState>()) return ClearanceStage::exclusions;
    if (in<lifecycle::ReleaseState>()) return ClearanceStage::release;
    return ClearanceStage::standing;
  }

 private:
  /// @brief Tells whether the machine is in a state.
  /// @tparam State One of the clearance states.
  /// @return True when it is.
  template <class State>
  auto in() const noexcept -> bool {
    return machine_.is(clearance_lifecycle::sml::state<State>);
  }
  /// @brief Feeds an event; one the table does not allow changes nothing.
  /// @tparam Event The event type.
  /// @param event The event.
  template <class Event>
  void accept(const Event& event) {
    static_cast<void>(machine_.process_event(event));
  }

  /// @brief The SML machine.
  clearance_lifecycle::sml::sm<clearance_lifecycle::ClearanceFsm> machine_;
};

}  // namespace libchiaking::catalog::cloud_stream
