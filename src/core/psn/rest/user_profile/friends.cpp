#include "src/core/psn/rest/user_profile/friends.hpp"

#include "src/core/net/transport/http/url.hpp"
#include "src/core/utils/json/ps_reply.hpp"
#include "src/core/utils/text/ascii.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <variant>

namespace libchiaking::psn::rest {

namespace ps = json::ps;

struct Friends::Page {
  using keys = ps::CamelCaseKeys;
  ps::Tolerant<std::vector<ps::DecimalText>> friends;
  ps::Tolerant<std::size_t> next_offset;
};

struct Friends::Profiles {
  struct Avatar {
    ps::Tolerant<std::string> size;
    ps::Tolerant<std::string> url;
  };
  struct Profile {
    using keys = ps::CamelCaseKeys;
    ps::Tolerant<std::string> online_id;
    ps::Tolerant<std::vector<ps::Tolerant<Avatar>>> avatars;
  };
  ps::Tolerant<std::vector<ps::Tolerant<Profile>>> profiles;
};

struct Friends::Presences {
  struct Platform {
    using keys = ps::CamelCaseKeys;
    ps::Tolerant<std::string> online_status;
  };
  struct Title {
    using keys = ps::CamelCaseKeys;
    ps::Tolerant<std::string> title_name;
  };
  struct Presence {
    using keys = ps::CamelCaseKeys;
    ps::Tolerant<Platform> primary_platform_info;
    ps::Tolerant<std::vector<ps::Tolerant<Title>>> game_title_info_list;
  };
  using keys = ps::CamelCaseKeys;
  ps::Tolerant<std::vector<ps::Tolerant<Presence>>> basic_presences;
};

Friends::Friends(net::http::Client& client, auth::BearerToken token) noexcept
    : client_(client), token_(std::move(token)) {}

auto Friends::joined(std::span<const std::string> account_ids) -> std::string {
  std::string ids;
  for (const auto& id :
       account_ids.first(std::min(account_ids.size(), page_size))) {
    if (!ids.empty()) ids += ',';
    ids += id;
  }
  return ids;
}

auto Friends::page(std::size_t offset, std::string_view language)
    -> Expected<FriendsPage, PageError> {
  net::http::Query url{std::string{profile_url} +
                       "/v2/internal/users/me/friends"};
  url.add("limit", std::to_string(page_size))
      .add("offset", std::to_string(offset))
      .add("order", "availability+realName+onlineId")
      .add("platforms", platforms);
  net::http::Request request{page_endpoint, std::move(url)};
  request.authorize(token_)
      .header("Accept", "application/json")
      .header("Accept-Language", language);
  auto reply = PageError::checked_reply<Page>(client_.send(std::move(request)),
                                              token_.view());
  if (!reply) return unexpected(std::move(reply.error()));
  FriendsPage page;
  if (reply->friends)
    for (const auto& id : *reply->friends) {
      auto text = id.trimmed();
      if (!text.empty()) page.account_ids.push_back(std::move(text));
    }
  if (reply->next_offset && *reply->next_offset > offset)
    page.next_offset = *reply->next_offset;
  return page;
}

auto Friends::profiles(std::span<const std::string> account_ids,
                       std::string_view language)
    -> Expected<std::vector<FriendProfile>, ProfilesError> {
  if (account_ids.empty()) return std::vector<FriendProfile>{};
  net::http::Query url{std::string{profile_url} +
                       "/v1/internal/users/profiles"};
  url.add("accountIds", joined(account_ids));
  net::http::Request request{profiles_endpoint, std::move(url)};
  request.authorize(token_)
      .header("Accept", "application/json")
      .header("Accept-Language", language);
  auto reply = ProfilesError::checked_reply<Profiles>(
      client_.send(std::move(request)), token_.view());
  if (!reply) return unexpected(std::move(reply.error()));
  std::vector<FriendProfile> found(std::min(account_ids.size(), page_size));
  if (!reply->profiles) return found;
  for (std::size_t index = 0;
       index < found.size() && index < reply->profiles->size(); ++index) {
    auto& slot = (*reply->profiles)[index];
    if (!slot) continue;
    found[index].online_id =
        std::string{text::trim(slot->online_id.value_or(""))};
    if (!slot->avatars) continue;
    for (const auto& avatar : *slot->avatars)
      if (avatar && avatar->size && text::ieq(*avatar->size, "m") &&
          avatar->url) {
        found[index].avatar_url = std::string{text::trim(*avatar->url)};
        break;
      }
  }
  return found;
}

auto Friends::presences(std::span<const std::string> account_ids,
                        std::string_view language)
    -> Expected<std::vector<FriendPresence>, PresencesError> {
  if (account_ids.empty()) return std::vector<FriendPresence>{};
  net::http::Query url{std::string{profile_url} +
                       "/v2/internal/users/basicPresences"};
  url.add("accountIds", joined(account_ids))
      .add("platforms", platforms)
      .add("type", "primary")
      .add("withOwnGameTitleInfo", "true");
  net::http::Request request{presences_endpoint, std::move(url)};
  request.authorize(token_)
      .header("Accept", "application/json")
      .header("Accept-Language", language);
  auto reply = PresencesError::checked_reply<Presences>(
      client_.send(std::move(request)), token_.view());
  if (!reply) return unexpected(std::move(reply.error()));
  std::vector<FriendPresence> found(std::min(account_ids.size(), page_size));
  if (!reply->basic_presences) return found;
  for (std::size_t index = 0;
       index < found.size() && index < reply->basic_presences->size();
       ++index) {
    auto& slot = (*reply->basic_presences)[index];
    if (!slot) continue;
    found[index].online =
        slot->primary_platform_info &&
        text::ieq(slot->primary_platform_info->online_status.value_or(""),
                  "online");
    if (slot->game_title_info_list && !slot->game_title_info_list->empty() &&
        slot->game_title_info_list->front())
      found[index].playing = std::string{text::trim(
          slot->game_title_info_list->front()->title_name.value_or(""))};
  }
  return found;
}

}  // namespace libchiaking::psn::rest
