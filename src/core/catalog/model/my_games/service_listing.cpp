#include "src/core/catalog/model/my_games/service_listing.hpp"

#include <utility>

namespace libchiaking::catalog {

auto ServiceListing::library_is_known() const noexcept -> bool {
  return service_ == ServiceGeneration::ps5_cloud
             ? inputs_.ps5_library == LibraryState::fetched
             : inputs_.library == LibraryState::fetched;
}

auto ServiceListing::below_the_streaming_tier() const noexcept -> bool {
  return inputs_.tier && *inputs_.tier < streaming_tier;
}

auto ServiceListing::holding_of(const std::optional<OwnedTitle>& ownership)
    const -> StreamRefusal::Holding {
  return StreamRefusal::Holding::of(ownership, library_is_known());
}

auto ServiceListing::opening(const Title& title) const -> std::vector<Step> {
  return {Step{.rule = Rule::platform,
               .outcome = Outcome::noted,
               .product = title.product_id()}};
}

auto ServiceListing::refused_by_tier(const Title& title,
                                     std::optional<HideReason> hides,
                                     std::span<const Step> matched) const
    -> std::vector<Step> {
  auto steps = opening(title);
  steps.push_back(Step{.rule = Rule::tier,
                       .outcome = Outcome::failed,
                       .hides = hides,
                       .tier = inputs_.tier});
  steps.insert(steps.end(), matched.begin(), matched.end());
  return steps;
}

void ServiceListing::close(std::vector<Step>& steps,
                           const std::optional<StreamRefusal>& refusal,
                           const std::optional<OwnedTitle>& ownership) const {
  if (inputs_.tier) pass_the_tier(steps);
  note_refusal_and_release(steps, refusal, ownership);
}

void ServiceListing::pass_the_tier(std::vector<Step>& steps) const {
  steps.push_back(Step{
      .rule = Rule::tier, .outcome = Outcome::passed, .tier = inputs_.tier});
}

void ServiceListing::note_refusal_and_release(
    std::vector<Step>& steps, const std::optional<StreamRefusal>& refusal,
    const std::optional<OwnedTitle>& ownership) const {
  if (refusal)
    steps.push_back(
        Step{.rule = Rule::remembered_refusal, .outcome = Outcome::noted});
  if (ownership && !ownership->release().is_unrestricted())
    steps.push_back(Step{.rule = Rule::release, .outcome = Outcome::noted});
}

void ServiceListing::list(ServiceGames& games, MyGame::Facts facts,
                          std::vector<Step> steps) const {
  facts.ownership_checked = library_is_known() || facts.ownership.has_value();
  facts.title = with_store_page(std::move(facts.title));
  const auto product = facts.title.product_id();
  auto game = MyGame::from(std::move(facts));
  if (!game) return;
  games.listed.push_back(
      {std::move(game).value(), Explanation::of(product, std::move(steps))});
}

auto ServiceListing::with_store_page(Title title) const -> Title {
  if (title.concept_page()) return title;
  auto facts = title.facts();
  facts.concept_page = StorePage::of_product(store_, title.product_id());
  auto paged = Title::from(std::move(facts));
  return paged ? std::move(paged).value() : title;
}

auto ServiceListing::superseded(const OwnedTitle& owned,
                                const std::optional<ProductId>& product,
                                const std::set<ProductId>& fully_owned)
    -> std::optional<Explanation> {
  if (owned.held_as() != HeldAs::trial_access || !product ||
      !fully_owned.contains(*product))
    return std::nullopt;
  return superseded_trial(owned, *product);
}

auto ServiceListing::superseded_trial(const OwnedTitle& owned,
                                      const ProductId& product) -> Explanation {
  return Explanation::of(product, {Step{.rule = Rule::trial_superseded,
                                        .outcome = Outcome::failed,
                                        .product = product,
                                        .ownership = owned.id()}});
}

auto ServiceListing::lent_facts(const Title* lender, const OwnedTitle& owned,
                                ProductId product) -> Title::Facts {
  Title::Facts facts{.product = std::move(product)};
  facts.concept_id = lender != nullptr && lender->concept_id()
                         ? lender->concept_id()
                         : owned.concept_id();
  facts.cover_url = lender != nullptr && !lender->cover_url().empty()
                        ? lender->cover_url()
                        : owned.cover_url();
  if (lender != nullptr) {
    facts.landscape_cover_url = lender->landscape_cover_url();
    facts.concept_page = lender->concept_page();
  }
  return facts;
}

}  // namespace libchiaking::catalog
