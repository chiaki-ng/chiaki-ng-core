/// @file
/// @brief The GraphQL persisted queries sent with the app token.
///
/// Their query documents are kept in `authenticated/queries/<name>.gql`, and a
/// test checks that each hashes to its operation's `sha256`.
#pragma once

#include "src/core/psn/graphql/operation.hpp"

#include <array>
#include <string_view>

namespace libchiaking::psn::graphql::authenticated {

/// @brief The gateway these operations are sent to by default, the app gateway.
inline constexpr std::string_view endpoint = app_endpoint;
/// @brief Where these operations' query documents live, relative to the GraphQL
/// folder.
inline constexpr std::string_view queries = "authenticated/queries";

/// @brief `metGetConceptById`: a concept's summary, default product and media,
/// by concept id.
inline constexpr Operation concept_by_id{
    .name = "metGetConceptById",
    .sha256 =
        "84b113e0c67e763f2f8bb2dbfc8f3d6cbc116d4c54ee33bf5cc42204ffde0feb",
    .endpoint = endpoint,
    .label = "psn.graphql.mobile.metGetConceptById",
    .auth = Auth::required,
    .version = 1,
};

/// @brief `metGetPricingDataByConceptId`: a concept's purchasable products,
/// their offers and the user's ownership.
inline constexpr Operation concept_pricing{
    .name = "metGetPricingDataByConceptId",
    .sha256 =
        "10ad417e6cb1408046769d041c526e0d1bd95d2e77e2050c00baccf65a181c25",
    .endpoint = endpoint,
    .label = "psn.graphql.mobile.metGetPricingDataByConceptId",
    .auth = Auth::required,
    .version = 1,
};

/// @brief `metGetConceptByProductIdQuery`: a product's concept, the concept's
/// products and ownership, and the product's offers.
inline constexpr Operation concept_by_product{
    .name = "metGetConceptByProductIdQuery",
    .sha256 =
        "e9ff5930be8bf1dc5c1e6c1c7edc615503fef5b75bf3cd7b3f24b34f88ee7141",
    .endpoint = endpoint,
    .label = "psn.graphql.mobile.metGetConceptByProductIdQuery",
    .auth = Auth::required,
    .version = 1,
};

/// @brief `getSubscriptions`: the user's subscriptions, sent to the web
/// gateway.
inline constexpr Operation get_subscriptions{
    .name = "getSubscriptions",
    .sha256 =
        "36c5aa5049115a8b37008e45815c3cd31414d52660cd94a45fcdb17294800db1",
    .endpoint = web_endpoint,
    .label = "psn.graphql.web.getSubscriptions",
    .auth = Auth::required,
    .version = 1,
};

/// @brief Every authenticated operation.
inline constexpr std::array operations{concept_by_id, concept_pricing,
                                       concept_by_product, get_subscriptions};

}  // namespace libchiaking::psn::graphql::authenticated
