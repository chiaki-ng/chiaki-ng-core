/// @file
/// @brief Declares Trophies, the service that reads a signed-in user's trophy
/// level, trophy list and each title's trophies from PSN, and finds the
/// trophy sets of the games the account played.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/trophies/current_trophies.hpp"
#include "libchiaking/detail/trophies/title_sets.hpp"
#include "libchiaking/detail/trophies/title_trophies.hpp"
#include "libchiaking/detail/trophies/trophy_summary.hpp"
#include "libchiaking/detail/trophies/trophy_title.hpp"
#include "libchiaking/detail/users/account_id.hpp"
#include "libchiaking/detail/users/np_title_id.hpp"
#include "libchiaking/detail/users/ps_user.hpp"
#include "libchiaking/detail/users/request_budget.hpp"
#include "libchiaking/platform/runtime/stop_token.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace libchiaking {
class ChiakiNg;
}  // namespace libchiaking

namespace libchiaking::testing {
class ServiceBuilder;
}  // namespace libchiaking::testing

namespace libchiaking::platform {
class ThreadingProvider;
}

namespace libchiaking::platform {
class ClockProvider;
class DiagnosticsProvider;
class HttpProvider;
class RecordStore;
}  // namespace libchiaking::platform

namespace libchiaking::trophies {

/// @brief Reads a signed-in user's trophies from PSN's trophy service.
///
/// Four questions can be asked: the account's level and totals
/// (summary_of), the titles it has trophies in (titles_of), one title's
/// groups and trophies with what the account earned (trophies_of), and the
/// trophy sets some console releases are filed under (titles_for). Each is
/// signed with the user's app token; when PSN refuses the token it is renewed
/// once and the request sent again, as every other app-token call in the
/// library does.
///
/// Every call blocks the calling thread while it talks to PSN and returns
/// when the answer is in; the stop token cancels it. Answers are kept in the
/// platform::RecordStore, per account, and reused while fresh (see
/// kept_for), so a repeated question is usually answered without a request.
/// Several threads may call one Trophies; callers asking the same question
/// for the same account at the same time share one request.
///
/// Requests are paced per account through a users::RequestBudget so that a
/// busy screen cannot get the account rate limited: at most burst_requests
/// start in any second, a request waits for its turn rather than failing,
/// and at most window_requests start in any window. A request beyond the
/// window's budget is refused with users::Unanswered::Reason::rate_limited
/// rather than sent. When PSN itself answers 429, the account's requests are
/// refused the same way for cooldown. A budget given at construction is
/// shared with the other services it is given to, such as the catalog, so
/// the account's requests are counted once for all of them.
///
/// A user who is signed out or whose sign-in has expired is turned away
/// before any request is made.
///
/// @see TrophySummary
/// @see TrophyTitle
/// @see TitleTrophies
/// @see TitleSets
/// @ingroup api_trophies
class Trophies {
 public:
  /// @brief How long an answer is reused before PSN is asked again.
  static constexpr std::chrono::hours kept_for{6};
  /// @brief Most requests that start for one account in any second.
  static constexpr std::uint32_t burst_requests =
      users::RequestBudget::burst_requests;
  /// @brief Most requests that start for one account in any window.
  static constexpr std::uint32_t window_requests =
      users::RequestBudget::window_requests;
  /// @brief The window window_requests is counted over.
  static constexpr std::chrono::minutes window = users::RequestBudget::window;
  /// @brief How long an account's requests are refused after PSN answered
  /// 429.
  static constexpr std::chrono::minutes cooldown =
      users::RequestBudget::cooldown;
  /// @brief Most releases one request to PSN asks about; titles_for splits
  /// longer lists into requests of this many.
  static constexpr std::size_t titles_per_request = 5;

