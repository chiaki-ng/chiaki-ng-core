/// @file
/// @brief Declares UserProfile, the PSN user profile service, used to wake a
/// PS4 for an internet session.
///
/// A PS4 is not started through cloud assisted navigation as a PS5 is: the
/// client asks the application server map (`asm.np.community.playstation.net`)
/// for the user profile service's base URL, then posts a `remoteConsole/wakeUp`
/// for the user, which PSN relays to the PS4 with the session to join.
#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/auth/credentials.hpp"
#include "src/core/psn/rest/error.hpp"
#include "src/core/psn/rest/session_manager/session_manager.hpp"

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/detail/wire_sizes.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace libchiaking::psn::rest {

/// @brief Requests to the user profile service with the Remote Play token
/// (user agent `RpNetHttpUtilImpl`,
/// explicit `Host` and `Connection: Keep-Alive`, a JSON content type even on
/// the GET).
///
/// Holds a reference to the HTTP client, which must outlive it, and the token.
/// Every call blocks for one round trip and keeps the reply even if the stop
/// token fires mid-request. Not for concurrent use.
class UserProfile {
 public:
  /// @brief The failure of wake_ps4().
  using WakePs4Error =
      Error<bad_request::InvalidMessage, bad_request::InvalidUrl,
            bad_request::InvalidOnlineId>;
  /// @brief The failure of profile_base_url().
  using ProfileBaseUrlError =
      Error<bad_reply::TooLarge, bad_reply::TooDeep, bad_reply::InvalidJson,
            bad_reply::InvalidSchema, bad_reply::InvalidUrl>;
  /// @brief The failure of an exchange with send().
  using ExchangeError = Error<>;
  /// @brief Prepares the requests.
  ///
  /// @param client The client the requests go through.
  /// @param token The Remote Play token.
  UserProfile(net::http::Client& client, auth::BearerToken token) noexcept;

  /// @brief Finds where the user profile service lives for this user.
  ///
  /// GET
  /// `https://asm.np.community.playstation.net/asm/v1/apps/me/baseUrls/userProfile`;
  /// the reply's `url` is read in PS's wire dialect.
  ///
  /// @return The base URL, an HTTPS URL of at most 2048 bytes.
  /// @retval bad_reply::InvalidSchema The reply has no `url`.
  /// @retval bad_reply::InvalidUrl The URL is not HTTPS or has a query,
  ///     fragment or unsafe path.
  /// @retval Error A transport failure, an error status or a parse failure.
  [[nodiscard]] auto profile_base_url()
      -> Expected<std::string, ProfileBaseUrlError>;
  /// @brief Asks PSN to wake the user's PS4 and have it join a Remote Play
  /// session.
  ///
  /// POST `<base>/v1/users/<online_id>/remoteConsole/wakeUp?platform=PS4` with
  /// `{"data":{"clientType":"Windows","data1","data2","roomId":0,
  /// "protocolVer":"10.0","sessionId"},"dataTypeSuffix":"remotePlay"}`, the
  /// data in base64, and `Host` set to the base URL's authority.
  ///
  /// @param profile_base_url The base URL from profile_base_url(); must not
  ///     end in `/`.
  /// @param online_id The user's online id.
  /// @param session The session the PS4 should join.
  /// @param data1 The session's first custom data block.
  /// @param data2 The session's second custom data block.
  /// @return Nothing, on a 2xx reply.
  /// @retval bad_request::InvalidUrl The base URL is not acceptable.
  /// @retval bad_request::InvalidOnlineId The online id is not acceptable.
  /// @retval bad_request::InvalidMessage The body could not be written.
  /// @retval Error A transport failure or an error status.
  [[nodiscard]] auto wake_ps4(
      std::string_view profile_base_url, std::string_view online_id,
      const SessionId& session,
      std::span<const std::byte, psn_custom_data_size> data1,
      std::span<const std::byte, psn_custom_data_size> data2)
      -> Expected<void, WakePs4Error>;

 private:
  /// @brief The HTTP methods.
  using Method = net::http::Method;
  /// @brief The application server map's host, sent as `Host`.
  static constexpr std::string_view profile_host =
      "asm.np.community.playstation.net";
  /// @brief Where the user profile service's base URL is looked up.
  static constexpr std::string_view base_urls =
      "https://asm.np.community.playstation.net/asm/v1/apps/me/baseUrls/"
      "userProfile";
  /// @brief The content type every request sends.
  static constexpr std::string_view content_type =
      "application/json; charset=utf-8";
  /// @brief The user agent every request sends.
  static constexpr std::string_view user_agent = "RpNetHttpUtilImpl";
  /// @brief The only scheme a base URL may have.
  static constexpr std::string_view secure_scheme = "https";
  /// @brief The longest base URL accepted, in bytes.
  static constexpr std::size_t url_max_bytes = 2048;

  /// @brief The base URL lookup.
  static constexpr net::http::Endpoint profile_base_url_endpoint{
      .label = "psn.user_profile.profile_base_url",
      .keep_reply_after_stop = true,
  };

  /// @brief The wake request.
  static constexpr net::http::Endpoint wake_endpoint{
      .label = "psn.user_profile.wake_ps4",
      .method = Method::post,
      .keep_reply_after_stop = true,
  };

  /// @brief The base URL reply's `url`.
  struct BaseUrls;
  /// @brief The wake request's body.
  struct WakeBody;

  /// @brief Checks a base URL.
  ///
  /// @param url The URL.
  /// @return Its authority (host and port), a view into `url`; nothing when
  ///     it is longer than 2048 bytes, not HTTPS, or has a path that does not
  ///     start with `/` or holds a blank, control byte, `\`, `?` or `#`.
  [[nodiscard]] static auto base_url_authority(std::string_view url)
      -> std::optional<std::string_view>;

  /// @brief Writes the wake request's body.
  ///
  /// @param session The session.
  /// @param data1 The first custom data block.
  /// @param data2 The second custom data block.
  /// @return The body.
  /// @retval bad_request::InvalidMessage It could not be written.
  static auto wake_body(const SessionId& session,
                        std::span<const std::byte, psn_custom_data_size> data1,
                        std::span<const std::byte, psn_custom_data_size> data2)
      -> Expected<SecretBytes, Fault<bad_request::InvalidMessage>>;

  /// @brief Sends a request with the shared headers and accepts
  /// only a 2xx reply.
  ///
  /// @param endpoint The endpoint.
  /// @param url The URL.
  /// @param host The `Host` header.
  /// @param body The body; may be empty.
  /// @return The reply.
  /// @retval Error A transport failure or an error status, the token
  ///     redacted.
  [[nodiscard]] auto send(net::http::Endpoint endpoint, std::string_view url,
                          std::string_view host, SecretBytes body)
      -> Expected<net::http::Response, ExchangeError>;

  /// @brief The client the requests go through.
  net::http::Client& client_;
  /// @brief The Remote Play token.
  auth::BearerToken token_;
};

}  // namespace libchiaking::psn::rest
