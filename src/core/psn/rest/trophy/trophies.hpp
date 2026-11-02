/// @file
/// @brief Declares Trophies, the client for PSN's trophy service, and the
/// plain rows it reads: an account's summary, its trophy titles, a set's
/// groups and trophies, the account's progress in each, and the sets that
/// title ids are filed under.
#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/auth/credentials.hpp"
#include "src/core/psn/rest/error.hpp"

#include <libchiaking/detail/expected.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::psn::rest {

/// @brief A count of trophies by grade, as PS writes `definedTrophies` and
/// `earnedTrophies`. Plain data; a grade PS omitted reads as 0.
struct TrophyTally {
  /// @brief The `bronze` count.
  std::uint32_t bronze{};
  /// @brief The `silver` count.
  std::uint32_t silver{};
  /// @brief The `gold` count.
  std::uint32_t gold{};
  /// @brief The `platinum` count.
  std::uint32_t platinum{};

  /// @brief Compares every grade.
  friend auto operator==(const TrophyTally&, const TrophyTally&)
      -> bool = default;
};

/// @brief The account's trophy summary. Plain data.
struct TrophySummaryRow {
  /// @brief The `trophyLevel`.
  std::uint32_t level{};
  /// @brief The `tier`; 0 when PS omitted it.
  std::uint32_t tier{};
  /// @brief The `progress` towards the next level, in percent.
  std::uint32_t progress{};
  /// @brief The `earnedTrophies`.
  TrophyTally earned;
};

/// @brief One row of the account's trophy titles. Plain data; text fields
/// are empty when PS omitted them.
struct TrophyTitleRow {
  /// @brief The `npCommunicationId`, unchecked.
  std::string np_communication_id;
  /// @brief The `npServiceName`, such as `trophy` or `trophy2`.
  std::string np_service_name;
  /// @brief The `trophyTitleName`.
  std::string name;
  /// @brief The `trophyTitleDetail`.
  std::string detail;
  /// @brief The `trophyTitleIconUrl`.
  std::string icon_url;
  /// @brief The `trophyTitlePlatform`, such as `PS5` or `PS3,PSVITA`.
  std::string platform;
  /// @brief The `trophySetVersion`.
  std::string set_version;
  /// @brief The `hasTrophyGroups`.
  bool has_groups{};
  /// @brief The `trophyGroupCount`.
  std::uint32_t group_count{};
  /// @brief The `definedTrophies`.
  TrophyTally defined;
  /// @brief The `earnedTrophies`.
  TrophyTally earned;
  /// @brief The `progress`, in percent.
  std::uint32_t progress{};
  /// @brief The `hiddenFlag`.
  bool hidden{};
  /// @brief The `lastUpdatedDateTime` as PS wrote it.
  std::string last_updated;
};

/// @brief One group of a trophy set, as the set defines it. Plain data.
struct TrophyGroupRow {
  /// @brief The `trophyGroupId`, unchecked.
  std::string id;
  /// @brief The `trophyGroupName`.
  std::string name;
  /// @brief The `trophyGroupDetail`.
  std::string detail;
  /// @brief The `trophyGroupIconUrl`.
  std::string icon_url;
  /// @brief The `definedTrophies`.
  TrophyTally defined;
};

/// @brief The account's progress in one group of a set. Plain data.
struct EarnedGroupRow {
  /// @brief The `trophyGroupId`, unchecked.
  std::string id;
  /// @brief The `earnedTrophies`.
  TrophyTally earned;
  /// @brief The `progress`, in percent.
  std::uint32_t progress{};
  /// @brief The `lastUpdatedDateTime` as PS wrote it.
  std::string last_updated;
};

/// @brief One trophy as its set defines it. Plain data.
struct TrophyRow {
  /// @brief The `trophyId`; nothing when PS omitted it or sent no number.
  std::optional<std::int64_t> id;
  /// @brief The `trophyGroupId`.
  std::string group_id;
  /// @brief The `trophyType`, such as `bronze`.
  std::string type;
  /// @brief The `trophyName`.
  std::string name;
  /// @brief The `trophyDetail`.
  std::string detail;
  /// @brief The `trophyIconUrl`.
  std::string icon_url;
  /// @brief The `trophyHidden`.
  bool hidden{};
  /// @brief The `trophyProgressTargetValue`, which PS sends as text; nothing
  /// for a trophy that counts nothing.
  std::optional<std::uint64_t> progress_target;
};

