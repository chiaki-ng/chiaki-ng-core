#include "src/core/psn/rest/session_manager/session_invitations.hpp"

#include "src/core/net/transport/http/url.hpp"
#include "src/core/psn/rest/error_reply.hpp"
#include "src/core/utils/json/ps_reply.hpp"
#include "src/core/utils/text/ascii.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

namespace libchiaking::psn::rest {

namespace ps = json::ps;

struct SessionInvitations::Joined {
  struct Session {
    using keys = ps::CamelCaseKeys;
    ps::Tolerant<std::string> session_id;
  };
  using keys = ps::CamelCaseKeys;
  ps::Tolerant<std::vector<ps::Tolerant<Session>>> player_sessions;
};

struct SessionInvitations::Shared {
  using keys = ps::CamelCaseKeys;
  ps::Tolerant<std::string> session_join_url;
  ps::Tolerant<std::string> session_join_image_url;
};

struct SessionInvitations::Invitations {
  struct To {
    using keys = ps::CamelCaseKeys;
    std::string_view account_id;
  };
  struct Invitee {
    To to;
  };
  using keys = ps::CamelCaseKeys;
  std::vector<Invitee> invitations;
  std::optional<std::string_view> invitation_type;
};

SessionInvitations::SessionInvitations(net::http::Client& client,
                                       auth::BearerToken token) noexcept
    : client_(client), token_(std::move(token)) {}

auto SessionInvitations::path_safe(std::string_view text) noexcept -> bool {
  return !text.empty() && text.size() <= 64 &&
         std::ranges::all_of(text, [](char c) {
           return text::ascii_alnum(c) || c == '-' || c == '_';
         });
}

auto SessionInvitations::joined(std::string_view account_id,
                                std::string_view title_id,
                                std::string_view platform,
                                std::string_view device_type,
                                std::string_view language)
    -> Expected<std::vector<std::string>, JoinedError> {
  if (!path_safe(account_id))
    return unexpected(JoinedError{bad_request::InvalidMessage{}});
  net::http::Query url{std::string{session_manager_url} + "/v1/users/" +
                       std::string{account_id} + "/playerSessions"};
  url.add("relatedCreatorNpTitleIdFilter", title_id)
      .add("platformFilter", platform)
      .add("deviceType", device_type);
  net::http::Request request{joined_endpoint, std::move(url)};
  request.authorize(token_)
      .header("Accept", "application/json")
      .header("Accept-Language", language);
  auto reply = JoinedError::checked_reply<Joined>(
      client_.send(std::move(request)), token_.view());
  if (!reply) return unexpected(std::move(reply.error()));
  std::vector<std::string> found;
  if (!reply->player_sessions) return found;
  for (auto& session : *reply->player_sessions) {
    if (!session || !session->session_id) continue;
    const auto id = text::trim(*session->session_id);
    if (!id.empty()) found.emplace_back(id);
  }
  return found;
}

auto SessionInvitations::share_link(std::string_view session_id,
                                    std::string_view language)
    -> Expected<SessionShareLink, ShareLinkError> {
  if (!path_safe(session_id))
    return unexpected(ShareLinkError{bad_request::InvalidMessage{}});
  net::http::Request request{
      share_endpoint,
      net::http::Query{std::string{share_url} + std::string{session_id}}};
  request.authorize(token_)
      .header("Accept", "application/json")
      .header("Accept-Language", language);
  auto reply = ShareLinkError::checked_reply<Shared>(
      client_.send(std::move(request)), token_.view());
  if (!reply) return unexpected(std::move(reply.error()));
  if (!reply->session_join_url || text::trim(*reply->session_join_url).empty())
    return unexpected(ShareLinkError{bad_reply::InvalidSchema{}});
  return SessionShareLink{
      std::string{text::trim(*reply->session_join_url)},
      std::string{text::trim(reply->session_join_image_url.value_or(""))}};
}

auto SessionInvitations::invite_to_party(
    std::string_view session_id, std::span<const std::string> account_ids,
    std::string_view language) -> Expected<void, InviteError> {
  if (!path_safe(session_id))
    return unexpected(InviteError{bad_request::InvalidMessage{}});
  return invite(party_endpoint,
                std::string{session_manager_url} + "/v2/partySessions/" +
                    std::string{session_id} + "/invitations",
                account_ids, "NORMAL", language);
}

auto SessionInvitations::invite_to_game(
    std::string_view session_id, std::span<const std::string> account_ids,
    std::string_view language) -> Expected<void, InviteError> {
  if (!path_safe(session_id))
    return unexpected(InviteError{bad_request::InvalidMessage{}});
  return invite(game_endpoint,
                std::string{session_manager_url} + "/v1/playerSessions/" +
                    std::string{session_id} + "/invitations",
                account_ids, {}, language);
}

auto SessionInvitations::invite(const net::http::Endpoint& endpoint,
                                std::string url,
                                std::span<const std::string> account_ids,
                                std::string_view invitation_type,
                                std::string_view language)
    -> Expected<void, InviteError> {
  if (account_ids.empty() || account_ids.size() > most_invitees ||
      !std::ranges::all_of(account_ids,
                           [](const std::string& id) { return path_safe(id); }))
    return unexpected(InviteError{bad_request::InvalidMessage{}});
  Invitations body;
  for (const auto& id : account_ids) body.invitations.push_back({{id}});
  if (!invitation_type.empty()) body.invitation_type = invitation_type;
  auto written = ps::write(body);
  if (!written) return unexpected(InviteError{bad_request::InvalidMessage{}});
  net::http::Request request{endpoint, net::http::Query{url}};
  request.authorize(token_)
      .header("Accept", "application/json")
      .header("Accept-Language", language)
      .body(content_type, SecretBytes{written->bytes()});
  auto sent =
      InviteError::checked(client_.send(std::move(request)), token_.view());
  if (!sent) return unexpected(std::move(sent.error()));
  return {};
}

}  // namespace libchiaking::psn::rest
