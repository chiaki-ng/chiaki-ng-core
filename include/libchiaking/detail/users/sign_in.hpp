/// @file
/// @brief Declares the refresh tokens a sign-in yields and SignInError, why a
/// sign-in failed.
#pragma once

#include "libchiaking/detail/users/credentials.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace libchiaking::users {

/// @brief Tag for the refresh token that renews the Remote Play token. Never
/// sent as a header.
struct RemotePlayRefreshKind {
  /// @brief Name used for the credential in diagnostics.
  static constexpr std::string_view label = "remote play refresh token";
  /// @brief The longest token accepted; see token_maximum_bytes.
  static constexpr std::size_t maximum_bytes = token_maximum_bytes;
  /// @brief Recorded in diagnostics as a sign-in grant.
  static constexpr platform::HttpCredential credential =
      platform::HttpCredential::sign_in_grant;
};

/// @brief Tag for the refresh token that renews the app token. Never sent as a
/// header.
struct AppRefreshKind {
  /// @brief Name used for the credential in diagnostics.
  static constexpr std::string_view label = "app refresh token";
  /// @brief The longest token accepted; see token_maximum_bytes.
  static constexpr std::size_t maximum_bytes = token_maximum_bytes;
  /// @brief Recorded in diagnostics as a sign-in grant.
  static constexpr platform::HttpCredential credential =
      platform::HttpCredential::sign_in_grant;
};

/// @brief The Remote Play refresh token. Secret; held in wiping memory.
using RemotePlayRefreshToken = Credential<RemotePlayRefreshKind>;
/// @brief The app refresh token. Secret; held in wiping memory.
using AppRefreshToken = Credential<AppRefreshKind>;

/// @brief The ways a sign-in exchange can fail.
enum class SignInErrorKind : std::uint8_t {
  /// @brief An argument was not usable. Nothing was sent.
  invalid_request,
  /// @brief The stop token was triggered.
  cancelled,
  /// @brief The request did not complete.
  transport,
  /// @brief PSN refused the credential, or asked for an interactive sign-in.
  /// The NPSSO or refresh token is no longer good.
  rejected,
  /// @brief PSN is rate limiting sign-in.
  rate_limited,
  /// @brief PSN answered with a status the exchange does not expect.
  unavailable,
  /// @brief PSN's reply could not be read.
  malformed,
  /// @brief The person refused the sign-in on the device that scanned the
  /// code.
  declined,
  /// @brief PSN kept asking for additional verification after the
  /// application token was renewed, until no replacement code was left.
  verification_required,
  /// @brief Every code issued for the sign-in expired or was invalidated
  /// before it was approved.
  expired,
  /// @brief PS's hosted sign-in page no longer has the shape the sign-in
  /// reads its registration from.
  layout_changed,
};

/// @brief Why a sign-in exchange failed, and at which step.
///
/// The error is a small value that holds views of fixed text, so it can be
/// copied freely and holds nothing secret.
class SignInError {
 public:
  /// @brief Creates an error.
  ///
  /// @param kind What went wrong.
  /// @param http_status The status of the reply behind the failure, or 0 when
  ///     there was none.
  /// @param cause A fuller explanation, or empty to use the kind's standard
  ///     one. It is not copied; pass a string literal.
  explicit constexpr SignInError(SignInErrorKind kind,
                                 std::uint16_t http_status = 0,
                                 std::string_view cause = {}) noexcept
      : kind_(kind), http_status_(http_status), cause_(cause) {}

  /// @brief What went wrong.
  ///
  /// @return The kind given at construction.
  constexpr auto kind() const noexcept -> SignInErrorKind { return kind_; }
  /// @brief The HTTP status of the reply behind the failure.
  ///
  /// @return The status, or 0 when no reply was received.
  constexpr auto http_status() const noexcept -> std::uint16_t {
    return http_status_;
  }
  /// @brief The name of the step of the exchange that failed.
  ///
  /// @return The step given to at(), or empty when none was recorded.
  constexpr auto step() const noexcept -> std::string_view { return step_; }

  /// @brief Returns a copy of the error tagged with the step it happened at.
  ///
  /// @param step The step's name. It is not copied; pass a string literal.
  /// @return The tagged copy.
  constexpr auto at(std::string_view step) const noexcept -> SignInError {
    auto located = *this;
    located.step_ = step;
    return located;
  }

  /// @brief An English explanation of the failure for diagnostics.
  ///
  /// @return The cause given at construction, or the kind's standard one
  ///     when none was given. It holds nothing secret.
  constexpr auto cause() const noexcept -> std::string_view {
    if (!cause_.empty()) return cause_;
    switch (kind_) {
      case SignInErrorKind::invalid_request:
        return "an argument is not usable";
      case SignInErrorKind::cancelled:
        return "the sign-in was cancelled";
      case SignInErrorKind::transport:
        return "the request did not complete";
      case SignInErrorKind::rejected:
        return "the server refused the credential";
      case SignInErrorKind::rate_limited:
        return "the server is rate limiting sign-in";
      case SignInErrorKind::unavailable:
        return "the server answered with an unexpected status";
      case SignInErrorKind::malformed:
        return "the reply could not be read";
      case SignInErrorKind::declined:
        return "the sign-in was declined on the approving device";
      case SignInErrorKind::verification_required:
        return "the server keeps asking for additional verification";
      case SignInErrorKind::expired:
        return "every sign-in code expired before it was approved";
      case SignInErrorKind::layout_changed:
        return "the hosted sign-in page changed its layout";
    }
    return "the request did not complete";
  }

  /// @brief The kind as an identifier for diagnostics.
  ///
  /// @return The identifier, such as "rate_limited".
  constexpr auto name() const noexcept -> std::string_view {
    switch (kind_) {
      case SignInErrorKind::invalid_request:
        return "invalid_request";
      case SignInErrorKind::cancelled:
        return "cancelled";
      case SignInErrorKind::transport:
        return "transport";
      case SignInErrorKind::rejected:
        return "rejected";
      case SignInErrorKind::rate_limited:
        return "rate_limited";
      case SignInErrorKind::unavailable:
        return "unavailable";
      case SignInErrorKind::malformed:
        return "malformed";
      case SignInErrorKind::declined:
        return "declined";
      case SignInErrorKind::verification_required:
        return "verification_required";
      case SignInErrorKind::expired:
        return "expired";
      case SignInErrorKind::layout_changed:
        return "layout_changed";
    }
    return "transport";
  }

 private:
  SignInErrorKind kind_;
  std::uint16_t http_status_;
  std::string_view cause_;
  std::string_view step_;
};

}  // namespace libchiaking::users