/// @brief The account's state for one trophy. Plain data.
struct EarnedTrophyRow {
  /// @brief The `trophyId`; nothing when PS omitted it or sent no number.
  std::optional<std::int64_t> id;
  /// @brief The `earned`.
  bool earned{};
  /// @brief The `earnedDateTime` as PS wrote it.
  std::string earned_at;
  /// @brief The `trophyRare` bucket, 0 the rarest.
  std::optional<std::int64_t> rarity;
  /// @brief The `trophyEarnedRate`, in percent, which PS sends as text.
  std::optional<double> earned_rate;
  /// @brief The `progress` count, which PS sends as text.
  std::optional<std::uint64_t> progress;
  /// @brief The `progressRate`, in percent.
  std::optional<std::uint32_t> progress_rate;
  /// @brief The `progressedDateTime` as PS wrote it.
  std::string progressed_at;
};

/// @brief One page of a paged trophy list. Plain data.
///
/// @tparam Row The row type read from each entry.
template <class Row>
struct TrophyPage {
  /// @brief The readable rows, in reply order.
  std::vector<Row> rows;
  /// @brief How many entries the page held, readable or not.
  std::size_t received{};
  /// @brief The `nextOffset`; nothing on the last page.
  std::optional<std::size_t> next_offset;
  /// @brief The `totalItemCount`, when PS gave it.
  std::optional<std::size_t> total;
};

/// @brief One title of a lookup from title ids to trophy sets. Plain data.
struct TitleSetsRow {
  /// @brief The `npTitleId`, unchecked.
  std::string np_title_id;
  /// @brief The rows of its `trophyTitles`, read as titles() reads the
  /// account's trophy titles; empty when PS listed none.
  std::vector<TrophyTitleRow> trophy_titles;
};

/// @brief Which trophy set a request is about. Plain data.
struct TrophySetRef {
  /// @brief The set's `npCommunicationId`, sent as a path segment.
  std::string np_communication_id;
  /// @brief The `npServiceName` query value; left out of the URL when empty.
  std::string np_service_name;
};

/// @brief Reads PSN's trophy service with a bearer token.
///
/// Holds a reference to the HTTP client, which must outlive it, and the
/// token. Each call blocks for one request, retried twice two seconds apart
/// on a server or transport failure and never on 429. Not for concurrent
/// use.
class Trophies {
 public:
  /// @brief The failure of title_sets().
  using TitleSetsError = Error<bad_request::InvalidUrl, bad_reply::InvalidJson,
                               bad_reply::InvalidSchema>;
  /// @brief The failure of earned_trophies().
  using EarnedTrophiesError =
      Error<bad_reply::InvalidJson, bad_reply::InvalidSchema>;
  /// @brief The failure of trophies().
  using TrophiesError = Error<bad_reply::InvalidJson, bad_reply::InvalidSchema>;
  /// @brief The failure of earned_groups().
  using EarnedGroupsError =
      Error<bad_reply::InvalidJson, bad_reply::InvalidSchema>;
  /// @brief The failure of groups().
  using GroupsError = Error<bad_reply::InvalidJson, bad_reply::InvalidSchema>;
  /// @brief The failure of titles().
  using TitlesError = Error<bad_reply::InvalidJson, bad_reply::InvalidSchema>;
  /// @brief The failure of summary().
  using SummaryError = Error<bad_reply::InvalidJson, bad_reply::InvalidSchema>;
  /// @brief The failure of a read with asked().
  using AskedError = Error<bad_reply::InvalidJson, bad_reply::InvalidSchema>;
  /// @brief How many entries one page asks for.
  static constexpr std::size_t page_size = 100;
  /// @brief Request label of summary().
  static constexpr std::string_view summary_label = "psn.trophy.summary";
  /// @brief Request label of titles().
  static constexpr std::string_view titles_label = "psn.trophy.titles";
  /// @brief Request label of groups().
  static constexpr std::string_view groups_label = "psn.trophy.groups";
  /// @brief Request label of earned_groups().
  static constexpr std::string_view earned_groups_label =
      "psn.trophy.earned_groups";
  /// @brief Request label of trophies().
  static constexpr std::string_view trophies_label = "psn.trophy.trophies";
  /// @brief Request label of earned_trophies().
  static constexpr std::string_view earned_trophies_label =
      "psn.trophy.earned_trophies";
  /// @brief Request label of title_sets().
  static constexpr std::string_view title_sets_label = "psn.trophy.title_sets";
  /// @brief Most title ids one title_sets() request may name.
  static constexpr std::size_t title_sets_limit = 5;

