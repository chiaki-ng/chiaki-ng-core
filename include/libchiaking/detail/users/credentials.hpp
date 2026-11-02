/// @file
/// @brief Declares the kinds of PSN credential and the Credential types made
/// from them.
#pragma once

#include <libchiaking/detail/users/credential.hpp>

#include <cstddef>
#include <string_view>

namespace libchiaking::users {

/// @brief The longest access token, refresh token or authorisation code the
/// library accepts, in bytes.
inline constexpr std::size_t token_maximum_bytes = 8192;

/// @brief Tag for the Remote Play access token.
struct BearerKind {
  /// @brief Name used for the credential in diagnostics.
  static constexpr std::string_view label = "bearer";
  /// @brief Sent as an `Authorization: Bearer` header.
  static constexpr CredentialPlacement placement{"Authorization", "Bearer "};
  /// @brief The longest token accepted; see token_maximum_bytes.
  static constexpr std::size_t maximum_bytes = token_maximum_bytes;
  /// @brief Recorded in diagnostics as an account token.
  static constexpr platform::HttpCredential credential =
      platform::HttpCredential::account_token;
};

/// @brief Tag for the NPSSO, the sign-in seed PSN issues after a web sign-in.
struct NpssoKind {
  /// @brief Name used for the credential in diagnostics.
  static constexpr std::string_view label = "npsso";
  /// @brief Sent as the `npsso` cookie.
  static constexpr CredentialPlacement placement{"Cookie", "npsso="};
  /// @brief The longest NPSSO accepted, in bytes.
  static constexpr std::size_t maximum_bytes = 4096;
  /// @brief Recorded in diagnostics as the sign-in cookie.
  static constexpr platform::HttpCredential credential =
      platform::HttpCredential::sign_in_cookie;
};

/// @brief Tag for an OAuth authorisation code, which is exchanged for tokens
/// and never sent as a header.
struct AuthCodeKind {
  /// @brief Name used for the credential in diagnostics.
  static constexpr std::string_view label = "auth code";
  /// @brief The longest code accepted; see token_maximum_bytes.
  static constexpr std::size_t maximum_bytes = token_maximum_bytes;
  /// @brief Recorded in diagnostics as a sign-in grant.
  static constexpr platform::HttpCredential credential =
      platform::HttpCredential::sign_in_grant;
};

/// @brief Tag for a `JSESSIONID` session cookie.
struct SessionCookieKind {
  /// @brief Name used for the credential in diagnostics.
  static constexpr std::string_view label = "session cookie";
  /// @brief Sent as the `JSESSIONID` cookie.
  static constexpr CredentialPlacement placement{"Cookie", "JSESSIONID="};
  /// @brief Recorded in diagnostics as a store session.
  static constexpr platform::HttpCredential credential =
      platform::HttpCredential::store_session;
};

/// @brief Tag for the app token, which authorises the store, profile and
/// library calls.
struct AppTokenKind {
  /// @brief Name used for the credential in diagnostics.
  static constexpr std::string_view label = "app token";
  /// @brief Sent as an `Authorization: Bearer` header.
  static constexpr CredentialPlacement placement{"Authorization", "Bearer "};
  /// @brief The longest token accepted; see token_maximum_bytes.
  static constexpr std::size_t maximum_bytes = token_maximum_bytes;
  /// @brief Recorded in diagnostics as an account token.
  static constexpr platform::HttpCredential credential =
      platform::HttpCredential::account_token;
};

/// @brief The Remote Play access token. Secret; held in wiping memory.
using BearerToken = Credential<BearerKind>;
/// @brief The NPSSO sign-in seed. Secret; held in wiping memory.
using Npsso = Credential<NpssoKind>;
/// @brief An OAuth authorisation code. Secret; held in wiping memory.
using AuthCode = Credential<AuthCodeKind>;
/// @brief A session cookie. Secret; held in wiping memory.
using SessionCookie = Credential<SessionCookieKind>;
/// @brief The app access token. Secret; held in wiping memory.
using AppToken = Credential<AppTokenKind>;

}  // namespace libchiaking::users
