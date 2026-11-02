/// @file
/// @brief Declares FetchState, where a full catalog fetch for one account
/// stands, and the observer told of every change to it.
#pragma once

#include "libchiaking/detail/catalog/call_failure.hpp"
#include "libchiaking/detail/progress.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace libchiaking::catalog {

/// @brief One step of a full catalog fetch, Catalog::process_library_and_list.
///
/// The steps run in the order declared, from FetchStep::checking_sign_in to
/// FetchStep::exclusions. Every step is passed, a step the fetch does not need
/// as StepOutcome::skipped, so a frontend can show them all in order. A fetch
/// ends in FetchStep::ready, FetchStep::unanswered or FetchStep::cancelled.
enum class FetchStep : std::uint8_t {
  /// @brief No fetch has run for the account since it was signed in or
  /// started over.
  idle,
  /// @brief The user's sign-in is being checked; a signed-out or expired user
  /// ends the fetch here.
  checking_sign_in,
  /// @brief The list of titles PS streams in the account's region is being
  /// read.
  cloud_list,
  /// @brief The games the account owns are being read.
  owned_library,
  /// @brief The PS5 titles the account's plan includes are being read.
  premium_category,
  /// @brief The PS5 titles the store sells for streaming are being read.
  for_sale_category,
  /// @brief Whether the account may stream PS5 titles from the cloud is being
  /// read.
  streaming_access,
  /// @brief The answers are being matched into the account's games.
  matching,
  /// @brief Trusted time is being synced, because a shown game awaits its
  /// release.
  trusted_time,
  /// @brief The list of titles excluded from PS5 cloud streaming is being
  /// read, because a PS5 game is shown.
  exclusions,
  /// @brief The fetch ended with the account's games.
  ready,
  /// @brief The fetch ended without games; FetchState::failure says why.
  unanswered,
  /// @brief The fetch was cancelled.
  cancelled,
};

/// @brief What a step is doing at the moment.
enum class FetchActivity : std::uint8_t {
  /// @brief An answer held from an earlier fetch is being used.
  reusing_held_answer,
  /// @brief The account region's public store is being walked without
  /// signing in.
  walking_public_store,
  /// @brief The account's PS Now shelves are being walked.
  walking_old_service_shelves,
  /// @brief The shelves could not be walked, so the public store is walked
  /// instead.
  walking_public_store_fallback,
  /// @brief Each listed PS4 title is checked for a streaming copy a plan
  /// covers.
  checking_streaming_copies,
  /// @brief The PS5 lists of the store app are being read.
  reading_ps5_store_lists,
  /// @brief The account's entitlements are being read, page by page.
  reading_entitlements,
  /// @brief The concepts of the account's streamed PS4 copies are being read.
  reading_streamed_ps4_concepts,
  /// @brief The account's PS5 library is being read, page by page.
  scanning_ps5_library,
  /// @brief A store category is being read, page by page.
  reading_category_pages,
  /// @brief The account's subscriptions are being read.
  reading_subscriptions,
  /// @brief The account's games are being built from the answers.
  matching_games,
  /// @brief The clock is being synced with a trusted server;
  /// FetchState::time_server names the one being asked.
  syncing_clock,
  /// @brief The exclusion list is being read.
  reading_exclusions,
};

/// @brief How a step that has ended was answered.
enum class StepOutcome : std::uint8_t {
  /// @brief The fetch did not need the step.
  skipped,
  /// @brief Answered from what was held, without asking PS.
  held,
  /// @brief Answered afresh: asked of PS, or for matching built anew.
  fetched,
  /// @brief Asked of PS and not fully answered; the fetch carried on without
  /// the missing part.
  degraded,
};

/// @brief A step a fetch has passed, and how.
struct PassedStep {
  /// @brief The step.
  FetchStep step{FetchStep::checking_sign_in};
  /// @brief How it ended.
  StepOutcome outcome{StepOutcome::skipped};

  /// @brief Compares two passed steps field by field.
  friend constexpr auto operator==(const PassedStep&,
                                   const PassedStep&) noexcept
      -> bool = default;
};

