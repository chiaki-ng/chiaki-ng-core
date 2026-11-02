/// @file
/// @brief Declares SessionClient, the PSN requests of a Remote Play internet
/// start made with the Remote Play token: the session manager session, the
/// push server address, waking the console (PS5 through cloud assisted
/// navigation, PS4 through the user profile service), and the holepunch
/// session messages.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/wire_sizes.hpp"
#include "libchiaking/platform/diagnostics/provider.hpp"
#include "libchiaking/platform/http/provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "src/core/net/deadline_stop_state.hpp"
#include "src/core/net/nat/holepunch/holepunch_codec.hpp"
#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/remote_play/signal_barriers.hpp"
#include "src/core/psn/rest/error.hpp"
#include "src/core/psn/rest/push/push.hpp"
#include "src/core/psn/rest/session_manager/session_manager.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace libchiaking::psn::remote_play {

/// @brief Why a Remote Play session request failed.
enum class SessionErrorKind : std::uint8_t {
  /// @brief The Remote Play token is empty, too long or holds an unsendable
  /// byte; nothing was sent.
  invalid_token,
  /// @brief The stop token fired.
  cancelled,
  /// @brief Random bytes for the push context could not be drawn.
  random_failure,
  /// @brief The request did not complete.
  transport_failure,
  /// @brief The service answered with an error status.
  http_status,
  /// @brief The reply could not be used.
  invalid_response,
  /// @brief The request could not be built, or was refused before sending.
  invalid_request,
};

/// @brief A Remote Play session request's failure. Plain data.
struct SessionError {
  /// @brief Why.
  SessionErrorKind kind{};
  /// @brief The HTTP provider's error, for transport_failure.
  platform::HttpError transport{platform::HttpError::backend_failure};
  /// @brief The error status, for http_status; 0 otherwise.
  std::uint16_t status{};
};

/// @brief How much of the session an observe request asks for.
using SessionView = psn::rest::SessionView;

/// @brief Makes the Remote Play start's PSN requests, each through a fresh
/// HTTP client on the host's provider with replies capped at the document
/// limit.
///
/// Every request checks the token and the stop token first and blocks for one
/// round trip. Holds references to the providers, which must outlive it, and
/// the command id of the last PS5 start for diagnostics.
///
/// One client is shared by the start, the PSN signalling thread and the data
/// route worker. Every request except start_ps5_session() and
/// diagnose_start_command() builds its own net::http::Client and only reads
/// the client's members, so those may run on several threads at once, relying
/// on the HTTP provider and the random source being safe for concurrent
/// calls. start_ps5_session() and diagnose_start_command() write the pending
/// start command, so they must not overlap each other; they may overlap the
/// other requests.
class SessionClient {
 public:
  /// @brief Checks a Remote Play token's form.
  ///
  /// @param token The token.
  /// @return True when auth::BearerToken admits it.
  static auto valid_token(const psn::auth::BearerToken& token) -> bool;
  /// @brief Wraps a Remote Play token for a request.
  ///
  /// @param token The token.
  /// @return A copy of it, labelled as a supplied PSN access token.
  static auto bearer(const psn::auth::BearerToken& token)
      -> psn::auth::BearerToken;

  /// @brief Binds the requests to the host's providers.
  ///
  /// @param http The HTTP provider; must outlive this object.
  /// @param clock The monotonic clock request deadlines are measured on. Must
  ///     outlive this object.
  /// @param random The secure random source; must outlive this object.
  /// @param diagnostics Where requests and PS5 start states are logged; may
  ///     be null.
  SessionClient(platform::HttpProvider& http,
                const platform::ClockProvider& clock,
                platform::SecureRandomProvider& random,
                platform::DiagnosticsProvider* diagnostics = nullptr) noexcept;

  /// @brief Creates the Remote Play session (SessionManager::
  /// create_remote_play_session()) with a fresh random push context.
  ///
  /// @param bearer_token The Remote Play token.
  /// @param stop Cancels the request.
  /// @return The session.
  /// @retval SessionErrorKind::invalid_token The token is not valid_token().
  /// @retval SessionErrorKind::random_failure No random bytes.
  /// @retval SessionError The request's failure.
  [[nodiscard]] auto create_session(const psn::auth::BearerToken& bearer_token,
                                    const platform::StopToken& stop)
      -> Expected<psn::rest::CreatedSession, SessionError>;

