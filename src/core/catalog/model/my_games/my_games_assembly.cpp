#include "src/core/catalog/model/my_games/my_games_assembly.hpp"

#include "src/core/catalog/model/cloud_stream/games.hpp"
#include "src/core/catalog/model/legacy_cloud_stream/games.hpp"
#include "src/core/catalog/model/my_games/remembered_refusals.hpp"
#include "src/core/utils/text/ascii.hpp"

#include <algorithm>
#include <compare>
#include <iterator>
#include <map>
#include <string>
#include <utility>

namespace libchiaking::catalog {

auto MyGamesAssembly::games_of(
    const Sources& sources, const users::Membership& membership,
    const std::optional<users::CloudStreaming>& streaming,
    std::span<const StreamRefusal> refusals, std::uint64_t refusals_revision,
    Time now) -> MyGames {
  const auto& library = sources.library;
  Inputs inputs;
  if (sources.list != nullptr)
    inputs.list_fetched_at = sources.list->fetched_at();
  inputs.library_fetched_at = library.fetched_at();
  inputs.library = sources.library_state;
  inputs.ps5_library = sources.ps5 != nullptr ? sources.ps5->state()
                       : sources.library_state == LibraryState::fetched
                           ? library.ps5_ownership()
                           : LibraryState::unavailable;
  if (sources.category != nullptr)
    inputs.included_fetched_at = sources.category->fetched_at();
  inputs.tier = membership.tier();
  inputs.refusals_revision = refusals_revision;
  inputs.used_trials_revision = sources.used_trials_revision;
  const RememberedRefusals remembered{library.account(), refusals, inputs.tier,
                                      now};
  auto legacy_cloud_stream =
      sources.list != nullptr
          ? legacy_cloud_stream::Games{library, *sources.list, inputs,
                                       remembered}
                .listed()
          : ServiceGames{};
  auto cloud_stream = cloud_stream::Games{sources.ps5,
                                          sources.category,
                                          sources.for_sale,
                                          inputs,
                                          streaming,
                                          remembered,
                                          ps5_store_of(sources),
                                          sources.used_trials}
                          .listed();
  cloud_stream::GenerationLinks links;
  if (sources.category != nullptr) links.add(sources.category->products());
  if (sources.for_sale != nullptr) links.add(sources.for_sale->products());
  return MyGamesAssembly{library.account(), inputs,
                         std::move(legacy_cloud_stream),
                         std::move(cloud_stream), links}
      .games();
}

MyGamesAssembly::MyGamesAssembly(AccountId account, Inputs inputs,
                                 ServiceGames legacy_cloud_stream,
                                 ServiceGames cloud_stream,
                                 const cloud_stream::GenerationLinks& links)
    : account_(account),
      inputs_(inputs),
      listed_(std::move(legacy_cloud_stream.listed)),
      legacy_cloud_stream_count_(listed_.size()),
      unlisted_(std::move(legacy_cloud_stream.unlisted)) {
  std::ranges::move(cloud_stream.listed, std::back_inserter(listed_));
  std::ranges::move(cloud_stream.unlisted, std::back_inserter(unlisted_));
  keep_legacy_cloud_stream_products_apart();
  note_copies_held_on_the_other_service();
  link_generations(links);
  order();
}

auto MyGamesAssembly::games() const -> MyGames {
  std::vector<MyGame> games;
  std::vector<Explanation> why_visible;
  std::vector<Explanation> why_hidden;
  for (const auto& listed : listed_) {
    games.push_back(listed.game);
    (listed.game.is_visible() ? why_visible : why_hidden)
        .push_back(listed.explanation);
  }
  std::ranges::move(why_hidden, std::back_inserter(why_visible));
  std::ranges::copy(unlisted_, std::back_inserter(why_visible));
  return MyGames::of(account_, std::move(games), inputs_,
                     std::move(why_visible))
      .value();
}

auto MyGamesAssembly::ps5_store_of(const Sources& sources) -> StoreLocale {
  if (sources.category != nullptr) return sources.category->locale();
  if (sources.for_sale != nullptr) return sources.for_sale->locale();
  return sources.region.value_or(StoreLocale::english_us());
}

void MyGamesAssembly::keep_legacy_cloud_stream_products_apart() {
  const auto legacy_cloud_stream =
      std::span{listed_}.first(legacy_cloud_stream_count_);
  const auto streamed = [legacy_cloud_stream](const ProductId& product) {
    return std::ranges::any_of(legacy_cloud_stream,
                               [&product](const ServiceGames::Listed& listed) {
                                 return listed.game.product_id() == product;
                               });
  };
  std::vector<ServiceGames::Listed> kept{legacy_cloud_stream.begin(),
                                         legacy_cloud_stream.end()};
  for (auto cloud = listed_.begin() +
                    static_cast<std::ptrdiff_t>(legacy_cloud_stream_count_);
       cloud != listed_.end(); ++cloud) {
    if (streamed(cloud->game.product_id())) {
      unlisted_.push_back(std::move(cloud->explanation));
      continue;
    }
    const std::vector<ProductId> aliases{cloud->game.title().aliases().begin(),
                                         cloud->game.title().aliases().end()};
    for (const auto& alias : aliases)
      if (streamed(alias)) cloud->game = cloud->game.without_alias(alias);
    kept.push_back(std::move(*cloud));
  }
  listed_ = std::move(kept);
}

void MyGamesAssembly::note_copies_held_on_the_other_service() {
  std::erase_if(unlisted_, [this](const Explanation& dropped) {
    const auto* refused = dropped.first_failure();
    if (refused == nullptr || refused->rule != Explanation::Rule::service)
      return false;
    const auto listed = std::ranges::find_if(
        listed_, [&dropped](const ServiceGames::Listed& row) {
          return row.game.title().is_known_as(dropped.product_id());
        });
    if (listed == listed_.end()) return false;
    std::vector<Explanation::Step> steps{listed->explanation.steps().begin(),
                                         listed->explanation.steps().end()};
    steps.push_back(Explanation::Step{.rule = Explanation::Rule::service,
                                      .outcome = Explanation::Outcome::noted,
                                      .ownership = refused->ownership});
    listed->explanation =
        Explanation::of(listed->explanation.product_id(), std::move(steps));
    return true;
  });
}

void MyGamesAssembly::link_generations(
    const cloud_stream::GenerationLinks& links) {
  if (links.empty()) return;
  const auto legacy = std::span{listed_}.first(legacy_cloud_stream_count_);
  const auto current = std::span{listed_}.subspan(legacy_cloud_stream_count_);
  const auto numbers_of = [](const MyGame& game) {
    std::vector<TitleNumber> numbers;
    if (auto number = game.product_id().title_number())
      numbers.push_back(std::move(*number));
    for (const auto& alias : game.title().aliases())
      if (auto number = alias.title_number();
          number && std::ranges::find(numbers, *number) == numbers.end())
        numbers.push_back(std::move(*number));
    return numbers;
  };
  const auto ps5_row_of = [&](const MyGame& game) -> ServiceGames::Listed* {
    for (const auto& number : numbers_of(game)) {
      const auto other = links.other_of(number);
      if (!other) continue;
      const auto found = std::ranges::find_if(
          current, [&other](const ServiceGames::Listed& row) {
            return !row.game.other_version() && !row.game.is_bundle() &&
                   row.game.product_id().title_number() == *other;
          });
      if (found != current.end()) return &*found;
    }
    return nullptr;
  };
  for (auto& ps4 : legacy) {
    if (ps4.game.is_bundle()) continue;
    auto* ps5 = ps5_row_of(ps4.game);
    if (ps5 == nullptr) continue;
    ps4.game = ps4.game.with_other_version(
        MyGame::OtherVersion{ps5->game.product_id(), false});
    ps5->game = ps5->game.with_other_version(
        MyGame::OtherVersion{ps4.game.product_id(), true});
  }
}

void MyGamesAssembly::order() {
  struct Lead {
    bool owned;
    std::string name;
    bool follows{};
  };
  std::map<ProductId, Lead> leads;
  for (const auto& row : listed_)
    if (row.game.other_version() && row.game.other_version()->leads)
      leads.emplace(row.game.product_id(),
                    Lead{row.game.is_owned(), row.game.title().name()});
  std::vector<std::pair<Lead, ServiceGames::Listed>> keyed;
  keyed.reserve(listed_.size());
  for (auto& row : listed_) {
    Lead lead{row.game.is_owned(), row.game.title().name()};
    if (const auto& other = row.game.other_version(); other && !other->leads)
      if (const auto found = leads.find(other->product); found != leads.end())
        lead = Lead{found->second.owned, found->second.name, true};
    keyed.emplace_back(std::move(lead), std::move(row));
  }
  std::ranges::stable_sort(keyed, [](const auto& left, const auto& right) {
    if (left.first.owned != right.first.owned) return left.first.owned;
    if (const auto named = text::casecmp(left.first.name, right.first.name);
        std::is_neq(named))
      return std::is_lt(named);
    return !left.first.follows && right.first.follows;
  });
  listed_.clear();
  for (auto& [lead, row] : keyed) listed_.push_back(std::move(row));
}

}  // namespace libchiaking::catalog
