#include "src/core/psn/rest/session_manager/player_sessions.hpp"

#include "src/core/net/transport/http/url.hpp"
#include "src/core/psn/rest/error_reply.hpp"
#include "src/core/utils/json/ps_reply.hpp"
#include "src/core/utils/text/ascii.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace libchiaking::psn::rest {

namespace ps = json::ps;

struct PlayerSessions::Reply {
  struct Player {
    using keys = ps::CamelCaseKeys;
    ps::DecimalText account_id;
    ps::Tolerant<std::string> online_id;
  };
  struct Other {
    using keys = ps::CamelCaseKeys;
    ps::Tolerant<std::string> player_id;
  };
  struct Members {
    using keys = ps::CamelCaseKeys;
    ps::Tolerant<std::vector<ps::Tolerant<Player>>> players;
    ps::Tolerant<std::vector<ps::Tolerant<Other>>> non_psn_players;
  };
  struct Title {
    using keys = ps::CamelCaseKeys;
    ps::Tolerant<std::string> np_title_id;
  };
  struct Intent {
    using keys = ps::CamelCaseKeys;
    ps::Tolerant<std::vector<ps::Tolerant<Title>>> np_title_ids;
  };
  struct Session {
    using keys = ps::CamelCaseKeys;
    ps::Tolerant<std::string> session_id;
    ps::Tolerant<std::string> session_name;
    ps::Tolerant<std::uint32_t> max_players;
    ps::Tolerant<Members> member;
    ps::Tolerant<Intent> player_game_intent;
    ps::Tolerant<SecretString> tokenized_player_game_intent;
    ps::Tolerant<std::string> joinable_user_type;
  };
  using keys = ps::CamelCaseKeys;
  ps::Tolerant<std::vector<ps::Tolerant<Session>>> player_sessions;
};

PlayerSessions::PlayerSessions(net::http::Client& client,
                               auth::BearerToken token) noexcept
    : client_(client), token_(std::move(token)) {}

auto PlayerSessions::read(std::span<const std::string> session_ids,
                          std::string_view language)
    -> Expected<std::vector<PlayerSession>, ReadError> {
  if (session_ids.empty()) return std::vector<PlayerSession>{};
  std::string ids;
  for (const auto& id :
       session_ids.first(std::min(session_ids.size(), player_sessions_batch))) {
    if (!ids.empty()) ids += ',';
    ids += id;
  }
  net::http::Query url{sessions_url};
  url.add_verbatim("fields", fields);
  net::http::Request request{read_endpoint, std::move(url)};
  request.authorize(token_)
      .header("Accept", "application/json")
      .header("Accept-Language", language)
      .header(session_ids_header, ids);
  auto reply = ReadError::checked_reply<Reply>(client_.send(std::move(request)),
                                               token_.view());
  if (!reply) return unexpected(std::move(reply.error()));
  std::vector<PlayerSession> found;
  if (!reply->player_sessions) return found;
  for (auto& slot : *reply->player_sessions) {
    if (!slot) continue;
    auto& entry = *slot;
    PlayerSession session{
        .session_id = std::move(entry.session_id).value_or(""),
        .name = std::move(entry.session_name).value_or(""),
        .max_players = entry.max_players.value_or(0)};
    if (entry.member) {
      if (entry.member->players)
        for (auto& player : *entry.member->players) {
          if (!player) continue;
          session.players.push_back(
              SessionPlayer{player->account_id.trimmed(),
                            std::move(player->online_id).value_or("")});
        }
      if (entry.member->non_psn_players)
        session.other_players = entry.member->non_psn_players->size();
    }
    if (entry.player_game_intent && entry.player_game_intent->np_title_ids)
      for (auto& title : *entry.player_game_intent->np_title_ids) {
        if (!title || !title->np_title_id) continue;
        session.title_ids.push_back(std::move(*title->np_title_id));
      }
    if (entry.tokenized_player_game_intent)
      session.game_intent = std::move(*entry.tokenized_player_game_intent);
    session.open_to_anyone =
        text::ieq(text::trim(entry.joinable_user_type.value_or("")), "ANYONE");
    found.push_back(std::move(session));
  }
  return found;
}

}  // namespace libchiaking::psn::rest
