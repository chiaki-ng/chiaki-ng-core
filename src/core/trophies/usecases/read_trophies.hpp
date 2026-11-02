/// @file
/// @brief Declares ReadTrophies, the use case behind Trophies: answering
/// from kept records while fresh, otherwise asking PSN and keeping the
/// answer.
#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/trophies/adapters/cache/trophy_records.hpp"
#include "src/core/trophies/adapters/ps/trophy_requests.hpp"
#include "src/core/trophies/model/freshness.hpp"
#include "src/core/users/request_pacer.hpp"
#include "src/core/utils/concurrency/shared_flight.hpp"

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/detail/trophies/current_trophies.hpp>
#include <libchiaking/detail/trophies/title_sets.hpp>
#include <libchiaking/detail/trophies/title_trophies.hpp>
#include <libchiaking/detail/trophies/trophy_summary.hpp>
#include <libchiaking/detail/trophies/trophy_title.hpp>
#include <libchiaking/detail/users/account_id.hpp>
#include <libchiaking/detail/users/np_title_id.hpp>
#include <libchiaking/detail/users/ps_user.hpp>
#include <libchiaking/platform/diagnostics/provider.hpp>
#include <libchiaking/platform/http/provider.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>
#include <libchiaking/platform/storage/record_store.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::trophies::usecases {

/// @brief Reads a user's trophies, from kept records when fresh.
///
/// Users who are signed out or whose sign-in expired are turned away before
/// anything is read. Callers asking the same question for the same account
/// at the same time share one fetch. Calls block the calling thread and may
/// come from several threads at once.
class ReadTrophies {
 public:
  /// @brief The stop token type.
  using Stop = platform::StopToken;

  /// @brief What the use case runs on. Every reference must outlive it.
  struct Providers {
    /// @brief Sends every request.
    platform::HttpProvider& http;
    /// @brief Where answers are kept.
    platform::RecordStore& records;
    /// @brief Makes locks and the waits of shared fetches.
    platform::ThreadingProvider& threading;
    /// @brief The monotonic clock requests are paced on.
    const platform::ClockProvider& clock;
    /// @brief Device time answers are stamped with.
    const platform::ClockProvider& wall_clock;
    /// @brief Makes the waiter each fetch pauses between retries on, so
    /// that concurrent fetches never share one.
    net::http::WaiterFactory waiters;
    /// @brief Receives request logs; may be null.
    platform::DiagnosticsProvider* diagnostics;
  };

  /// @brief Builds the use case.
  ///
  /// @param providers What it runs on.
  /// @param pacer Paces every account's requests, possibly shared with other
  ///     services. Must outlive the use case.
  /// @param freshness When a kept answer is reused.
  ReadTrophies(Providers providers, users::RequestPacer& pacer,
               Freshness freshness);

  /// @brief Returns the account's summary, kept or fetched.
  ///
  /// @param user The user.
  /// @param stop Cancels any fetch.
  /// @return The summary.
  /// @retval users::Unanswered The failures Trophies::summary_of lists.
  [[nodiscard]] auto summary(users::PsUser& user, const Stop& stop)
      -> Expected<TrophySummary, users::Unanswered>;
  /// @brief Returns the account's trophy titles, kept or fetched.
  ///
  /// @param user The user.
  /// @param stop Cancels any fetch.
  /// @return The titles.
  /// @retval users::Unanswered The failures Trophies::titles_of lists.
  [[nodiscard]] auto titles(users::PsUser& user, const Stop& stop)
      -> Expected<std::vector<TrophyTitle>, users::Unanswered>;
  /// @brief Returns one title's trophies, kept while they match the title,
  /// otherwise fetched.
  ///
  /// @param user The user.
  /// @param title The title.
  /// @param stop Cancels any fetch.
  /// @return The trophies.
  /// @retval users::Unanswered The failures Trophies::trophies_of lists.
  [[nodiscard]] auto trophies(users::PsUser& user, const TrophyTitle& title,
                              const Stop& stop)
      -> Expected<TitleTrophies, users::Unanswered>;
  /// @brief Returns the trophy sets of some console releases, each kept or
  /// fetched in batches.
  ///
  /// @param user The user.
  /// @param titles The releases; repeats are answered once.
  /// @param stop Cancels any fetch.
  /// @return One answer for each distinct release, in the order first given.
  /// @retval users::Unanswered The failures Trophies::titles_for lists.
  [[nodiscard]] auto title_sets(users::PsUser& user,
                                std::span<const NpTitleId> titles,
                                const Stop& stop)
      -> Expected<std::vector<TitleSets>, users::Unanswered>;

