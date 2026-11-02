/// @file
/// @brief Declares the user's active PlayStation Plus subscriptions as the
/// `getSubscriptions` GraphQL operation reports them.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace libchiaking::psn::graphql {

/// @brief One active PlayStation Plus subscription. Plain data.
struct ActivePlusSubscription {
  /// @brief The `tierLabel`: `TIER_10`, `TIER_20` or `TIER_30`.
  std::string tier_label;
  /// @brief The subscription's `id`, its SKU, which names the tier as well.
  std::string sku_id;
  /// @brief The `productName`; empty when absent.
  std::string product_name;
  /// @brief The `endDate` as PS wrote it; empty when absent.
  std::string end_date;
};

/// @brief The user's active PlayStation Plus subscriptions. Plain data.
struct PlusSubscriptions {
  /// @brief The active ones, in reply order; empty when none.
  std::vector<ActivePlusSubscription> active;
  /// @brief The reply's status.
  std::uint16_t http_status{};
  /// @brief HTTP attempts made.
  int attempts{};
};

}  // namespace libchiaking::psn::graphql
