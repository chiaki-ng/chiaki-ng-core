#include "src/core/psn/rest/trophy/trophies.hpp"

#include "src/core/net/transport/http/percent_encoding.hpp"
#include "src/core/net/transport/http/url.hpp"
#include "src/core/utils/json/ps_reply.hpp"

#include <span>
#include <string>
#include <utility>

namespace libchiaking::psn::rest {

struct Trophies::Wire {
  template <class Value>
  using Field = json::ps::Tolerant<Value>;
  template <class Number>
  using Count = json::ps::Tolerant<json::ps::LenientNumber<Number>>;

  struct Tally {
    using keys = json::ps::CamelCaseKeys;
    Count<std::uint32_t> bronze;
    Count<std::uint32_t> silver;
    Count<std::uint32_t> gold;
    Count<std::uint32_t> platinum;
  };

  struct Summary {
    using keys = json::ps::CamelCaseKeys;
    Count<std::uint32_t> trophy_level;
    Count<std::uint32_t> tier;
    Count<std::uint32_t> progress;
    Field<Tally> earned_trophies;
  };

  struct Title {
    using keys = json::ps::CamelCaseKeys;
    Field<std::string> np_communication_id;
    Field<std::string> np_service_name;
    Field<std::string> trophy_title_name;
    Field<std::string> trophy_title_detail;
    Field<std::string> trophy_title_icon_url;
    Field<std::string> trophy_title_platform;
    Field<std::string> trophy_set_version;
    Field<bool> has_trophy_groups;
    Count<std::uint32_t> trophy_group_count;
    Field<Tally> defined_trophies;
    Field<Tally> earned_trophies;
    Count<std::uint32_t> progress;
    Field<bool> hidden_flag;
    Field<std::string> last_updated_date_time;
  };

  struct Titles {
    using keys = json::ps::CamelCaseKeys;
    std::optional<std::vector<Field<Title>>> trophy_titles;
    Count<std::size_t> next_offset;
    Count<std::size_t> total_item_count;
  };

  struct TitleSets {
    using keys = json::ps::CamelCaseKeys;
    Field<std::string> np_title_id;
    std::optional<std::vector<Field<Title>>> trophy_titles;
  };

  struct TitleSetsList {
    std::optional<std::vector<Field<TitleSets>>> titles;
  };

  struct Group {
    using keys = json::ps::CamelCaseKeys;
    Field<std::string> trophy_group_id;
    Field<std::string> trophy_group_name;
    Field<std::string> trophy_group_detail;
    Field<std::string> trophy_group_icon_url;
    Field<Tally> defined_trophies;
    Field<Tally> earned_trophies;
    Count<std::uint32_t> progress;
    Field<std::string> last_updated_date_time;
  };

  struct Groups {
    using keys = json::ps::CamelCaseKeys;
    std::optional<std::vector<Field<Group>>> trophy_groups;
  };

  struct Trophy {
    using keys = json::ps::CamelCaseKeys;
    Count<std::int64_t> trophy_id;
    Field<std::string> trophy_group_id;
    Field<std::string> trophy_type;
    Field<std::string> trophy_name;
    Field<std::string> trophy_detail;
    Field<std::string> trophy_icon_url;
    Field<bool> trophy_hidden;
    Count<std::uint64_t> trophy_progress_target_value;
    Field<bool> earned;
    Field<std::string> earned_date_time;
    Count<std::int64_t> trophy_rare;
    Count<double> trophy_earned_rate;
    Count<std::uint64_t> progress;
    Count<std::uint32_t> progress_rate;
    Field<std::string> progressed_date_time;
  };

  struct TrophyList {
    using keys = json::ps::CamelCaseKeys;
    std::optional<std::vector<Field<Trophy>>> trophies;
    Count<std::size_t> next_offset;
    Count<std::size_t> total_item_count;
  };

  template <class Number>
  static auto number(const Count<Number>& count) -> std::optional<Number> {
    if (!count) return std::nullopt;
    return count->value();
  }

  static auto text(Field<std::string>& field) -> std::string {
    return std::move(field).value_or(std::string{});
  }

  static auto tally(const Field<Tally>& field) -> TrophyTally {
    if (!field) return {};
    return TrophyTally{
        number(field->bronze).value_or(0), number(field->silver).value_or(0),
        number(field->gold).value_or(0), number(field->platinum).value_or(0)};
  }

