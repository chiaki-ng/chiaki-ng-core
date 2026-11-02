/// @file
/// @brief Declares TrophyRequests, which asks PSN's trophy service for one
/// user, paced, and turns the replies into the trophies context's values.
#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/rest/trophy/trophies.hpp"
#include "src/core/trophies/model/title_assembly.hpp"
#include "src/core/users/ports/trophy_calls.hpp"
#include "src/core/users/request_pacer.hpp"

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/detail/trophies/title_sets.hpp>
#include <libchiaking/detail/trophies/title_trophies.hpp>
#include <libchiaking/detail/trophies/trophy_summary.hpp>
#include <libchiaking/detail/trophies/trophy_title.hpp>
#include <libchiaking/detail/users/np_title_id.hpp>
#include <libchiaking/detail/users/ps_user.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace libchiaking::trophies::ps {

/// @brief One user's requests to PSN's trophy service.
///
/// A short-lived view made for each question: it refers to the HTTP client,
/// the user and the pacer, all of which must outlive it. Every request waits
/// for its turn from the pacer and is signed through users::TrophyCalls;
/// a 429 starts the account's cooldown. Rows that cannot be read into a
/// value, such as a title without a valid set id, are left out. Calls block
/// the calling thread.
class TrophyRequests {
 public:
  /// @brief One release's trophy sets and the first set's title, read
  /// together. Plain data.
  struct Release {
    /// @brief The release's sets.
    TitleSets sets;
    /// @brief The first set PS listed, with the account's progress, as
    /// titles() reads it; nothing when PS knows no set for the release.
    std::optional<TrophyTitle> title;
  };

  /// @brief Most pages read of one list before it counts as malformed.
  static constexpr int page_limit = 50;
  /// @brief Most releases one title_sets() request names.
  static constexpr std::size_t titles_per_request =
      psn::rest::Trophies::title_sets_limit;

  /// @brief Views a user's trophy requests.
  ///
  /// @param http The client the requests go through; its stop token
  ///     cancels them.
  /// @param user The user asking.
  /// @param pacer The pacer the requests wait on.
  TrophyRequests(net::http::Client& http, users::PsUser& user,
                 users::RequestPacer& pacer) noexcept;

  /// @brief Reads the account's trophy summary.
  ///
  /// @return The summary.
  /// @retval users::Unanswered The pacer's or the call's failure.
  [[nodiscard]] auto summary() -> Expected<TrophySummary, users::Unanswered>;
  /// @brief Reads every page of the account's trophy titles.
  ///
  /// @return The readable titles in PS's order.
  /// @retval users::Unanswered::Reason::malformed A page's next offset did
  ///     not advance, or page_limit pages were read.
  /// @retval users::Unanswered The pacer's or a call's failure.
  [[nodiscard]] auto titles()
      -> Expected<std::vector<TrophyTitle>, users::Unanswered>;
  /// @brief Reads a title's trophies and groups and merges them with the
  /// account's progress.
  ///
  /// @param title The title.
  /// @return The merged trophies.
  /// @retval users::Unanswered::Reason::malformed Paging failed as in
  ///     titles(), or the set defines no readable trophy.
  /// @retval users::Unanswered The pacer's or a call's failure, except a
  ///     failure of the group progress alone, which is tallied instead.
  [[nodiscard]] auto trophies_of(const TrophyTitle& title)
      -> Expected<TitleTrophies, users::Unanswered>;
  /// @brief Asks in one request which trophy sets some releases are filed
  /// under.
  ///
  /// @param titles One to titles_per_request distinct releases.
  /// @return One TitleSets for each release, in the order given; a release
  ///     PS left out of its reply has no sets.
  /// @retval users::Unanswered::Reason::rejected No releases, or more than
  ///     titles_per_request.
  /// @retval users::Unanswered The pacer's or the call's failure.
  [[nodiscard]] auto title_sets(std::span<const NpTitleId> titles)
      -> Expected<std::vector<TitleSets>, users::Unanswered>;
  /// @brief Reads one release's trophy sets with the account's progress, in
  /// one request.
  ///
  /// @param title The release, such as the title id of the game being
  ///     played.
  /// @return The release's sets and its first set's title.
  /// @retval users::Unanswered The pacer's or the call's failure.
  [[nodiscard]] auto release(const NpTitleId& title)
      -> Expected<Release, users::Unanswered>;

