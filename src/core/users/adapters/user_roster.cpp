#include "src/core/users/adapters/user_roster.hpp"

#include <algorithm>
#include <utility>

namespace libchiaking::users {

UserRoster::UserRoster(const PsUser::Core::Providers& providers)
    : providers_(providers), mutex_(providers.threading) {}

UserRoster::~UserRoster() {
  for (const auto& user : users_) PsUser::Core::of(*user).leave();
}

auto UserRoster::admit(SavedSignIn saved) -> Admitted {
  return enrolled(PsUser::Core::admit(providers_, std::move(saved)));
}

auto UserRoster::admit(SavedHomeUser saved) -> Admitted {
  return enrolled(PsUser::Core::admit(providers_, std::move(saved)));
}

auto UserRoster::enrolled(Admitted admitted) -> Admitted {
  if (!admitted) return admitted;
  platform::Lock lock{mutex_};
  const auto same = std::ranges::find(users_, admitted.value()->account_id(),
                                      &PsUser::account_id);
  if (same != users_.end()) {
    PsUser::Core::of(**same).leave();
    *same = admitted.value();
  } else {
    users_.push_back(admitted.value());
  }
  return admitted;
}

auto UserRoster::forget(AccountId account)
    -> Expected<void, CredentialStore::Error> {
  std::shared_ptr<PsUser> leaving;
  {
    platform::Lock lock{mutex_};
    const auto found = std::ranges::find(users_, account, &PsUser::account_id);
    if (found != users_.end()) {
      leaving = std::move(*found);
      users_.erase(found);
    }
  }
  if (leaving) PsUser::Core::of(*leaving).leave();
  return providers_.credentials.remove(account);
}

auto UserRoster::find(AccountId account) const -> std::shared_ptr<PsUser> {
  platform::Lock lock{mutex_};
  const auto found = std::ranges::find(users_, account, &PsUser::account_id);
  return found != users_.end() ? *found : nullptr;
}

auto UserRoster::everyone() const -> std::vector<std::shared_ptr<PsUser>> {
  platform::Lock lock{mutex_};
  return users_;
}

auto UserRoster::holds(const PsUser& user) const -> bool {
  return find(user.account_id()).get() == &user;
}

}  // namespace libchiaking::users
