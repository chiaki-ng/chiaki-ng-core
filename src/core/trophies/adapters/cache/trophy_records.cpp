#include "src/core/trophies/adapters/cache/trophy_records.hpp"

#include "src/core/utils/json/json.hpp"
#include "src/core/utils/text/ascii.hpp"
#include "src/core/utils/timing/wall_milliseconds.hpp"

#include <glaze/json.hpp>

#include <algorithm>
#include <chrono>
#include <iterator>
#include <span>
#include <utility>

template <>
struct glz::meta<libchiaking::trophies::Grade> {
  using enum libchiaking::trophies::Grade;
  static constexpr auto value = glz::enumerate(
      "bronze", bronze, "silver", silver, "gold", gold, "platinum", platinum);
};

template <>
struct glz::meta<libchiaking::trophies::Rarity> {
  using enum libchiaking::trophies::Rarity;
  static constexpr auto value =
      glz::enumerate("ultra_rare", ultra_rare, "very_rare", very_rare, "rare",
                     rare, "common", common);
};

template <>
struct glz::meta<libchiaking::trophies::TrophySet> {
  using enum libchiaking::trophies::TrophySet;
  static constexpr auto value = glz::enumerate("classic", classic, "ps5", ps5);
};

template <>
struct glz::meta<libchiaking::trophies::TrophyPlatform> {
  using enum libchiaking::trophies::TrophyPlatform;
  static constexpr auto value = glz::enumerate(
      "ps3", ps3, "ps4", ps4, "ps5", ps5, "ps_vita", ps_vita, "pc", pc);
};

namespace libchiaking::trophies::cache {

struct TrophyRecords::Wire {
  template <class Body>
  struct Envelope {
    std::string record;
    std::int64_t schema{};
    std::int64_t written_at_ms{};
    std::string account;
    Body body{};
  };

  struct TitleRow {
    std::string id;
    std::optional<TrophySet> set;
    std::string name;
    std::string detail;
    std::string icon_url;
    std::vector<TrophyPlatform> platforms;
    std::string set_version;
    bool has_groups{};
    std::uint32_t group_count{};
    TrophyCounts defined;
    TrophyCounts earned;
    std::uint32_t progress{};
    bool hidden{};
    std::optional<std::int64_t> last_updated_ms;
  };

  struct TitlesBody {
    std::vector<TitleRow> titles;
  };

  struct GroupRow {
    std::string id;
    std::string name;
    std::string detail;
    std::string icon_url;
    TrophyCounts defined;
    TrophyCounts earned;
    std::uint32_t progress{};
    std::optional<std::int64_t> last_updated_ms;
  };

  struct ProgressRow {
    std::uint64_t value{};
    std::uint32_t percent{};
    std::optional<std::int64_t> updated_at_ms;
  };

  struct TrophyRow {
    std::uint32_t id{};
    std::string group;
    Grade grade{};
    std::string name;
    std::string detail;
    std::string icon_url;
    bool hidden{};
    std::optional<std::uint64_t> progress_target;
    bool earned{};
    std::optional<std::int64_t> earned_at_ms;
    std::optional<Rarity> rarity;
    std::optional<double> earned_rate;
    std::optional<ProgressRow> progress;
  };

  struct DetailBody {
    std::string title;
    std::optional<std::int64_t> last_updated_ms;
    std::vector<GroupRow> groups;
    std::vector<TrophyRow> trophies;
  };

  struct SetsBody {
    std::string title;
    std::vector<std::string> sets;
  };

  static auto body_of(const TitleSets& sets) -> SetsBody {
    SetsBody body{.title = sets.title.text(), .sets = {}};
    for (const auto& set : sets.sets) body.sets.push_back(set.text());
    return body;
  }

  static auto sets_of(const SetsBody& body) -> std::optional<TitleSets> {
    auto title = NpTitleId::parse(body.title);
    if (!title) return std::nullopt;
    TitleSets sets{.title = std::move(*title), .sets = {}};
    for (const auto& text : body.sets) {
      auto set = NpCommunicationId::parse(text);
      if (!set) return std::nullopt;
      sets.sets.push_back(std::move(*set));
    }
    return sets;
  }

  static constexpr glz::opts reading{.error_on_unknown_keys = false,
                                     .error_on_missing_keys = true};

  static auto row_of(const TrophyTitle& title) -> TitleRow {
    return TitleRow{
        .id = title.id.text(),
        .set = title.set,
        .name = title.name,
        .detail = title.detail,
        .icon_url = title.icon_url,
        .platforms = title.platforms,
        .set_version = title.set_version,
        .has_groups = title.has_groups,
        .group_count = title.group_count,
        .defined = title.defined,
        .earned = title.earned,
        .progress = title.progress,
        .hidden = title.hidden,
        .last_updated_ms = timing::milliseconds_of(title.last_updated)};
  }

