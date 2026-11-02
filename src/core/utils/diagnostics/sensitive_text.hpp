/// @file
/// @brief Declares SensitiveText, the one place that decides which headers,
/// query parameters and JSON members carry secrets that logs and captures
/// must mask.
#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

namespace libchiaking::diagnostics {

/// @brief Recognises the names under which credentials travel in HTTP
/// traffic.
///
/// Every check ignores ASCII case. Stateless; all members are static and
/// safe to call from any thread.
class SensitiveText {
 public:
  /// @brief Tells whether an HTTP header carries a secret.
  ///
  /// @param name The header name.
  /// @return True for `Authorization`, `Proxy-Authorization`, `Cookie`,
  ///     `Set-Cookie` and every `X-Gaikai-` header.
  static auto header(std::string_view name) noexcept -> bool;
  /// @brief Tells whether a URL query or form parameter carries a secret.
  ///
  /// @param key The parameter name, as written; not percent-decoded.
  /// @return True for `code`, `access_token`, `refresh_token`, `npsso`,
  ///     `token`, `id_token`, `client_secret` and `password`.
  static auto param(std::string_view key) noexcept -> bool;
  /// @brief Where a sensitive parameter's value lies in a text.
  struct ParameterValue {
    /// @brief Where the value starts.
    std::size_t offset{};
    /// @brief Its length in bytes; 0 for an empty value.
    std::size_t length{};
  };
  /// @brief Finds the values of the sensitive parameters in a query, a
  /// fragment or a form body.
  ///
  /// The text is split at every `?`, `#` and `&`; a field is `name=value`,
  /// split at its first `=`, and its name is matched with param() as
  /// written, without percent-decoding. A field without `=` has no value.
  ///
  /// @param text The text.
  /// @param from Where to start; at or past the end finds nothing.
  /// @return The values of the fields param() accepts, in order.
  static auto parameter_values(std::string_view text, std::size_t from = 0)
      -> std::vector<ParameterValue>;
  /// @brief Tells whether a JSON member's value is a secret or personal
  /// data.
  ///
  /// Three rules, any of which masks:
  /// - the key contains `token`, `npsso`, `secret` or `password` anywhere,
  ///   so `accessToken`, `refresh_token`, `clientSecret` and `npsso` match;
  /// - the key is exactly `code` (an OAuth authorisation code),
  ///   `authorization_code`, `device_code` or `user_code`, in snake or
  ///   joined camel case;
  /// - the key ends in `key` (`skey`, `handshakeKey`, `configKey`,
  ///   `lockKey`, `session_key`, `apiKey`), in `authCode` or `auth_code`
  ///   (`gkCloudAuthCode`, `streamServerAuthCode`, `ps3AuthCode`), or in
  ///   `postalCode`, `postal_code`, `zipCode` or `zip_code`, which are
  ///   personal data.
  ///
  /// Other keys that merely contain `code` or `key` stay visible, among
  /// them `status_code`, `statusCode`, `errorCode`, `error_code`,
  /// `reasonCode`, `resultCode`, `countryCode`, `languageCode`,
  /// `currencyCode`, `regionCode`, `videoCodec` and `keyArt`.
  ///
  /// @param key The member's key.
  /// @return True when one of the rules matches.
  static auto json_key(std::string_view key) noexcept -> bool;

 private:
  /// @brief Tells whether text of any case ends with a lower-case word.
  ///
  /// @param value The text to test.
  /// @param expected The ending, in lower case.
  /// @return True when `value` ends with `expected` ignoring ASCII case.
  static auto ends_with_ignoring_case(std::string_view value,
                                      std::string_view expected) noexcept
      -> bool;
  /// @brief Searches for a lower-case word in text of any case.
  ///
  /// @param value The text to search.
  /// @param expected The word, in lower case.
  /// @return True when `value` contains `expected` ignoring ASCII case.
  static auto contains_ignoring_case(std::string_view value,
                                     std::string_view expected) noexcept
      -> bool;
};

}  // namespace libchiaking::diagnostics
