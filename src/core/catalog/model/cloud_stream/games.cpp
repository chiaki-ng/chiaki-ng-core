#include "src/core/catalog/model/cloud_stream/games.hpp"

#include "src/core/utils/text/ascii.hpp"

#include <algorithm>
#include <utility>

namespace libchiaking::catalog::cloud_stream {

Games::Games(const OwnedPs5Games* owned, const PremiumCategory* category,
             const ForSaleCategory* for_sale, const MyGames::Inputs& inputs,
             std::optional<users::CloudStreaming> streaming,
             const RememberedRefusals& refusals, StoreLocale store,
             std::span<const ProductId> used_trials)
    : listing_(inputs, ServiceGeneration::ps5_cloud, std::move(store)),
      streaming_(std::move(streaming)) {
  if (owned != nullptr) owned_and_active_ = owned->owned_and_active();
  if (category != nullptr) offer_premium_rows(*category);
  if (owned != nullptr) place_owned(*owned);
  settle_bundles();
  if (for_sale != nullptr && listing_.library_is_known())
    offer_rows_for_sale(*for_sale, used_trials);
  claim_aliases();
  remember(refusals);
}

void Games::offer_premium_rows(const PremiumCategory& category) {
  for (const auto& product : category.products()) {
    if (premium_products_.contains(product.id())) continue;
    auto offer = product.included_offer();
    if (!offer) {
      decline(product);
      continue;
    }
    Row row{title_of(product, category.locale()),
            PlusInclusion::with_premium(PlusInclusion::Source::store_tags)};
    row.steps = {
        Step{.rule = Rule::platform,
             .outcome = Outcome::noted,
             .product = product.id()},
        Step{.rule = Rule::premium_branding, .outcome = Outcome::passed},
        Step{.rule = Rule::premium_entitlement,
             .outcome = Outcome::passed,
             .ownership = offer->entitlement.id()},
        Step{.rule = Rule::included_with_plus,
             .outcome = Outcome::passed,
             .inclusion_source = PlusInclusion::Source::store_tags},
    };
    row.bundle = product.is_bundle();
    if (row.bundle) {
      for (const auto& child : product.streamable_children()) {
        const auto& id = *child.entitlement.id();
        row.children.push_back(
            BundledGame{.entitlement = id,
                        .product = id.as_product(),
                        .owned = owned_and_active_.contains(id)});
      }
      row.bundle_owned = product.is_owned_given(owned_and_active_);
    }
    const auto at = rows_.size();
    premium_products_.emplace(product.id(), at);
    if (const auto& id = offer->entitlement.id())
      premium_entitlements_.emplace(*id, at);
    for (const auto& child : row.children)
      premium_entitlements_.emplace(child.entitlement, at);
    row.offer = std::move(offer);
    rows_.push_back(std::move(row));
  }
}

void Games::offer_rows_for_sale(const ForSaleCategory& category,
                                std::span<const ProductId> used_trials) {
  auto taken = claimed();
  for (const auto& product : category.products()) {
    auto sale = product.offer_for_sale();
    if (!sale || is_claimed(product, taken)) continue;
    claim(product, taken);
    Row row{title_of(product, category.locale()),
            PlusInclusion::not_included(PlusInclusion::Source::store_tags)};
    row.steps = {
        Step{.rule = Rule::platform,
             .outcome = Outcome::noted,
             .product = product.id()},
        Step{.rule = Rule::storefront,
             .outcome = Outcome::passed,
             .product = product.id()},
        Step{.rule = Rule::counts_as_game,
             .outcome = Outcome::passed,
             .ownership = sale->entitlement.id()},
    };
    row.sale = std::move(sale);
    row.trial = trial_of(
        product, owned_and_active_,
        std::ranges::find(used_trials, product.id()) != used_trials.end());
    for_sale_products_.insert(product.id());
    rows_.push_back(std::move(row));
  }
}

auto Games::claimed() const -> Claimed {
  Claimed taken{.entitlements = owned_and_active_};
  for (const auto& row : rows_) {
    taken.products.insert(row.title.product);
    taken.products.insert(row.title.aliases.begin(), row.title.aliases.end());
    if (row.offer && row.offer->entitlement.id())
      taken.entitlements.insert(*row.offer->entitlement.id());
    for (const auto& child : row.children) {
      taken.entitlements.insert(child.entitlement);
      if (child.product) taken.products.insert(*child.product);
    }
    if (row.ownership) {
      taken.entitlements.insert(row.ownership->id());
      if (const auto& product = row.ownership->product_id())
        taken.products.insert(*product);
    }
  }
  return taken;
}

auto Games::is_claimed(const PremiumProduct& product, const Claimed& claimed)
    -> bool {
  return claimed.products.contains(product.id()) ||
         std::ranges::any_of(
             product.streamable_children(),
             [&claimed](const PremiumProduct::Offer& child) {
               return claimed.entitlements.contains(*child.entitlement.id());
             });
}

void Games::claim(const PremiumProduct& product, Claimed& claimed) {
  claimed.products.insert(product.id());
  for (const auto& child : product.streamable_children())
    claimed.entitlements.insert(*child.entitlement.id());
}

void Games::decline(const PremiumProduct& product) {
  const bool branded = std::ranges::any_of(
      product.skus(), &PremiumSku::is_premium_stream_branded);
  std::vector<Step> steps{
      Step{.rule = Rule::platform,
           .outcome = Outcome::noted,
           .product = product.id()},
      Step{.rule = Rule::premium_branding,
           .outcome = branded ? Outcome::passed : Outcome::failed},
  };
  if (branded)
    steps.push_back(
        Step{.rule = Rule::premium_entitlement, .outcome = Outcome::failed});
  steps.push_back(Step{.rule = Rule::included_with_plus,
                       .outcome = Outcome::failed,
                       .inclusion_source = PlusInclusion::Source::store_tags});
  declined_.push_back(Explanation::of(product.id(), std::move(steps)));
}

void Games::place_owned(const OwnedPs5Games& owned) {
  const auto full = full_games_of(owned);
  for (const auto& record : owned.records()) {
    const auto why = record.why_not_a_game();
    if (why && *why != NotAGame::play_first_trial) {
      exclude(record, *why);
      continue;
    }
    const auto title = why ? record.as_ended_trial() : record.as_owned_title();
    if (!title) continue;
    if (is_superseded(*title, full) || is_in_premium(*title)) {
      unlisted_.push_back(ServiceListing::superseded_trial(
          *title, title->id().as_product().value_or(*title->product_id())));
      continue;
    }
    if (const auto joined = premium_row_for(*title))
      join(*joined, *title);
    else
      list_owned(*title);
  }
}

auto Games::full_games_of(const OwnedPs5Games& owned) -> FullGames {
  FullGames full;
  for (const auto& title : owned.games()) {
    if (title.held_as() == HeldAs::trial_access) continue;
    if (const auto& product = title.product_id())
      full.products.insert(*product);
    if (const auto& concept_id = title.concept_id())
      full.concepts.insert(*concept_id);
  }
  return full;
}

auto Games::is_superseded(const OwnedTitle& owned, const FullGames& full)
    -> bool {
  if (owned.held_as() != HeldAs::trial_access) return false;
  const auto& product = owned.product_id();
  const auto& concept_id = owned.concept_id();
  return (product && full.products.contains(*product)) ||
         (concept_id && full.concepts.contains(*concept_id));
}

auto Games::is_in_premium(const OwnedTitle& owned) const -> bool {
  if (owned.held_as() != HeldAs::trial_access) return false;
  if (premium_row_for(owned)) return true;
  const auto& concept_id = owned.concept_id();
  const auto& product = owned.product_id();
  return std::ranges::any_of(rows_, [&](const Row& row) {
    if (!row.offer) return false;
    if (concept_id && row.title.concept_id == concept_id) return true;
    return product &&
           std::ranges::any_of(row.children, [&](const BundledGame& child) {
             return child.product == product;
           });
  });
}

void Games::exclude(const Ps5Entitlement& record, NotAGame why) {
  const auto& named = record.facts().product;
  const auto product = named ? named : record.id().as_product();
  if (!product) return;
  unlisted_.push_back(
      Explanation::of(*product, {Step{.rule = Rule::platform,
                                      .outcome = Outcome::noted,
                                      .product = product},
                                 Step{.rule = Rule::counts_as_game,
                                      .outcome = Outcome::failed,
                                      .not_a_game = why,
                                      .ownership = record.id()}}));
}

auto Games::premium_row_for(const OwnedTitle& owned) const
    -> std::optional<Join> {
  if (const auto found = premium_entitlements_.find(owned.id());
      found != premium_entitlements_.end())
    return Join{found->second, Rule::joined_by_entitlement};
  if (const auto& product = owned.product_id())
    if (const auto found = premium_products_.find(*product);
        found != premium_products_.end())
      return Join{found->second, Rule::joined_by_product};
  return std::nullopt;
}

void Games::join(Join joined, const OwnedTitle& owned) {
  auto& row = rows_[joined.first];
  row.steps.push_back(Step{.rule = joined.second,
                           .outcome = Outcome::passed,
                           .product = row.title.product,
                           .ownership = owned.id()});
  if (row.bundle) return;
  if (row.ownership) {
    row.steps.push_back(Step{.rule = Rule::owned_duplicate,
                             .outcome = Outcome::noted,
                             .ownership = owned.id()});
    return;
  }
  row.ownership = owned;
}

void Games::list_owned(const OwnedTitle& owned) {
  auto listed_as = *owned.product_id();
  if (const auto taken = owned_products_.find(listed_as);
      taken != owned_products_.end()) {
    const auto own = owned.id().as_product();
    if (!own || owned_products_.contains(*own) ||
        premium_products_.contains(*own)) {
      rows_[taken->second].steps.push_back(Step{.rule = Rule::owned_duplicate,
                                                .outcome = Outcome::noted,
                                                .ownership = owned.id()});
      return;
    }
    listed_as = *own;
  }
  Row row{title_of(owned, listed_as),
          PlusInclusion::not_included(PlusInclusion::Source::store_tags)};
  row.ownership = owned;
  row.steps = {
      Step{.rule = Rule::platform,
           .outcome = Outcome::noted,
           .product = listed_as},
      Step{.rule = Rule::included_with_plus,
           .outcome = Outcome::noted,
           .inclusion_source = PlusInclusion::Source::store_tags},
      Step{.rule = Rule::counts_as_game,
           .outcome = Outcome::passed,
           .ownership = owned.id()},
      Step{.rule = Rule::own_listing,
           .outcome = Outcome::passed,
           .ownership = owned.id()},
  };
  owned_products_.emplace(listed_as, rows_.size());
  rows_.push_back(std::move(row));
}

void Games::settle_bundles() {
  for (auto& row : rows_) {
    if (row.bundle)
      row.steps.push_back(
          Step{.rule = Rule::bundle_ownership,
               .outcome = row.bundle_owned ? Outcome::passed : Outcome::noted});
  }
}

void Games::claim_aliases() {
  std::set<ProductId> claimed;
  for (const auto& row : rows_) claimed.insert(row.title.product);
  for (auto& row : rows_) {
    for (const auto& child : row.children)
      if (child.product && claimed.insert(*child.product).second)
        row.title.aliases.push_back(*child.product);
    const auto held =
        row.ownership ? row.ownership->id().as_product() : std::nullopt;
    if (held && claimed.insert(*held).second)
      row.title.aliases.push_back(*held);
  }
}

void Games::remember(const RememberedRefusals& refusals) {
  for (auto& row : rows_) {
    const auto title = Title::from(row.title);
    if (!title) continue;
    row.refusal = refusals.standing_against(title.value(),
                                            listing_.holding_of(row.ownership));
    if (!row.refusal || !row.refusal->ends_the_trial()) continue;
    if (!row.ownership || row.ownership->held_as() != HeldAs::trial_access) {
      row.refusal.reset();
      continue;
    }
    row.ownership = row.ownership->as_ended_trial();
  }
}

auto Games::listed() const -> ServiceGames {
  ServiceGames games;
  for (const auto& row : rows_) {
    if (auto facts = facts_of(row))
      listing_.list(games, std::move(*facts), steps_of(row));
  }
  games.unlisted = unlisted_;
  for (const auto& declined : declined_) {
    const auto& product = declined.product_id();
    if (!premium_products_.contains(product) &&
        !owned_products_.contains(product) &&
        !for_sale_products_.contains(product))
      games.unlisted.push_back(declined);
  }
  return games;
}

auto Games::facts_of(const Row& row) const -> std::optional<MyGame::Facts> {
  auto title = Title::from(row.title);
  if (!title) return std::nullopt;
  MyGame::Facts facts{
      .title = std::move(title).value(),
      .inclusion = row.inclusion,
      .path = path_of(row),
      .visibility = streaming_hides_tiles()
                        ? Visibility::hidden(HideReason::tier_does_not_include)
                        : Visibility::visible(),
      .launch_identifier = launch_of(row),
  };
  facts.ownership = row.ownership;
  facts.refusal = row.refusal;
  if (!row.ownership && row.offer) facts.library_offer = offer_of(*row.offer);
  facts.children = row.children;
  facts.bundle_owned = row.bundle_owned;
  facts.trial_offer = row.trial;
  return facts;
}

auto Games::steps_of(const Row& row) const -> std::vector<Step> {
  std::vector<Step> steps{row.steps.front()};
  if (streaming_hides_tiles())
    steps.push_back(Step{.rule = Rule::tier,
                         .outcome = Outcome::failed,
                         .hides = HideReason::tier_does_not_include,
                         .tier = listing_.inputs().tier});
  steps.insert(steps.end(), row.steps.begin() + 1, row.steps.end());
  steps.push_back(Step{.rule = Rule::access_route,
                       .outcome = Outcome::noted,
                       .path = path_of(row)});
  steps.push_back(ownership_step(row));
  if (streaming_is_allowed()) listing_.pass_the_tier(steps);
  listing_.note_refusal_and_release(steps, row.refusal, row.ownership);
  return steps;
}

auto Games::ownership_step(const Row& row) const -> Step {
  if (row.ownership)
    return Step{.rule = Rule::ownership,
                .outcome = Outcome::passed,
                .ownership = row.ownership->id()};
  return Step{.rule = Rule::ownership,
              .outcome = listing_.library_is_known() ? Outcome::noted
                                                     : Outcome::unchecked};
}

auto Games::streaming_hides_tiles() const noexcept -> bool {
  return streaming_ && streaming_->hides_tiles();
}

auto Games::streaming_is_allowed() const noexcept -> bool {
  return streaming_ && streaming_->is_allowed();
}

auto Games::path_of(const Row& row) noexcept -> PlayPath {
  if (row.ownership) return PlayPath::owned;
  return row.sale ? PlayPath::store_only : PlayPath::included;
}

auto Games::launch_of(const Row& row) -> LaunchIdentifier {
  if (row.ownership) return LaunchIdentifier::of(row.ownership->id());
  if (row.offer && row.offer->entitlement.id())
    return LaunchIdentifier::of(*row.offer->entitlement.id());
  if (row.sale && row.sale->entitlement.id())
    return LaunchIdentifier::of(*row.sale->entitlement.id());
  return LaunchIdentifier::of(row.title.product);
}

auto Games::title_of(const PremiumProduct& product, const StoreLocale& locale)
    -> Title::Facts {
  const auto& listed = product.facts();
  Title::Facts facts{.product = listed.id};
  facts.concept_page = StorePage::of_product(locale, listed.id);
  facts.name = text::trim(listed.name).empty() ? listed.id.text() : listed.name;
  facts.platform = Platform::ps5;
  facts.service = ServiceGeneration::ps5_cloud;
  facts.streamability = ListStreamability::listed;
  facts.concept_id = listed.concept_id;
  facts.cover_url = listed.master_image_url;
  return facts;
}

auto Games::title_of(const OwnedTitle& owned, const ProductId& listed_as)
    -> Title::Facts {
  Title::Facts facts{.product = listed_as};
  facts.name =
      text::trim(owned.name()).empty() ? listed_as.text() : owned.name();
  facts.platform = Platform::ps5;
  facts.service = ServiceGeneration::ps5_cloud;
  facts.streamability = ListStreamability::listed;
  facts.concept_id = owned.concept_id();
  facts.cover_url = owned.cover_url();
  return facts;
}

auto Games::offer_of(const PremiumProduct::Offer& offer)
    -> StorefrontAnswer::LibraryOffer {
  StorefrontAnswer::LibraryOffer library_offer{
      .sku_id = offer.sku ? offer.sku->text() : std::string{},
      .entitled_through_subscription = true};
  if (const auto& id = offer.entitlement.id())
    library_offer.entitlement_ids.push_back(id->text());
  return library_offer;
}

auto Games::trial_of(const PremiumProduct& product,
                     const std::set<OwnershipId>& owned_and_active,
                     bool trial_used) -> TrialOffer {
  using Feature = PremiumEntitlement::Feature;
  if (!trial_used &&
      product.offers(Feature::play_first_trial, owned_and_active))
    return TrialOffer::play_first_trial;
  if (product.offers(Feature::trial, owned_and_active)) return TrialOffer::demo;
  return TrialOffer::none;
}

}  // namespace libchiaking::catalog::cloud_stream