  static auto title_row(Title& title) -> TrophyTitleRow {
    return TrophyTitleRow{text(title.np_communication_id),
                          text(title.np_service_name),
                          text(title.trophy_title_name),
                          text(title.trophy_title_detail),
                          text(title.trophy_title_icon_url),
                          text(title.trophy_title_platform),
                          text(title.trophy_set_version),
                          title.has_trophy_groups.value_or(false),
                          number(title.trophy_group_count).value_or(0),
                          tally(title.defined_trophies),
                          tally(title.earned_trophies),
                          number(title.progress).value_or(0),
                          title.hidden_flag.value_or(false),
                          text(title.last_updated_date_time)};
  }

  template <class Row, class Read, class Rows>
  static auto page_of(Rows& rows, const Count<std::size_t>& next,
                      const Count<std::size_t>& total, const Read& read)
      -> TrophyPage<Row> {
    TrophyPage<Row> page;
    page.received = rows.size();
    page.next_offset = number(next);
    page.total = number(total);
    for (auto& slot : rows)
      if (slot) page.rows.push_back(read(*slot));
    return page;
  }
};

Trophies::Trophies(net::http::Client& client, auth::BearerToken token) noexcept
    : client_(client), token_(std::move(token)) {}

auto Trophies::set_url(std::string_view owner, const TrophySetRef& set,
                       std::string_view leaf) -> net::http::Query {
  std::string address{base};
  address += owner;
  address += "/npCommunicationIds/";
  address += net::http::PercentEncoding::component(set.np_communication_id);
  address += leaf;
  net::http::Query url{address};
  if (!set.np_service_name.empty())
    url.add("npServiceName", set.np_service_name);
  return url;
}

void Trophies::page(net::http::Query& url, std::size_t offset) {
  url.add_verbatim("limit", std::to_string(page_size))
      .add_verbatim("offset", std::to_string(offset));
}

template <class Reply>
auto Trophies::asked(const net::http::Endpoint& endpoint, net::http::Query url)
    -> Expected<Reply, AskedError> {
  net::http::Request request{endpoint, std::move(url)};
  request.authorize(token_).header("Accept", "application/json");
  return AskedError::checked_reply<Reply>(client_.send(std::move(request)),
                                          token_.view());
}

auto Trophies::summary() -> Expected<TrophySummaryRow, SummaryError> {
  std::string address{base};
  address += "/users/me/trophySummary";
  auto reply =
      asked<Wire::Summary>(summary_endpoint, net::http::Query{address});
  if (!reply) return unexpected(std::move(reply.error()));
  if (!reply->trophy_level)
    return unexpected(SummaryError{bad_reply::InvalidSchema{}});
  return TrophySummaryRow{Wire::number(reply->trophy_level).value_or(0),
                          Wire::number(reply->tier).value_or(0),
                          Wire::number(reply->progress).value_or(0),
                          Wire::tally(reply->earned_trophies)};
}

auto Trophies::titles(std::size_t offset)
    -> Expected<TrophyPage<TrophyTitleRow>, TitlesError> {
  std::string address{base};
  address += "/users/me/trophyTitles";
  net::http::Query url{address};
  page(url, offset);
  auto reply = asked<Wire::Titles>(titles_endpoint, std::move(url));
  if (!reply) return unexpected(std::move(reply.error()));
  if (!reply->trophy_titles)
    return unexpected(TitlesError{bad_reply::InvalidSchema{}});
  return Wire::page_of<TrophyTitleRow>(
      *reply->trophy_titles, reply->next_offset, reply->total_item_count,
      &Wire::title_row);
}

auto Trophies::groups(const TrophySetRef& set)
    -> Expected<std::vector<TrophyGroupRow>, GroupsError> {
  auto reply =
      asked<Wire::Groups>(groups_endpoint, set_url("", set, "/trophyGroups"));
  if (!reply) return unexpected(std::move(reply.error()));
  if (!reply->trophy_groups)
    return unexpected(GroupsError{bad_reply::InvalidSchema{}});
  std::vector<TrophyGroupRow> rows;
  for (auto& slot : *reply->trophy_groups) {
    if (!slot) continue;
    auto& group = *slot;
    rows.push_back(TrophyGroupRow{Wire::text(group.trophy_group_id),
                                  Wire::text(group.trophy_group_name),
                                  Wire::text(group.trophy_group_detail),
                                  Wire::text(group.trophy_group_icon_url),
                                  Wire::tally(group.defined_trophies)});
  }
  return rows;
}

auto Trophies::earned_groups(const TrophySetRef& set)
    -> Expected<std::vector<EarnedGroupRow>, EarnedGroupsError> {
  auto reply = asked<Wire::Groups>(earned_groups_endpoint,
                                   set_url("/users/me", set, "/trophyGroups"));
  if (!reply) return unexpected(std::move(reply.error()));
  if (!reply->trophy_groups)
    return unexpected(EarnedGroupsError{bad_reply::InvalidSchema{}});
  std::vector<EarnedGroupRow> rows;
  for (auto& slot : *reply->trophy_groups) {
    if (!slot) continue;
    auto& group = *slot;
    rows.push_back(EarnedGroupRow{Wire::text(group.trophy_group_id),
                                  Wire::tally(group.earned_trophies),
                                  Wire::number(group.progress).value_or(0),
                                  Wire::text(group.last_updated_date_time)});
  }
  return rows;
}

auto Trophies::trophies(const TrophySetRef& set, std::size_t offset)
    -> Expected<TrophyPage<TrophyRow>, TrophiesError> {
  auto url = set_url("", set, "/trophyGroups/all/trophies");
  page(url, offset);
  auto reply = asked<Wire::TrophyList>(trophies_endpoint, std::move(url));
  if (!reply) return unexpected(std::move(reply.error()));
  if (!reply->trophies)
    return unexpected(TrophiesError{bad_reply::InvalidSchema{}});
  return Wire::page_of<TrophyRow>(
      *reply->trophies, reply->next_offset, reply->total_item_count,
      [](Wire::Trophy& trophy) {
        return TrophyRow{Wire::number(trophy.trophy_id),
                         Wire::text(trophy.trophy_group_id),
                         Wire::text(trophy.trophy_type),
                         Wire::text(trophy.trophy_name),
                         Wire::text(trophy.trophy_detail),
                         Wire::text(trophy.trophy_icon_url),
                         trophy.trophy_hidden.value_or(false),
                         Wire::number(trophy.trophy_progress_target_value)};
      });
}

auto Trophies::earned_trophies(const TrophySetRef& set, std::size_t offset)
    -> Expected<TrophyPage<EarnedTrophyRow>, EarnedTrophiesError> {
  auto url = set_url("/users/me", set, "/trophyGroups/all/trophies");
  page(url, offset);
  auto reply =
      asked<Wire::TrophyList>(earned_trophies_endpoint, std::move(url));
  if (!reply) return unexpected(std::move(reply.error()));
  if (!reply->trophies)
    return unexpected(EarnedTrophiesError{bad_reply::InvalidSchema{}});
  return Wire::page_of<EarnedTrophyRow>(
      *reply->trophies, reply->next_offset, reply->total_item_count,
      [](Wire::Trophy& trophy) {
        return EarnedTrophyRow{Wire::number(trophy.trophy_id),
                               trophy.earned.value_or(false),
                               Wire::text(trophy.earned_date_time),
                               Wire::number(trophy.trophy_rare),
                               Wire::number(trophy.trophy_earned_rate),
                               Wire::number(trophy.progress),
                               Wire::number(trophy.progress_rate),
                               Wire::text(trophy.progressed_date_time)};
      });
}

auto Trophies::title_sets(std::span<const std::string> np_title_ids)
    -> Expected<std::vector<TitleSetsRow>, TitleSetsError> {
  if (np_title_ids.empty() || np_title_ids.size() > title_sets_limit)
    return unexpected(TitleSetsError{bad_request::InvalidUrl{}});
  std::string ids;
  for (const auto& id : np_title_ids) {
    if (!ids.empty()) ids += ',';
    ids += net::http::PercentEncoding::component(id);
  }
  std::string address{base};
  address += "/users/me/titles/trophyTitles";
  net::http::Query url{address};
  url.add_verbatim("npTitleIds", ids);
  auto reply = asked<Wire::TitleSetsList>(title_sets_endpoint, std::move(url));
  if (!reply) return unexpected(TitleSetsError{reply.error()});
  if (!reply->titles)
    return unexpected(TitleSetsError{bad_reply::InvalidSchema{}});
  std::vector<TitleSetsRow> rows;
  for (auto& slot : *reply->titles) {
    if (!slot) continue;
    TitleSetsRow row{Wire::text(slot->np_title_id), {}};
    if (slot->trophy_titles)
      for (auto& title : *slot->trophy_titles)
        if (title) row.trophy_titles.push_back(Wire::title_row(*title));
    rows.push_back(std::move(row));
  }
  return rows;
}

}  // namespace libchiaking::psn::rest