/// @brief Where an account's full catalog fetch stands: a snapshot that never
/// changes once made.
///
/// Read the latest one with Catalog::fetch_state, or follow every change with
/// the FetchObserver given to Catalog::process_library_and_list.
/// @ingroup api_catalog
class FetchState {
 public:
  /// @brief The state of an account no fetch has run for: FetchStep::idle,
  /// attempt 0.
  FetchState() noexcept = default;
  /// @brief Makes a snapshot.
  ///
  /// @param attempt The fetch the snapshot belongs to, counted from 1 for
  ///     each account.
  /// @param step The step reached.
  /// @param activity What the step is doing, when it has said.
  /// @param pages_read Pages read so far by the current activity.
  /// @param pages_expected Pages the current activity expects in all, when
  ///     the sources it reads have stated their totals.
  /// @param time_server The host of the time server
  /// FetchActivity::syncing_clock
  ///     is asking, once it has said.
  /// @param passed The steps already passed, in order.
  /// @param failure Why the fetch ended without games; only for
  ///     FetchStep::unanswered.
  FetchState(std::uint64_t attempt, FetchStep step,
             std::optional<FetchActivity> activity, std::uint32_t pages_read,
             std::optional<std::uint32_t> pages_expected,
             std::optional<std::string> time_server,
             std::vector<PassedStep> passed,
             std::optional<GamesFailure> failure);

  /// @brief The fetch this snapshot belongs to.
  ///
  /// @return The attempt, counted from 1 for each account; 0 before any.
  auto attempt() const noexcept -> std::uint64_t { return attempt_; }
  /// @brief The step reached.
  ///
  /// @return The step.
  auto step() const noexcept -> FetchStep { return step_; }
  /// @brief What the step is doing.
  ///
  /// @return The activity, or nothing while the step waits for an answer
  ///     another fetch is getting, and once the fetch has ended.
  auto activity() const noexcept -> std::optional<FetchActivity> {
    return activity_;
  }
  /// @brief Pages read so far by the current activity.
  ///
  /// @return The count; 0 for an activity that does not page.
  auto pages_read() const noexcept -> std::uint32_t { return pages_read_; }
  /// @brief Pages the current activity expects in all.
  ///
  /// It can grow while the activity runs, as each source it reads states
  /// its own total.
  ///
  /// @return The total, or nothing while no source has stated one.
  auto pages_expected() const noexcept -> std::optional<std::uint32_t> {
    return pages_expected_;
  }
  /// @brief The time server the trusted-time step is asking.
  ///
  /// Changes as the sync falls back from one server to the next.
  ///
  /// @return The host name, such as `time.cloudflare.com`, or nothing
  ///     outside FetchActivity::syncing_clock and before it has asked one.
  auto time_server() const noexcept -> const std::optional<std::string>& {
    return time_server_;
  }
  /// @brief The snapshot as a common progress report.
  ///
  /// @return A status whose position is the pages read out of the pages
  ///     expected, when a total is known.
  auto status() const -> progress::Status;
  /// @brief The steps already passed, in order.
  ///
  /// @return The passed steps, valid while this snapshot lives.
  auto passed() const noexcept -> std::span<const PassedStep> {
    return passed_;
  }
  /// @brief Why the fetch ended without games.
  ///
  /// @return The failure for FetchStep::unanswered; nothing otherwise.
  auto failure() const noexcept -> const std::optional<GamesFailure>& {
    return failure_;
  }
  /// @brief Whether the fetch is still running.
  ///
  /// @return False for FetchStep::idle, FetchStep::ready,
  ///     FetchStep::unanswered and FetchStep::cancelled.
  auto is_running() const noexcept -> bool;

 private:
  /// @brief The fetch the snapshot belongs to.
  std::uint64_t attempt_{};
  /// @brief The step reached.
  FetchStep step_{FetchStep::idle};
  /// @brief What the step is doing.
  std::optional<FetchActivity> activity_{};
  /// @brief Pages read by the current activity.
  std::uint32_t pages_read_{};
  /// @brief Pages the current activity expects.
  std::optional<std::uint32_t> pages_expected_{};
  /// @brief The time server being asked.
  std::optional<std::string> time_server_{};
  /// @brief The steps passed.
  std::vector<PassedStep> passed_{};
  /// @brief Why the fetch ended without games.
  std::optional<GamesFailure> failure_{};
};

/// @brief Told every change of an account's fetch state, in order.
///
/// Called on the thread that runs the fetch, which may be another caller's
/// thread when this caller joined a fetch already running. It must return
/// quickly and must not start or join a catalog fetch; it may read
/// Catalog::fetch_state and call Catalog::start_over for the user whose fetch
/// it observes, but not for another user, whose fetch may be telling its own
/// observers on another thread that waits for this one.
using FetchObserver = std::function<void(const FetchState&)>;

}  // namespace libchiaking::catalog
