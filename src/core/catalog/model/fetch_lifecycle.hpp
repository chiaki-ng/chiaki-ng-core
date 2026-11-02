/// @file
/// @brief Declares the state machine of an account's full catalog fetch: the
/// sign-in check, the cloud list, the owned library, the two PS5 categories,
/// streaming access, matching, trusted time and exclusions, in that order.
///
/// The machine only decides; it performs nothing. Each transition updates the
/// LifecycleContext, and once an event is taken Lifecycle queues a Publish of
/// the new FetchState, read from the machine's state, which the catalog's
/// FetchBoard hands to observers. Every event carries the attempt it belongs
/// to; one from an older attempt is dropped, as is one the table does not
/// allow from the current state.
#pragma once

#include "libchiaking/detail/catalog/fetch_state.hpp"
#include "src/core/catalog/model/unanswered.hpp"

#include <boost/sml.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace libchiaking::catalog::fetch_lifecycle {

/// @brief Boost.SML, which the machine is written in.
namespace sml = boost::sml;

/// @brief No fetch has run since the account was signed in or started over.
struct IdleState {};
/// @brief The user's sign-in is being checked.
struct CheckingSignInState {};
/// @brief The cloud list is being read.
struct CloudListState {};
/// @brief The owned library is being read.
struct OwnedLibraryState {};
/// @brief The Premium category is being read.
struct PremiumCategoryState {};
/// @brief The for-sale category is being read.
struct ForSaleCategoryState {};
/// @brief Streaming access is being read.
struct StreamingAccessState {};
/// @brief The answers are being matched into games.
struct MatchingState {};
/// @brief Trusted time is being synced.
struct TrustedTimeState {};
/// @brief The exclusion list is being read.
struct ExclusionsState {};
/// @brief The fetch ended with games.
struct ReadyState {};
/// @brief The fetch ended without games.
struct UnansweredState {};
/// @brief The fetch was cancelled.
struct CancelledState {};

/// @brief A new fetch starts, beginning with the sign-in check.
struct Start {
  /// @brief The new attempt; must be greater than every earlier one.
  std::uint64_t attempt_id{};
};
/// @brief The next step begins.
struct StepBegan {
  /// @brief The attempt the event belongs to.
  std::uint64_t attempt_id{};
  /// @brief The step; must be the one after the step that has just ended.
  FetchStep step{FetchStep::cloud_list};
  /// @brief What the step starts doing, when already known.
  std::optional<FetchActivity> activity{};
};
/// @brief The current step starts doing something else.
struct ActivityChanged {
  /// @brief The attempt the event belongs to.
  std::uint64_t attempt_id{};
  /// @brief The new activity.
  FetchActivity activity{FetchActivity::reusing_held_answer};
};
/// @brief The current activity has read more pages.
struct PagesRead {
  /// @brief The attempt the event belongs to.
  std::uint64_t attempt_id{};
  /// @brief Pages read so far by the activity.
  std::uint32_t read{};
  /// @brief Pages the activity expects in all, when known.
  std::optional<std::uint32_t> expected{};
};
/// @brief The trusted-time step asks another time server.
struct TimeServerAsked {
  /// @brief The attempt the event belongs to.
  std::uint64_t attempt_id{};
  /// @brief The server's host name; read only while the event is processed.
  std::string_view host{};
};
/// @brief The current step has ended.
struct StepEnded {
  /// @brief The attempt the event belongs to.
  std::uint64_t attempt_id{};
  /// @brief How it was answered.
  StepOutcome outcome{StepOutcome::fetched};
};
/// @brief The fetch ends without games.
struct Failed {
  /// @brief The attempt the event belongs to.
  std::uint64_t attempt_id{};
  /// @brief Why; never a cancellation, which is Cancelled.
  Unanswered why{Unanswered::Reason::unavailable};
};
/// @brief The fetch is cancelled.
struct Cancelled {
  /// @brief The attempt the event belongs to.
  std::uint64_t attempt_id{};
};
/// @brief The fetch ends with games, after the last step has ended.
struct Finished {
  /// @brief The attempt the event belongs to.
  std::uint64_t attempt_id{};
};
/// @brief An ended fetch is forgotten, as when the account starts over.
struct Reset {};

/// @brief The only effect: a new state to hand to observers.
struct Publish {
  /// @brief The state.
  FetchState state;
};

