/// @file
/// @brief Declares TokenCache, which remembers the OAuth access tokens a
/// session exchanged its NPSSO for, so each grant is exchanged once per
/// lifetime and concurrent callers share one exchange.
///
/// A grant is what was asked of the authorisation server: a client id, a set of
/// scopes and whether a token or a code is wanted. Only token grants are
/// cached; authorisation codes are single-use. A cached token also serves a
/// request for fewer scopes of the same client.
#pragma once

#include "src/core/psn/auth/auth_error.hpp"
#include "src/core/psn/auth/credentials.hpp"
#include "src/core/psn/auth/timeline.hpp"
#include "src/core/utils/concurrency/single_flight.hpp"

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::psn::auth {

/// @brief How much sooner than PS's `expires_in` a cached token is treated
/// as expired, in seconds (60).
inline constexpr std::uint32_t token_margin_seconds = 60;
/// @brief How long a token is cached when PS states no `expires_in`, in
/// seconds (50 minutes).
inline constexpr std::uint32_t token_default_seconds = 50 * 60;
/// @brief The longest `expires_in` believed, in seconds (2 hours); longer
/// statements are cut to it.
inline constexpr std::uint32_t token_ceiling_seconds = 2 * 60 * 60;

/// @brief What an OAuth authorize request asks for, its `response_type`.
enum class ResponseType : std::uint8_t {
  /// @brief A single-use authorisation code (`code` in the redirect), handed
  /// to another service to redeem; never cached.
  code,
  /// @brief An access token directly (implicit grant, `access_token` in the
  /// redirect).
  token,
};

/// @brief What one authorize request asks of the authorisation server. Plain
/// data.
struct Grant {
  /// @brief The OAuth client the credential is issued to.
  std::string client_id;
  /// @brief The scopes asked for, separated by spaces, in any order.
  std::string scope;
  /// @brief Whether a token or a code is asked for.
  ResponseType response{ResponseType::token};
};

/// @brief A Grant in canonical form, ordering the cache: the scopes split,
/// sorted and deduplicated, so grants differing only in scope order or
/// repetition are the same key. Immutable value.
class GrantKey {
 public:
  /// @brief Canonicalises a grant.
  ///
  /// @param grant The grant.
  /// @return Its key; empty scopes between repeated spaces are dropped.
  static auto of(const Grant& grant) -> GrantKey;
  /// @brief Tells whether a token issued for this grant may serve another.
  ///
  /// @param wanted The grant asked for.
  /// @return True when both have the same client and response type and this
  ///     grant's scopes include all of `wanted`'s.
  auto covers(const GrantKey& wanted) const -> bool;
  /// @brief Reads the canonical scopes.
  ///
  /// @return The scopes, sorted and unique.
  auto scopes() const noexcept -> const std::vector<std::string>& {
    return scopes_;
  }

  /// @brief Orders keys by response type, client id, then scopes, for use as
  /// a map key.
  friend auto operator<=>(const GrantKey&, const GrantKey&) = default;

 private:
  /// @brief Makes a key from canonical parts; used by of().
  ///
  /// @param response The response type.
  /// @param client_id The client id.
  /// @param scopes The scopes, already sorted and unique.
  GrantKey(ResponseType response, std::string client_id,
           std::vector<std::string> scopes);

  /// @brief The grant's response type.
  ResponseType response_;
  /// @brief The grant's client id.
  std::string client_id_;
  /// @brief The grant's scopes, sorted and unique.
  std::vector<std::string> scopes_;
};

/// @brief Where a token's expiry came from, for diagnostics.
enum class ExpirySource : std::uint8_t {
  /// @brief PS's `expires_in`, less token_margin_seconds and capped.
  expires_in,
  /// @brief token_default_seconds, because PS stated no `expires_in`.
  fallback,
  /// @brief The expiry the app token was supplied with.
  app,
};

/// @brief Where a token stands, as diagnostics report it.
enum class TokenState : std::uint8_t {
  /// @brief No token has been obtained for the grant.
  none,
  /// @brief A token is held and has not expired.
  valid,
  /// @brief The token's lifetime has run out.
  expired,
  /// @brief A service refused the token, or it was forgotten as stale, before
  /// it expired.
  invalidated,
};

/// @brief Counters of how one token, or one grant, has been used, since the
/// cache or slot was last cleared. Plain data.
struct TokenUsage {
  /// @brief How many times the NPSSO was exchanged for the grant.
  std::uint64_t exchanges{};
  /// @brief How many requests reported using the token.
  std::uint64_t uses{};
  /// @brief How many of those requests were refused with HTTP 401 or 403.
  std::uint64_t refusals{};
  /// @brief How many times a refused token was renewed and the request
  /// retried.
  std::uint64_t retries{};
  /// @brief The HTTP status of the last exchange; 0 when it got no reply or
  /// none was made.
  std::uint16_t last_exchange_status{};
  /// @brief The HTTP status of the last request that used the token.
  std::uint16_t last_use_status{};

  /// @brief Compares every counter.
  ///
  /// @return Whether all counters are equal.
  friend auto operator==(const TokenUsage&, const TokenUsage&)
      -> bool = default;
};

/// @brief A diagnostic description of one token: the grant, its lifetime and
/// usage. Plain data; holds no secret.
struct TokenStats {
  /// @brief A human name for the token, such as `owned entitlements`; the
  /// client id and scope when none was given.
  std::string label;
  /// @brief The client the token was issued to.
  std::string client_id;
  /// @brief The scopes it was asked with.
  std::string scope;
  /// @brief When it was obtained, in wall-clock milliseconds since the Unix
  /// epoch; nothing when never obtained.
  std::optional<std::int64_t> obtained_at_ms;
  /// @brief When it expires, in wall-clock milliseconds since the Unix epoch;
  /// nothing when unknown.
  std::optional<std::int64_t> expires_at_ms;
  /// @brief Where expires_at_ms came from.
  ExpirySource expiry_source{ExpirySource::fallback};
  /// @brief Where the token stands now.
  TokenState state{TokenState::none};
  /// @brief How the grant has been used.
  TokenUsage usage;
  /// @brief When a request last reported using it, in wall-clock milliseconds
  /// since the Unix epoch.
  std::optional<std::int64_t> last_used_at_ms;
};

/// @brief The cache's totals and its tokens' statistics at one moment, for
/// the session diagnostics document. Plain data.
struct TokenCacheSnapshot {
  /// @brief Exchanges made across all grants.
  std::uint64_t exchanges{};
  /// @brief Uses reported across all grants.
  std::uint64_t uses{};
  /// @brief Refusals reported across all grants.
  std::uint64_t refusals{};
  /// @brief One entry per grant that was exchanged, used or retried, in grant
  /// key order.
  std::vector<TokenStats> tokens;
};

/// @brief What one exchange of the NPSSO produced. Plain data.
struct Exchanged {
  /// @brief The access token; empty when the server issued none.
  BearerToken token;
  /// @brief The lifetime PS stated, in seconds, from `expires_in` in the
  /// redirect; 0 when absent or unreadable.
  std::uint32_t expires_in{};
  /// @brief The HTTP status of the exchange's reply (302 on success).
  std::uint16_t http_status{};
};

/// @brief A token handed out for a grant. Plain data.
struct Lease {
  /// @brief The token.
  BearerToken token;
  /// @brief The grant the token was issued for, which may cover more scopes
  /// than the one asked for.
  Grant grant;
  /// @brief True when the token came from the cache, false when just
  /// exchanged.
  bool cached{};
};

/// @brief A session's NPSSO and the access tokens exchanged from it, one entry
/// per grant.
///
/// acquire() serves a fresh cached token whose grant covers the one asked for,
/// or makes the exchange; callers asking for the same grant while an exchange
/// is in flight wait for it and share its outcome (a failure other than
/// cancellation is shared too). A token is fresh for its stated lifetime less
/// token_margin_seconds, on the monotonic clock. A service's refusal makes the
/// caller invalidate() the token, so the next acquire() exchanges again.
///
/// Owned by the psn::Session's state for the session's lifetime. Thread-safe:
/// every member takes one mutex; acquire() releases it while an exchange runs
/// and while waiting for one, so other grants are served meanwhile.
class TokenCache {
 public:
  /// @brief Exchanges the NPSSO for a token for the grant being acquired; it
  /// performs the network request and may block for its duration.
  using Exchange =
      std::function<Expected<Exchanged, AuthError>(const Npsso& npsso)>;

  /// @brief Makes an empty cache for an NPSSO.
  ///
  /// @param npsso The sign-in seed every exchange spends.
  /// @param threading Provider of the cache's mutex and of the condition
  ///     variables waiting callers park on; must outlive the cache.
  /// @param clock The monotonic clock token lifetimes are measured on; must
  ///     outlive the cache.
  /// @param wall The wall clock statistics are reported in; must outlive the
  ///     cache.
  TokenCache(Npsso npsso, platform::ThreadingProvider& threading,
             const platform::ClockProvider& clock,
             const platform::ClockProvider& wall);

  /// @brief Reads the NPSSO.
  ///
  /// @return The NPSSO; valid for the cache's lifetime.
  auto npsso() const noexcept -> const Npsso& { return npsso_; }
  /// @brief Compares an NPSSO with the cache's in constant time.
  ///
  /// @param npsso The candidate.
  /// @return Whether they are equal.
  auto matches(std::string_view npsso) const noexcept -> bool;

  /// @brief Leases a token for a grant, exchanging the NPSSO when no fresh
  /// covering token is cached.
  ///
  /// Blocks while another caller's exchange for the same grant is in flight,
  /// and for the duration of its own exchange; `stop` ends the wait, not an
  /// exchange already running. When the exchange throws, the waiting callers
  /// see a transport failure.
  ///
  /// @param grant The grant wanted; must ask for a token.
  /// @param label The name the grant's statistics take, if it has none yet;
  ///     may be empty.
  /// @param stop Ends the wait for another caller's exchange.
  /// @param exchange Makes the exchange; called at most once, without the
  ///     lock.
  /// @return The token, its grant and whether it came from the cache. A token
  ///     whose stated lifetime is within token_margin_seconds is returned but
  ///     not cached.
  /// @retval AuthErrorKind::invalid_request `grant` asks for a code.
  /// @retval AuthErrorKind::cancelled `stop` fired before or while waiting.
  /// @retval AuthError The failure of this caller's exchange, or of the
  ///     exchange it waited for.
  [[nodiscard]] auto acquire(const Grant& grant, std::string_view label,
                             const platform::StopToken& stop,
                             const Exchange& exchange)
      -> Expected<Lease, AuthError>;

  /// @brief Forgets the token cached for exactly this grant, marking it
  /// invalidated.
  ///
  /// @param grant The grant.
  void invalidate(const Grant& grant);
  /// @brief Forgets a refused token wherever it is cached for the grant's
  /// client and response type, which may be under a wider grant that served
  /// it; a newer token since cached is left alone.
  ///
  /// @param grant The grant the token served.
  /// @param stale The refused token.
  void invalidate(const Grant& grant, const BearerToken& stale);
  /// @brief Records that a request used the grant's token, counting a 401 or
  /// 403 as a refusal; it does not invalidate the token.
  ///
  /// @param grant The grant the token was issued for.
  /// @param http_status The reply's HTTP status.
  void record_use(const Grant& grant, std::uint16_t http_status);
  /// @brief Records that a refused request is being retried with a renewed
  /// token.
  ///
  /// @param grant The grant the token was issued for.
  void record_retry(const Grant& grant);
  /// @brief Forgets every token and every count, as on sign-out; exchanges in
  /// flight complete but their tokens are not cached.
  void clear();

  /// @brief Describes the cache for diagnostics.
  ///
  /// @return The totals and the statistics of every grant exchanged, used or
  ///     retried.
  auto snapshot() -> TokenCacheSnapshot;
  /// @brief Describes the entry for exactly one grant.
  ///
  /// @param grant The grant.
  /// @return Its statistics, or nothing when the grant was never asked for.
  [[nodiscard]] auto standing(const Grant& grant) -> std::optional<TokenStats>;
  /// @brief Copies the token cached for exactly one grant, fresh or not.
  ///
  /// @param grant The grant.
  /// @return The token text, or nothing when none is held.
  [[nodiscard]] auto held(const Grant& grant) -> std::optional<SecretString>;
  /// @brief Tells whether any grant's last exchange failed in a way that
  /// means PSN refuses the NPSSO (AuthError::refuses_npsso()).
  ///
  /// @return True when one did.
  [[nodiscard]] auto npsso_refused() -> bool;

  /// @brief Decides how long a token is cached.
  ///
  /// @param expires_in PS's stated lifetime in seconds; 0 when absent.
  /// @return token_default_seconds for 0; otherwise the lifetime capped at
  ///     token_ceiling_seconds less token_margin_seconds, and 0 (do not
  ///     cache) when that leaves nothing.
  static auto lifetime_seconds(std::uint32_t expires_in) noexcept
      -> std::uint32_t;

 private:
  /// @brief Builds the failure of a cancelled acquire().
  ///
  /// @return An AuthErrorKind::cancelled failure.
  static auto cancelled() -> Unexpected<AuthError>;

  /// @brief An entry's lifetime, in monotonic microseconds.
  struct Life {
    /// @brief When the last token was obtained.
    std::optional<std::uint64_t> obtained_us;
    /// @brief When the last token stops being fresh.
    std::optional<std::uint64_t> expires_us;
    /// @brief Where expires_us came from.
    ExpirySource expiry_source{ExpirySource::fallback};
    /// @brief Whether the last token was forgotten before it expired.
    bool invalidated{};
    /// @brief When a request last reported using it.
    std::optional<std::uint64_t> last_used_us;
  };

  /// @brief Everything the cache keeps for one grant.
  struct Entry {
    /// @brief Makes an empty entry whose flight parks waiters on the cache's
    /// providers.
    ///
    /// @param threading Provider of the flight's condition variable.
    /// @param clock Monotonic clock the flight's waits measure on.
    Entry(platform::ThreadingProvider& threading,
          const platform::ClockProvider& clock)
        : flight(threading, clock) {}

    /// @brief The cached token; empty when none is fresh or it was
    /// forgotten.
    SecretString token;
    /// @brief The exchange in flight, which waiting callers share, and the
    /// outcome of the last one.
    concurrency::SingleFlight<Expected<void, AuthError>> flight;
    /// @brief The grant as first asked for.
    Grant grant;
    /// @brief The diagnostic name; empty until one is given.
    std::string label;
    /// @brief The token's lifetime.
    Life life;
    /// @brief The grant's counters.
    TokenUsage usage;
  };

  /// @brief Tells whether an entry's token may be served; the mutex must be
  /// held.
  ///
  /// @param entry The entry.
  /// @return True when it holds a token before its expiry.
  auto fresh(const Entry& entry) const noexcept -> bool;
  /// @brief Describes an entry; the mutex must be held.
  ///
  /// @param entry The entry.
  /// @return Its statistics.
  auto stats_of(const Entry& entry) const -> TokenStats;
  /// @brief Finds the entry for a grant, creating it; the mutex must be held.
  ///
  /// @param grant The grant.
  /// @return The entry; stays valid for the cache's lifetime.
  auto entry(const Grant& grant) -> Entry&;
  /// @brief Finds a fresh entry that can serve a grant; the mutex must be
  /// held.
  ///
  /// @param grant The grant wanted.
  /// @return The grant's own entry when fresh, else the first fresh entry in
  ///     key order whose grant covers it; null when none.
  auto covering(const Grant& grant) -> const Entry*;
  /// @brief Records an exchange's outcome in its entry; the mutex must be
  /// held.
  ///
  /// A token is cached only when the cache was not cleared since the exchange
  /// began and its lifetime is not zero.
  ///
  /// @param entry The entry exchanged for.
  /// @param result What the exchange produced.
  /// @param generation The value of generation_ when the exchange began.
  /// @return The outcome shared with waiting callers: success, or the
  ///     exchange's failure.
  auto finish(Entry& entry, const Expected<Exchanged, AuthError>& result,
              std::uint64_t generation) -> Expected<void, AuthError>;
  /// @brief Reads how an entry's last exchange failed.
  ///
  /// @param entry The entry.
  /// @return The failure, or nothing when the last exchange succeeded or none
  ///     has landed.
  static auto failed_exchange(const Entry& entry) -> std::optional<AuthError>;
  /// @brief Drops an entry's token, marking it invalidated when one was held.
  ///
  /// @param entry The entry.
  void forget(Entry& entry) noexcept;

  /// @brief The sign-in seed exchanges spend.
  Npsso npsso_;
  /// @brief The clocks lifetimes and statistics are measured with.
  Timeline time_;
  /// @brief Provider of the condition variable each new entry's flight
  /// parks waiters on.
  platform::ThreadingProvider& threading_;
  /// @brief The monotonic clock each entry's flight waits on.
  const platform::ClockProvider& clock_;
  /// @brief Guards every member below and every entry.
  platform::Mutex mutex_;
  /// @brief One entry per grant ever asked for; entries are never removed,
  /// only emptied.
  std::map<GrantKey, Entry> entries_;
  /// @brief Incremented by clear(), so exchanges begun before it do not cache
  /// their tokens.
  std::uint64_t generation_{};
  /// @brief Exchanges made since the last clear().
  std::uint64_t exchanges_{};
  /// @brief Uses reported since the last clear().
  std::uint64_t uses_{};
  /// @brief Refusals reported since the last clear().
  std::uint64_t refusals_{};
};

}  // namespace libchiaking::psn::auth