  /// @brief Not copyable; one service owns its pacing and in-flight
  /// requests.
  Trophies(const Trophies&) = delete;
  /// @brief Not copy-assignable; one service owns its pacing and in-flight
  /// requests.
  auto operator=(const Trophies&) -> Trophies& = delete;
  /// @brief Releases the service; answers already kept stay in the record
  /// store.
  ~Trophies();

  /// @brief Returns the account's trophy level and the trophies it has
  /// earned.
  ///
  /// A kept summary under kept_for old is returned without a request.
  /// Otherwise one request asks PSN and the answer is kept.
  ///
  /// @param user The signed-in user whose account is asked about.
  /// @param stop Cancels the request, and any wait for its turn.
  /// @return The summary.
  /// @retval users::Unanswered::Reason::signed_out The user has no PSN
  ///     sign-in.
  /// @retval users::Unanswered::Reason::sign_in_expired PSN refused the
  ///     sign-in; the user must sign in again.
  /// @retval users::Unanswered::Reason::store_access_lapsed PSN refused the
  ///     app token even after it was renewed.
  /// @retval users::Unanswered::Reason::cancelled The stop token fired.
  /// @retval users::Unanswered::Reason::rate_limited PSN answered 429, or
  ///     the account's request budget is spent; see the class description.
  /// @retval users::Unanswered::Reason::malformed PSN's reply could not be
  ///     read.
  /// @retval users::Unanswered::Reason::rejected PSN rejected the request.
  /// @retval users::Unanswered::Reason::unavailable PSN could not be reached
  ///     or answered with another error status.
  [[nodiscard]] auto summary_of(users::PsUser& user,
                                const platform::StopToken& stop = {})
      -> Expected<TrophySummary, users::Unanswered>;
  /// @brief Returns every title the account has trophies in.
  ///
  /// A kept list under kept_for old is returned without a request.
  /// Otherwise the list is read a page of 100 titles at a time until PSN
  /// gives no next page, and the whole list is kept. Rows PSN sent without a
  /// readable trophy set id are left out.
  ///
  /// @param user The signed-in user whose account is asked about.
  /// @param stop Cancels the requests, and any wait for their turn.
  /// @return The titles in PSN's order, most recently updated first.
  /// @retval users::Unanswered::Reason::malformed A reply could not be read,
  ///     or a page's next offset did not move past the one asked for.
  /// @retval users::Unanswered::Reason The other failures of summary_of.
  [[nodiscard]] auto titles_of(users::PsUser& user,
                               const platform::StopToken& stop = {})
      -> Expected<std::vector<TrophyTitle>, users::Unanswered>;
  /// @brief Returns one title's groups and trophies, each merged with what
  /// the account has earned.
  ///
  /// Kept trophies are returned without a request when the title's
  /// TrophyTitle::last_updated matches the one they were read for, or, when
  /// the title carries no such time, when they are under kept_for old. A
  /// title whose progress changed since is read again.
  ///
  /// Otherwise the set's trophy definitions and the account's earned state
  /// are read, a page of 100 trophies at a time, and merged by TrophyId. For
  /// a set with groups the group definitions and the account's group
  /// progress are read too; when only the progress cannot be read, each
  /// group's earned counts are tallied from its earned trophies instead. A
  /// set without groups gets one base-game group built from the title.
  ///
  /// @param user The signed-in user whose account is asked about.
  /// @param title The title, as titles_of returned it.
  /// @param stop Cancels the requests, and any wait for their turn.
  /// @return The title's groups and trophies.
  /// @retval users::Unanswered::Reason::malformed A reply could not be read,
  ///     paging did not advance, or PSN listed no trophies for the set.
  /// @retval users::Unanswered::Reason The other failures of summary_of.
  [[nodiscard]] auto trophies_of(users::PsUser& user, const TrophyTitle& title,
                                 const platform::StopToken& stop = {})
      -> Expected<TitleTrophies, users::Unanswered>;
  /// @brief Finds the trophy sets each of some console releases is filed
  /// under, linking the games in the account's play history to the titles
  /// titles_of lists.
  ///
  /// Each release's answer is kept on its own and returned without a
  /// request while under kept_for old. The other releases are asked about
  /// titles_per_request at a time, each request paced like every other and
  /// each repeated id asked once. A release PSN leaves out of its reply is
  /// kept as having no sets.
  ///
  /// @param user The signed-in user whose account is asked about.
  /// @param titles The releases, such as the title ids of the account's play
  ///     history; in any order, and repeats are allowed.
  /// @param stop Cancels the requests, and any wait for their turn.
  /// @return One TitleSets for each distinct release, in the order first
  ///     given; empty, without a request, for no releases.
  /// @retval users::Unanswered::Reason The failures of summary_of, when any
  ///     request fails; the releases earlier requests answered stay kept.
  [[nodiscard]] auto titles_for(users::PsUser& user,
                                std::span<const NpTitleId> titles,
                                const platform::StopToken& stop = {})
      -> Expected<std::vector<TitleSets>, users::Unanswered>;

