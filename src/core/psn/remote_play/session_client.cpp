#include "src/core/psn/remote_play/session_client.hpp"

#include "src/core/net/deadline_stop_state.hpp"
#include "src/core/net/psn_signalling_limits.hpp"
#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/auth/credentials.hpp"
#include "src/core/psn/rest/cloud_assisted_navigation/cloud_assisted_navigation.hpp"
#include "src/core/psn/rest/push/push.hpp"
#include "src/core/psn/rest/session_manager/session_manager.hpp"
#include "src/core/psn/rest/user_profile/user_profile.hpp"
#include "src/core/utils/json/json.hpp"
#include "src/core/utils/json/wire_dialect.hpp"
#include "src/core/utils/text/ascii.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace libchiaking::psn::remote_play {

SessionClient::SessionClient(
    platform::HttpProvider& http, const platform::ClockProvider& clock,
    platform::SecureRandomProvider& random,
    platform::DiagnosticsProvider* diagnostics) noexcept
    : http_(http), clock_(clock), random_(random), diagnostics_(diagnostics) {}

auto SessionClient::valid_token(const psn::auth::BearerToken& token) -> bool {
  return !token.refusal();
}

auto SessionClient::bearer(const psn::auth::BearerToken& token)
    -> psn::auth::BearerToken {
  return psn::auth::BearerToken{token.secret().clone(), supplied_token_label};
}

auto SessionClient::fail(SessionErrorKind kind) -> Unexpected<SessionError> {
  return unexpected(SessionError{kind});
}

auto SessionClient::session_error(const net::http::Failure& failure)
    -> SessionError {
  if (failure.is_cancelled()) return SessionError{SessionErrorKind::cancelled};
  return SessionError{
      SessionErrorKind::transport_failure,
      failure.error().value_or(platform::HttpError::backend_failure)};
}

auto SessionClient::refused(const psn::auth::BearerToken& token,
                            const platform::StopToken& stop)
    -> std::optional<SessionError> {
  if (!valid_token(token)) return SessionError{SessionErrorKind::invalid_token};
  if (stop.stop_requested()) return SessionError{SessionErrorKind::cancelled};
  return std::nullopt;
}

auto SessionClient::http_for(const platform::StopToken& stop) const
    -> net::http::Client {
  return net::http::Client{http_, clock_, stop, diagnostics_}.limited(
      max_response_size);
}

auto SessionClient::create_session(const psn::auth::BearerToken& bearer_token,
                                   const platform::StopToken& stop)
    -> Expected<psn::rest::CreatedSession, SessionError> {
  if (auto error = refused(bearer_token, stop)) return unexpected(*error);
  std::array<std::byte, 16> random_bytes{};
  if (!random_.fill_bytes(random_bytes))
    return fail(SessionErrorKind::random_failure);
  return with_session(
      bearer_token, stop,
      [&](net::http::Client& http, psn::auth::BearerToken token) {
        return psn::rest::SessionManager{http, std::move(token)}
            .create_remote_play_session(random_bytes);
      });
}

auto SessionClient::check_session(const psn::auth::BearerToken& bearer_token,
                                  std::string_view session_id, SessionView view,
                                  const platform::StopToken& stop)
    -> Expected<void, SessionError> {
  return with_session(
      bearer_token, stop,
      [&](net::http::Client& http,
          psn::auth::BearerToken token) -> Expected<void, SessionError> {
        const auto session = psn::rest::SessionId::parse(session_id);
        if (!session) return unexpected(unsendable());
        return mapped(
            psn::rest::SessionManager{http, std::move(token)}.observe_sessions(
                *session, view));
      });
}

auto SessionClient::observe_session(const psn::auth::BearerToken& bearer_token,
                                    std::string_view session_id,
                                    SessionView view,
                                    const platform::StopToken& stop)
    -> Expected<void, CreationWaitError> {
  const auto checked = check_session(bearer_token, session_id, view, stop);
  if (!checked && diagnostics_)
    diagnostics_->write(
        {platform::LogSeverity::info,
         std::format("PSN session observe failed kind={} status={}; still "
                     "waiting",
                     std::to_underlying(checked.error().kind),
                     checked.error().status)});
  if (stop.stop_requested())
    return unexpected(stop.deadline_expired() ? CreationWaitError::timed_out
                                              : CreationWaitError::cancelled);
  return {};
}

auto SessionClient::start_ps5_session(
    const psn::auth::BearerToken& bearer_token,
    const psn::rest::CreatedSession& created,
    std::span<const std::byte, psn_device_uid_size> device_uid,
    std::span<const std::byte, psn_custom_data_size> data1,
    std::span<const std::byte, psn_custom_data_size> data2,
    const platform::StopToken& stop) -> Expected<void, SessionError> {
  pending_command_.reset();
  auto command = with_session(
      bearer_token, stop,
      [&](net::http::Client& http, psn::auth::BearerToken token)
          -> Expected<std::optional<SecretString>, SessionError> {
        const auto session = psn::rest::SessionId::parse(created.session_id);
        if (!session) return unexpected(unsendable());
        return mapped(psn::rest::CloudAssistedNavigation{http, std::move(token)}
                          .start_remote_play(*session, created.account_id,
                                             device_uid, data1, data2));
      });
  if (!command) return unexpected(command.error());
  if (command.value())
    pending_command_.emplace(created.session_id, std::move(*command.value()));
  else if (diagnostics_)
    diagnostics_->write(
        {platform::LogSeverity::info,
         "PSN start command: accepted; status receipt unavailable"});
  return {};
}