  /// @brief Asks the session manager about a session
  /// (SessionManager::observe_sessions()).
  ///
  /// @param bearer_token The Remote Play token.
  /// @param session_id The session.
  /// @param view How much to ask for.
  /// @param stop Cancels the request.
  /// @return Nothing, on a 2xx reply.
  /// @retval SessionErrorKind::invalid_request The session id is malformed.
  /// @retval SessionError The request's failure.
  [[nodiscard]] auto check_session(const psn::auth::BearerToken& bearer_token,
                                   std::string_view session_id,
                                   SessionView view,
                                   const platform::StopToken& stop)
      -> Expected<void, SessionError>;

  /// @brief Asks the session manager about a session while waiting for its
  /// creation; a failed request is logged and does not end the wait.
  ///
  /// @param bearer_token The Remote Play token.
  /// @param session_id The session.
  /// @param view How much to ask for.
  /// @param stop Cancels the request and ends the wait.
  /// @return Nothing, unless `stop` fired.
  /// @retval CreationWaitError::timed_out `stop` fired because its deadline
  ///     passed.
  /// @retval CreationWaitError::cancelled `stop` fired otherwise.
  [[nodiscard]] auto observe_session(const psn::auth::BearerToken& bearer_token,
                                     std::string_view session_id,
                                     SessionView view,
                                     const platform::StopToken& stop)
      -> Expected<void, CreationWaitError>;

  /// @brief Asks the PS5 to join the session
  /// (CloudAssistedNavigation::start_remote_play()), remembering the command
  /// id for diagnose_start_command().
  ///
  /// @param bearer_token The Remote Play token.
  /// @param created The session.
  /// @param device_uid The console's device unique id.
  /// @param data1 The session's first custom data block.
  /// @param data2 The session's second custom data block.
  /// @param stop Cancels the request.
  /// @return Nothing, when the command was accepted.
  /// @retval SessionErrorKind::invalid_request The session id is malformed or
  ///     a body could not be written.
  /// @retval SessionError The request's failure.
  [[nodiscard]] auto start_ps5_session(
      const psn::auth::BearerToken& bearer_token,
      const psn::rest::CreatedSession& created,
      std::span<const std::byte, psn_device_uid_size> device_uid,
      std::span<const std::byte, psn_custom_data_size> data1,
      std::span<const std::byte, psn_custom_data_size> data2,
      const platform::StopToken& stop) -> Expected<void, SessionError>;

  /// @brief Logs how the last PS5 start command for a session went, once,
  /// when there is a log: polls it once with a 2 s budget and logs its status
  /// and error code. Does nothing otherwise.
  ///
  /// @param bearer_token The Remote Play token.
  /// @param session_id The session the command was for.
  /// @param stop Cancels the poll.
  void diagnose_start_command(const psn::auth::BearerToken& bearer_token,
                              std::string_view session_id,
                              const platform::StopToken& stop = {});

  /// @brief Finds the user profile service a PS4 is woken through
  /// (UserProfile::profile_base_url()).
  ///
  /// @param bearer_token The Remote Play token.
  /// @param stop Cancels the request.
  /// @return The base URL.
  /// @retval SessionError The request's failure.
  [[nodiscard]] auto get_ps4_profile_base_url(
      const psn::auth::BearerToken& bearer_token,
      const platform::StopToken& stop) -> Expected<std::string, SessionError>;

  /// @brief Finds the push server (rest::Push::server_address()).
  ///
  /// @param bearer_token The Remote Play token.
  /// @param stop Cancels the request.
  /// @return The push server host.
  /// @retval SessionError The request's failure.
  [[nodiscard]] auto get_push_fqdn(const psn::auth::BearerToken& bearer_token,
                                   const platform::StopToken& stop)
      -> Expected<std::string, SessionError>;

  /// @brief Asks PSN to wake the PS4 and have it join the session
  /// (UserProfile::wake_ps4()).
  ///
  /// @param bearer_token The Remote Play token.
  /// @param session_id The session.
  /// @param profile_base_url The base URL from get_ps4_profile_base_url().
  /// @param online_id The user's online id.
  /// @param data1 The session's first custom data block.
  /// @param data2 The session's second custom data block.
  /// @param stop Cancels the request.
  /// @return Nothing, on a 2xx reply.
  /// @retval SessionErrorKind::invalid_request The session id, URL or online
  ///     id is not acceptable.
  /// @retval SessionError The request's failure.
  [[nodiscard]] auto wake_ps4_session(
      const psn::auth::BearerToken& bearer_token, std::string_view session_id,
      std::string_view profile_base_url, std::string_view online_id,
      std::span<const std::byte, psn_custom_data_size> data1,
      std::span<const std::byte, psn_custom_data_size> data2,
      const platform::StopToken& stop) -> Expected<void, SessionError>;