  /// @brief Returns the trophies of the game being played as PSN has them
  /// now: the release's progress is always asked for, and its trophies are
  /// read again only when that progress changed since they were kept.
  ///
  /// @param user The user.
  /// @param title The release being played.
  /// @param stop Cancels the requests.
  /// @return The game's set and trophies, or nothing when PSN knows no set
  ///     for the release.
  /// @retval users::Unanswered The failures Trophies::current_trophies lists.
  [[nodiscard]] auto current(users::PsUser& user, const NpTitleId& title,
                             const Stop& stop)
      -> Expected<std::optional<CurrentTrophies>, users::Unanswered>;

  /// @brief Erases the account's kept answers; pacing is kept.
  ///
  /// @param account The account.
  void start_over(AccountId account);
  /// @brief Erases the account's kept answers and its pacing, in every
  /// service sharing the pacer.
  ///
  /// @param account The account.
  void forget(AccountId account);

 private:
  /// @brief Refuses a user who cannot ask PSN.
  ///
  /// @param user The user.
  /// @return The refusal for a signed-out or expired user; nothing
  ///     otherwise.
  [[nodiscard]] static auto turned_away(const users::PsUser& user)
      -> std::optional<users::Unanswered>;
  /// @brief Builds the key that tells shared fetches apart.
  ///
  /// @param kind The kind of question.
  /// @param account The account.
  /// @param about What the question is about, such as a set id; may be
  ///     empty.
  /// @return The key.
  static auto question(std::string_view kind, AccountId account,
                       std::string_view about = {}) -> std::string;
  /// @brief Names a batch of releases for a shared fetch's key.
  ///
  /// @param titles The releases.
  /// @return Their ids joined by commas.
  static auto joined(std::span<const NpTitleId> titles) -> std::string;
  /// @brief Makes the HTTP client for one fetch.
  ///
  /// @param stop Cancels the fetch.
  /// @param waiter Pauses between the fetch's retries; must outlive the
  ///     client.
  /// @return The client, pausing through the waiter.
  auto client(const Stop& stop, net::http::Waiter& waiter) -> net::http::Client;

  /// @brief Sends every request.
  platform::HttpProvider& http_;
  /// @brief The monotonic clock.
  const platform::ClockProvider& clock_;
  /// @brief Makes the waiter of each fetch.
  net::http::WaiterFactory waiters_;
  /// @brief Receives request logs; may be null.
  platform::DiagnosticsProvider* log_;
  /// @brief When a kept answer is reused.
  Freshness freshness_;
  /// @brief The kept answers.
  cache::TrophyRecords records_;
  /// @brief Paces every account's requests; not owned.
  users::RequestPacer& pacer_;
  /// @brief Summary fetches in flight.
  concurrency::SharedFlight<TrophySummary, users::Unanswered> summaries_;
  /// @brief Title list fetches in flight.
  concurrency::SharedFlight<std::vector<TrophyTitle>, users::Unanswered>
      titles_;
  /// @brief Title trophy fetches in flight.
  concurrency::SharedFlight<TitleTrophies, users::Unanswered> details_;
  /// @brief Trophy set lookups in flight, one per batch of releases.
  concurrency::SharedFlight<std::vector<TitleSets>, users::Unanswered> sets_;
};

}  // namespace libchiaking::trophies::usecases