/// @brief What the machine knows beside its state, and the effects it queued.
///
/// Owned by the caller and borrowed by Lifecycle; read the effects after each
/// event and drain them.
struct LifecycleContext {
  /// @brief States to publish, oldest first.
  std::deque<Publish> effects{};
  /// @brief The attempt in progress; empty when none runs.
  std::optional<std::uint64_t> active_attempt_id{};
  /// @brief The latest attempt started, running or ended.
  std::uint64_t attempt{};
  /// @brief What the step is doing.
  std::optional<FetchActivity> activity{};
  /// @brief Pages read by the activity.
  std::uint32_t pages_read{};
  /// @brief Pages the activity expects.
  std::optional<std::uint32_t> pages_expected{};
  /// @brief The time server the trusted-time step is asking.
  std::optional<std::string> time_server{};
  /// @brief The steps passed.
  std::vector<PassedStep> passed{};
  /// @brief Why the fetch ended without games.
  std::optional<GamesFailure> failure{};

  /// @brief Whether an event belongs to the attempt in progress.
  ///
  /// @param attempt_id The event's attempt.
  /// @return True when it names the running attempt.
  auto is_current(std::uint64_t attempt_id) const noexcept -> bool {
    return active_attempt_id && *active_attempt_id == attempt_id;
  }
  /// @brief The state as it stands at a step.
  ///
  /// @param step The step the machine is in.
  /// @return A snapshot of the fields above at that step.
  auto snapshot(FetchStep step) const -> FetchState {
    return {attempt,        step,        activity, pages_read,
            pages_expected, time_server, passed,   failure};
  }
  /// @brief Clears what an activity has read and which server it asks.
  void forget_details() noexcept {
    pages_read = 0;
    pages_expected.reset();
    time_server.reset();
  }
};

/// @brief Step state: the step reached is still working. The initial step
/// state.
struct StepOpenState {};
/// @brief Step state: the step reached has ended, so the next may begin.
struct StepClosedState {};
/// @brief Internal event: a step began, or the fetch started or was reset.
struct OpenStep {};
/// @brief Internal event: the step reached ended.
struct CloseStep {};

