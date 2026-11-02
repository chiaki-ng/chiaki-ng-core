/// @file
/// @brief Declares Request, an HTTP request built against an Endpoint and sent
/// through Client.
#pragma once

#include "src/core/net/transport/http/credential.hpp"
#include "src/core/net/transport/http/endpoint.hpp"
#include "src/core/net/transport/http/url.hpp"

#include <libchiaking/detail/secret_string.hpp>
#include <libchiaking/platform/http/provider.hpp>

#include <concepts>
#include <span>
#include <string_view>
#include <vector>

namespace libchiaking::net::http {

/// @brief A credential type that a request can carry, whose kind says what
/// diagnostics call it.
///
/// @tparam T A type offering `label()`, `secret()` and a `kind` whose static
///     `credential` names the platform::HttpCredential it is recorded as.
template <class T>
concept CarriedCredential = requires(const T& credential) {
  { credential.label() } -> std::convertible_to<std::string_view>;
  { credential.secret() } -> std::same_as<const SecretString&>;
  { T::kind::credential } -> std::convertible_to<platform::HttpCredential>;
};

/// @brief A credential type that can be attached to an HTTP request as a
/// header.
///
/// @tparam T A CarriedCredential whose `kind` also has a static `placement`
///     that names the header and prefix.
template <class T>
concept HeaderCredential = CarriedCredential<T> && requires {
  { T::kind::placement } -> std::convertible_to<CredentialPlacement>;
};

class Client;

/// @brief An HTTP request being built: endpoint, URL, headers, body and the
/// credentials it carries.
///
/// The URL, header values and body are held as secrets and are moved, not
/// copied, into the platform request when sent. The request records which
/// credential kinds it carries and the credential's label for diagnostics,
/// never the secret. Redirects are never followed. Movable; built and sent on
/// one thread.
class Request {
 public:
  /// @brief Starts a request to a secret URL.
  ///
  /// @param endpoint The operation; copied.
  /// @param url The full URL.
  Request(const Endpoint& endpoint, SecretString url);
  /// @brief Starts a request to a URL that holds no secret.
  ///
  /// @param endpoint The operation; copied.
  /// @param url The full URL; copied into a secret string.
  Request(const Endpoint& endpoint, std::string_view url);
  /// @brief Starts a request to a URL built with Query.
  ///
  /// @param endpoint The operation; copied.
  /// @param url The built URL.
  Request(const Endpoint& endpoint, Query url);

  /// @brief Adds a header.
  ///
  /// @param name The header name.
  /// @param value The value; copied into a secret string.
  /// @return This request.
  auto header(std::string_view name, std::string_view value) -> Request&;
  /// @brief Adds a header with a secret value.
  ///
  /// @param name The header name.
  /// @param value The value.
  /// @return This request.
  auto header(std::string_view name, SecretString value) -> Request&;

  /// @brief Adds a credential as the header its kind is placed in, such as
  /// `Authorization: Bearer <token>`, and records the credential.
  ///
  /// @tparam C A credential type with a header placement.
  /// @param credential The credential; its secret is copied into the header.
  /// @return This request.
  template <HeaderCredential C>
  auto authorize(const C& credential) -> Request& {
    constexpr CredentialPlacement placement = C::kind::placement;
    SecretString value;
    value.reserve(placement.prefix.size() + credential.secret().size());
    value.append(placement.prefix);
    value.append(credential.secret().view());
    credential_ = credential.label();
    credentials_ = credentials_.with(C::kind::credential);
    return header(placement.header, std::move(value));
  }

  /// @brief Records a credential the request carries some other way, such as
  /// in
  /// its URL or body, so diagnostics can name and scrub it.
  ///
  /// @tparam C A credential type.
  /// @param credential The credential; its secret is cloned for scrubbing.
  /// @return This request.
  template <CarriedCredential C>
  auto carries(const C& credential) -> Request& {
    credential_ = credential.label();
    credentials_ = credentials_.with(C::kind::credential);
    carried_.push_back(credential.secret().clone());
    return *this;
  }

  /// @brief Sets the credential label shown in logs.
  ///
  /// @param label The label; must outlive the request.
  /// @return This request.
  auto credential_label(std::string_view label) noexcept -> Request&;
  /// @brief Sets the body without a content type.
  ///
  /// @param body The body.
  /// @return This request.
  auto body(SecretBytes body) noexcept -> Request&;
  /// @brief Sets the body and adds its `Content-Type` header.
  ///
  /// @param content_type The media type.
  /// @param body The body.
  /// @return This request.
  auto body(std::string_view content_type, SecretBytes body) -> Request&;
  /// @brief Sets a JSON body.
  ///
  /// @param body The JSON text; copied.
  /// @return This request.
  auto json(std::string_view body) -> Request&;
  /// @brief Sets a JSON body that holds a secret.
  ///
  /// @param body The JSON text; copied into secret bytes.
  /// @return This request.
  auto json(const SecretString& body) -> Request&;
  /// @brief Sets a form-encoded body.
  ///
  /// @param form The built form.
  /// @return This request.
  auto form(Form form) -> Request&;

  /// @brief The operation.
  ///
  /// @return The endpoint copy.
  auto endpoint() const noexcept -> const Endpoint& { return endpoint_; }
  /// @brief The URL.
  ///
  /// @return A view valid until the request is sent.
  auto url() const noexcept -> std::string_view { return url_.view(); }
  /// @brief The headers added so far.
  ///
  /// @return A view valid until the request changes or is sent.
  auto headers() const noexcept -> std::span<const platform::HttpHeader> {
    return headers_;
  }
  /// @brief The body.
  ///
  /// @return A view valid until the request changes or is sent; empty when no
  ///     body was set.
  auto body() const noexcept -> std::string_view { return body_.view(); }
  /// @brief The label of the credential the request carries.
  ///
  /// @return The label; empty when none.
  auto credential_label() const noexcept -> std::string_view {
    return credential_;
  }
  /// @brief The kinds of credential the request carries.
  ///
  /// @return The set of kinds.
  auto credentials() const noexcept -> platform::HttpCredentials {
    return credentials_;
  }

 private:
  /// @brief The client caps the size limit and releases the request when
  /// sending.
  friend class Client;

  /// @brief The `Content-Type` header name.
  static constexpr std::string_view content_type_header = "Content-Type";
  /// @brief The JSON media type.
  static constexpr std::string_view json_content_type = "application/json";
  /// @brief The form media type.
  static constexpr std::string_view form_content_type =
      "application/x-www-form-urlencoded";

  /// @brief Moves the request into the platform's form.
  ///
  /// The total timeout is at least 1 ms; the connect timeout is at least 1 ms
  /// and at most the total. Redirects are denied.
  ///
  /// @return The platform request.
  auto release() && -> platform::HttpRequest;

  /// @brief The operation.
  Endpoint endpoint_;
  /// @brief The URL.
  SecretString url_;
  /// @brief The headers, in the order added.
  std::vector<platform::HttpHeader> headers_;
  /// @brief The body; empty when none.
  SecretBytes body_;
  /// @brief The credential label for logs.
  std::string_view credential_;
  /// @brief The kinds of credential carried.
  platform::HttpCredentials credentials_;
  /// @brief Clones of credentials carried outside headers, for scrubbing.
  std::vector<SecretString> carried_;
};

}  // namespace libchiaking::net::http