  /// @brief Returns the trophies of the game being played, as PSN has them
  /// now, for showing them during play.
  ///
  /// One request always asks PSN for the release's trophy set and the
  /// account's progress in it, so a trophy earned a moment ago counts. The
  /// set's trophies are then answered as trophies_of answers them: kept ones
  /// are reused while that progress is unchanged and read again once it
  /// moved. The release's sets are kept for titles_for too.
  ///
  /// @param user The signed-in user whose account is asked about.
  /// @param title The release being played, such as `PPSA01234_00`.
  /// @param stop Cancels the requests, and any wait for their turn.
  /// @return The game's set and trophies, or nothing when PSN knows no
  ///     trophy set for the release, as for a game without trophies or one the
  ///     account has never earned progress in.
  /// @retval users::Unanswered::Reason The failures of trophies_of.
  [[nodiscard]] auto current_trophies(users::PsUser& user,
                                      const NpTitleId& title,
                                      const platform::StopToken& stop = {})
      -> Expected<std::optional<CurrentTrophies>, users::Unanswered>;

  /// @brief Discards what is kept for the user so the next call of each kind
  /// asks PSN again.
  ///
  /// Pacing is not reset. Failures to erase are not reported.
  ///
  /// @param user The user to start over for.
  void start_over(users::PsUser& user);
  /// @brief Removes everything kept about an account, for sign-out or
  /// account removal, and forgets its pacing.
  ///
  /// Failures to erase are not reported.
  ///
  /// @param account The account to forget.
  void forget(AccountId account);

  /// @brief The service's working parts, defined inside the library.
  class Core;

 private:
  friend class libchiaking::ChiakiNg;
  friend class libchiaking::testing::ServiceBuilder;

  /// @brief Builds the service.
  ///
  /// @param http Sends every request to PSN. Must outlive the service.
  /// @param records Where answers are kept between runs. Must outlive the
  ///     service.
  /// @param threading Makes the service's locks and the waits of shared
  ///     requests. Must outlive the service.
  /// @param clock Monotonic clock that requests are paced on. Must outlive
  ///     the service.
  /// @param wall_clock Device time, used to stamp answers and to decide when
  ///     a kept answer has gone stale. Must outlive the service.
  /// @param budget The per-account request budget shared with other
  ///     services. Must outlive the service.
  /// @param diagnostics Receives log lines; may be null to log nothing. Must
  ///     outlive the service when given.
  Trophies(platform::HttpProvider& http, platform::RecordStore& records,
           platform::ThreadingProvider& threading,
           const platform::ClockProvider& clock,
           const platform::ClockProvider& wall_clock,
           users::RequestBudget& budget,
           platform::DiagnosticsProvider* diagnostics = nullptr);

  /// @brief The working parts; never null.
  std::unique_ptr<Core> core_;
};

}  // namespace libchiaking::trophies
