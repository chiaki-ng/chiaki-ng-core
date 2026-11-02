/// @file
/// @brief Declares Authorization, which sends one OAuth authorize request to
/// PS's account server, signed with the NPSSO cookie.
///
/// Every PSN credential the library derives from a sign-in (commerce tokens, PS
/// Now codes, GK codes, the app and Remote Play codes) starts as a GET to
/// `ca.account.sony.com` carrying the `npsso` cookie and `prompt=none`,
/// answered by a 302 whose `Location` holds the code or token. This class owns
/// that request's shape; Authenticator and SignIn build the URLs and read the
/// redirects.
#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/auth/credentials.hpp"

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/platform/diagnostics/provider.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace libchiaking::psn::auth {

/// @brief Sends authorize requests to the account server and logs them under
/// the `[AUTH]` prefix.
///
/// It refers to an HTTP client and holds nothing else; it must not outlive the
/// client. It is as thread-safe as the client.
class Authorization {
 public:
  /// @brief The order of the `User-Agent` and `Cookie` headers.
  enum class HeaderOrder : std::uint8_t {
    /// @brief The `npsso` cookie, then the user agent.
    cookie_first,
    /// @brief The user agent, then the `npsso` cookie.
    user_agent_first,
  };

  /// @brief Why request() sent nothing or got no reply.
  enum class Fault : std::uint8_t {
    /// @brief The NPSSO is not usable; nothing was sent.
    invalid_npsso,
    /// @brief The URL is not sendable(); nothing was sent.
    invalid_request,
    /// @brief The HTTP client's stop token fired.
    cancelled,
    /// @brief The request failed in transport.
    transport,
  };

  /// @brief The root of PS's account server API.
  static constexpr std::string_view account_api =
      "https://ca.account.sony.com/api";
  /// @brief The OAuth v3 authorize endpoint, used for PS5 cloud streaming
  /// codes and the sign-in codes.
  static constexpr std::string_view authorize_url =
      "https://ca.account.sony.com/api/authz/v3/oauth/authorize";
  /// @brief The `service_entity` every authorize request names: the PSN
  /// service.
  static constexpr std::string_view service_entity = "urn:service-entity:psn";

  static_assert(authorize_url.starts_with(account_api));

  /// @brief Binds the requests to an HTTP client.
  ///
  /// @param client The client; must outlive this object.
  explicit Authorization(net::http::Client& client) noexcept
      : client_(&client) {}

  /// @brief Sends a GET to an authorize URL with the NPSSO as the `npsso`
  /// cookie.
  ///
  /// Redirects are not followed; the caller reads the `Location` of the
  /// reply. Blocks for the request, within the client's timeouts and stop
  /// token.
  ///
  /// @param endpoint The endpoint the request is recorded under, with its
  ///     size limit.
  /// @param url The full URL with its query; secret, as it can carry the
  ///     DUID.
  /// @param npsso The sign-in seed.
  /// @param user_agent The `User-Agent` to send; none when empty.
  /// @param order Which of the two headers goes first.
  /// @return The reply, whatever its status.
  /// @retval Fault::invalid_npsso The NPSSO is unusable.
  /// @retval Fault::invalid_request The URL is not sendable().
  /// @retval Fault::cancelled The stop token fired.
  /// @retval Fault::transport The request failed otherwise.
  [[nodiscard]] auto request(const net::http::Endpoint& endpoint,
                             SecretString url, const Npsso& npsso,
                             std::string_view user_agent,
                             HeaderOrder order) const
      -> Expected<net::http::Response, Fault>;

  /// @brief Classifies a failed send.
  ///
  /// @param failure The client's failure.
  /// @return cancelled when the stop token fired; transport otherwise.
  static auto fault_of(const net::http::Failure& failure) noexcept -> Fault;
  /// @brief Tells whether a URL may be sent.
  ///
  /// @param url The URL.
  /// @return True when it is not empty, shorter than url_limit, printable
  ///     ASCII only and has no fragment.
  static auto sendable(std::string_view url) noexcept -> bool;

  /// @brief Logs a line through the client's log, prefixed `[AUTH]` and the
  /// label.
  ///
  /// @param severity The line's severity.
  /// @param label The endpoint or step the line is about.
  /// @param message The text; must hold no secret.
  void log(platform::LogSeverity severity, std::string_view label,
           std::string_view message) const;
  /// @brief Logs a failed step: at info when it was cancelled, at warning
  /// otherwise.
  ///
  /// @param label The endpoint or step that failed.
  /// @param cancelled Whether the failure was a cancellation.
  /// @param failure The failure's name.
  /// @param http_status The reply's status, 0 when none.
  /// @param detail Appended verbatim; must hold no secret.
  void report(std::string_view label, bool cancelled, std::string_view failure,
              std::uint16_t http_status, std::string_view detail) const;

 private:
  /// @brief The prefix of every line logged.
  static constexpr std::string_view log_prefix = "[AUTH] ";
  /// @brief One more than the longest URL sent, in bytes.
  static constexpr std::size_t url_limit = 2048;

  /// @brief The client requests and logs go through; never null.
  net::http::Client* client_;
};

}  // namespace libchiaking::psn::auth