  /// @brief Leaves the session (SessionManager::leave_session()).
  ///
  /// @param bearer_token The Remote Play token.
  /// @param session_id The session.
  /// @param stop Cancels the request.
  /// @return Nothing, on a 2xx reply.
  /// @retval SessionErrorKind::invalid_request The session id is malformed.
  /// @retval SessionError The request's failure.
  [[nodiscard]] auto release_session(const psn::auth::BearerToken& bearer_token,
                                     std::string_view session_id,
                                     const platform::StopToken& stop)
      -> Expected<void, SessionError>;

  /// @brief Sends the console a holepunch message without a connection
  /// request, such as an acknowledgement (SessionManager::
  /// send_session_message()).
  ///
  /// @param bearer_token The Remote Play token.
  /// @param session_id The session.
  /// @param action The holepunch action.
  /// @param req_id The request id it answers or makes.
  /// @param error The error code, 0 for none.
  /// @param account_id The recipient's account id.
  /// @param console_uid The console's device unique id.
  /// @param family The console's generation.
  /// @param stop Cancels the request.
  /// @return Nothing, on a 2xx reply.
  /// @retval SessionErrorKind::invalid_request The session id is malformed or
  ///     the message too large.
  /// @retval SessionError The request's failure.
  [[nodiscard]] auto send_short_message(
      const psn::auth::BearerToken& bearer_token, std::string_view session_id,
      net::nat::holepunch::Action action, std::uint16_t req_id,
      std::uint16_t error, std::int64_t account_id,
      std::span<const std::byte, psn_device_uid_size> console_uid,
      psn::rest::ConsoleFamily family, const platform::StopToken& stop)
      -> Expected<void, SessionError>;

  /// @brief Sends the console a holepunch message with a connection request:
  /// the client's candidate addresses for a route.
  ///
  /// @param bearer_token The Remote Play token.
  /// @param session_id The session.
  /// @param action The holepunch action.
  /// @param req_id The request id.
  /// @param error The error code, 0 for none.
  /// @param conn_request The connection request.
  /// @param account_id The recipient's account id.
  /// @param console_uid The console's device unique id.
  /// @param family The console's generation.
  /// @param stop Cancels the request.
  /// @return Nothing, on a 2xx reply.
  /// @retval SessionErrorKind::invalid_request The session id is malformed or
  ///     the message could not be written.
  /// @retval SessionError The request's failure.
  [[nodiscard]] auto send_full_message(
      const psn::auth::BearerToken& bearer_token, std::string_view session_id,
      net::nat::holepunch::Action action, std::uint16_t req_id,
      std::uint16_t error, const net::nat::holepunch::ConnRequest& conn_request,
      std::int64_t account_id,
      std::span<const std::byte, psn_device_uid_size> console_uid,
      psn::rest::ConsoleFamily family, const platform::StopToken& stop)
      -> Expected<void, SessionError>;

 private:
  /// @brief The largest reply accepted, in bytes.
  static constexpr std::size_t max_response_size = json::max_document_bytes;
  /// @brief The diagnostic name of the token.
  static constexpr std::string_view supplied_token_label =
      "psn access token (supplied)";

  /// @brief Makes a failure.
  ///
  /// @param kind Why.
  /// @return The failure.
  static auto fail(SessionErrorKind kind) -> Unexpected<SessionError>;
  /// @brief Translates a REST call's transport failure.
  ///
  /// @param failure The failure.
  /// @return cancelled or transport_failure.
  static auto session_error(const net::http::Failure& failure) -> SessionError;
  /// @brief Translates a REST failure.
  ///
  /// @tparam Reasons The call's reasons.
  /// @param error The failure.
  /// @return cancelled or transport_failure for a failure to send;
  ///     http_status; invalid_request; invalid_response.
  template <psn::rest::ReasonTag... Reasons>
  static auto session_error(const psn::rest::Error<Reasons...>& error)
      -> SessionError {
    switch (error.cause()) {
      case psn::rest::Cause::failure:
        return session_error(*error.failure());
      case psn::rest::Cause::status:
        return SessionError{SessionErrorKind::http_status,
                            platform::HttpError::backend_failure,
                            error.http_status()};
      case psn::rest::Cause::request:
        return SessionError{SessionErrorKind::invalid_request};
      case psn::rest::Cause::response:
        break;
    }
    return SessionError{SessionErrorKind::invalid_response};
  }