void SessionClient::diagnose_start_command(
    const psn::auth::BearerToken& bearer_token, std::string_view session_id,
    const platform::StopToken& stop) {
  if (stop.stop_requested() || !diagnostics_ || !pending_command_ ||
      !pending_command_->matches(session_id) || !valid_token(bearer_token))
    return;
  auto command = std::move(*pending_command_);
  pending_command_.reset();
  auto http = net::http::Client{http_, clock_, stop}.limited(max_response_size);
  psn::rest::CloudAssistedNavigation navigation{http, bearer(bearer_token)};
  auto result = navigation.poll_remote_play_command(
      command.command(), std::chrono::milliseconds{2000});
  if (!result) {
    diagnostics_->write(
        {platform::LogSeverity::info,
         "PSN start command: status query failed; readiness failure retained"});
    return;
  }
  const char* status = "unrecognized";
  if (!result.value().state)
    status = "not_returned";
  else {
    for (const auto known :
         {"pending", "executing", "completed", "failed", "rejected", "error"}) {
      if (text::ieq(result.value().state->status, known)) status = known;
    }
  }
  std::array<char, 160> buffer{};
  const auto code =
      result.value().state ? result.value().state->error_code : std::nullopt;
  const int count = std::snprintf(
      buffer.data(), buffer.size(),
      "PSN start command: status=%s error_code_present=%u error_code=%u",
      status, code ? 1U : 0U, code.value_or(0));
  if (count > 0 && static_cast<std::size_t>(count) < buffer.size())
    diagnostics_->write({platform::LogSeverity::info,
                         {buffer.data(), static_cast<std::size_t>(count)}});
}

auto SessionClient::get_ps4_profile_base_url(
    const psn::auth::BearerToken& bearer_token, const platform::StopToken& stop)
    -> Expected<std::string, SessionError> {
  return with_session(
      bearer_token, stop,
      [](net::http::Client& http, psn::auth::BearerToken token) {
        return psn::rest::UserProfile{http, std::move(token)}
            .profile_base_url();
      });
}

auto SessionClient::get_push_fqdn(const psn::auth::BearerToken& bearer_token,
                                  const platform::StopToken& stop)
    -> Expected<std::string, SessionError> {
  return with_session(
      bearer_token, stop,
      [](net::http::Client& http, psn::auth::BearerToken token) {
        return psn::rest::Push{http, std::move(token)}.server_address();
      });
}

auto SessionClient::wake_ps4_session(
    const psn::auth::BearerToken& bearer_token, std::string_view session_id,
    std::string_view profile_base_url, std::string_view online_id,
    std::span<const std::byte, psn_custom_data_size> data1,
    std::span<const std::byte, psn_custom_data_size> data2,
    const platform::StopToken& stop) -> Expected<void, SessionError> {
  return with_session(
      bearer_token, stop,
      [&](net::http::Client& http,
          psn::auth::BearerToken token) -> Expected<void, SessionError> {
        const auto session = psn::rest::SessionId::parse(session_id);
        if (!session) return unexpected(unsendable());
        return mapped(psn::rest::UserProfile{http, std::move(token)}.wake_ps4(
            profile_base_url, online_id, *session, data1, data2));
      });
}

auto SessionClient::release_session(const psn::auth::BearerToken& bearer_token,
                                    std::string_view session_id,
                                    const platform::StopToken& stop)
    -> Expected<void, SessionError> {
  return with_session(
      bearer_token, stop,
      [&](net::http::Client& http,
          psn::auth::BearerToken token) -> Expected<void, SessionError> {
        const auto session = psn::rest::SessionId::parse(session_id);
        if (!session) return unexpected(unsendable());
        return mapped(
            psn::rest::SessionManager{http, std::move(token)}.leave_session(
                *session));
      });
}

auto SessionClient::send_short_message(
    const psn::auth::BearerToken& bearer_token, std::string_view session_id,
    net::nat::holepunch::Action action, std::uint16_t req_id,
    std::uint16_t error, std::int64_t account_id,
    std::span<const std::byte, psn_device_uid_size> console_uid,
    psn::rest::ConsoleFamily family, const platform::StopToken& stop)
    -> Expected<void, SessionError> {
  return with_session(
      bearer_token, stop,
      [&](net::http::Client& http,
          psn::auth::BearerToken token) -> Expected<void, SessionError> {
        const auto session = psn::rest::SessionId::parse(session_id);
        if (!session) return unexpected(unsendable());
        return mapped(psn::rest::SessionManager{http, std::move(token)}
                          .send_session_message(
                              *session,
                              net::nat::holepunch::MessageCodec::format_short(
                                  action, req_id, error),
                              {account_id, console_uid, family}));
      });
}

auto SessionClient::send_full_message(
    const psn::auth::BearerToken& bearer_token, std::string_view session_id,
    net::nat::holepunch::Action action, std::uint16_t req_id,
    std::uint16_t error, const net::nat::holepunch::ConnRequest& conn_request,
    std::int64_t account_id,
    std::span<const std::byte, psn_device_uid_size> console_uid,
    psn::rest::ConsoleFamily family, const platform::StopToken& stop)
    -> Expected<void, SessionError> {
  return with_session(
      bearer_token, stop,
      [&](net::http::Client& http,
          psn::auth::BearerToken token) -> Expected<void, SessionError> {
        const auto session = psn::rest::SessionId::parse(session_id);
        if (!session) return unexpected(unsendable());
        const auto payload = net::nat::holepunch::MessageCodec::format_full(
            action, req_id, error, conn_request, account_id);
        if (!payload) return unexpected(unsendable());
        return mapped(
            psn::rest::SessionManager{http, std::move(token)}
                .send_session_message(*session, payload.value().view(),
                                      {account_id, console_uid, family}));
      });
}

}  // namespace libchiaking::psn::remote_play
