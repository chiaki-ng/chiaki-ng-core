/// @file
/// @brief Declares the HTTP values a request carries: its method, headers and
/// the credentials it may hold.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

#include <libchiaking/detail/secret_string.hpp>

namespace libchiaking::platform {

/// @brief HTTP request methods the library uses.
enum class HttpMethod : std::uint8_t {
  /// @brief GET. The request body is not sent.
  get,
  /// @brief POST. HttpRequest::body is sent as the request body.
  post,
  /// @brief DELETE. HttpRequest::body is sent as the request body. The
  /// trailing underscore avoids the C++ keyword.
  delete_,
  /// @brief HEAD. No request body is sent and no response body is read.
  head,
  /// @brief PUT. HttpRequest::body is sent as the request body.
  put,
  /// @brief PATCH. HttpRequest::body is sent as the request body.
  patch,
};

/// @brief Returns the method as it appears on the request line.
///
/// @param method The method to name.
/// @return "GET", "POST", "DELETE", "HEAD", "PUT" or "PATCH" as a static
///     string.
constexpr auto http_method_name(HttpMethod method) noexcept -> const char* {
  switch (method) {
    case HttpMethod::get:
      return "GET";
    case HttpMethod::post:
      return "POST";
    case HttpMethod::delete_:
      return "DELETE";
    case HttpMethod::head:
      return "HEAD";
    case HttpMethod::put:
      return "PUT";
    case HttpMethod::patch:
      return "PATCH";
  }
  return "GET";
}

/// @brief One request or response header.
///
/// The value is a SecretString because headers carry bearer tokens and cookies;
/// it is wiped when the header is destroyed. A header can therefore be moved
/// but not copied; use clone() for a deliberate copy.
struct HttpHeader {
  /// @brief Header name without the colon. Providers reject a request whose
  /// header name is not an HTTP token.
  std::string name;
  /// @brief Header value. It must contain no carriage return, line feed or
  /// NUL.
  SecretString value;

  /// @brief Makes an independent copy of the header, including its secret
  /// value.
  ///
  /// @return A header with the same name and a separately wiped copy of the
  ///     value.
  auto clone() const -> HttpHeader { return {name, value.clone()}; }
};

/// @brief Copies a header list, preserving order and duplicates.
///
/// @param headers The headers to copy.
/// @return An independent copy; each value is duplicated into its own wiping
///     storage.
inline auto clone(const std::vector<HttpHeader>& headers)
    -> std::vector<HttpHeader> {
  std::vector<HttpHeader> copies;
  copies.reserve(headers.size());
  std::ranges::transform(headers, std::back_inserter(copies),
                         &HttpHeader::clone);
  return copies;
}

/// @brief A kind of credential the library attaches to a request.
///
/// The kind is recorded by the library at the place it attaches the credential,
/// so it says what was sent rather than what a header looks like. Providers do
/// not need to read it.
enum class HttpCredential : std::uint8_t {
  /// @brief A PlayStation Network access token sent as an
  /// `Authorization: Bearer` header, or in the URL of the token lookup.
  account_token,
  /// @brief The NPSSO sign-in cookie.
  sign_in_cookie,
  /// @brief A cloud streaming session identifier or configuration key, sent
  /// in the `X-Gaikai-SessionId` or `X-Gaikai-Session` header.
  gk_session,
  /// @brief The client identifier and secret of a PS sign-in client, sent
  /// as an `Authorization: Basic` header.
  client_credentials,
  /// @brief The `JSESSIONID` cookie of a PlayStation Now store session.
  store_session,
  /// @brief An authorisation code or refresh token sent in a form body to be
  /// exchanged for tokens.
  sign_in_grant,
};

/// @brief Every HttpCredential, in declaration order.
inline constexpr std::array every_http_credential{
    HttpCredential::account_token, HttpCredential::sign_in_cookie,
    HttpCredential::gk_session,    HttpCredential::client_credentials,
    HttpCredential::store_session, HttpCredential::sign_in_grant};

/// @brief The set of credential kinds one request carries.
///
/// An empty set means the request was sent anonymously.
class HttpCredentials {
 public:
  /// @brief The empty set: an anonymous request.
  constexpr HttpCredentials() noexcept = default;

  /// @brief Adds one kind.
  ///
  /// @param credential The kind to add.
  /// @return This set with the kind included.
  [[nodiscard]] constexpr auto with(HttpCredential credential) const noexcept
      -> HttpCredentials {
    HttpCredentials more = *this;
    more.bits_ |= bit(credential);
    return more;
  }
  /// @brief Whether the set holds a kind.
  ///
  /// @param credential The kind to look for.
  /// @return True when the request carried a credential of that kind.
  constexpr auto contains(HttpCredential credential) const noexcept -> bool {
    return (bits_ & bit(credential)) != 0;
  }
  /// @brief Whether the request carried no credential at all.
  ///
  /// @return True for the empty set.
  constexpr auto anonymous() const noexcept -> bool { return bits_ == 0; }
  /// @brief Compares two sets by their members.
  friend constexpr auto operator==(HttpCredentials, HttpCredentials) noexcept
      -> bool = default;

 private:
  static constexpr auto bit(HttpCredential credential) noexcept
      -> std::uint8_t {
    return static_cast<std::uint8_t>(1U << static_cast<unsigned>(credential));
  }

  std::uint8_t bits_{};
};

}  // namespace libchiaking::platform