  /// @brief Prepares the requests.
  ///
  /// @param client The client the requests go through.
  /// @param token The bearer token to sign them with.
  Trophies(net::http::Client& client, auth::BearerToken token) noexcept;

  /// @brief Reads the account's trophy summary.
  ///
  /// GET `/api/trophy/v1/users/me/trophySummary`.
  ///
  /// @return The summary.
  /// @retval bad_reply::InvalidSchema The reply has no `trophyLevel`.
  /// @retval Error A transport failure, an error status or an unreadable
  ///     reply.
  [[nodiscard]] auto summary() -> Expected<TrophySummaryRow, SummaryError>;
  /// @brief Reads one page of the account's trophy titles.
  ///
  /// GET `/api/trophy/v1/users/me/trophyTitles?limit=100&offset=<offset>`.
  ///
  /// @param offset Index of the first title.
  /// @return The page.
  /// @retval bad_reply::InvalidSchema The reply has no `trophyTitles`.
  /// @retval Error A transport failure, an error status or an unreadable
  ///     reply.
  [[nodiscard]] auto titles(std::size_t offset)
      -> Expected<TrophyPage<TrophyTitleRow>, TitlesError>;
  /// @brief Reads the groups a set defines.
  ///
  /// GET `/api/trophy/v1/npCommunicationIds/<id>/trophyGroups`, naming the
  /// service when the set has one.
  ///
  /// @param set The set.
  /// @return The groups in reply order.
  /// @retval bad_reply::InvalidSchema The reply has no `trophyGroups`.
  /// @retval Error A transport failure, an error status or an unreadable
  ///     reply.
  [[nodiscard]] auto groups(const TrophySetRef& set)
      -> Expected<std::vector<TrophyGroupRow>, GroupsError>;
  /// @brief Reads the account's progress in each group of a set.
  ///
  /// GET `/api/trophy/v1/users/me/npCommunicationIds/<id>/trophyGroups`.
  ///
  /// @param set The set.
  /// @return The progress rows in reply order.
  /// @retval bad_reply::InvalidSchema The reply has no `trophyGroups`.
  /// @retval Error A transport failure, an error status or an unreadable
  ///     reply.
  [[nodiscard]] auto earned_groups(const TrophySetRef& set)
      -> Expected<std::vector<EarnedGroupRow>, EarnedGroupsError>;
  /// @brief Reads one page of the trophies a set defines.
  ///
  /// GET `/api/trophy/v1/npCommunicationIds/<id>/trophyGroups/all/trophies`
  /// with `limit=100&offset=<offset>`.
  ///
  /// @param set The set.
  /// @param offset Index of the first trophy.
  /// @return The page.
  /// @retval bad_reply::InvalidSchema The reply has no `trophies`.
  /// @retval Error A transport failure, an error status or an unreadable
  ///     reply.
  [[nodiscard]] auto trophies(const TrophySetRef& set, std::size_t offset)
      -> Expected<TrophyPage<TrophyRow>, TrophiesError>;
  /// @brief Reads one page of the account's state for a set's trophies.
  ///
  /// GET `/api/trophy/v1/users/me/npCommunicationIds/<id>/trophyGroups/`
  /// `all/trophies` with `limit=100&offset=<offset>`.
  ///
  /// @param set The set.
  /// @param offset Index of the first trophy.
  /// @return The page.
  /// @retval bad_reply::InvalidSchema The reply has no `trophies`.
  /// @retval Error A transport failure, an error status or an unreadable
  ///     reply.
  [[nodiscard]] auto earned_trophies(const TrophySetRef& set,
                                     std::size_t offset)
      -> Expected<TrophyPage<EarnedTrophyRow>, EarnedTrophiesError>;
  /// @brief Reads the trophy sets PS files each of some titles under, with
  /// the account's progress in each.
  ///
  /// GET `/api/trophy/v1/users/me/titles/trophyTitles?npTitleIds=<ids>`,
  /// the ids joined by commas.
  ///
  /// @param np_title_ids One to title_sets_limit title ids.
  /// @return The titles PS answered for, in reply order.
  /// @retval bad_request::InvalidUrl No ids, or more than
  ///     title_sets_limit; nothing is sent.
  /// @retval bad_reply::InvalidSchema The reply has no `titles`.
  /// @retval Error A transport failure, an error status or an unreadable
  ///     reply.
  [[nodiscard]] auto title_sets(std::span<const std::string> np_title_ids)
      -> Expected<std::vector<TitleSetsRow>, TitleSetsError>;

