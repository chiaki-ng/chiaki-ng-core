/// @file
/// @brief Declares cloud_stream::ForSaleCategory, the public Store streaming
/// category that unowned purchase-only PS5 tiles come from.
///
/// The category is asked anonymously, held per locale and age, and only ever
/// produces "for sale" tiles. Its membership never grants access.
#pragma once

#include "src/core/catalog/model/cloud_stream/premium_category.hpp"

#include <libchiaking/detail/catalog/store_locale.hpp>

#include <chrono>
#include <cstddef>
#include <vector>

namespace libchiaking::catalog::cloud_stream {

/// @brief One scan of the public Store streaming category.
///
/// Products use the PremiumProduct model, but only offer_for_sale is read
/// from them. Immutable; held behind a shared pointer to const and read from
/// several threads.
class ForSaleCategory {
 public:
  /// @brief Wall-clock time in milliseconds since the Unix epoch.
  using Time = std::chrono::sys_time<std::chrono::milliseconds>;
  /// @brief How the scan went.
  using Scan = CategoryScan;

  /// @brief Keeps one scan of the category.
  ///
  /// @param locale The country and language it was asked with.
  /// @param products The products kept, in server order.
  /// @param scan How the scan went.
  /// @param fetched_at When the scan finished.
  ForSaleCategory(StoreLocale locale, std::vector<PremiumProduct> products,
                  Scan scan, Time fetched_at) noexcept;

  /// @brief The country and language the category was asked with.
  ///
  /// @return The locale, valid while the category lives.
  auto locale() const noexcept -> const StoreLocale& { return locale_; }
  /// @brief The products kept.
  ///
  /// @return The products in server order, valid while the category lives.
  auto products() const noexcept -> const std::vector<PremiumProduct>& {
    return products_;
  }
  /// @brief How the scan went.
  ///
  /// @return The scan, valid while the category lives.
  auto scan() const noexcept -> const Scan& { return scan_; }
  /// @brief Whether the scan reached the end of the category.
  ///
  /// @return The scan's complete flag.
  auto is_complete() const noexcept -> bool { return scan_.complete; }
  /// @brief When the scan finished.
  ///
  /// @return Wall-clock time in milliseconds.
  auto fetched_at() const noexcept -> Time { return fetched_at_; }
  /// @brief How many products have an entitlement that streams.
  ///
  /// @return The number of products with an offer_for_sale.
  auto for_sale_count() const -> std::size_t;
  /// @brief Whether the scan looks like a real answer worth keeping.
  ///
  /// @return True when complete and at least one product is for sale.
  auto is_plausible() const -> bool;

  /// @brief Compares every field.
  friend auto operator==(const ForSaleCategory&, const ForSaleCategory&)
      -> bool = default;

 private:
  /// @brief The country and language asked with.
  StoreLocale locale_;
  /// @brief The products kept, in server order.
  std::vector<PremiumProduct> products_;
  /// @brief How the scan went.
  Scan scan_;
  /// @brief When the scan finished; wall-clock milliseconds.
  Time fetched_at_;
};

}  // namespace libchiaking::catalog::cloud_stream
