#include "src/core/catalog/model/cloud_stream/for_sale_category.hpp"

#include <algorithm>
#include <utility>

namespace libchiaking::catalog::cloud_stream {

ForSaleCategory::ForSaleCategory(StoreLocale locale,
                                 std::vector<PremiumProduct> products,
                                 Scan scan, Time fetched_at) noexcept
    : locale_(std::move(locale)),
      products_(std::move(products)),
      scan_(scan),
      fetched_at_(fetched_at) {}

auto ForSaleCategory::for_sale_count() const -> std::size_t {
  return static_cast<std::size_t>(
      std::ranges::count_if(products_, [](const PremiumProduct& product) {
        return product.offer_for_sale().has_value();
      }));
}

auto ForSaleCategory::is_plausible() const -> bool {
  return is_complete() && for_sale_count() > 0;
}

}  // namespace libchiaking::catalog::cloud_stream