/// @brief The transition table.
///
/// Two orthogonal regions: the fetch's steps, and whether the step reached
/// has ended. Lifecycle feeds the second region only after the first took an
/// event, and lets a step begin, or the fetch finish, only once the step
/// before has ended.
struct LifecycleFsm {
  /// @brief Builds the table.
  ///
  /// @return The SML transition table.
  auto operator()() const noexcept {
    using namespace sml;
    const auto start = [](const Start& event, LifecycleContext& context) {
      context.active_attempt_id = event.attempt_id;
      context.attempt = event.attempt_id;
      context.activity.reset();
      context.forget_details();
      context.passed.clear();
      context.failure.reset();
    };
    const auto begin = [](const StepBegan& event, LifecycleContext& context) {
      context.activity = event.activity;
      context.forget_details();
    };
    const auto turn = [](const ActivityChanged& event,
                         LifecycleContext& context) {
      context.activity = event.activity;
      context.forget_details();
    };
    const auto count = [](const PagesRead& event, LifecycleContext& context) {
      context.pages_read = event.read;
      context.pages_expected = event.expected;
    };
    const auto ask = [](const TimeServerAsked& event,
                        LifecycleContext& context) {
      context.time_server = std::string{event.host};
    };
    const auto names_another = [](const TimeServerAsked& event,
                                  LifecycleContext& context) {
      return context.time_server != event.host;
    };
    const auto end = [](LifecycleContext& context) {
      context.activity.reset();
      context.forget_details();
    };
    const auto fail = [](const Failed& event, LifecycleContext& context) {
      context.failure = event.why.as(GamesReason::unavailable);
      context.activity.reset();
      context.forget_details();
      context.active_attempt_id.reset();
    };
    const auto cancel = [](LifecycleContext& context) {
      context.activity.reset();
      context.forget_details();
      context.active_attempt_id.reset();
    };
    const auto finish = [](LifecycleContext& context) {
      context.active_attempt_id.reset();
    };
    const auto forget = [](LifecycleContext& context) {
      context.activity.reset();
      context.forget_details();
      context.passed.clear();
      context.failure.reset();
    };
    const auto begins = [](FetchStep next) {
      return [next](const StepBegan& event) { return event.step == next; };
    };
    return make_transition_table(
        *state<IdleState> + event<Start> / start = state<CheckingSignInState>,
        state<ReadyState> + event<Start> / start = state<CheckingSignInState>,
        state<UnansweredState> + event<Start> / start =
            state<CheckingSignInState>,
        state<CancelledState> + event<Start> / start =
            state<CheckingSignInState>,
        state<ReadyState> + event<Reset> / forget = state<IdleState>,
        state<UnansweredState> + event<Reset> / forget = state<IdleState>,
        state<CancelledState> + event<Reset> / forget = state<IdleState>,

        state<CheckingSignInState> +
            event<StepBegan>[begins(FetchStep::cloud_list)] / begin =
            state<CloudListState>,
        state<CloudListState> +
            event<StepBegan>[begins(FetchStep::owned_library)] / begin =
            state<OwnedLibraryState>,
        state<OwnedLibraryState> +
            event<StepBegan>[begins(FetchStep::premium_category)] / begin =
            state<PremiumCategoryState>,
        state<PremiumCategoryState> +
            event<StepBegan>[begins(FetchStep::for_sale_category)] / begin =
            state<ForSaleCategoryState>,
        state<ForSaleCategoryState> +
            event<StepBegan>[begins(FetchStep::streaming_access)] / begin =
            state<StreamingAccessState>,
        state<StreamingAccessState> +
            event<StepBegan>[begins(FetchStep::matching)] / begin =
            state<MatchingState>,
        state<MatchingState> +
            event<StepBegan>[begins(FetchStep::trusted_time)] / begin =
            state<TrustedTimeState>,
        state<TrustedTimeState> +
            event<StepBegan>[begins(FetchStep::exclusions)] / begin =
            state<ExclusionsState>,
        state<ExclusionsState> + event<Finished> / finish = state<ReadyState>,

        state<CheckingSignInState> + event<ActivityChanged> / turn,
        state<CloudListState> + event<ActivityChanged> / turn,
        state<OwnedLibraryState> + event<ActivityChanged> / turn,
        state<PremiumCategoryState> + event<ActivityChanged> / turn,
        state<ForSaleCategoryState> + event<ActivityChanged> / turn,
        state<StreamingAccessState> + event<ActivityChanged> / turn,
        state<MatchingState> + event<ActivityChanged> / turn,
        state<TrustedTimeState> + event<ActivityChanged> / turn,
        state<ExclusionsState> + event<ActivityChanged> / turn,

        state<CheckingSignInState> + event<PagesRead> / count,
        state<CloudListState> + event<PagesRead> / count,
        state<OwnedLibraryState> + event<PagesRead> / count,
        state<PremiumCategoryState> + event<PagesRead> / count,
        state<ForSaleCategoryState> + event<PagesRead> / count,
        state<StreamingAccessState> + event<PagesRead> / count,
        state<MatchingState> + event<PagesRead> / count,
        state<TrustedTimeState> + event<PagesRead> / count,
        state<ExclusionsState> + event<PagesRead> / count,

        state<TrustedTimeState> + event<TimeServerAsked>[names_another] / ask,

        state<CheckingSignInState> + event<StepEnded> / end,
        state<CloudListState> + event<StepEnded> / end,
        state<OwnedLibraryState> + event<StepEnded> / end,
        state<PremiumCategoryState> + event<StepEnded> / end,
        state<ForSaleCategoryState> + event<StepEnded> / end,
        state<StreamingAccessState> + event<StepEnded> / end,
        state<MatchingState> + event<StepEnded> / end,
        state<TrustedTimeState> + event<StepEnded> / end,
        state<ExclusionsState> + event<StepEnded> / end,

        state<CheckingSignInState> + event<Failed> / fail =
            state<UnansweredState>,
        state<CloudListState> + event<Failed> / fail = state<UnansweredState>,
        state<OwnedLibraryState> + event<Failed> / fail =
            state<UnansweredState>,
        state<PremiumCategoryState> + event<Failed> / fail =
            state<UnansweredState>,
        state<ForSaleCategoryState> + event<Failed> / fail =
            state<UnansweredState>,
        state<StreamingAccessState> + event<Failed> / fail =
            state<UnansweredState>,
        state<MatchingState> + event<Failed> / fail = state<UnansweredState>,
        state<TrustedTimeState> + event<Failed> / fail = state<UnansweredState>,
        state<ExclusionsState> + event<Failed> / fail = state<UnansweredState>,

        state<CheckingSignInState> + event<Cancelled> / cancel =
            state<CancelledState>,
        state<CloudListState> + event<Cancelled> / cancel =
            state<CancelledState>,
        state<OwnedLibraryState> + event<Cancelled> / cancel =
            state<CancelledState>,
        state<PremiumCategoryState> + event<Cancelled> / cancel =
            state<CancelledState>,
        state<ForSaleCategoryState> + event<Cancelled> / cancel =
            state<CancelledState>,
        state<StreamingAccessState> + event<Cancelled> / cancel =
            state<CancelledState>,
        state<MatchingState> + event<Cancelled> / cancel =
            state<CancelledState>,
        state<TrustedTimeState> + event<Cancelled> / cancel =
            state<CancelledState>,
        state<ExclusionsState> + event<Cancelled> / cancel =
            state<CancelledState>,

        *state<StepOpenState> + event<CloseStep> = state<StepClosedState>,
        state<StepOpenState> + event<OpenStep> / [] {},
        state<StepClosedState> + event<OpenStep> = state<StepOpenState>);
  }
};

