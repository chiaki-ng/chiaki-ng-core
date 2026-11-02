/// @file
/// @brief Declares SignIn, the exchange of an NPSSO or a refresh token for PSN
/// tokens, with its results.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/users/account_id.hpp"
#include "libchiaking/platform/runtime/stop_token.hpp"
#include "src/core/psn/auth/credentials.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace libchiaking::platform {
class ClockProvider;
class DiagnosticsProvider;
class HttpProvider;
}  // namespace libchiaking::platform

namespace libchiaking::psn::auth {

/// @brief An access token and the refresh token issued with it.
///
/// @tparam Access The access token's Credential type.
/// @tparam Refresh The refresh token's Credential type.
template <class Access, class Refresh>
struct IssuedTokens {
  /// @brief The access token. Secret.
  Access access;
  /// @brief The refresh token. Secret.
  Refresh refresh;
  /// @brief How long the access token lasts from the moment it was issued.
  std::chrono::seconds expires_in{};
  /// @brief How long the refresh token lasts, when PSN said.
  std::optional<std::chrono::seconds> refresh_expires_in{};

  /// @brief Works out when the refresh token expires.
  ///
  /// @param obtained_at When the tokens were issued.
  /// @return The expiry, or nothing when PSN gave no lifetime.
  auto refresh_expires_at(std::chrono::system_clock::time_point obtained_at)
      const -> std::optional<std::chrono::system_clock::time_point> {
    if (!refresh_expires_in) return std::nullopt;
    return obtained_at + *refresh_expires_in;
  }
};

/// @brief The Remote Play access and refresh tokens.
using RemotePlayTokens = IssuedTokens<BearerToken, RemotePlayRefreshToken>;
/// @brief The app access and refresh tokens.
using AppTokens = IssuedTokens<AppToken, AppRefreshToken>;

/// @brief The account a token belongs to.
struct Account {
  /// @brief The account's number.
  AccountId id;
  /// @brief The account's online id.
  std::string online_id;
};

/// @brief Everything a complete sign-in yields.
struct SignedIn {
  /// @brief The account that signed in.
  Account account;
  /// @brief The tokens for listing the account's consoles and playing over
  /// the internet.
  RemotePlayTokens remote_play;
  /// @brief The tokens for the store, profile and library calls.
  AppTokens app;
};

/// @brief Exchanges an NPSSO or a refresh token for PSN tokens.
///
/// This is the protocol client underneath users::SignedInUsers and
/// users::PsUser, which call it and keep the results; most applications never
/// use it directly. Each member makes its requests on the calling thread and
/// blocks until they finish. Nothing is stored: the tokens are returned to the
/// caller, in wiping memory.
class SignIn {
 public:
  /// @brief Creates an exchange client.
  ///
  /// @param http Carries the requests. Must outlive this object.
  /// @param clock Monotonic clock request deadlines are measured on. Must
  ///     outlive this object.
  /// @param stop Cancels every request this object makes.
  /// @param log Receives diagnostics. May be null.
  SignIn(platform::HttpProvider& http, const platform::ClockProvider& clock,
         platform::StopToken stop = {},
         platform::DiagnosticsProvider* log = nullptr);
  /// @brief Not copyable; the object owns its HTTP client.
  SignIn(const SignIn&) = delete;
  /// @brief Not copy-assignable; the object owns its HTTP client.
  auto operator=(const SignIn&) -> SignIn& = delete;
  /// @brief Releases the exchange client.
  ~SignIn();

  /// @brief Performs a complete sign-in from an NPSSO.
  ///
  /// Three things happen in order: Remote Play tokens are obtained, the
  /// account they belong to is looked up, and app tokens are obtained. The
  /// first failure ends the sign-in and is returned.
  ///
  /// @param npsso The sign-in seed.
  /// @param duid The device id to sign in as: 48 hexadecimal digits, as
  ///     users::DeviceId::view gives.
  /// @param correlation_id An identifier for the authorisation request, in
  ///     UUID form.
  /// @return The account and both sets of tokens.
  /// @retval SignInErrorKind::invalid_request The NPSSO, device id or
  ///     correlation id is not usable.
  /// @retval SignInErrorKind::rejected PSN refused the NPSSO.
  /// @retval SignInErrorKind::cancelled The stop token was triggered.
  /// @retval SignInErrorKind::transport A request did not complete.
  /// @retval SignInErrorKind::rate_limited PSN is rate limiting sign-in.
  /// @retval SignInErrorKind::unavailable PSN answered with an unexpected
  ///     status.
  /// @retval SignInErrorKind::malformed A reply could not be read.
  [[nodiscard]] auto sign_in(const Npsso& npsso, std::string_view duid,
                             std::string_view correlation_id)
      -> Expected<SignedIn, SignInError>;
  /// @brief Obtains Remote Play tokens from an NPSSO.
  ///
  /// @param npsso The sign-in seed.
  /// @param duid The device id to sign in as, 48 hexadecimal digits.
  /// @param correlation_id An identifier for the authorisation request, in
  ///     UUID form.
  /// @return The tokens, or the error, whose kinds are those of sign_in().
  [[nodiscard]] auto remote_play_tokens(const Npsso& npsso,
                                        std::string_view duid,
                                        std::string_view correlation_id)
      -> Expected<RemotePlayTokens, SignInError>;
  /// @brief Obtains app tokens from an NPSSO.
  ///
  /// Exchanges the NPSSO directly through the ``sso_token`` grant, so the
  /// issued token carries the ``ANDROID_APP``
  /// device type that ``sessionManager`` authorization requires.
  ///
  /// @param npsso The sign-in seed.
  /// @param duid The device id to sign in as, 48 hexadecimal digits, from
  ///     which the Android device identity is derived.
  /// @return The tokens, or the error, whose kinds are those of sign_in().
  [[nodiscard]] auto app_tokens(const Npsso& npsso, std::string_view duid)
      -> Expected<AppTokens, SignInError>;
  /// @brief Renews the Remote Play tokens.
  ///
  /// @param token The Remote Play refresh token.
  /// @return New access and refresh tokens. SignInErrorKind::rejected means
  ///     the refresh token is no longer good.
  [[nodiscard]] auto refresh(const RemotePlayRefreshToken& token)
      -> Expected<RemotePlayTokens, SignInError>;
  /// @brief Renews the app tokens.
  ///
  /// @param token The app refresh token.
  /// @return New access and refresh tokens. SignInErrorKind::rejected means
  ///     the refresh token is no longer good.
  [[nodiscard]] auto refresh(const AppRefreshToken& token)
      -> Expected<AppTokens, SignInError>;
  /// @brief Looks up which account a Remote Play token belongs to.
  ///
  /// @param token The Remote Play access token.
  /// @return The account's number and online id, or the error.
  [[nodiscard]] auto account(const BearerToken& token)
      -> Expected<Account, SignInError>;

 private:
  /// @brief The requests and their providers.
  class Impl;
  /// @brief The requests and their providers; never null.
  std::unique_ptr<Impl> impl_;
};

}  // namespace libchiaking::psn::auth
