#include "src/core/users/usecases/sign_in.hpp"

#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "src/core/psn/auth/credentials.hpp"
#include "src/core/psn/auth/sign_in.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>

namespace libchiaking::users::usecases {

SignIn::SignIn(platform::HttpProvider& http,
               const platform::ClockProvider& clock,
               CredentialStore& credentials,
               const platform::ClockProvider& wall, UserRoster& roster,
               platform::DiagnosticsProvider* log) noexcept
    : http_(http),
      clock_(clock),
      credentials_(credentials),
      wall_(wall),
      roster_(roster),
      log_(log) {}

auto SignIn::first_time(Seed seed, const DeviceId& device,
                        std::string_view correlation_id, const Stop& stop)
    -> Expected<std::shared_ptr<PsUser>, psn::auth::SignInError> {
  auto signed_in = psn::auth::SignIn{http_, clock_, stop, log_}.sign_in(
      seed.npsso, device.view(), correlation_id);
  if (!signed_in) return unexpected(signed_in.error());
  return kept(std::move(signed_in.value()), seed, device);
}

auto SignIn::again(const PsUser& user, Seed seed, const DeviceId& device,
                   std::string_view correlation_id, const Stop& stop)
    -> Expected<std::shared_ptr<PsUser>, Refusal> {
  if (!roster_.holds(user)) return unexpected(Refusal::unknown_user());
  auto signed_in = psn::auth::SignIn{http_, clock_, stop, log_}.sign_in(
      seed.npsso, device.view(), correlation_id);
  if (!signed_in) return unexpected(Refusal::sign_in_failed(signed_in.error()));
  auto& account = signed_in.value().account;
  if (account.id != user.account_id())
    return unexpected(
        Refusal::different_account({account.id, std::move(account.online_id)}));
  if (!roster_.holds(user)) return unexpected(Refusal::unknown_user());
  auto again = kept(std::move(signed_in.value()), seed, device);
  if (!again) return unexpected(Refusal::sign_in_failed(again.error()));
  return std::move(again.value());
}

auto SignIn::welcome_back()
    -> Expected<SignedInUsers::Returned, CredentialStore::Error> {
  auto saved = credentials_.load_saved_users();
  if (!saved) return unexpected(saved.error());
  SignedInUsers::Returned returned;
  for (auto& user : saved.value()) {
    if (auto present = roster_.find(user.account)) {
      returned.users.push_back(std::move(present));
      continue;
    }
    const bool identified = user.device.has_value();
    auto admitted = roster_.admit(std::move(user));
    if (!admitted) {
      ++returned.rejected;
      continue;
    }
    if (identified)
      PsUser::Core::of(*admitted.value()).kept_already();
    else
      PsUser::Core::of(*admitted.value()).save();
    returned.users.push_back(std::move(admitted.value()));
  }
  auto home = credentials_.load_home_users();
  if (!home) return unexpected(home.error());
  for (auto& user : home.value()) {
    if (auto present = roster_.find(user.account)) {
      if (std::ranges::find(returned.users, present) == returned.users.end())
        returned.users.push_back(std::move(present));
      continue;
    }
    auto admitted = roster_.admit(std::move(user));
    if (!admitted) {
      ++returned.rejected;
      continue;
    }
    PsUser::Core::of(*admitted.value()).kept_already();
    returned.users.push_back(std::move(admitted.value()));
  }
  return returned;
}

auto SignIn::home_user(AccountId account, std::string_view name)
    -> Expected<std::shared_ptr<PsUser>, PsUser::Invalid> {
  if (auto present = roster_.find(account);
      present && present->has_psn_account())
    return present;
  auto admitted = roster_.admit(SavedHomeUser{account, std::string{name}});
  if (admitted) PsUser::Core::of(*admitted.value()).save();
  return admitted;
}

auto SignIn::kept(psn::auth::SignedIn issued, Seed seed, const DeviceId& device)
    -> Expected<std::shared_ptr<PsUser>, psn::auth::SignInError> {
  const SavedSignIn::Time now{
      std::chrono::microseconds{wall_.now_microseconds()}};
  SavedSignIn saved{issued.account.id, std::move(issued.account.online_id),
                    seed.npsso.clone()};
  saved.npsso_obtained_at = now;
  saved.npsso_expires_at = seed.expires_at;
  saved.store.emplace(std::move(issued.app.access), now + issued.app.expires_in,
                      now);
  if (!issued.app.refresh.empty())
    saved.store_refresh.emplace(std::move(issued.app.refresh), now,
                                issued.app.refresh_expires_at(now));
  saved.remote_play.emplace(std::move(issued.remote_play.access),
                            now + issued.remote_play.expires_in, now);
  saved.remote_play_refresh.emplace(std::move(issued.remote_play.refresh), now,
                                    issued.remote_play.refresh_expires_at(now));
  saved.device.emplace(device.clone());
  auto admitted = roster_.admit(std::move(saved));
  if (!admitted)
    return unexpected(
        psn::auth::SignInError{psn::auth::SignInErrorKind::malformed});
  PsUser::Core::of(*admitted.value()).save();
  return std::move(admitted.value());
}

}  // namespace libchiaking::users::usecases
