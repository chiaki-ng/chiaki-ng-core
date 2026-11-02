/// @file
/// @brief Declares AccountsClient, the PSN accounts service's attribute check
/// that the LegacyCloudStream checkout makes before it lets a subscription be
/// bought.
///
/// LegacyCloudStream requires the account to have filled in certain attributes
/// and privacy settings. The accounts service answers a validation request
/// either with success or with the list of attributes still missing, which the
/// user must complete on PS's site.
#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/auth/credentials.hpp"

#include <libchiaking/detail/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace libchiaking::psn::rest {

/// @brief The diagnostic label of the attribute validation request.
inline constexpr std::string_view accounts_validate_attributes_label =
    "psn.accounts.validate_attributes";

/// @brief A failed attribute validation: either the request did not complete,
/// or the service answered with a status other than 200 or 204, possibly
/// naming the missing attributes. An immutable value.
class AccountsError {
 public:
  /// @brief Wraps a failure to send.
  ///
  /// @param failure The client's failure.
  explicit AccountsError(net::http::Failure failure) noexcept
      : failure_(failure) {}
  /// @brief Wraps a refusal.
  ///
  /// @param http_status The reply's status.
  /// @param missing_elements The attributes the reply named as missing; may
  ///     be empty.
  AccountsError(std::uint16_t http_status,
                std::vector<std::string> missing_elements) noexcept
      : http_status_(http_status),
        missing_elements_(std::move(missing_elements)) {}

  /// @brief Reads the failure to send.
  ///
  /// @return It, or nothing when the service answered.
  auto failure() const noexcept -> const std::optional<net::http::Failure>& {
    return failure_;
  }
  /// @brief Reads the refusal's status.
  ///
  /// @return It, or 0 when the request did not complete.
  auto http_status() const noexcept -> std::uint16_t { return http_status_; }
  /// @brief Tells whether the service refused the token.
  ///
  /// @return True when it answered 401 or 403.
  auto auth_refused() const noexcept -> bool;
  /// @brief Reads the attributes the account still lacks.
  ///
  /// @return The `name` of every `error.validationErrors[].missingElements[]`
  ///     entry, in reply order; empty when none was named.
  auto missing_elements() const noexcept -> const std::vector<std::string>& {
    return missing_elements_;
  }
  /// @brief Tells whether the user must complete the account before
  /// LegacyCloudStream can be bought.
  ///
  /// @return True when any attribute was named missing.
  auto privacy_upgrade_required() const noexcept -> bool {
    return !missing_elements_.empty();
  }

 private:
  /// @brief The failure to send; nothing when the service answered.
  std::optional<net::http::Failure> failure_;
  /// @brief The refusal's status; 0 for a failure to send.
  std::uint16_t http_status_{};
  /// @brief The missing attributes.
  std::vector<std::string> missing_elements_;
};

/// @brief A successful validation. Plain data.
struct AttributesAccepted {
  /// @brief The reply's status, 200 or 204.
  std::uint16_t http_status{};
};

/// @brief Validates the account's attributes for the LegacyCloudStream
/// checkout.
///
/// Owns a copy of the HTTP client (whose provider must outlive it). Each call
/// blocks for one round trip. Not for concurrent use.
class AccountsClient {
 public:
  /// @brief Prepares the request.
  ///
  /// @param client The client the request goes through.
  /// @param user_agent The user agent to send; none when empty; must outlive
  ///     this object.
  explicit AccountsClient(net::http::Client client,
                          std::string_view user_agent = {}) noexcept;

  /// @brief Asks whether the account has every attribute LegacyCloudStream
  /// requires.
  ///
  /// POST
  /// `https://accounts.api.playstation.com/api/v2/accounts/me/attributes`
  /// with `{"attributes":[...]}` naming the online id, birth date, city, real
  /// name and eight privacy settings, and the LegacyCloudStream checkout token
  /// as a bearer token.
  ///
  /// @param bearer The LegacyCloudStream checkout token.
  /// @return The status, when the service answered 200 or 204.
  /// @retval AccountsError A failure to send (an invalid request when the
  ///     body could not be written), or any other status with the missing
  ///     attributes the body names.
  [[nodiscard]] auto validate_attributes(const auth::BearerToken& bearer)
      -> Expected<AttributesAccepted, AccountsError>;

 private:
  /// @brief The account attributes endpoint.
  static constexpr std::string_view attributes_url =
      "https://accounts.api.playstation.com/api/v2/accounts/me/attributes";

  /// @brief The validation request.
  static constexpr net::http::Endpoint validate_attributes_endpoint{
      .label = accounts_validate_attributes_label,
      .method = net::http::Method::post,
  };

  /// @brief The attributes validated, in the order sent.
  static constexpr std::array validated_attributes{
      std::string_view{"ONLINE_ID"},
      std::string_view{"BIRTH_DATE"},
      std::string_view{"CITY"},
      std::string_view{"REAL_NAME"},
      std::string_view{"PRIVACY_SETTING_ACTIVITYSTREAM"},
      std::string_view{"PRIVACY_SETTING_FRIENDSLIST"},
      std::string_view{"PRIVACY_SETTING_FRIENDREQUESTS"},
      std::string_view{"PRIVACY_SETTING_MESSAGES"},
      std::string_view{"PRIVACY_SETTING_TRUENAME"},
      std::string_view{"PRIVACY_SETTING_SEARCH"},
      std::string_view{"PRIVACY_SETTING_RECOMMENDUSERS"},
      std::string_view{"PRIVACY_SETTING_BROADCAST"},
  };

  /// @brief The validation request's body.
  struct AttributesBody;
  /// @brief The missing attributes a refusal names.
  struct Refusal;

  /// @brief Reads the missing attributes from a refusal.
  ///
  /// @param response The reply.
  /// @return The non-empty names, in reply order; empty when the body does
  ///     not name any or cannot be read.
  static auto missing_elements(const net::http::Response& response)
      -> std::vector<std::string>;

  /// @brief The client the request goes through.
  net::http::Client client_;
  /// @brief The user agent sent; none when empty.
  std::string_view user_agent_;
};

}  // namespace libchiaking::psn::rest
