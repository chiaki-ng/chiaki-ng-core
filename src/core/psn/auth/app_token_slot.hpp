/// @file
/// @brief Declares AppTokenSlot, where a session keeps the app
/// access token, the credential behind GraphQL, the app library, subscriptions,
/// the cloud game catalogue and the console directory.
///
/// Unlike the commerce tokens in TokenCache, the app token is not exchanged
/// from the NPSSO here: the users context obtains and refreshes it through
/// SignIn and supplies it. The slot only holds it, judges whether it can still
/// be spent, and records how services received it.
#pragma once

#include "src/core/psn/auth/auth_error.hpp"
#include "src/core/psn/auth/credentials.hpp"
#include "src/core/psn/auth/timeline.hpp"
#include "src/core/psn/auth/token_cache.hpp"

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

namespace libchiaking::psn::auth {

/// @brief The label the app token carries in diagnostics and token statistics.
inline constexpr std::string_view app_token_label = "app token (supplied)";
/// @brief The OAuth client id the app token is issued to; reported in token
/// statistics.
inline constexpr std::string_view app_client_id =
    "09515159-7237-4370-9b40-3806e67c0891";
/// @brief The scopes the app token is issued with; reported in token
/// statistics.
inline constexpr std::string_view app_scope =
    "psn:mobile.v2.core psn:clientapp";
/// @brief How long before its stated expiry the app token stops being leased
/// (30 s), so a request is not sent with a token about to lapse in flight.
inline constexpr std::chrono::milliseconds app_token_margin{30'000};

/// @brief The session's one app token, its expiry and its usage counts.
///
/// States, as TokenState reports them: none while no token is held; valid;
/// expired once the wall clock is within app_token_margin of the stated expiry;
/// invalidated once a service has refused it (HTTP 401 or 403), until a new
/// token is supplied. Expiry is compared on the wall clock because the expiry
/// is supplied as a wall-clock instant.
///
/// Owned by the psn::Session's state, for the session's lifetime. Every member
/// takes the slot's mutex briefly and never blocks on anything else, so any
/// thread may call any member at any time.
class AppTokenSlot {
 public:
  /// @brief Makes an empty slot.
  ///
  /// @param threading Provider of the slot's mutex; must outlive the slot.
  /// @param clock The monotonic clock usage times are taken on; must outlive
  ///     the slot.
  /// @param wall The wall clock expiry is judged on; must outlive the slot.
  AppTokenSlot(platform::ThreadingProvider& threading,
               const platform::ClockProvider& clock,
               const platform::ClockProvider& wall);

  /// @brief Holds a newly issued app token in place of any other, clearing a
  /// previous refusal; usage counts are kept.
  ///
  /// @param token The token.
  /// @param expires_at When PS said it expires, by the wall clock; the
  ///     epoch itself means no stated expiry.
  void supply(AppToken token, std::chrono::system_clock::time_point expires_at);
  /// @brief Drops the token and its refusal, keeping its expiry and usage
  /// counts for diagnostics.
  void clear() noexcept;
  /// @brief Drops the token and everything recorded about it, as on sign-out.
  void reset() noexcept;
  /// @brief Hands out a copy of the token for one request.
  ///
  /// @return The token, labelled app_token_label.
  /// @retval AuthErrorKind::missing No token is held.
  /// @retval AuthErrorKind::refused A service refused the token.
  /// @retval AuthErrorKind::expired The token expires within
  ///     app_token_margin.
  [[nodiscard]] auto lease() -> Expected<AppToken, AuthError>;
  /// @brief Records the outcome of a request sent with a leased token.
  ///
  /// Ignored when `token` is not the one held now (it was replaced or dropped
  /// meanwhile). A 401 or 403 marks the held token invalidated.
  ///
  /// @param token The token the request carried.
  /// @param http_status The reply's HTTP status.
  /// @param retries How many times the request was retried; negative counts
  ///     as 0.
  void record_use(const AppToken& token, std::uint16_t http_status,
                  int retries);
  /// @brief Describes the token for diagnostics.
  ///
  /// @return The statistics, with the state judged against the stated expiry
  ///     without the margin; nothing when no token was ever supplied and none
  ///     was used since the last reset().
  auto stats() -> std::optional<TokenStats>;
  /// @brief Copies the held token whatever its state, for saving it.
  ///
  /// @return The token text, or nothing when none is held.
  [[nodiscard]] auto held() -> std::optional<SecretString>;
  /// @brief Tells what lease() would do now, without leasing.
  ///
  /// @return The token's state, judged with app_token_margin.
  auto leasable() -> TokenState;

 private:
  /// @brief Judges the held token; the mutex must be held.
  ///
  /// @param margin How long before the stated expiry the token already counts
  ///     as expired.
  /// @return none, invalidated, expired or valid, in that order of
  ///     precedence; a token with no stated expiry never expires.
  auto state(std::chrono::milliseconds margin) const noexcept -> TokenState;

  /// @brief The clocks usage and expiry are measured with.
  Timeline time_;
  /// @brief Guards every member below.
  platform::Mutex mutex_;
  /// @brief The app token; empty when none is held.
  SecretString token_;
  /// @brief The stated expiry, in wall-clock milliseconds since the Unix
  /// epoch; 0 means none.
  std::int64_t expires_at_ms_{};
  /// @brief Whether a service has refused the held token.
  bool refused_{};
  /// @brief When the token was last supplied, in monotonic microseconds;
  /// nothing since construction or reset().
  std::optional<std::uint64_t> supplied_us_;
  /// @brief When a request last reported using the token, in monotonic
  /// microseconds.
  std::optional<std::uint64_t> last_used_us_;
  /// @brief Counts of the requests that used the token since the last
  /// reset().
  TokenUsage usage_;
};

}  // namespace libchiaking::psn::auth
