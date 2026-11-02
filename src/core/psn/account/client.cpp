#include "src/core/psn/account/client.hpp"

#include "libchiaking/platform/factory.hpp"
#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/auth/credentials.hpp"
#include "src/core/psn/rest/user_profile/account_profile.hpp"

#include <utility>

namespace libchiaking::psn::account {

auto Client::failure_of(
    const rest::AccountProfile::DetailsError& error) noexcept -> Failure {
  const auto status = error.http_status();
  switch (error.cause()) {
    case rest::Cause::failure:
      if (error.is_cancelled()) return {Error::cancelled, status};
      if (error.failure() &&
          error.failure()->kind() == net::http::Failure::Kind::invalid_request)
        return {Error::invalid_argument, status};
      return {Error::unavailable, status};
    case rest::Cause::status:
      if (error.auth_refused()) return {Error::auth, status};
      if (status == http_too_many_requests)
        return {Error::rate_limited, status};
      return {Error::unavailable, status};
    case rest::Cause::request:
      return {Error::invalid_argument, status};
    case rest::Cause::response:
      break;
  }
  return {Error::malformed, status};
}

Client::Client(platform::HttpProvider& http,
               const platform::ClockProvider& clock, platform::StopToken stop,
               platform::DiagnosticsProvider* log) noexcept
    : http_(http), clock_(clock), stop_(std::move(stop)), log_(log) {}

auto Client::on(platform::PlatformFactory& platform, platform::StopToken stop)
    -> std::optional<Client> {
  if (!platform.supplies(platform::PlatformCapability::http))
    return std::nullopt;
  return Client{platform.http(), platform.clock(), std::move(stop),
                platform.supplies(platform::PlatformCapability::diagnostics)
                    ? &platform.diagnostics()
                    : nullptr};
}

auto Client::profile(const auth::AppToken& token, AccountId account)
    -> Expected<Profile, Failure> {
  if (!usable(token)) return unexpected(Failure{Error::invalid_argument});
  if (stop_.stop_requested()) return unexpected(Failure{Error::cancelled});
  net::http::Client http{http_, clock_, stop_, log_};
  auto details = rest::AccountProfile{http, token.clone()}.details(account);
  if (!details) return unexpected(failure_of(details.error()));
  return Profile{std::move(details.value().online_id),
                 std::move(details.value().avatar_url),
                 details.value().plus_status};
}

}  // namespace libchiaking::psn::account
