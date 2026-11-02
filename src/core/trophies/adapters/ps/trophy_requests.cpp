#include "src/core/trophies/adapters/ps/trophy_requests.hpp"

#include "src/core/utils/json/ps_fields.hpp"

#include <algorithm>
#include <array>
#include <ranges>
#include <span>
#include <string>
#include <utility>

namespace libchiaking::trophies::ps {

TrophyRequests::TrophyRequests(net::http::Client& http, users::PsUser& user,
                               users::RequestPacer& pacer) noexcept
    : http_(http), calls_(user), account_(user.account_id()), pacer_(pacer) {}

template <class Ask>
auto TrophyRequests::paced(const Ask& ask) -> decltype(ask()) {
  if (auto admitted = pacer_.admit(account_, http_.stop_token()); !admitted)
    return unexpected(admitted.error());
  auto reply = ask();
  if (!reply && reply.error().reason == users::Unanswered::Reason::rate_limited)
    pacer_.throttled(account_);
  return reply;
}

template <class Row, class Ask>
auto TrophyRequests::every_page(const Ask& ask)
    -> Expected<std::vector<Row>, users::Unanswered> {
  std::vector<Row> rows;
  std::size_t offset = 0;
  for (int page = 0; page < page_limit; ++page) {
    auto read = paced([&] { return ask(offset); });
    if (!read) return unexpected(read.error());
    for (auto& row : read->rows) rows.push_back(std::move(row));
    if (read->received == 0 || !read->next_offset) return rows;
    if (*read->next_offset <= offset)
      return unexpected(
          users::Unanswered{users::Unanswered::Reason::malformed});
    offset = *read->next_offset;
  }
  return unexpected(users::Unanswered{users::Unanswered::Reason::malformed});
}

auto TrophyRequests::summary() -> Expected<TrophySummary, users::Unanswered> {
  auto read = paced([&] { return calls_.trophy_summary(http_); });
  if (!read) return unexpected(read.error());
  return TrophySummary{.level = read->level,
                       .tier = read->tier,
                       .progress = read->progress,
                       .earned = counts_of(read->earned)};
}

auto TrophyRequests::titles()
    -> Expected<std::vector<TrophyTitle>, users::Unanswered> {
  auto rows = every_page<psn::rest::TrophyTitleRow>(
      [&](std::size_t offset) { return calls_.trophy_titles(http_, offset); });
  if (!rows) return unexpected(rows.error());
  std::vector<TrophyTitle> titles;
  titles.reserve(rows->size());
  for (auto& row : *rows)
    if (auto title = title_of(row)) titles.push_back(std::move(*title));
  return titles;
}

auto TrophyRequests::trophies_of(const TrophyTitle& title)
    -> Expected<TitleTrophies, users::Unanswered> {
  const auto set = set_of(title);
  auto defined = every_page<psn::rest::TrophyRow>(
      [&](std::size_t offset) { return calls_.trophies(http_, set, offset); });
  if (!defined) return unexpected(defined.error());
  auto earned_rows =
      every_page<psn::rest::EarnedTrophyRow>([&](std::size_t offset) {
        return calls_.earned_trophies(http_, set, offset);
      });
  if (!earned_rows) return unexpected(earned_rows.error());
  std::vector<Trophy> definitions;
  definitions.reserve(defined->size());
  for (auto& row : *defined)
    if (auto trophy = trophy_of(row)) definitions.push_back(std::move(*trophy));
  std::vector<EarnedState> earned;
  earned.reserve(earned_rows->size());
  for (const auto& row : *earned_rows)
    if (auto state = earned_of(row)) earned.push_back(std::move(*state));
  std::vector<TrophyGroup> groups;
  std::optional<std::vector<GroupProgress>> progress;
  if (title.has_groups) {
    auto listed = groups_of(title);
    if (!listed) return unexpected(listed.error());
    groups = std::move(*listed);
    auto reached = progress_of(title);
    if (!reached) return unexpected(reached.error());
    progress = std::move(*reached);
  }
  auto assembled = TitleAssembly::assembled(title, std::move(groups), progress,
                                            std::move(definitions), earned);
  if (!assembled)
    return unexpected(users::Unanswered{users::Unanswered::Reason::malformed});
  return std::move(*assembled);
}

auto TrophyRequests::title_sets(std::span<const NpTitleId> titles)
    -> Expected<std::vector<TitleSets>, users::Unanswered> {
  std::vector<std::string> ids;
  ids.reserve(titles.size());
  for (const auto& title : titles) ids.push_back(title.text());
  auto rows =
      paced([&] { return calls_.trophy_title_sets(http_, std::span{ids}); });
  if (!rows) return unexpected(rows.error());
  std::vector<TitleSets> answered;
  answered.reserve(titles.size());
  for (const auto& title : titles)
    answered.push_back(
        TitleSets{.title = title, .sets = sets_of(title, *rows)});
  return answered;
}

auto TrophyRequests::release(const NpTitleId& title)
    -> Expected<Release, users::Unanswered> {
  const std::array<std::string, 1> ids{title.text()};
  auto rows =
      paced([&] { return calls_.trophy_title_sets(http_, std::span{ids}); });
  if (!rows) return unexpected(rows.error());
  Release found{
      .sets = TitleSets{.title = title, .sets = sets_of(title, *rows)},
      .title = std::nullopt};
  for (auto& row : *rows) {
    if (row.np_title_id != title.text()) continue;
    for (auto& listed : row.trophy_titles) {
      if (auto read = title_of(listed)) {
        found.title = std::move(*read);
        return found;
      }
    }
  }
  return found;
}

auto TrophyRequests::sets_of(const NpTitleId& title,
                             std::span<const psn::rest::TitleSetsRow> rows)
    -> std::vector<NpCommunicationId> {
  std::vector<NpCommunicationId> sets;
  for (const auto& row : rows) {
    if (row.np_title_id != title.text()) continue;
    for (const auto& listed : row.trophy_titles) {
      auto id = NpCommunicationId::parse(listed.np_communication_id);
      if (id && !std::ranges::contains(sets, *id))
        sets.push_back(std::move(*id));
    }
  }
  return sets;
}

auto TrophyRequests::groups_of(const TrophyTitle& title)
    -> Expected<std::vector<TrophyGroup>, users::Unanswered> {
  auto rows = paced([&] { return calls_.trophy_groups(http_, set_of(title)); });
  if (!rows) return unexpected(rows.error());
  std::vector<TrophyGroup> groups;
  for (auto& row : *rows)
    if (auto group = group_of(row)) groups.push_back(std::move(*group));
  return groups;
}

auto TrophyRequests::progress_of(const TrophyTitle& title)
    -> Expected<std::optional<std::vector<GroupProgress>>, users::Unanswered> {
  auto rows =
      paced([&] { return calls_.earned_trophy_groups(http_, set_of(title)); });
  if (!rows) {
    if (rows.error().is_cancelled()) return unexpected(rows.error());
    return std::optional<std::vector<GroupProgress>>{};
  }
  std::vector<GroupProgress> progress;
  for (const auto& row : *rows)
    if (auto group = group_progress_of(row))
      progress.push_back(std::move(*group));
  return std::optional{std::move(progress)};
}

auto TrophyRequests::set_of(const TrophyTitle& title)
    -> psn::rest::TrophySetRef {
  std::string service;
  if (title.set) service = *title.set == TrophySet::ps5 ? "trophy2" : "trophy";
  return psn::rest::TrophySetRef{title.id.text(), std::move(service)};
}

auto TrophyRequests::counts_of(const psn::rest::TrophyTally& tally)
    -> TrophyCounts {
  return TrophyCounts{.bronze = tally.bronze,
                      .silver = tally.silver,
                      .gold = tally.gold,
                      .platinum = tally.platinum};
}

auto TrophyRequests::time_of(std::string_view text) -> std::optional<Time> {
  const auto instant = json::ps::Instant::from_text(text);
  if (!instant) return std::nullopt;
  return instant->time();
}

auto TrophyRequests::service_of(std::string_view name)
    -> std::optional<TrophySet> {
  if (name == "trophy") return TrophySet::classic;
  if (name == "trophy2") return TrophySet::ps5;
  return std::nullopt;
}

auto TrophyRequests::platforms_of(std::string_view text)
    -> std::vector<TrophyPlatform> {
  std::vector<TrophyPlatform> platforms;
  for (const auto part : text | std::views::split(',')) {
    const std::string_view name{part.begin(), part.end()};
    if (name == "PS3") platforms.push_back(TrophyPlatform::ps3);
    if (name == "PS4") platforms.push_back(TrophyPlatform::ps4);
    if (name == "PS5") platforms.push_back(TrophyPlatform::ps5);
    if (name == "PSVITA") platforms.push_back(TrophyPlatform::ps_vita);
    if (name == "PSPC") platforms.push_back(TrophyPlatform::pc);
  }
  return platforms;
}

auto TrophyRequests::grade_of(std::string_view type) -> std::optional<Grade> {
  if (type == "bronze") return Grade::bronze;
  if (type == "silver") return Grade::silver;
  if (type == "gold") return Grade::gold;
  if (type == "platinum") return Grade::platinum;
  return std::nullopt;
}

auto TrophyRequests::rarity_of(std::optional<std::int64_t> bucket)
    -> std::optional<Rarity> {
  if (!bucket) return std::nullopt;
  switch (*bucket) {
    case 0:
      return Rarity::ultra_rare;
    case 1:
      return Rarity::very_rare;
    case 2:
      return Rarity::rare;
    case 3:
      return Rarity::common;
    default:
      return std::nullopt;
  }
}

auto TrophyRequests::title_of(psn::rest::TrophyTitleRow& row)
    -> std::optional<TrophyTitle> {
  auto id = NpCommunicationId::parse(row.np_communication_id);
  if (!id) return std::nullopt;
  return TrophyTitle{.id = std::move(*id),
                     .set = service_of(row.np_service_name),
                     .name = std::move(row.name),
                     .detail = std::move(row.detail),
                     .icon_url = std::move(row.icon_url),
                     .platforms = platforms_of(row.platform),
                     .set_version = std::move(row.set_version),
                     .has_groups = row.has_groups,
                     .group_count = row.group_count,
                     .defined = counts_of(row.defined),
                     .earned = counts_of(row.earned),
                     .progress = row.progress,
                     .hidden = row.hidden,
                     .last_updated = time_of(row.last_updated)};
}

auto TrophyRequests::group_of(psn::rest::TrophyGroupRow& row)
    -> std::optional<TrophyGroup> {
  auto id = TrophyGroupId::parse(row.id);
  if (!id) return std::nullopt;
  return TrophyGroup{.id = std::move(*id),
                     .name = std::move(row.name),
                     .detail = std::move(row.detail),
                     .icon_url = std::move(row.icon_url),
                     .defined = counts_of(row.defined),
                     .earned = {},
                     .progress = 0,
                     .last_updated = std::nullopt};
}

auto TrophyRequests::group_progress_of(const psn::rest::EarnedGroupRow& row)
    -> std::optional<GroupProgress> {
  auto id = TrophyGroupId::parse(row.id);
  if (!id) return std::nullopt;
  return GroupProgress{.id = std::move(*id),
                       .earned = counts_of(row.earned),
                       .progress = row.progress,
                       .last_updated = time_of(row.last_updated)};
}

auto TrophyRequests::trophy_of(psn::rest::TrophyRow& row)
    -> std::optional<Trophy> {
  const auto id = row.id ? TrophyId::of(*row.id) : std::nullopt;
  const auto grade = grade_of(row.type);
  auto group = row.group_id.empty() ? std::optional{TrophyGroupId::base_game()}
                                    : TrophyGroupId::parse(row.group_id);
  if (!id || !grade || !group) return std::nullopt;
  return Trophy{.id = *id,
                .group = std::move(*group),
                .grade = *grade,
                .name = std::move(row.name),
                .detail = std::move(row.detail),
                .icon_url = std::move(row.icon_url),
                .hidden = row.hidden,
                .progress_target = row.progress_target,
                .earned = false,
                .earned_at = std::nullopt,
                .rarity = std::nullopt,
                .earned_rate = std::nullopt,
                .progress = std::nullopt};
}

auto TrophyRequests::earned_of(const psn::rest::EarnedTrophyRow& row)
    -> std::optional<EarnedState> {
  const auto id = row.id ? TrophyId::of(*row.id) : std::nullopt;
  if (!id) return std::nullopt;
  std::optional<TrophyProgress> progress;
  if (row.progress)
    progress = TrophyProgress{.value = *row.progress,
                              .percent = row.progress_rate.value_or(0),
                              .updated_at = time_of(row.progressed_at)};
  return EarnedState{.id = *id,
                     .earned = row.earned,
                     .earned_at = time_of(row.earned_at),
                     .rarity = rarity_of(row.rarity),
                     .earned_rate = row.earned_rate,
                     .progress = progress};
}

}  // namespace libchiaking::trophies::ps
