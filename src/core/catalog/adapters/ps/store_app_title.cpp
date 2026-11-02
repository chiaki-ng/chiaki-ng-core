#include "src/core/catalog/adapters/ps/store_app_title.hpp"

#include "src/core/users/ports/catalog_calls.hpp"
#include "src/core/utils/retry/with_renewal.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace libchiaking::catalog::ps {

StoreAppTitle::StoreAppTitle(users::PsUser& user, net::http::Client& http,
                             const platform::ClockProvider& wall) noexcept
    : user_(user), http_(http), stamp_(wall) {}

auto StoreAppTitle::answer_for(const ProductId& product)
    -> Expected<StorefrontAnswer, Unanswered> {
  const auto found =
      asked_of_the_store([&](psn::graphql::AuthenticatedClient& store) {
        return store.concept_by_product(product.without_sku().text(),
                                        account_store);
      });
  if (!found) return unexpected(found.error());
  return answered(product, found.value());
}

template <class Ask>
auto StoreAppTitle::asked_of_the_store(const Ask& ask)
    -> Expected<psn::graphql::TitleDetails, Unanswered> {
  auto opened = users::CatalogCalls{user_}.store_app(http_);
  if (!opened) return unexpected(Unanswered{opened.error()});
  std::optional<psn::graphql::AuthenticatedClient> store{
      std::move(opened.value())};
  auto renewed = retry::with_renewal(
      [&] { return ask(*store); },
      [](const psn::graphql::GraphqlError& refused) {
        return refused.kind() == psn::graphql::GraphqlError::Kind::auth_refused;
      },
      [&](const psn::graphql::GraphqlError& refused)
          -> Expected<void, Unanswered> {
        auto again = users::CatalogCalls{user_}.store_app(http_);
        if (!again)
          return unexpected(Unanswered{
              users::Unanswered{again.error().reason, refused.http_status()}});
        store.emplace(std::move(again.value()));
        return {};
      });
  if (!renewed) return unexpected(renewed.error());
  return std::move(renewed.value())
      .transform_error([](const psn::graphql::GraphqlError& error) {
        return unanswered(error);
      });
}

auto StoreAppTitle::answer_for(const ProductId& product,
                               const ConceptId& grouped_under)
    -> Expected<StorefrontAnswer, Unanswered> {
  const auto priced = asked_of_the_store(
      [&](psn::graphql::AuthenticatedClient& store)
          -> Expected<psn::graphql::TitleDetails, psn::graphql::GraphqlError> {
        auto found = store.concept_by_id(grouped_under.text(), account_store);
        if (!found) return unexpected(std::move(found.error()));
        return store.concept_pricing(std::move(found.value()), account_store);
      });
  if (!priced) return unexpected(priced.error());
  if (const auto edition = same_title(product, priced.value())) {
    http_.log(platform::LogSeverity::info,
              "[STORE] " + product.text() + " is answered by " +
                  edition->text() + " of the same title");
    return answered(*edition, priced.value());
  }
  return answered(product, priced.value());
}

auto StoreAppTitle::same_title(const ProductId& product,
                               const psn::graphql::TitleDetails& details)
    -> std::optional<ProductId> {
  const auto& asked = product.text();
  const auto first = asked.find('-');
  const auto second =
      first == std::string::npos ? first : asked.find('-', first + 1);
  if (second == std::string::npos) return std::nullopt;
  const auto title = std::string_view{asked}.substr(0, second + 1);
  std::optional<ProductId> first_listed;
  std::optional<ProductId> first_stating;
  for (const auto& edition : details.products()) {
    const auto id = ProductId::parse(edition.id);
    if (!id) continue;
    if (product.is_answered_by(*id)) return std::nullopt;
    if (!id->text().starts_with(title)) continue;
    if (!first_stating && edition.streaming.notices) first_stating = *id;
    if (!first_listed) first_listed = *id;
  }
  return first_stating ? first_stating : first_listed;
}

auto StoreAppTitle::answered(const ProductId& product,
                             const psn::graphql::TitleDetails& details) const
    -> StorefrontAnswer {
  note_offers(details);
  std::vector<StorefrontAnswer::Product> editions;
  for (const auto& edition : details.products()) {
    auto id = ProductId::parse(edition.id);
    if (!id) continue;
    StorefrontAnswer::Product told{std::move(*id)};
    if (edition.streaming.notices)
      told.streaming_supported = edition.streaming.supported;
    told.notice = edition.streaming_notice;
    told.owned = edition.owned;
    const auto offer = std::ranges::find(edition.offers, library_offer_action,
                                         &psn::graphql::TitleOffer::cta_type);
    if (offer != edition.offers.end())
      told.library_offer =
          StorefrontAnswer::LibraryOffer{offer->sku_id, offer->entitlement_ids,
                                         offer->entitled_via_subscription};
    told.offers_trial = std::ranges::any_of(
        edition.offers, [](const psn::graphql::TitleOffer& made) {
          return std::ranges::find(trial_actions, made.cta_type) !=
                 trial_actions.end();
        });
    editions.push_back(std::move(told));
  }
  return StorefrontAnswer::of(user_.account_id(), product, editions,
                              stamp_.now());
}

void StoreAppTitle::note_offers(
    const psn::graphql::TitleDetails& details) const {
  for (const auto& edition : details.products()) {
    std::string offers;
    for (const auto& offer : edition.offers) {
      offers += offers.empty() ? "" : "; ";
      offers += offer.cta_type + " " + offer.action_type;
      if (!offer.sku_id.empty()) offers += " sku=" + offer.sku_id;
      for (const auto& reason : offer.ineligibility_reasons)
        offers += " ineligible=" + reason;
    }
    http_.log(platform::LogSeverity::info,
              "[STORE] offers for " + edition.id + ": " +
                  (offers.empty() ? std::string{"none"} : offers));
  }
}

auto StoreAppTitle::unanswered(const psn::graphql::GraphqlError& error) noexcept
    -> Unanswered {
  using Kind = psn::graphql::GraphqlError::Kind;
  using Reason = Unanswered::Reason;
  const auto status = error.http_status();
  switch (error.kind()) {
    case Kind::cancelled:
      return Unanswered::cancelled();
    case Kind::auth_refused:
    case Kind::missing_credential:
      return Unanswered{users::Unanswered{
          users::Unanswered::Reason::store_access_lapsed, status}};
    case Kind::partial:
    case Kind::invalid_json:
    case Kind::malformed:
      return Unanswered{Reason::malformed, status};
    case Kind::not_found:
      return Unanswered{Reason::not_listed, status};
    case Kind::invalid_request:
    case Kind::errors:
    case Kind::rejected:
      return Unanswered{Reason::rejected, status};
    case Kind::transport:
    case Kind::server_error:
      break;
  }
  return Unanswered{Reason::unavailable, status};
}

}  // namespace libchiaking::catalog::ps
