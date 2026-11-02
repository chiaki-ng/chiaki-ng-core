#include "src/core/catalog/adapters/cache/held_storefront.hpp"

namespace libchiaking::catalog::cache {

auto HeldStorefront::fresh(AccountId account, const ProductId& product,
                           Time now) const -> std::optional<StorefrontAnswer> {
  const auto held = answers_.find(account.value());
  if (held == answers_.end()) return std::nullopt;
  const auto found = held->second.find(product);
  if (found == held->second.end() || found->second.account() != account ||
      !found->second.is_fresh_at(now))
    return std::nullopt;
  return found->second;
}

void HeldStorefront::keep(AccountId account, const ProductId& product,
                          const StorefrontAnswer& answer) {
  answers_[account.value()].insert_or_assign(product, answer);
}

void HeldStorefront::drop(AccountId account, const ProductId& product) {
  const auto held = answers_.find(account.value());
  if (held != answers_.end()) held->second.erase(product);
}

void HeldStorefront::forget(AccountId account) {
  answers_.erase(account.value());
}

}  // namespace libchiaking::catalog::cache
