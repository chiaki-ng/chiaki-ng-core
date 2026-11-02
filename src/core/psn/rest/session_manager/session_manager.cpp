#include "src/core/psn/rest/session_manager/session_manager.hpp"

#include "src/core/net/transport/http/url.hpp"
#include "src/core/utils/json/ps_reply.hpp"
#include "src/core/utils/json/wire_dialect.hpp"
#include "src/core/utils/text/ascii.hpp"
#include "src/core/utils/text/hex.hpp"
#include "src/core/utils/text/integer.hpp"
#include "src/core/utils/text/uuid.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <limits>
#include <string>
#include <utility>

namespace libchiaking::psn::rest {

namespace ps = json::ps;

struct SessionManager::CreateBody {
  struct PushContext {
    using keys = ps::CamelCaseKeys;
    std::string_view push_context_id;
  };
  struct Member {
    using keys = ps::CamelCaseKeys;
    std::string_view account_id{"me"};
    std::string_view device_unique_id{"me"};
    std::string_view platform{"me"};
    std::array<PushContext, 1> push_contexts;
  };
  struct Session {
    std::array<Member, 1> members;
  };
  using keys = ps::CamelCaseKeys;
  std::array<Session, 1> remote_play_sessions;
};

struct SessionManager::CreatedReply {
  struct Member {
    using keys = ps::CamelCaseKeys;
    std::optional<glz::raw_json> account_id;
  };
  struct Session {
    using keys = ps::CamelCaseKeys;
    std::optional<std::string> session_id;
    std::optional<std::vector<Member>> members;
  };
  using keys = ps::CamelCaseKeys;
  std::optional<std::vector<Session>> remote_play_sessions;
};

struct SessionManager::MessageBody {
  struct Recipient {
    using keys = ps::CamelCaseKeys;
    std::string account_id;
    SecretString device_unique_id;
    std::string_view platform;
  };
  std::string_view channel{"remote_play:1"};
  SecretString payload;
  std::array<Recipient, 1> to;
};

auto SessionManager::session_url(const SessionId& session,
                                 std::string_view suffix) -> std::string {
  return std::format("{}/{}{}", sessions_url, session.view(), suffix);
}

auto SessionManager::account_number(std::string_view raw)
    -> std::optional<std::int64_t> {
  if (!raw.starts_with('"')) return text::parse_integer<std::int64_t>(raw);
  const auto text = ps::read_reply<std::string>(raw);
  if (!text || text->empty() || text->front() == '+') return std::nullopt;
  return text::parse_integer<std::int64_t>(*text);
}

auto SessionManager::created_session(std::string push_context,
                                     std::string_view body)
    -> Expected<CreatedSession, CreatedFault> {
  auto reply = json::WireDialect::read<CreatedReply>(body);
  if (!reply) return unexpected(CreatedFault::parsed(reply.error()));
  const auto& sessions = reply->remote_play_sessions;
  if (!sessions || sessions->empty() || !sessions->front().session_id ||
      !sessions->front().members || sessions->front().members->empty())
    return unexpected(CreatedFault{bad_reply::InvalidSchema{}});
  const auto& first = sessions->front();
  if (!SessionId::parse(*first.session_id))
    return unexpected(CreatedFault{bad_reply::InvalidSessionId{}});
  const auto& account = first.members->front().account_id;
  const auto raw =
      account ? std::string_view{account->str} : std::string_view{};
  if (raw.empty() || !(raw.front() == '"' || raw.front() == '-' ||
                       text::ascii_digit(raw.front())))
    return unexpected(CreatedFault{bad_reply::InvalidSchema{}});
  const auto account_id = account_number(raw);
  if (!account_id)
    return unexpected(CreatedFault{bad_reply::InvalidAccountId{}});
  return CreatedSession{std::move(push_context), *first.session_id,
                        *account_id};
}

SessionId::SessionId(std::string_view text) : value_(text) {}

auto SessionId::parse(std::string_view text) -> std::optional<SessionId> {
  const auto allowed = [](char value) {
    return text::ascii_alnum(value) || value == '-';
  };
  if (text.size() != session_id_length || !std::ranges::all_of(text, allowed))
    return std::nullopt;
  return SessionId{text};
}

SessionManager::SessionManager(net::http::Client& client,
                               auth::BearerToken token) noexcept
    : client_(client), token_(std::move(token)) {}

auto SessionManager::send(net::http::Request request)
    -> Expected<net::http::Response, ExchangeError> {
  return ExchangeError::checked(client_.send(std::move(request)),
                                token_.view());
}

auto SessionManager::create_remote_play_session(
    std::span<const std::byte, 16> random)
    -> Expected<CreatedSession, CreateError> {
  const auto context = text::Uuid::version4(random);
  CreateBody create;
  create.remote_play_sessions[0].members[0].push_contexts[0].push_context_id =
      context.view();
  auto body = ps::write(create);
  if (!body) return unexpected(CreateError{bad_request::InvalidMessage{}});
  net::http::Request request{create_endpoint, sessions_url};
  request.authorize(token_).body(content_type, SecretBytes{body->bytes()});
  auto response = send(std::move(request));
  if (!response) return unexpected(CreateError{response.error()});
  auto created =
      created_session(std::string{context.view()}, response.value().body());
  if (!created) return unexpected(CreateError{created.error()});
  return std::move(created.value());
}

auto SessionManager::observe_sessions(const SessionId& session,
                                      SessionView view)
    -> Expected<void, ObserveError> {
  net::http::Query url{sessions_url};
  if (view == SessionView::detailed) url.add_verbatim("view", "v1.0");
  net::http::Request request{observe_endpoint, std::move(url)};
  request.authorize(token_).header("X-PSN-SESSION-MANAGER-SESSION-IDS",
                                   session.view());
  auto sent = send(std::move(request));
  if (!sent) return unexpected(ObserveError{sent.error()});
  return {};
}

auto SessionManager::leave_session(const SessionId& session)
    -> Expected<void, LeaveError> {
  net::http::Request request{
      leave_endpoint, std::string_view{session_url(session, "/members/me")}};
  request.authorize(token_).body(content_type, SecretBytes{});
  auto sent = send(std::move(request));
  if (!sent) return unexpected(LeaveError{sent.error()});
  return {};
}

auto SessionManager::send_session_message(const SessionId& session,
                                          std::string_view payload,
                                          const MessageRecipient& recipient)
    -> Expected<void, SessionMessageError> {
  MessageBody message;
  message.payload.append(message_prefix);
  message.payload.append(payload);
  auto& to = message.to[0];
  to.account_id = std::to_string(recipient.account_id);
  text::append_hex(to.device_unique_id, recipient.device_uid);
  to.platform = recipient.family == ConsoleFamily::ps4 ? "PS4" : "PS5";
  auto body = ps::write(message);
  if (!body || body->size() > maximum_message_bytes)
    return unexpected(SessionMessageError{bad_request::InvalidMessage{}});
  net::http::Request request{
      message_endpoint,
      std::string_view{session_url(session, "/sessionMessage")}};
  request.authorize(token_).body(content_type, SecretBytes{body->bytes()});
  auto sent = send(std::move(request));
  if (!sent) return unexpected(SessionMessageError{sent.error()});
  return {};
}

}  // namespace libchiaking::psn::rest
