#include "src/core/psn/auth/token_cache.hpp"

#include "libchiaking/platform/runtime/time_units.hpp"
#include "src/core/net/transport/http/status.hpp"
#include "src/core/utils/memory/constant_time.hpp"

#include <algorithm>
#include <functional>
#include <ranges>
#include <span>
#include <utility>

namespace libchiaking::psn::auth {

auto TokenCache::cancelled() -> Unexpected<AuthError> {
  return unexpected(AuthError{AuthErrorKind::cancelled});
}

GrantKey::GrantKey(ResponseType response, std::string client_id,
                   std::vector<std::string> scopes)
    : response_(response),
      client_id_(std::move(client_id)),
      scopes_(std::move(scopes)) {}

auto GrantKey::of(const Grant& grant) -> GrantKey {
  std::vector<std::string> scopes;
  for (const auto part :
       std::views::split(std::string_view{grant.scope}, ' ')) {
    const std::string_view item{part.begin(), part.end()};
    if (!item.empty()) scopes.emplace_back(item);
  }
  std::ranges::sort(scopes);
  const auto [first, last] = std::ranges::unique(scopes);
  scopes.erase(first, last);
  return GrantKey{grant.response, grant.client_id, std::move(scopes)};
}

auto GrantKey::covers(const GrantKey& wanted) const -> bool {
  return response_ == wanted.response_ && client_id_ == wanted.client_id_ &&
         std::ranges::includes(scopes_, wanted.scopes_);
}

TokenCache::TokenCache(Npsso npsso, platform::ThreadingProvider& threading,
                       const platform::ClockProvider& clock,
                       const platform::ClockProvider& wall)
    : npsso_(std::move(npsso)),
      time_(clock, wall),
      threading_(threading),
      clock_(clock),
      mutex_(threading) {}

auto TokenCache::matches(std::string_view npsso) const noexcept -> bool {
  return memory::constant_time_equal(std::as_bytes(std::span{npsso_.view()}),
                                     std::as_bytes(std::span{npsso}));
}

auto TokenCache::lifetime_seconds(std::uint32_t expires_in) noexcept
    -> std::uint32_t {
  if (expires_in == 0) return token_default_seconds;
  const auto bounded = std::min(expires_in, token_ceiling_seconds);
  return bounded > token_margin_seconds ? bounded - token_margin_seconds : 0;
}

auto TokenCache::fresh(const Entry& entry) const noexcept -> bool {
  return !entry.token.empty() && entry.life.expires_us &&
         time_.now_us() < *entry.life.expires_us;
}

auto TokenCache::entry(const Grant& grant) -> Entry& {
  auto [found, created] =
      entries_.try_emplace(GrantKey::of(grant), threading_, clock_);
  if (created) found->second.grant = grant;
  return found->second;
}

auto TokenCache::covering(const Grant& grant) -> const Entry* {
  const auto wanted = GrantKey::of(grant);
  const auto exact = entries_.find(wanted);
  if (exact != entries_.end() && fresh(exact->second)) return &exact->second;
  for (const auto& [key, held] : entries_)
    if (fresh(held) && key.covers(wanted)) return &held;
  return nullptr;
}

auto TokenCache::acquire(const Grant& grant, std::string_view label,
                         const platform::StopToken& stop,
                         const Exchange& exchange)
    -> Expected<Lease, AuthError> {
  if (grant.response != ResponseType::token)
    return unexpected(AuthError{AuthErrorKind::invalid_request});
  platform::Lock lock{mutex_};
  for (;;) {
    if (const auto* held = covering(grant))
      return Lease{BearerToken{held->token.clone()}, held->grant, true};
    auto& slot = entry(grant);
    slot.token.clear();
    if (!slot.flight.in_flight()) break;
    if (!slot.flight.await_landing(lock, stop)) return cancelled();
    if (fresh(slot)) continue;
    if (const auto failure = failed_exchange(slot);
        failure && failure->kind() != AuthErrorKind::cancelled)
      return unexpected(*failure);
  }
  if (stop.stop_requested()) return cancelled();
  auto& slot = entry(grant);
  if (slot.label.empty() && !label.empty()) slot.label = std::string{label};
  const auto generation = generation_;
  ++slot.usage.exchanges;
  ++exchanges_;
  auto result = slot.flight.lead(
      lock, [&] { return exchange(npsso_); },
      [&](const Expected<Exchanged, AuthError>& landed) {
        return finish(slot, landed, generation);
      },
      [&] {
        return finish(slot, unexpected(AuthError{AuthErrorKind::transport}),
                      generation);
      });
  if (!result) return unexpected(result.error());
  return Lease{std::move(result.value().token), slot.grant, false};
}

auto TokenCache::finish(Entry& slot,
                        const Expected<Exchanged, AuthError>& result,
                        std::uint64_t generation) -> Expected<void, AuthError> {
  slot.token.clear();
  if (!result) {
    slot.usage.last_exchange_status = result.error().http_status();
    return unexpected(result.error());
  }
  const auto& exchanged = result.value();
  slot.usage.last_exchange_status = exchanged.http_status;
  if (!exchanged.token.empty() && generation == generation_) {
    const auto now = time_.now_us();
    const auto lifetime = lifetime_seconds(exchanged.expires_in);
    slot.life.obtained_us = now;
    slot.life.expires_us = now + lifetime * microseconds_per_second;
    slot.life.expiry_source = exchanged.expires_in != 0
                                  ? ExpirySource::expires_in
                                  : ExpirySource::fallback;
    slot.life.invalidated = false;
    if (lifetime != 0) slot.token = exchanged.token.secret().clone();
  }
  return {};
}

auto TokenCache::failed_exchange(const Entry& slot)
    -> std::optional<AuthError> {
  const auto& landed = slot.flight.landed();
  if (!landed || landed->has_value()) return std::nullopt;
  return landed->error();
}

void TokenCache::forget(Entry& slot) noexcept {
  if (!slot.token.empty()) slot.life.invalidated = true;
  slot.token.clear();
}

void TokenCache::invalidate(const Grant& grant) {
  platform::Lock lock{mutex_};
  const auto found = entries_.find(GrantKey::of(grant));
  if (found != entries_.end()) forget(found->second);
}

void TokenCache::invalidate(const Grant& grant, const BearerToken& stale) {
  platform::Lock lock{mutex_};
  for (auto& [key, slot] : entries_)
    if (slot.grant.client_id == grant.client_id &&
        slot.grant.response == grant.response &&
        slot.token.view() == stale.view())
      forget(slot);
}

void TokenCache::record_use(const Grant& grant, std::uint16_t http_status) {
  platform::Lock lock{mutex_};
  auto& slot = entry(grant);
  ++slot.usage.uses;
  ++uses_;
  slot.usage.last_use_status = http_status;
  slot.life.last_used_us = time_.now_us();
  if (net::HttpStatus{http_status}.is_auth_refused()) {
    ++slot.usage.refusals;
    ++refusals_;
  }
}

void TokenCache::record_retry(const Grant& grant) {
  platform::Lock lock{mutex_};
  ++entry(grant).usage.retries;
}

void TokenCache::clear() {
  platform::Lock lock{mutex_};
  ++generation_;
  for (auto& [key, slot] : entries_) {
    slot.token.clear();
    slot.life = {};
    slot.usage = {};
  }
  exchanges_ = 0;
  uses_ = 0;
  refusals_ = 0;
}

auto TokenCache::npsso_refused() -> bool {
  platform::Lock lock{mutex_};
  return std::ranges::any_of(entries_, [](const auto& held) {
    const auto failure = failed_exchange(held.second);
    return failure && failure->refuses_npsso();
  });
}

auto TokenCache::stats_of(const Entry& slot) const -> TokenStats {
  TokenStats stats;
  stats.label = slot.label.empty()
                    ? slot.grant.client_id + " " + slot.grant.scope
                    : slot.label;
  stats.client_id = slot.grant.client_id;
  stats.scope = slot.grant.scope;
  if (slot.life.obtained_us)
    stats.obtained_at_ms = time_.unix_ms(*slot.life.obtained_us);
  if (slot.life.expires_us)
    stats.expires_at_ms = time_.unix_ms(*slot.life.expires_us);
  stats.expiry_source = slot.life.expiry_source;
  stats.state = !slot.life.obtained_us  ? TokenState::none
                : slot.life.invalidated ? TokenState::invalidated
                : fresh(slot)           ? TokenState::valid
                                        : TokenState::expired;
  stats.usage = slot.usage;
  if (slot.life.last_used_us)
    stats.last_used_at_ms = time_.unix_ms(*slot.life.last_used_us);
  return stats;
}

auto TokenCache::snapshot() -> TokenCacheSnapshot {
  platform::Lock lock{mutex_};
  TokenCacheSnapshot result{exchanges_, uses_, refusals_, {}};
  for (const auto& [key, slot] : entries_)
    if (slot.usage.exchanges != 0 || slot.usage.uses != 0 ||
        slot.usage.retries != 0)
      result.tokens.push_back(stats_of(slot));
  return result;
}

auto TokenCache::standing(const Grant& grant) -> std::optional<TokenStats> {
  platform::Lock lock{mutex_};
  const auto found = entries_.find(GrantKey::of(grant));
  if (found == entries_.end()) return std::nullopt;
  return stats_of(found->second);
}

auto TokenCache::held(const Grant& grant) -> std::optional<SecretString> {
  platform::Lock lock{mutex_};
  const auto found = entries_.find(GrantKey::of(grant));
  if (found == entries_.end() || found->second.token.empty())
    return std::nullopt;
  return found->second.token.clone();
}

}  // namespace libchiaking::psn::auth
