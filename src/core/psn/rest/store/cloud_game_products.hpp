/// @file
/// @brief Declares CloudGameProducts, the store's cloud game resolver that a
/// PS5 cloud launch consults for the selected game.
///
/// Given a product or entitlement and the user's PlayStation Plus benefit
/// plans, the resolver lists what the store would stream for it on a PS5
/// storefront.
#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/auth/credentials.hpp"
#include "src/core/psn/rest/error.hpp"

#include <libchiaking/detail/expected.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::psn::rest {

/// @brief What the resolver is asked about. Plain data; the views and span must
/// outlive the request.
struct CloudGameQuestion {
  /// @brief Which kind of id the question names.
  enum class Asking : std::uint8_t {
    /// @brief A store product id, sent as `productId`.
    product_id,
    /// @brief An entitlement id, sent as `entitlementId`.
    entitlement_id,
  };

  /// @brief Which kind of id `id` is.
  Asking asking{};
  /// @brief The product or entitlement id.
  std::string_view id;
  /// @brief The store country.
  std::string_view country;
  /// @brief The store language.
  std::string_view language;
  /// @brief The user's cloud streaming benefit plan ids, from the
  /// subscriptions service's cloud game feature; sent comma-separated, each
  /// percent-encoded.
  std::span<const std::string> benefit_plan_ids;
};

/// @brief A title the resolver lists under a product. Plain data.
struct CloudGameTitle {
  /// @brief The `titleId`; empty when absent or malformed.
  std::string title_id;
  /// @brief The `entitlementId`; empty when absent or malformed.
  std::string entitlement_id;
  /// @brief The `nativePlatform`, such as `PS5`; empty when absent.
  std::string native_platform;
  /// @brief The `isDownloadable` flag; nothing when absent.
  std::optional<bool> is_downloadable;
  /// @brief The `gameIntent`; nothing when absent.
  std::optional<std::string> game_intent;
};

/// @brief A product the resolver lists. Plain data.
struct CloudGameProduct {
  /// @brief The `productId`; empty when absent or malformed.
  std::string product_id;
  /// @brief Its `titles`, in reply order; nothing when absent.
  std::optional<std::vector<CloudGameTitle>> titles;
};

/// @brief The resolver's answer. Plain data.
struct CloudGameReply {
  /// @brief The reply's status, a 2xx.
  std::uint16_t http_status{};
  /// @brief The first entry of `productInfo`; nothing when the resolver
  /// listed nothing, which means the game is not listed.
  std::optional<CloudGameProduct> first;
};

/// @brief Asks the cloud game resolver with the app token.
///
/// Holds a reference to the HTTP client, which must outlive it, and the token.
/// Each call blocks for one round trip. Not for concurrent use.
class CloudGameProducts {
 public:
  /// @brief The failure of first_listed().
  using FirstListedError =
      Error<bad_reply::InvalidJson, bad_reply::InvalidSchema>;
  /// @brief The request's diagnostic label.
  static constexpr std::string_view label = "psn.store.cloud_game_products";
  /// @brief The age sent, 99, so no content is filtered out.
  static constexpr int age = 99;
  /// @brief The most products asked for.
  static constexpr int limit = 1000;

  /// @brief Prepares the request.
  ///
  /// @param client The client the request goes through.
  /// @param token The app token.
  CloudGameProducts(net::http::Client& client,
                    auth::BearerToken token) noexcept;

  /// @brief Asks the resolver and reads the first product it lists.
  ///
  /// GET
  /// `https://m.np.playstation.com/api/catalog/v2/products/cloudgame?productId|entitlementId=<id>&country&language&age=99&limit=1000&benefitPlanIds=<plans>&homeshare=false&storeFront=PS5`;
  /// only the first `productInfo` entry is decoded, its fields tolerated when
  /// malformed.
  ///
  /// @param question What to ask.
  /// @return The status and the first product, if any.
  /// @retval bad_reply::InvalidSchema The first entry is not a product.
  /// @retval Error A transport failure, an error status or an unreadable
  ///     reply.
  [[nodiscard]] auto first_listed(const CloudGameQuestion& question)
      -> Expected<CloudGameReply, FirstListedError>;

 private:
  /// @brief The resolver endpoint.
  static constexpr std::string_view url =
      "https://m.np.playstation.com/api/catalog/v2/products/cloudgame";
  /// @brief The request, logged with its query.
  static constexpr net::http::Endpoint endpoint{.label = label,
                                                .log_query = true};

  /// @brief The reply's `productInfo`, each entry kept as raw JSON.
  struct Reply;
  /// @brief A product entry's fields.
  struct ListedProduct;
  /// @brief A title entry's fields.
  struct ListedTitle;

  /// @brief Builds the query.
  ///
  /// @param question What to ask.
  /// @return The resolver URL with its query.
  static auto query_of(const CloudGameQuestion& question) -> net::http::Query;
  /// @brief Converts a product entry.
  ///
  /// @param listed The entry, consumed.
  /// @return The product.
  static auto product_of(ListedProduct listed) -> CloudGameProduct;
  /// @brief Converts a title entry.
  ///
  /// @param listed The entry, consumed.
  /// @return The title.
  static auto title_of(ListedTitle listed) -> CloudGameTitle;

  /// @brief The client the request goes through.
  net::http::Client& client_;
  /// @brief The app token.
  auth::BearerToken token_;
};

}  // namespace libchiaking::psn::rest
