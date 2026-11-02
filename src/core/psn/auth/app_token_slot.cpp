#include "src/core/psn/auth/app_token_slot.hpp"

#include "src/core/net/transport/http/status.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace libchiaking::psn::auth {

AppTokenSlot::AppTokenSlot(platform::ThreadingProvider& threading,
                           const platform::ClockProvider& clock,
                           const platform::ClockProvider& wall)
    : time_(clock, wall), mutex_(threading) {}

void AppTokenSlot::supply(AppToken token,
                          std::chrono::system_clock::time_point expires_at) {
  const auto expires_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      expires_at.time_since_epoch());
  platform::Lock lock{mutex_};
  token_ = token.secret().clone();
  expires_at_ms_ = expires_ms.count();
  refused_ = false;
  supplied_us_ = time_.now_us();
}

void AppTokenSlot::clear() noexcept {
  platform::Lock lock{mutex_};
  token_.clear();
  refused_ = false;
}

void AppTokenSlot::reset() noexcept {
  platform::Lock lock{mutex_};
  token_.clear();
  expires_at_ms_ = 0;
  refused_ = false;
  supplied_us_.reset();
  last_used_us_.reset();
  usage_ = {};
}

auto AppTokenSlot::state(std::chrono::milliseconds margin) const noexcept
    -> TokenState {
  if (token_.empty()) return TokenState::none;
  if (refused_) return TokenState::invalidated;
  if (expires_at_ms_ != 0 && time_.wall_ms() + margin.count() >= expires_at_ms_)
    return TokenState::expired;
  return TokenState::valid;
}

auto AppTokenSlot::lease() -> Expected<AppToken, AuthError> {
  platform::Lock lock{mutex_};
  switch (state(app_token_margin)) {
    case TokenState::none:
      return unexpected(AuthError{AuthErrorKind::missing});
    case TokenState::invalidated:
      return unexpected(AuthError{AuthErrorKind::refused});
    case TokenState::expired:
      return unexpected(AuthError{AuthErrorKind::expired});
    case TokenState::valid:
      break;
  }
  return AppToken{token_.clone(), app_token_label};
}

void AppTokenSlot::record_use(const AppToken& token, std::uint16_t http_status,
                              int retries) {
  platform::Lock lock{mutex_};
  if (token.empty() || token_.view() != token.view()) return;
  ++usage_.uses;
  usage_.retries += static_cast<std::uint64_t>(std::max(retries, 0));
  usage_.last_use_status = http_status;
  last_used_us_ = time_.now_us();
  if (net::HttpStatus{http_status}.is_auth_refused()) {
    ++usage_.refusals;
    refused_ = true;
  }
}

auto AppTokenSlot::held() -> std::optional<SecretString> {
  platform::Lock lock{mutex_};
  if (token_.empty()) return std::nullopt;
  return token_.clone();
}

auto AppTokenSlot::leasable() -> TokenState {
  platform::Lock lock{mutex_};
  return state(app_token_margin);
}

auto AppTokenSlot::stats() -> std::optional<TokenStats> {
  platform::Lock lock{mutex_};
  if (!supplied_us_ && usage_.uses == 0) return std::nullopt;
  TokenStats stats;
  stats.label = std::string{app_token_label};
  stats.client_id = std::string{app_client_id};
  stats.scope = std::string{app_scope};
  if (supplied_us_) stats.obtained_at_ms = time_.unix_ms(*supplied_us_);
  if (expires_at_ms_ != 0) stats.expires_at_ms = expires_at_ms_;
  stats.expiry_source = ExpirySource::app;
  stats.state = state(std::chrono::milliseconds::zero());
  stats.usage = usage_;
  if (last_used_us_) stats.last_used_at_ms = time_.unix_ms(*last_used_us_);
  return stats;
}

}  // namespace libchiaking::psn::auth