/// @brief One account's fetch machine over a borrowed context.
///
/// Not thread-safe; FetchBoard serialises every call.
class Lifecycle {
 public:
  /// @brief Starts the machine idle over a context.
  ///
  /// @param context The context the machine reads and writes; must outlive
  ///     the machine.
  explicit Lifecycle(LifecycleContext& context)
      : context_(context), machine_(context) {}

  /// @brief Starts a new attempt.
  ///
  /// @param event The start; its attempt must be greater than every earlier
  ///     one, and no attempt may be running.
  /// @return Whether the machine accepted it.
  [[nodiscard]] auto process(const Start& event) -> bool {
    if (event.attempt_id <= context_.attempt || context_.active_attempt_id)
      return false;
    return taken(event);
  }
  /// @brief Forgets an ended attempt.
  ///
  /// @param event The reset.
  /// @return Whether the machine accepted it; refused while an attempt runs
  ///     and when already idle.
  [[nodiscard]] auto process(const Reset& event) -> bool {
    return taken(event);
  }
  /// @brief Feeds an event of an attempt.
  ///
  /// @tparam Event One of the attempt-tagged events.
  /// @param event The event.
  /// @return Whether the machine accepted it; an event of another attempt is
  ///     dropped, as is one the table does not allow.
  template <class Event>
  [[nodiscard]] auto process(const Event& event) -> bool {
    if (!context_.is_current(event.attempt_id)) return false;
    return taken(event);
  }
  /// @brief The state as it stands.
  ///
  /// @return The context's fields at the step the machine is in.
  auto snapshot() const -> FetchState { return context_.snapshot(step()); }
  /// @brief The step the machine is in.
  ///
  /// @return The step.
  auto step() const noexcept -> FetchStep {
    if (machine_.is(sml::state<CheckingSignInState>))
      return FetchStep::checking_sign_in;
    if (machine_.is(sml::state<CloudListState>)) return FetchStep::cloud_list;
    if (machine_.is(sml::state<OwnedLibraryState>))
      return FetchStep::owned_library;
    if (machine_.is(sml::state<PremiumCategoryState>))
      return FetchStep::premium_category;
    if (machine_.is(sml::state<ForSaleCategoryState>))
      return FetchStep::for_sale_category;
    if (machine_.is(sml::state<StreamingAccessState>))
      return FetchStep::streaming_access;
    if (machine_.is(sml::state<MatchingState>)) return FetchStep::matching;
    if (machine_.is(sml::state<TrustedTimeState>))
      return FetchStep::trusted_time;
    if (machine_.is(sml::state<ExclusionsState>)) return FetchStep::exclusions;
    if (machine_.is(sml::state<ReadyState>)) return FetchStep::ready;
    if (machine_.is(sml::state<UnansweredState>)) return FetchStep::unanswered;
    if (machine_.is(sml::state<CancelledState>)) return FetchStep::cancelled;
    return FetchStep::idle;
  }

 private:
  /// @brief Tells whether the step reached has ended.
  ///
  /// @return True once StepEnded was taken and no step has begun since.
  auto step_ended() const noexcept -> bool {
    return machine_.is(sml::state<StepClosedState>);
  }
  /// @brief Feeds an event, then updates the step region and publishes.
  ///
  /// @tparam Event The event type.
  /// @param event The event.
  /// @return Whether the table allowed it.
  template <class Event>
  [[nodiscard]] auto taken(const Event& event) -> bool {
    constexpr bool needs_ended =
        std::is_same_v<Event, StepBegan> || std::is_same_v<Event, Finished>;
    constexpr bool needs_open = std::is_same_v<Event, ActivityChanged> ||
                                std::is_same_v<Event, PagesRead> ||
                                std::is_same_v<Event, TimeServerAsked> ||
                                std::is_same_v<Event, StepEnded>;
    if (needs_ended && !step_ended()) return false;
    if (needs_open && step_ended()) return false;
    if (!machine_.process_event(event)) return false;
    if constexpr (std::is_same_v<Event, StepEnded>) {
      context_.passed.push_back({step(), event.outcome});
      static_cast<void>(machine_.process_event(CloseStep{}));
    }
    if constexpr (std::is_same_v<Event, Start> ||
                  std::is_same_v<Event, StepBegan> ||
                  std::is_same_v<Event, Reset>)
      static_cast<void>(machine_.process_event(OpenStep{}));
    context_.effects.push_back({snapshot()});
    return true;
  }

  /// @brief The borrowed context.
  LifecycleContext& context_;
  /// @brief The SML machine.
  sml::sm<LifecycleFsm> machine_;
};

}  // namespace libchiaking::catalog::fetch_lifecycle