 private:
  /// @brief Waits for a turn, asks, and starts the cooldown on a 429.
  ///
  /// @tparam Ask Callable returning `Expected<T, users::Unanswered>`.
  /// @param ask Sends the request.
  /// @return The reply.
  /// @retval users::Unanswered The pacer's or the request's failure.
  template <class Ask>
  [[nodiscard]] auto paced(const Ask& ask) -> decltype(ask());
  /// @brief Reads a paged list until PS gives no next page.
  ///
  /// @tparam Row The row type of the list.
  /// @tparam Ask Callable taking an offset and returning
  ///     `Expected<psn::rest::TrophyPage<Row>, users::Unanswered>`.
  /// @param ask Sends one page's request.
  /// @return Every row read.
  /// @retval users::Unanswered::Reason::malformed The next offset did not
  ///     advance, or page_limit pages were read.
  /// @retval users::Unanswered The pacer's or a request's failure.
  template <class Row, class Ask>
  [[nodiscard]] auto every_page(const Ask& ask)
      -> Expected<std::vector<Row>, users::Unanswered>;
  /// @brief Reads the groups a title's set defines.
  ///
  /// @param title The title.
  /// @return The readable groups, with nothing earned yet.
  /// @retval users::Unanswered The pacer's or the request's failure.
  [[nodiscard]] auto groups_of(const TrophyTitle& title)
      -> Expected<std::vector<TrophyGroup>, users::Unanswered>;
  /// @brief Reads the account's progress in each group of a title's set.
  ///
  /// @param title The title.
  /// @return The progress, or nothing when it could not be read.
  /// @retval users::Unanswered::Reason::cancelled The stop token fired.
  [[nodiscard]] auto progress_of(const TrophyTitle& title)
      -> Expected<std::optional<std::vector<GroupProgress>>, users::Unanswered>;

  /// @brief Names a title's set for a request.
  ///
  /// @param title The title.
  /// @return The set's id and service name, `trophy` or `trophy2`; no
  ///     service name when the title's TrophySet is unknown.
  static auto set_of(const TrophyTitle& title) -> psn::rest::TrophySetRef;
  /// @brief Reads a tally.
  ///
  /// @param tally The tally as read.
  /// @return The same counts.
  static auto counts_of(const psn::rest::TrophyTally& tally) -> TrophyCounts;
  /// @brief Reads a time as PS writes it.
  ///
  /// @param text An ISO 8601 time with its zone.
  /// @return The time, or nothing when unreadable.
  static auto time_of(std::string_view text) -> std::optional<Time>;
  /// @brief Reads an `npServiceName`.
  ///
  /// @param name The name.
  /// @return TrophySet::classic for `trophy`, TrophySet::ps5 for `trophy2`,
  ///     nothing otherwise.
  static auto service_of(std::string_view name) -> std::optional<TrophySet>;
  /// @brief Reads a `trophyTitlePlatform`.
  ///
  /// @param text Comma-separated platform names.
  /// @return The known platforms, in order.
  static auto platforms_of(std::string_view text)
      -> std::vector<TrophyPlatform>;
  /// @brief Reads a `trophyType`.
  ///
  /// @param type The type.
  /// @return The grade, or nothing for an unknown type.
  static auto grade_of(std::string_view type) -> std::optional<Grade>;
  /// @brief Reads a `trophyRare` bucket.
  ///
  /// @param bucket The bucket, 0 the rarest.
  /// @return The rarity, or nothing for a missing or unknown bucket.
  static auto rarity_of(std::optional<std::int64_t> bucket)
      -> std::optional<Rarity>;
  /// @brief Reads a title row.
  ///
  /// @param row The row; its text is moved from.
  /// @return The title, or nothing without a valid set id.
  static auto title_of(psn::rest::TrophyTitleRow& row)
      -> std::optional<TrophyTitle>;
  /// @brief Collects the sets PS listed for one release.
  ///
  /// @param title The release.
  /// @param rows Every row of the reply.
  /// @return The valid set ids of the rows naming the release, in reply
  ///     order and each once.
  static auto sets_of(const NpTitleId& title,
                      std::span<const psn::rest::TitleSetsRow> rows)
      -> std::vector<NpCommunicationId>;
  /// @brief Reads a group definition row.
  ///
  /// @param row The row; its text is moved from.
  /// @return The group, or nothing without a valid group id.
  static auto group_of(psn::rest::TrophyGroupRow& row)
      -> std::optional<TrophyGroup>;
  /// @brief Reads a group progress row.
  ///
  /// @param row The row.
  /// @return The progress, or nothing without a valid group id.
  static auto group_progress_of(const psn::rest::EarnedGroupRow& row)
      -> std::optional<GroupProgress>;
  /// @brief Reads a trophy definition row.
  ///
  /// @param row The row; its text is moved from.
  /// @return The trophy, not earned, or nothing without a valid id, grade or
  ///     group.
  static auto trophy_of(psn::rest::TrophyRow& row) -> std::optional<Trophy>;
  /// @brief Reads an earned trophy row.
  ///
  /// @param row The row.
  /// @return The earned state, or nothing without a valid id.
  static auto earned_of(const psn::rest::EarnedTrophyRow& row)
      -> std::optional<EarnedState>;

  /// @brief The client the requests go through.
  net::http::Client& http_;
  /// @brief The user's authorised calls.
  users::TrophyCalls calls_;
  /// @brief The user's account, which the pacer keys on.
  AccountId account_;
  /// @brief The pacer the requests wait on.
  users::RequestPacer& pacer_;
};

}  // namespace libchiaking::trophies::ps
