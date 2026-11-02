/// @file
/// @brief Declares UserSettings, the PSN settings service's per-account
/// switches, read with the app token.
#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/auth/credentials.hpp"
#include "src/core/psn/rest/error.hpp"

#include <libchiaking/detail/expected.hpp>

#include <string_view>

namespace libchiaking::psn::rest {

/// @brief Reads the account's PSN settings with the app token.
///
/// Holds a reference to the HTTP client, which must outlive it, and the token.
/// Each call blocks for one round trip. Not for concurrent use.
class UserSettings {
 public:
  /// @brief The failure of switched_on().
  using SwitchedOnError =
      Error<bad_reply::InvalidJson, bad_reply::InvalidSchema>;
  /// @brief The setting a PS5 reads to decide whether captures taken in a
  /// cloud game are downloaded to the device streaming it.
  static constexpr std::string_view cloud_capture_download =
      "cronosugcautodownloadtomediagallery";

  /// @brief Prepares the request.
  ///
  /// @param client The client the request goes through.
  /// @param token The app token.
  UserSettings(net::http::Client& client, auth::BearerToken token) noexcept;

  /// @brief Reads one on/off setting.
  ///
  /// GET
  /// `https://m.np.playstation.com/api/settings/v2/users/me/settings?settings=<name>`.
  /// The reply is an array whose first object holds `value`, or `default`
  /// when the account never chose; either is `on` or `off`.
  ///
  /// @param name The setting.
  /// @return Whether it is on.
  /// @retval bad_reply::InvalidSchema The reply holds neither field.
  /// @retval Error A transport failure, an error status or an unreadable
  ///     reply.
  [[nodiscard]] auto switched_on(std::string_view name)
      -> Expected<bool, SwitchedOnError>;

 private:
  /// @brief The account's settings.
  static constexpr std::string_view settings_url =
      "https://m.np.playstation.com/api/settings/v2/users/me/settings";

  /// @brief The request, logged with its query.
  static constexpr net::http::Endpoint setting_endpoint{
      .label = "psn.settings.setting",
      .log_query = true,
  };

  /// @brief The client the request goes through.
  net::http::Client& client_;
  /// @brief The app token.
  auth::BearerToken token_;
};

}  // namespace libchiaking::psn::rest
