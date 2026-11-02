/// @file
/// @brief Declares ps::CloudGameResolver, the adapter for PS's
/// selected-title resolver that a PS5 launch awaits.
///
/// GET /api/catalog/v2/products/cloudgame with the row's product, the account's
/// country and language, the user's plan ids and a bearer token. Its answer
/// never replaces the owned entitlement; a failed request refuses the launch.
#pragma once

#include "src/core/catalog/model/cloud_stream/cloud_game_listing.hpp"
#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/rest/store/cloud_game_products.hpp"
#include "src/core/users/cloud_streaming.hpp"

#include <libchiaking/detail/catalog/owned_title.hpp>
#include <libchiaking/detail/catalog/product_id.hpp>
#include <libchiaking/detail/users/ps_user.hpp>

#include <cstddef>
#include <span>
#include <string_view>

namespace libchiaking::catalog::ps {

/// @brief Asks the resolver about one product for one user.
///
/// Borrows the user and the HTTP client, which must outlive it. Used on the
/// thread that runs the launch check.
class CloudGameResolver {
 public:
  /// @brief Sets up the resolver for a user.
  ///
  /// @param user The signed-in user; supplies the token and account locale.
  ///     Must outlive the resolver.
  /// @param http Sends the request. Must outlive the resolver.
  /// @param maximum_response_bytes The largest reply accepted.
  CloudGameResolver(users::PsUser& user, net::http::Client& http,
                    std::size_t maximum_response_bytes =
                        net::http::default_maximum_response_bytes) noexcept;

  /// @brief Asks which titles the resolver lists for a product.
  ///
  /// Blocks on PS's reply; the request is bounded by the HTTP provider's
  /// timeouts and retries and abandoned when the client's stop token fires.
  ///
  /// @param product The product to ask about; a bundle child asks with its
  ///     bundle's product.
  /// @param plans The user's benefit plan ids.
  /// @return Not asked when there are no plan ids or the account has no country
  ///     or language; failed with the request's failure; not listed when PS
  ///     listed no product; listed with the first product otherwise.
  auto for_product(const ProductId& product,
                   std::span<const users::BenefitPlanId> plans)
      -> cloud_stream::CloudGameListing;

 private:
  /// @brief Asks the resolver one question.
  ///
  /// @param asking What the id names.
  /// @param id The id asked about.
  /// @param plans The user's benefit plan ids.
  /// @return As for_product.
  auto resolved(psn::rest::CloudGameQuestion::Asking asking,
                std::string_view id,
                std::span<const users::BenefitPlanId> plans)
      -> cloud_stream::CloudGameListing;
  /// @brief Reads the first listed product.
  ///
  /// @param listed The product as PS sent it.
  /// @return The product with its titles; ids that do not parse are left empty.
  static auto product_of(const psn::rest::CloudGameProduct& listed)
      -> cloud_stream::CloudGameListing::Product;

  /// @brief The user asking; not owned.
  users::PsUser& user_;
  /// @brief Sends requests; not owned.
  net::http::Client& http_;
  /// @brief The largest reply accepted, in bytes.
  std::size_t maximum_response_bytes_;
};

}  // namespace libchiaking::catalog::ps