 private:
  /// @brief The service's base URL.
  static constexpr std::string_view base =
      "https://m.np.playstation.com/api/trophy/v1";
  /// @brief Two retries, two seconds apart, never on 429.
  static constexpr net::http::Retry retry{
      .retries = 2,
      .delay = std::chrono::seconds{2},
      .on_too_many_requests = false,
  };
  /// @brief The summary request.
  static constexpr net::http::Endpoint summary_endpoint{
      .label = summary_label, .retry = retry, .log_query = true};
  /// @brief The titles request.
  static constexpr net::http::Endpoint titles_endpoint{
      .label = titles_label, .retry = retry, .log_query = true};
  /// @brief The group definitions request.
  static constexpr net::http::Endpoint groups_endpoint{
      .label = groups_label, .retry = retry, .log_query = true};
  /// @brief The group progress request.
  static constexpr net::http::Endpoint earned_groups_endpoint{
      .label = earned_groups_label, .retry = retry, .log_query = true};
  /// @brief The trophy definitions request.
  static constexpr net::http::Endpoint trophies_endpoint{
      .label = trophies_label, .retry = retry, .log_query = true};
  /// @brief The earned trophies request.
  static constexpr net::http::Endpoint earned_trophies_endpoint{
      .label = earned_trophies_label, .retry = retry, .log_query = true};
  /// @brief The title to trophy set request.
  static constexpr net::http::Endpoint title_sets_endpoint{
      .label = title_sets_label, .retry = retry, .log_query = true};

  /// @brief The reply shapes and the reading of them into rows.
  struct Wire;

  /// @brief Builds the URL of a request about one set.
  ///
  /// @param owner Empty for the set's definitions, `/users/me` for the
  ///     account's progress.
  /// @param set The set.
  /// @param leaf The path after the set's id.
  /// @return The URL, with `npServiceName` when the set names a service.
  static auto set_url(std::string_view owner, const TrophySetRef& set,
                      std::string_view leaf) -> net::http::Query;
  /// @brief Appends the page size and offset.
  ///
  /// @param url The URL to extend.
  /// @param offset Index of the first entry.
  static void page(net::http::Query& url, std::size_t offset);
  /// @brief Sends a signed GET and reads its reply.
  ///
  /// @tparam Reply The reply shape.
  /// @param endpoint The request's endpoint.
  /// @param url The URL.
  /// @return The reply.
  /// @retval Error A transport failure, an error status or an unreadable
  ///     reply.
  template <class Reply>
  [[nodiscard]] auto asked(const net::http::Endpoint& endpoint,
                           net::http::Query url) -> Expected<Reply, AskedError>;

  /// @brief The client the requests go through.
  net::http::Client& client_;
  /// @brief The bearer token.
  auth::BearerToken token_;
};

}  // namespace libchiaking::psn::rest