  /// @brief Makes a refusal to send.
  ///
  /// @return invalid_request.
  static auto unsendable() -> SessionError {
    return SessionError{SessionErrorKind::invalid_request};
  }
  /// @brief Passes on an outcome that is already translated.
  ///
  /// @tparam T The value type, possibly void.
  /// @param result The outcome.
  /// @return It.
  template <class T>
  static auto mapped(Expected<T, SessionError> result)
      -> Expected<T, SessionError> {
    return result;
  }
  /// @brief Translates a REST outcome.
  ///
  /// @tparam T The value type, possibly void.
  /// @tparam Reasons The call's reasons.
  /// @param result The outcome.
  /// @return The value, or the failure session_error() gives.
  template <class T, psn::rest::ReasonTag... Reasons>
  static auto mapped(Expected<T, psn::rest::Error<Reasons...>> result)
      -> Expected<T, SessionError> {
    if (!result) return unexpected(session_error(result.error()));
    if constexpr (std::is_void_v<T>)
      return {};
    else
      return std::move(result.value());
  }
  /// @brief Checks a request may be sent.
  ///
  /// @param token The Remote Play token.
  /// @param stop The stop token.
  /// @return invalid_token or cancelled, or nothing when it may be sent.
  static auto refused(const psn::auth::BearerToken& token,
                      const platform::StopToken& stop)
      -> std::optional<SessionError>;
  /// @brief Makes an HTTP client for one request.
  ///
  /// @param stop Cancels it.
  /// @return A client capped at max_response_size.
  auto http_for(const platform::StopToken& stop) const -> net::http::Client;

  /// @brief Checks the token and stop, then runs a request with an HTTP
  /// client and the token.
  ///
  /// @tparam Use Callable as `use (net::http::Client&,
  ///     psn::auth::BearerToken)` returning `Expected<T,
  ///     psn::rest::Error<...>>`.
  /// @param token The Remote Play token.
  /// @param stop Cancels the request.
  /// @param use Makes the request.
  /// @return Its outcome, translated.
  template <class Use>
  auto with_session(const psn::auth::BearerToken& token,
                    const platform::StopToken& stop, Use&& use)
      -> decltype(mapped(
          std::forward<Use>(use)(std::declval<net::http::Client&>(),
                                 std::declval<psn::auth::BearerToken>()))) {
    if (auto error = refused(token, stop)) return unexpected(*error);
    auto http = http_for(stop);
    return mapped(std::forward<Use>(use)(http, bearer(token)));
  }

  /// @brief The last PS5 start command and the session it was for; secret.
  class PendingCommand {
   public:
    /// @brief Remembers a command.
    ///
    /// @param session The session id.
    /// @param command The command id.
    explicit PendingCommand(std::string_view session, SecretString command)
        : session_(session), command_(std::move(command)) {}
    /// @brief Tells whether the command was for a session.
    ///
    /// @param session The session id.
    /// @return True when it was.
    auto matches(std::string_view session) const noexcept -> bool {
      return session_.view() == session;
    }
    /// @brief Reads the command id.
    ///
    /// @return The id.
    auto command() const noexcept -> std::string_view {
      return command_.view();
    }

   private:
    /// @brief The session id.
    SecretString session_;
    /// @brief The command id.
    SecretString command_;
  };
  /// @brief The last PS5 start command, until diagnosed.
  std::optional<PendingCommand> pending_command_;
  /// @brief The host's HTTP provider.
  platform::HttpProvider& http_;
  /// @brief The monotonic clock request deadlines are measured on.
  const platform::ClockProvider& clock_;
  /// @brief The host's secure random source.
  platform::SecureRandomProvider& random_;
  /// @brief Where requests are logged; may be null.
  platform::DiagnosticsProvider* diagnostics_{};
};

}  // namespace libchiaking::psn::remote_play
