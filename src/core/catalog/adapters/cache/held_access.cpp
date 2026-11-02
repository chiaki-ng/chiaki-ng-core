#include "src/core/catalog/adapters/cache/held_access.hpp"

namespace libchiaking::catalog::cache {

auto HeldAccess::fresh(AccountId account, users::SignInState signed_in_as,
                       Time now) const -> std::optional<CloudAccess> {
  const auto found = shown_.find(account.value());
  if (found == shown_.end() || found->second.signed_in_as != signed_in_as ||
      found->second.access.account() != account ||
      !found->second.access.is_fresh_at(now))
    return std::nullopt;
  return found->second.access;
}

void HeldAccess::note(AccountId account, users::SignInState signed_in_as,
                      const CloudAccess& access) {
  shown_.insert_or_assign(account.value(), Shown{access, signed_in_as});
}

void HeldAccess::forget(AccountId account) { shown_.erase(account.value()); }

}  // namespace libchiaking::catalog::cache
