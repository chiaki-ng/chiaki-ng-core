#include "src/core/catalog/model/legacy_cloud_stream/games.hpp"
#include "src/core/catalog/model/list/title_name.hpp"

#include <libchiaking/detail/catalog/owned_title.hpp>
#include <libchiaking/detail/catalog/product_id.hpp>
#include <libchiaking/detail/catalog/title.hpp>
#include <libchiaking/detail/catalog/title_number.hpp>

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace libchiaking::catalog::legacy_cloud_stream {

/// @brief The editions the user licenses, indexed by every way another
/// listing of the same game can be recognised, so that an offered title that
/// is an edition of a game the user already licenses is hidden as owned.
///
/// LegacyCloudStream lists every edition of a game separately. Matching is
/// loose by design: product, alias, title number, the name without edition
/// words, or concept plus squashed name.
///
/// Filled with add, then queried with holder_of. When two licensed rows
/// claim one key the first added keeps it. Not safe to add while another
/// thread reads; built and used on one thread by legacy_cloud_stream::Games.
class Games::OwnedEditions {
 public:
  /// @brief Records a listed title the user licenses.
  ///
  /// The title's product, its aliases, the copy's entitlement id read as a
  /// product, the copy's product, the title numbers inside all of those,
  /// its name without edition words plus platform, and its concept plus
  /// platform plus squashed name are all made to point at the title's
  /// product.
  ///
  /// @param title The listed title the copy was merged into.
  /// @param owned The licensed copy.
  void add(const Title& title, const OwnedTitle& owned);
  /// @brief Which licensed edition covers a title, if any.
  ///
  /// Looks up the title's product, then its title number, then its name
  /// without edition words on the same platform, then its concept, platform
  /// and squashed name.
  ///
  /// @param title The offered title.
  /// @return The product of the licensed edition, which may be the title's
  ///     own product, or std::nullopt when no licensed edition matches.
  auto holder_of(const Title& title) const -> std::optional<ProductId>;

 private:
  /// @brief The key suffix that keeps platforms apart.
  ///
  /// @param title The title.
  /// @return "|" followed by the numeric Platform value.
  static auto platform_suffix(const Title& title) -> std::string;
  /// @brief The edition key: concept, platform and squashed name.
  ///
  /// @param title The title.
  /// @return The key, or std::nullopt when the title has no concept.
  static auto edition_of(const Title& title) -> std::optional<std::string>;
  /// @brief Points a product, and the title number inside it, at the
  /// licensed edition.
  ///
  /// @param product The product to recognise.
  /// @param owner The licensed edition's product.
  void know(const ProductId& product, const ProductId& owner);

  /// @brief Licensed edition by recognised product.
  std::map<ProductId, ProductId> products_;
  /// @brief Licensed edition by title number.
  std::map<TitleNumber, ProductId> numbers_;
  /// @brief Licensed edition by name without edition words plus platform.
  std::map<std::string, ProductId> names_;
  /// @brief Licensed edition by concept, platform and squashed name.
  std::map<std::string, ProductId> editions_;
};

void Games::OwnedEditions::add(const Title& title, const OwnedTitle& owned) {
  const auto& owner = title.product_id();
  know(title.product_id(), owner);
  for (const auto& alias : title.aliases()) know(alias, owner);
  if (const auto held = owned.id().as_product()) know(*held, owner);
  if (owned.product_id()) know(*owned.product_id(), owner);
  if (const auto name = TitleName{title.name()}.without_edition_words();
      !name.empty())
    names_.emplace(name + platform_suffix(title), owner);
  if (const auto edition = edition_of(title))
    editions_.emplace(*edition, owner);
}

auto Games::OwnedEditions::holder_of(const Title& title) const
    -> std::optional<ProductId> {
  if (const auto found = products_.find(title.product_id());
      found != products_.end())
    return found->second;
  if (const auto number = title.title_number())
    if (const auto found = numbers_.find(*number); found != numbers_.end())
      return found->second;
  if (const auto name = TitleName{title.name()}.without_edition_words();
      !name.empty())
    if (const auto found = names_.find(name + platform_suffix(title));
        found != names_.end())
      return found->second;
  if (const auto edition = edition_of(title))
    if (const auto found = editions_.find(*edition); found != editions_.end())
      return found->second;
  return std::nullopt;
}

auto Games::OwnedEditions::platform_suffix(const Title& title) -> std::string {
  return "|" + std::to_string(static_cast<int>(title.platform()));
}

auto Games::OwnedEditions::edition_of(const Title& title)
    -> std::optional<std::string> {
  if (!title.concept_id()) return std::nullopt;
  return title.concept_id()->text() + platform_suffix(title) + "|" +
         TitleName{title.name()}.squashed();
}

void Games::OwnedEditions::know(const ProductId& product,
                                const ProductId& owner) {
  products_.emplace(product, owner);
  if (const auto number = product.title_number())
    numbers_.emplace(*number, owner);
}

class Games::StoreMetadata {
 public:
  explicit StoreMetadata(const StreamableList& list) : index_(list) {}

  auto for_copy(const OwnedTitle& owned, const Library& library) const -> Lent {
    return index_.for_copy_or_sibling(owned, library);
  }

  auto lists_for_ps5(const OwnedTitle& owned) const -> bool {
    const auto on_ps5 = [](const Entry* entry) {
      return entry != nullptr && entry->title.platform() == Platform::ps5;
    };
    const auto& streams = index_.streams();
    const auto& product = owned.product_id();
    const auto held = owned.id().as_product();
    return on_ps5(streams.by_product(product)) ||
           on_ps5(streams.by_product(held)) ||
           on_ps5(
               streams.by_key(product ? product->title_key() : std::nullopt)) ||
           on_ps5(streams.by_key(owned.id().title_key()));
  }

 private:
  ListingIndex index_;
};

Games::Row::Row(Title title, PlusInclusion inclusion, LaunchIdentifier launch)
    : title_(std::move(title)),
      inclusion_(inclusion),
      launch_(std::move(launch)) {}

auto Games::Row::on_shelf(const Entry& entry) -> Row {
  Row row{entry.title, entry.inclusion,
          LaunchIdentifier::of(entry.title.product_id())};
  if (entry.title.streamability() == ListStreamability::marked_unstreamable)
    row.state_ = State::outside_every_plan;
  return row;
}

auto Games::Row::off_shelf(Title title, const ProductId& launched_as,
                           const OwnedTitle& owned) -> Row {
  Row row{std::move(title),
          PlusInclusion::not_included(PlusInclusion::Source::old_service),
          LaunchIdentifier::of(launched_as)};
  row.state_ = State::licensed_off_shelf;
  row.ownership_ = owned;
  row.matched_.push_back(Step{.rule = Rule::library_match,
                              .outcome = Outcome::passed,
                              .product = launched_as,
                              .ownership = owned.id()});
  row.matched_.push_back(Step{.rule = Rule::own_listing,
                              .outcome = Outcome::passed,
                              .ownership = owned.id()});
  return row;
}

auto Games::Row::visibility() const -> Visibility {
  switch (state_) {
    case State::covered_by_a_licensed_edition:
      return Visibility::hidden(HideReason::already_owned, covered_by_);
    case State::below_the_streaming_tier:
      return Visibility::hidden(HideReason::tier_does_not_include);
    case State::outside_every_plan:
      return Visibility::hidden(HideReason::not_in_service);
    case State::offered:
    case State::licensed:
    case State::licensed_off_shelf:
      break;
  }
  return Visibility::visible();
}

auto Games::Row::facts() const -> MyGame::Facts {
  MyGame::Facts facts{
      .title = title_,
      .inclusion = inclusion_,
      .path = title_.platform() == Platform::ps5 ? PlayPath::included
                                                 : PlayPath::old_service,
      .visibility = visibility(),
      .launch_identifier = launch_,
  };
  facts.ownership = ownership_;
  facts.refusal = refusal_;
  return facts;
}

void Games::Row::license(const OwnedTitle& owned) {
  if (ownership_) {
    const bool streams_instead =
        owned.streams() == true && ownership_->streams() != true;
    const auto displaced = streams_instead ? ownership_->id() : owned.id();
    ownership_ = streams_instead
                     ? owned.released_no_later_than(ownership_->release())
                     : ownership_->released_no_later_than(owned.release());
    matched_.push_back(Step{.rule = Rule::owned_duplicate,
                            .outcome = Outcome::noted,
                            .ownership = displaced});
    return;
  }
  state_ = State::licensed;
  ownership_ = owned;
  matched_.push_back(Step{.rule = Rule::library_match,
                          .outcome = Outcome::passed,
                          .product = title_.product_id(),
                          .ownership = owned.id()});
  matched_.push_back(Step{.rule = Rule::merged_into_listing,
                          .outcome = Outcome::passed,
                          .product = title_.product_id(),
                          .ownership = owned.id()});
}

void Games::Row::cover(ProductId licensed_edition) {
  if (state_ != State::offered) return;
  state_ = State::covered_by_a_licensed_edition;
  covered_by_ = std::move(licensed_edition);
}

void Games::Row::fall_below_the_streaming_tier() {
  state_ = State::below_the_streaming_tier;
  covered_by_.reset();
}

void Games::Row::remember(std::optional<StreamRefusal> refusal) {
  refusal_ = std::move(refusal);
}

Games::Games(const Library& library, const StreamableList& list,
             const MyGames::Inputs& inputs, const RememberedRefusals& refusals)
    : listing_(inputs, ServiceGeneration::old_service, list.settled_locale()) {
  shelve(list);
  hold(library, list);
  hide();
  remember(refusals);
}

auto Games::listed() const -> ServiceGames {
  ServiceGames games;
  for (const auto& row : rows_)
    listing_.list(games, row.facts(), steps_of(row));
  games.unlisted = unlisted_;
  return games;
}

void Games::shelve(const StreamableList& list) {
  for (const auto& entry : list.titles()) {
    if (entry.title.service() != ServiceGeneration::old_service) continue;
    rows_.push_back(Row::on_shelf(entry));
    index(rows_.size() - 1);
  }
}

void Games::hold(const Library& library, const StreamableList& list) {
  struct Placement {
    const OwnedTitle* owned{};
    std::vector<std::size_t> shelved{};
    Lent lent{};
  };
  const StoreMetadata store{list};
  std::vector<Placement> placements;
  std::set<ProductId> fully_owned;
  for (const auto& owned : library.titles()) {
    if (owned.service() != ServiceGeneration::old_service) continue;
    Placement placement{&owned, shelved_for(owned, library)};
    const auto product = product_of(owned);
    if (placement.shelved.empty() && store.lists_for_ps5(owned)) {
      if (product)
        unlisted_.push_back(
            Explanation::of(*product, {Step{.rule = Rule::service,
                                            .outcome = Outcome::failed,
                                            .ownership = owned.id()}}));
      continue;
    }
    if (placement.shelved.empty())
      placement.lent = store.for_copy(owned, library);
    if (placement.shelved.empty() && placement.lent.entry == nullptr) {
      if (product) unlisted_.push_back(unmatched(*product, owned));
      continue;
    }
    if (placement.shelved.empty() && !streams_as_held(placement.lent, owned)) {
      if (product)
        unlisted_.push_back(off_the_list(
            *product, owned, placement.lent.entry->title.product_id()));
      continue;
    }
    if (product && owned.held_as() != HeldAs::trial_access)
      fully_owned.insert(*product);
    placements.push_back(std::move(placement));
  }
  for (const auto& placement : placements) {
    const auto& owned = *placement.owned;
    const auto product = product_of(owned);
    if (auto superseded =
            ServiceListing::superseded(owned, product, fully_owned)) {
      unlisted_.push_back(std::move(*superseded));
      continue;
    }
    for (const auto at : placement.shelved) rows_[at].license(owned);
    if (!placement.shelved.empty() || !product) continue;
    if (const auto listed = row_listing(*product))
      rows_[*listed].license(owned);
    else
      list_unshelved(owned, placement.lent, *product);
  }
}

void Games::hide() {
  OwnedEditions licensed;
  for (const auto& row : rows_)
    if (row.ownership()) licensed.add(row.title(), *row.ownership());
  for (auto& row : rows_)
    if (auto edition = licensed.holder_of(row.title()))
      row.cover(std::move(*edition));
  if (listing_.below_the_streaming_tier())
    for (auto& row : rows_) row.fall_below_the_streaming_tier();
}

void Games::remember(const RememberedRefusals& refusals) {
  for (auto& row : rows_)
    row.remember(refusals.standing_against(
        row.title(), listing_.holding_of(row.ownership())));
}

void Games::index(std::size_t at) {
  const auto& title = rows_[at].title();
  by_product_.emplace(title.product_id(), at);
  if (const auto key = title.product_id().title_key())
    by_key_.emplace(*key, at);
  if (const auto& grouped = title.concept_id())
    by_concept_.emplace(*grouped, at);
}

auto Games::shelved_for(const OwnedTitle& owned, const Library& library) const
    -> std::vector<std::size_t> {
  const auto held = owned.id().as_product();
  for (const auto& product : {owned.product_id(), held})
    if (product)
      if (const auto found = by_product_.find(*product);
          found != by_product_.end())
        return {found->second};
  const auto product_key =
      owned.product_id() ? owned.product_id()->title_key() : std::nullopt;
  for (const auto& key : {product_key, owned.id().title_key()})
    if (key)
      if (const auto found = by_key_.find(*key); found != by_key_.end())
        return {found->second};
  if (const auto& grouped = owned.concept_id())
    if (const auto found = by_concept_.find(*grouped);
        found != by_concept_.end())
      return {found->second};
  std::vector<std::size_t> placed;
  if (!owned.product_id()) return placed;
  for (const auto& sibling : library.siblings_of(*owned.product_id())) {
    const auto at = row_listing(sibling);
    if (at && std::ranges::find(placed, *at) == placed.end())
      placed.push_back(*at);
  }
  return placed;
}

auto Games::row_listing(const ProductId& product) const
    -> std::optional<std::size_t> {
  if (const auto found = by_product_.find(product); found != by_product_.end())
    return found->second;
  if (const auto key = product.title_key())
    if (const auto found = by_key_.find(*key); found != by_key_.end())
      return found->second;
  return std::nullopt;
}

void Games::list_unshelved(const OwnedTitle& owned, const Lent& lent,
                           const ProductId& product) {
  const auto& store = lent.entry->title;
  auto facts = ServiceListing::lent_facts(&store, owned, product);
  facts.name = !store.name().empty() ? store.name() : owned.name();
  if (facts.name.empty()) facts.name = product.text();
  facts.platform = owned.platform();
  facts.service = ServiceGeneration::old_service;
  facts.streamability = ListStreamability::listed;
  auto title = Title::from(std::move(facts));
  if (!title) return;
  rows_.push_back(
      Row::off_shelf(std::move(title).value(), store.product_id(), owned));
  index(rows_.size() - 1);
}

auto Games::steps_of(const Row& row) const -> std::vector<Step> {
  const auto visibility = row.visibility();
  const auto& matched = row.matched();
  if (row.state() == State::below_the_streaming_tier)
    return listing_.refused_by_tier(row.title(), visibility.hide_reason(),
                                    matched);
  auto steps = listing_.opening(row.title());
  const bool outside = row.state() == State::outside_every_plan;
  steps.push_back(
      Step{.rule = Rule::included_with_plus,
           .outcome = outside ? Outcome::failed : Outcome::passed,
           .hides = outside ? visibility.hide_reason() : std::nullopt,
           .inclusion_source = row.facts().inclusion.source()});
  steps.insert(steps.end(), matched.begin(), matched.end());
  if (outside) return steps;
  if (!row.ownership())
    steps.push_back(Step{.rule = Rule::already_owned,
                         .outcome = row.state() == State::offered
                                        ? Outcome::passed
                                        : Outcome::failed,
                         .hides = visibility.hide_reason(),
                         .product = visibility.because_of()});
  listing_.close(steps, row.refusal(), row.ownership());
  return steps;
}

auto Games::product_of(const OwnedTitle& owned) -> std::optional<ProductId> {
  return owned.product_id() ? owned.product_id() : owned.id().as_product();
}

auto Games::unmatched(const ProductId& product, const OwnedTitle& owned)
    -> Explanation {
  return Explanation::of(product, {Step{.rule = Rule::library_match,
                                        .outcome = Outcome::failed,
                                        .ownership = owned.id()}});
}

auto Games::streams_as_held(const Lent& lent, const OwnedTitle& owned) -> bool {
  if (lent.entry == nullptr || !lent.streams) return false;
  const auto& listed = lent.entry->title;
  const auto& held = owned.product_id();
  const auto as_held = owned.id().as_product();
  return (held && listed.is_known_as(*held)) ||
         (as_held && listed.is_known_as(*as_held));
}

auto Games::off_the_list(const ProductId& product, const OwnedTitle& owned,
                         const ProductId& related) -> Explanation {
  return Explanation::of(product, {Step{.rule = Rule::library_match,
                                        .outcome = Outcome::passed,
                                        .product = related,
                                        .ownership = owned.id()},
                                   Step{.rule = Rule::listed_by_service,
                                        .outcome = Outcome::failed,
                                        .ownership = owned.id()}});
}

}  // namespace libchiaking::catalog::legacy_cloud_stream
