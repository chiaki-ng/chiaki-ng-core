#include "src/core/users/signed_in_users_core.hpp"

#include "src/core/net/transport/http/sleep_waiter.hpp"

#include <memory>
#include <string>
#include <utility>

namespace libchiaking::users {

SignedInUsers::Core::Core(platform::HttpProvider& http,
                          CredentialStore& credentials,
                          platform::ThreadingProvider& threading,
                          const platform::ClockProvider& clock,
                          const platform::ClockProvider& wall,
                          platform::SecureRandomProvider& random,
                          platform::DiagnosticsProvider* log)
    : clock_(clock),
      wall_(wall),
      server_time_(clock_),
      roster_(PsUser::Core::Providers{http, credentials, threading, clock_,
                                      wall_, random, log, server_time_}),
      sign_in_(http, clock_, credentials, wall_, roster_, log),
      qr_sign_in_(
          http, clock_, wall_, sign_in_,
          [&threading, &clock]() -> std::unique_ptr<net::http::Waiter> {
            return std::make_unique<net::http::SleepWaiter>(threading, clock);
          },
          log) {}

SignedInUsers::SignedInUsers(platform::HttpProvider& http,
                             CredentialStore& credentials,
                             platform::ThreadingProvider& threading,
                             const platform::ClockProvider& clock,
                             const platform::ClockProvider& wall,
                             platform::SecureRandomProvider& random,
                             platform::DiagnosticsProvider* log)
    : core_(std::make_unique<Core>(http, credentials, threading, clock, wall,
                                   random, log)) {}

SignedInUsers::~SignedInUsers() = default;

auto SignedInUsers::SignInAgainRefusal::different_account(
    OtherAccount signed_in_as) -> SignInAgainRefusal {
  return SignInAgainRefusal{std::move(signed_in_as)};
}

auto SignedInUsers::SignInAgainRefusal::unknown_user() -> SignInAgainRefusal {
  return SignInAgainRefusal{UnknownUser{}};
}

auto SignedInUsers::SignInAgainRefusal::sign_in_failed(
    psn::auth::SignInError error) -> SignInAgainRefusal {
  return SignInAgainRefusal{error};
}

auto SignedInUsers::welcome_back()
    -> Expected<Returned, CredentialStore::Error> {
  return core_->sign_in().welcome_back();
}

auto SignedInUsers::sign_in(const psn::auth::Npsso& npsso,
                            const DeviceId& device,
                            std::string_view correlation_id,
                            const platform::StopToken& stop,
                            std::optional<SavedSignIn::Time> npsso_expires_at)
    -> Expected<std::shared_ptr<PsUser>, psn::auth::SignInError> {
  return core_->sign_in().first_time({npsso, npsso_expires_at}, device,
                                     correlation_id, stop);
}

auto SignedInUsers::sign_in_with_qr(const DeviceId& device,
                                    std::string_view correlation_id,
                                    const platform::StopToken& stop,
                                    QrSignInObserver observer,
                                    const QrSignInOptions& options)
    -> Expected<std::shared_ptr<PsUser>, psn::auth::SignInError> {
  return core_->qr_sign_in().run(device, correlation_id, stop, observer,
                                 options);
}

auto SignedInUsers::sign_in_again(
    const PsUser& user, const psn::auth::Npsso& npsso, const DeviceId& device,
    std::string_view correlation_id, const platform::StopToken& stop,
    std::optional<SavedSignIn::Time> npsso_expires_at)
    -> Expected<std::shared_ptr<PsUser>, SignInAgainRefusal> {
  return core_->sign_in().again(user, {npsso, npsso_expires_at}, device,
                                correlation_id, stop);
}

auto SignedInUsers::add_home_user(AccountId account, std::string_view name)
    -> Expected<std::shared_ptr<PsUser>, PsUser::Invalid> {
  return core_->sign_in().home_user(account, name);
}

auto SignedInUsers::sign_out(AccountId account)
    -> Expected<void, CredentialStore::Error> {
  return core_->roster().forget(account);
}

auto SignedInUsers::find(AccountId account) const -> std::shared_ptr<PsUser> {
  return core_->roster().find(account);
}

auto SignedInUsers::everyone() const -> std::vector<std::shared_ptr<PsUser>> {
  return core_->roster().everyone();
}

auto SignedInUsers::server_time() const noexcept
    -> std::optional<ServerInstant> {
  return core_->server_time().now();
}

}  // namespace libchiaking::users
