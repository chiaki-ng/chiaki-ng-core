#include "src/core/catalog/model/cloud_stream/generation_links.hpp"

namespace libchiaking::catalog::cloud_stream {

void GenerationLinks::add(std::span<const PremiumProduct> products) {
  for (const auto& product : products) {
    const auto ps5 = product.id().title_number();
    if (!ps5 || ps5->platform() != Platform::ps5) continue;
    for (const auto& sku : product.skus())
      for (const auto& entitlement : sku.entitlements()) {
        if (!entitlement.id()) continue;
        const auto ps4 = entitlement.id()->title_number();
        if (!ps4 || ps4->platform() != Platform::ps4) continue;
        if (ps5_of_ps4_.contains(*ps4) || ps4_of_ps5_.contains(*ps5)) continue;
        ps5_of_ps4_.emplace(*ps4, *ps5);
        ps4_of_ps5_.emplace(*ps5, *ps4);
      }
  }
}

auto GenerationLinks::other_of(const TitleNumber& title) const
    -> std::optional<TitleNumber> {
  const auto& links =
      title.platform() == Platform::ps4 ? ps5_of_ps4_ : ps4_of_ps5_;
  const auto found = links.find(title);
  if (found == links.end()) return std::nullopt;
  return found->second;
}

}  // namespace libchiaking::catalog::cloud_stream
