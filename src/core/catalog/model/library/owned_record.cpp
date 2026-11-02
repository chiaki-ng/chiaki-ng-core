#include "src/core/catalog/model/library/owned_record.hpp"

#include "src/core/catalog/model/list/title_name.hpp"
#include "src/core/utils/text/ascii.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace libchiaking::catalog {

auto OwnedRecord::why_not_a_game() const -> std::optional<NotAGame> {
  constexpr std::string_view trial_package = "GT";
  constexpr std::string_view trial_sku = "trial";
  const TitleName name{fields_.name};
  const std::string_view package{fields_.package_type};
  const std::string_view sku{fields_.sku_type};
  if (!fields_.has_game_details) return NotAGame::no_game_details;
  if (!fields_.active) return NotAGame::inactive;
  if (fields_.feature_type == add_on_feature) return NotAGame::add_on;
  if (package.size() >= trial_package.size() &&
      text::ieq(package.substr(package.size() - trial_package.size()),
                trial_package))
    return NotAGame::trial_package;
  const auto marks_trial = std::ranges::search(
      sku, trial_sku,
      [](char left, char right) { return text::ascii_lower(left) == right; });
  if (!marks_trial.empty()) return NotAGame::trial_sku;
  if (name.is_demo()) return NotAGame::demo_name;
  if (name.is_trial()) return NotAGame::trial_name;
  if (!is_ps5_native()) return std::nullopt;
  if (text::ieq(package, "PSMEDIA")) return NotAGame::media_app;
  if (text::ieq(package, "PSTRACK")) return NotAGame::tracking_placeholder;
  if (name.is_digital_extra()) return NotAGame::digital_extra;
  return std::nullopt;
}

auto OwnedRecord::is_ps5_native() const noexcept -> bool {
  return TitleNumber::platform_marked_in(launch_id()) == Platform::ps5;
}

auto OwnedRecord::stated_platform() const noexcept -> std::optional<Platform> {
  if (is_ps5_native() || text::ieq(fields_.platform_id, "ps5"))
    return Platform::ps5;
  if (text::ieq(fields_.platform_id, "ps4")) return Platform::ps4;
  if (text::ieq(fields_.platform_id, "ps3")) return Platform::ps3;
  return std::nullopt;
}

auto OwnedRecord::named_platform() const noexcept -> std::optional<Platform> {
  return TitleNumber::platform_marked_in(
      fields_.product_id.empty() ? fields_.id : fields_.product_id);
}

auto OwnedRecord::service() const noexcept -> ServiceGeneration {
  const auto platform =
      stated_platform().or_else([this] { return named_platform(); });
  return platform && *platform != Platform::ps5 ? ServiceGeneration::old_service
                                                : ServiceGeneration::ps5_cloud;
}

auto OwnedRecord::platform() const noexcept -> Platform {
  return stated_platform()
      .or_else([this] { return named_platform(); })
      .value_or(Platform::ps3);
}

auto OwnedRecord::stream_rank() const noexcept -> int {
  constexpr int launches_as_its_product = 4;
  constexpr int ps5_native = 3;
  constexpr int full_game = 2;
  constexpr int identified = 1;
  int rank = 0;
  if (!fields_.product_id.empty() && fields_.product_id == fields_.id)
    rank += launches_as_its_product;
  if (fields_.feature_type == full_game_feature ||
      fields_.package_type.ends_with("GD"))
    rank += full_game;
  if (!fields_.id.empty()) rank += identified;
  if (TitleNumber::platform_marked_in(fields_.id) == Platform::ps5)
    rank += ps5_native;
  return rank;
}

auto OwnedRecord::streams_better_than(const OwnedRecord& other) const noexcept
    -> bool {
  if (stream_rank() != other.stream_rank())
    return stream_rank() > other.stream_rank();
  if (is_streaming_package() != other.is_streaming_package())
    return is_streaming_package();
  if (const int compared = fields_.sku_id.compare(other.fields_.sku_id);
      compared != 0)
    return compared < 0;
  if (const int compared = fields_.product_id.compare(other.fields_.product_id);
      compared != 0)
    return compared < 0;
  return fields_.id < other.fields_.id;
}

auto OwnedRecord::as_owned_title() const -> std::optional<OwnedTitle> {
  if (why_not_a_game()) return std::nullopt;
  auto id = OwnershipId::parse(launch_id());
  if (!id) return std::nullopt;
  OwnedTitle::Facts facts{.id = std::move(*id)};
  facts.name = fields_.name;
  facts.platform = platform();
  facts.service = service();
  facts.held = held_as();
  facts.release = release();
  facts.product = product_id();
  facts.concept_id = ConceptId::parse(fields_.concept_id);
  facts.cover_url = fields_.cover_url;
  if (is_streaming_package()) facts.streams = true;
  auto title = OwnedTitle::from(std::move(facts));
  if (!title) return std::nullopt;
  return std::move(title).value();
}

auto OwnedRecord::library_of(AccountId account,
                             std::span<const OwnedRecord> records,
                             Library::Time fetched_at) -> Library {
  std::vector<Library::Bundle> bundles;
  std::vector<const OwnedRecord*> preferred;
  for (const auto& record : records) {
    preferred.push_back(&record);
    const auto product = record.product_id();
    const auto sibling = ProductId::parse(record.fields().id);
    if (!product || !sibling) continue;
    auto bundle =
        std::ranges::find(bundles, *product, &Library::Bundle::product);
    if (bundle == bundles.end())
      bundle = bundles.insert(bundles.end(), Library::Bundle{*product});
    if (std::ranges::find(bundle->siblings, *sibling) == bundle->siblings.end())
      bundle->siblings.push_back(*sibling);
  }
  std::ranges::stable_sort(
      preferred, [](const OwnedRecord* left, const OwnedRecord* right) {
        return left->streams_better_than(*right);
      });
  std::vector<OwnedTitle> titles;
  for (const auto* record : preferred) {
    auto title = record->as_owned_title();
    if (!title) continue;
    const auto held = std::ranges::find(titles, title->id(), &OwnedTitle::id);
    if (held != titles.end())
      *held = held->released_no_later_than(title->release());
    else
      titles.push_back(std::move(*title));
  }
  return Library::of(account, std::move(titles), std::move(bundles), fetched_at)
      .value();
}

auto OwnedRecord::launch_id() const noexcept -> std::string_view {
  return fields_.id.empty() ? std::string_view{fields_.product_id}
                            : std::string_view{fields_.id};
}

auto OwnedRecord::is_streaming_package() const noexcept -> bool {
  return fields_.package_type.ends_with("GS");
}

}  // namespace libchiaking::catalog