  static auto title_of(const TitleRow& row) -> std::optional<TrophyTitle> {
    auto id = NpCommunicationId::parse(row.id);
    if (!id) return std::nullopt;
    return TrophyTitle{.id = std::move(*id),
                       .set = row.set,
                       .name = row.name,
                       .detail = row.detail,
                       .icon_url = row.icon_url,
                       .platforms = row.platforms,
                       .set_version = row.set_version,
                       .has_groups = row.has_groups,
                       .group_count = row.group_count,
                       .defined = row.defined,
                       .earned = row.earned,
                       .progress = row.progress,
                       .hidden = row.hidden,
                       .last_updated = timing::time_of(row.last_updated_ms)};
  }

  static auto row_of(const TrophyGroup& group) -> GroupRow {
    return GroupRow{
        .id = group.id.text(),
        .name = group.name,
        .detail = group.detail,
        .icon_url = group.icon_url,
        .defined = group.defined,
        .earned = group.earned,
        .progress = group.progress,
        .last_updated_ms = timing::milliseconds_of(group.last_updated)};
  }

  static auto group_of(const GroupRow& row) -> std::optional<TrophyGroup> {
    auto id = TrophyGroupId::parse(row.id);
    if (!id) return std::nullopt;
    return TrophyGroup{.id = std::move(*id),
                       .name = row.name,
                       .detail = row.detail,
                       .icon_url = row.icon_url,
                       .defined = row.defined,
                       .earned = row.earned,
                       .progress = row.progress,
                       .last_updated = timing::time_of(row.last_updated_ms)};
  }

  static auto row_of(const Trophy& trophy) -> TrophyRow {
    std::optional<ProgressRow> progress;
    if (trophy.progress)
      progress = ProgressRow{.value = trophy.progress->value,
                             .percent = trophy.progress->percent,
                             .updated_at_ms = timing::milliseconds_of(
                                 trophy.progress->updated_at)};
    return TrophyRow{.id = trophy.id.value(),
                     .group = trophy.group.text(),
                     .grade = trophy.grade,
                     .name = trophy.name,
                     .detail = trophy.detail,
                     .icon_url = trophy.icon_url,
                     .hidden = trophy.hidden,
                     .progress_target = trophy.progress_target,
                     .earned = trophy.earned,
                     .earned_at_ms = timing::milliseconds_of(trophy.earned_at),
                     .rarity = trophy.rarity,
                     .earned_rate = trophy.earned_rate,
                     .progress = progress};
  }

  static auto trophy_of(const TrophyRow& row) -> std::optional<Trophy> {
    const auto id = TrophyId::of(row.id);
    auto group = TrophyGroupId::parse(row.group);
    if (!id || !group) return std::nullopt;
    std::optional<TrophyProgress> progress;
    if (row.progress)
      progress = TrophyProgress{
          .value = row.progress->value,
          .percent = row.progress->percent,
          .updated_at = timing::time_of(row.progress->updated_at_ms)};
    return Trophy{.id = *id,
                  .group = std::move(*group),
                  .grade = row.grade,
                  .name = row.name,
                  .detail = row.detail,
                  .icon_url = row.icon_url,
                  .hidden = row.hidden,
                  .progress_target = row.progress_target,
                  .earned = row.earned,
                  .earned_at = timing::time_of(row.earned_at_ms),
                  .rarity = row.rarity,
                  .earned_rate = row.earned_rate,
                  .progress = progress};
  }

  static auto body_of(const TitleTrophies& trophies) -> DetailBody {
    DetailBody body{
        .title = trophies.title.text(),
        .last_updated_ms = timing::milliseconds_of(trophies.last_updated),
        .groups = {},
        .trophies = {}};
    for (const auto& group : trophies.groups)
      body.groups.push_back(row_of(group));
    for (const auto& trophy : trophies.trophies)
      body.trophies.push_back(row_of(trophy));
    return body;
  }

  static auto trophies_of(const DetailBody& body)
      -> std::optional<TitleTrophies> {
    auto title = NpCommunicationId::parse(body.title);
    if (!title || body.groups.empty() || body.trophies.empty())
      return std::nullopt;
    TitleTrophies trophies{
        .title = std::move(*title),
        .last_updated = timing::time_of(body.last_updated_ms),
        .groups = {},
        .trophies = {}};
    for (const auto& row : body.groups) {
      auto group = group_of(row);
      if (!group) return std::nullopt;
      trophies.groups.push_back(std::move(*group));
    }
    for (const auto& row : body.trophies) {
      auto trophy = trophy_of(row);
      if (!trophy) return std::nullopt;
      trophies.trophies.push_back(std::move(*trophy));
    }
    return trophies;
  }
};

TrophyRecords::TrophyRecords(platform::RecordStore& store,
                             const platform::ClockProvider& wall_clock) noexcept
    : store_(store), wall_clock_(wall_clock) {}

auto TrophyRecords::now() const noexcept -> Time {
  return Time{std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::microseconds{wall_clock_.now_microseconds()})};
}

