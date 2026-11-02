/// @file
/// @brief The GraphQL persisted queries sent anonymously to the web gateway,
/// as the PlayStation Store web site sends them.
///
/// Their query documents are kept in `public_web/queries/<name>.gql`, and a
/// test checks that each hashes to its operation's `sha256`.
#pragma once

#include "src/core/psn/graphql/operation.hpp"

#include <array>
#include <string_view>

namespace libchiaking::psn::graphql::public_web {

/// @brief The gateway these operations are sent to, the web gateway.
inline constexpr std::string_view endpoint = web_endpoint;
/// @brief Where these operations' query documents live, relative to the GraphQL
/// folder.
inline constexpr std::string_view queries = "public_web/queries";

/// @brief `categoryGridRetrieve`: one page of a store category's products with
/// their PlayStation Plus branding.
inline constexpr Operation category_grid_retrieve{
    .name = "categoryGridRetrieve",
    .sha256 =
        "88c0b9a1273c6d320c51cd73e390924e21ae28bf09f01cde8b84b1034b16cd03",
    .endpoint = endpoint,
    .label = "psn.graphql.web.categoryGridRetrieve",
    .auth = Auth::none,
    .version = 1,
};

/// @brief `conceptRetrieveForMedia`: a concept's and its default product's
/// media.
inline constexpr Operation concept_media{
    .name = "conceptRetrieveForMedia",
    .sha256 =
        "615a2c4618229aa2f11c10fe497eaf4fdc151e4dcc0b6b82e154aeacb0123c2d",
    .endpoint = endpoint,
    .label = "psn.graphql.web.conceptRetrieveForMedia",
    .auth = Auth::none,
    .version = 1,
};

/// @brief `wcaConceptStarRatingRetrive` (PS's spelling): a concept's star
/// rating.
inline constexpr Operation concept_star_rating{
    .name = "wcaConceptStarRatingRetrive",
    .sha256 =
        "6c476325b232d51aca55ce143d6f946860e22174a263967fbb9e1da4f78489fa",
    .endpoint = endpoint,
    .label = "psn.graphql.web.wcaConceptStarRatingRetrive",
    .auth = Auth::none,
    .version = 1,
};

/// @brief `conceptRetrieveForContentRating`: a concept's age rating.
inline constexpr Operation concept_content_rating{
    .name = "conceptRetrieveForContentRating",
    .sha256 =
        "b504e0bc68af3dc08bc56c0001b27da26ed15d70827420f80805b2d031a95aa8",
    .endpoint = endpoint,
    .label = "psn.graphql.web.conceptRetrieveForContentRating",
    .auth = Auth::none,
    .version = 1,
};

/// @brief `conceptRetrieveForGameInfo`: a concept's publisher, release date and
/// genres.
inline constexpr Operation concept_game_info{
    .name = "conceptRetrieveForGameInfo",
    .sha256 =
        "156bf37e6d6091b4d584ebf5f430a65e818b6120525dd82a0745352d21619da6",
    .endpoint = endpoint,
    .label = "psn.graphql.web.conceptRetrieveForGameInfo",
    .auth = Auth::none,
    .version = 1,
};

/// @brief Every anonymous operation.
inline constexpr std::array operations{
    category_grid_retrieve, concept_media, concept_star_rating,
    concept_content_rating, concept_game_info};

}  // namespace libchiaking::psn::graphql::public_web
