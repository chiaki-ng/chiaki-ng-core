#include "src/core/catalog/adapters/cache/held_ownership.hpp"

#include "src/core/catalog/adapters/cache/library_record.hpp"
#include "src/core/catalog/model/library/owned_record.hpp"

#include <utility>

namespace libchiaking::catalog::cache {

auto HeldOwnership::is_fresh(const Library& library, Time now) -> bool {
  return now >= library.fetched_at() &&
         now - library.fetched_at() <= LibraryRecord::lifetime;
}

auto HeldOwnership::fresh(AccountId account, Time now) const -> LibraryPtr {
  const auto found = accounts_.find(account.value());
  if (found != accounts_.end() && found->second.library &&
      is_fresh(*found->second.library, now))
    return found->second.library;
  return nullptr;
}

auto HeldOwnership::ps5_of(AccountId account) const -> Ps5Ptr {
  const auto found = accounts_.find(account.value());
  return found != accounts_.end() ? found->second.ps5 : nullptr;
}

void HeldOwnership::hold_stored(AccountId account, LibraryPtr library,
                                Ps5Ptr ps5) {
  auto& held = accounts_[account.value()];
  held.library = std::move(library);
  held.ps5 = std::move(ps5);
}

void HeldOwnership::hold_read(const LibraryPtr& owned, Ps5Ptr ps5) {
  auto& held = accounts_[owned->account().value()];
  held.library = owned;
  held.ps5 = std::move(ps5);
  held.library_unavailable = false;
  held.checked_alone.reset();
  held.ps5_checked_alone.reset();
}

auto HeldOwnership::with(AccountId account, OwnedTitle title, Time now)
    -> Owned {
  auto& held = accounts_[account.value()];
  if (held.library) {
    held.library = std::make_shared<const Library>(
        held.library->with(std::move(title), now));
    return Owned{held.library, LibraryState::fetched, held.ps5};
  }
  const auto before = held.checked_alone
                          ? *held.checked_alone
                          : OwnedRecord::library_of(account, {}, now);
  held.checked_alone =
      std::make_shared<const Library>(before.with(std::move(title), now));
  held.library_unavailable = true;
  return Owned{held.checked_alone, LibraryState::unavailable,
               held.ps5_checked_alone};
}

auto HeldOwnership::with(AccountId account, Ps5Entitlement record, Time now)
    -> Owned {
  auto title = record.as_owned_title();
  auto& held = accounts_[account.value()];
  const bool kept = held.library != nullptr;
  auto& library = kept ? held.library : held.checked_alone;
  auto& ps5 = kept ? held.ps5 : held.ps5_checked_alone;
  const auto before =
      library ? *library : OwnedRecord::library_of(account, {}, now);
  library =
      title
          ? std::make_shared<const Library>(before.with(std::move(*title), now))
          : std::make_shared<const Library>(before);
  ps5 = std::make_shared<const OwnedPs5Games>(
      (ps5 ? *ps5 : OwnedPs5Games::unread(now)).with(std::move(record), now));
  held.library_unavailable = !kept;
  return Owned{library,
               kept ? LibraryState::fetched : LibraryState::unavailable, ps5};
}

auto HeldOwnership::after_read(AccountId account, LibraryPtr fetched) -> Owned {
  auto& held = accounts_[account.value()];
  held.library_unavailable = !fetched;
  if (fetched) {
    held.checked_alone.reset();
    held.ps5_checked_alone.reset();
    return Owned{std::move(fetched), LibraryState::fetched, held.ps5};
  }
  if (!held.checked_alone)
    held.checked_alone = std::make_shared<const Library>(
        OwnedRecord::library_of(account, {}, Time{}));
  return Owned{held.checked_alone, LibraryState::unavailable,
               held.ps5_checked_alone};
}

void HeldOwnership::hold_ps5_alone(AccountId account, Ps5Ptr ps5) {
  auto& held = accounts_[account.value()];
  const auto before = held.checked_alone ? *held.checked_alone
                                         : OwnedRecord::library_of(
                                               account, {}, ps5->fetched_at());
  held.checked_alone = std::make_shared<const Library>(
      before.with_ps5_part(ps5->games(), ps5->state()));
  held.ps5_checked_alone = std::move(ps5);
  held.library_unavailable = true;
}

auto HeldOwnership::rechecked_alone(AccountId account) const
    -> std::optional<Owned> {
  const auto found = accounts_.find(account.value());
  if (found == accounts_.end() || !found->second.library_unavailable ||
      !found->second.checked_alone)
    return std::nullopt;
  return Owned{found->second.checked_alone, LibraryState::unavailable,
               found->second.ps5_checked_alone};
}

void HeldOwnership::drop_library(AccountId account) {
  auto& held = accounts_[account.value()];
  held.library.reset();
  held.ps5.reset();
}

void HeldOwnership::drop_every_library() {
  for (auto& [account, held] : accounts_) {
    held.library.reset();
    held.ps5.reset();
  }
}

void HeldOwnership::forget(AccountId account) {
  accounts_.erase(account.value());
}

}  // namespace libchiaking::catalog::cache