auto TrophyRecords::name_of(AccountId account, std::string_view leaf)
    -> std::optional<platform::RecordName> {
  const auto root = platform::RecordName::parse(shelf);
  const auto owner = root ? root->child(account.decimal()) : std::nullopt;
  return owner ? owner->child(leaf) : std::nullopt;
}

auto TrophyRecords::lowered_leaf(std::string_view prefix, std::string_view id)
    -> std::string {
  std::string leaf{prefix};
  std::ranges::transform(id, std::back_inserter(leaf), text::ascii_lower);
  return leaf;
}

auto TrophyRecords::title_leaf(const NpCommunicationId& title) -> std::string {
  return lowered_leaf("title-", title.text());
}

auto TrophyRecords::played_leaf(const NpTitleId& title) -> std::string {
  return lowered_leaf("played-", title.text());
}

template <class Body>
auto TrophyRecords::read(AccountId account, std::string_view kind,
                         std::string_view leaf) -> std::optional<Kept<Body>> {
  const auto name = name_of(account, leaf);
  if (!name) return std::nullopt;
  auto stored = store_.read(*name);
  if (!stored) return std::nullopt;
  const auto& bytes = stored->data;
  Wire::Envelope<Body> envelope{};
  if (json::read_into<Wire::reading>(
          envelope,
          std::string_view{reinterpret_cast<const char*>(bytes.data()),
                           bytes.size()}) ||
      envelope.record != kind || envelope.schema != schema ||
      envelope.account != account.decimal()) {
    static_cast<void>(store_.erase(*name));
    return std::nullopt;
  }
  return Kept<Body>{std::move(envelope.body),
                    timing::time_of(envelope.written_at_ms)};
}

template <class Body>
void TrophyRecords::write(AccountId account, std::string_view kind,
                          std::string_view leaf, const Body& body) {
  const auto name = name_of(account, leaf);
  if (!name) return;
  const Wire::Envelope<Body> envelope{
      .record = std::string{kind},
      .schema = schema,
      .written_at_ms = timing::milliseconds_of(now()),
      .account = account.decimal(),
      .body = body};
  std::string text;
  if (glz::write<glz::opts{}>(envelope, text)) return;
  static_cast<void>(store_.write_atomic(
      *name, std::as_bytes(std::span{text.data(), text.size()})));
}

auto TrophyRecords::summary(AccountId account)
    -> std::optional<Kept<TrophySummary>> {
  return read<TrophySummary>(account, "summary", "summary");
}

auto TrophyRecords::titles(AccountId account)
    -> std::optional<Kept<std::vector<TrophyTitle>>> {
  auto kept = read<Wire::TitlesBody>(account, "titles", "titles");
  if (!kept) return std::nullopt;
  std::vector<TrophyTitle> titles;
  titles.reserve(kept->value.titles.size());
  for (const auto& row : kept->value.titles) {
    auto title = Wire::title_of(row);
    if (!title) return std::nullopt;
    titles.push_back(std::move(*title));
  }
  return Kept<std::vector<TrophyTitle>>{std::move(titles), kept->written_at};
}

auto TrophyRecords::trophies(AccountId account, const NpCommunicationId& title)
    -> std::optional<Kept<TitleTrophies>> {
  auto kept = read<Wire::DetailBody>(account, "title", title_leaf(title));
  if (!kept) return std::nullopt;
  auto trophies = Wire::trophies_of(kept->value);
  if (!trophies || trophies->title != title) return std::nullopt;
  return Kept<TitleTrophies>{std::move(*trophies), kept->written_at};
}

auto TrophyRecords::title_sets(AccountId account, const NpTitleId& title)
    -> std::optional<Kept<TitleSets>> {
  auto kept = read<Wire::SetsBody>(account, "played", played_leaf(title));
  if (!kept) return std::nullopt;
  auto sets = Wire::sets_of(kept->value);
  if (!sets || sets->title != title) return std::nullopt;
  return Kept<TitleSets>{std::move(*sets), kept->written_at};
}

void TrophyRecords::keep(AccountId account, const TrophySummary& summary) {
  write(account, "summary", "summary", summary);
}

void TrophyRecords::keep(AccountId account,
                         const std::vector<TrophyTitle>& titles) {
  Wire::TitlesBody body;
  body.titles.reserve(titles.size());
  for (const auto& title : titles) body.titles.push_back(Wire::row_of(title));
  write(account, "titles", "titles", body);
}

void TrophyRecords::keep(AccountId account, const TitleTrophies& trophies) {
  write(account, "title", title_leaf(trophies.title), Wire::body_of(trophies));
}

void TrophyRecords::keep(AccountId account, const TitleSets& sets) {
  write(account, "played", played_leaf(sets.title), Wire::body_of(sets));
}

void TrophyRecords::forget(AccountId account) {
  const auto root = platform::RecordName::parse(shelf);
  const auto owner = root ? root->child(account.decimal()) : std::nullopt;
  if (owner) static_cast<void>(store_.erase_prefix(*owner));
}

}  // namespace libchiaking::trophies::cache
