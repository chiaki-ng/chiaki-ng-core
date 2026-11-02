#include "src/core/psn/rest/user_profile/account_profile.hpp"

#include "src/core/net/transport/http/strict_url.hpp"
#include "src/core/utils/text/ascii.hpp"
#include "src/core/utils/text/hex.hpp"

#include "src/core/utils/json/ps_reply.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <string_view>
#include <utility>

namespace libchiaking::psn::rest {

struct AccountProfile::Reply {
  struct Avatar {
    std::optional<std::string> size;
    std::optional<std::string> url;
  };
  using keys = json::ps::CamelCaseKeys;
  std::optional<std::string> online_id;
  std::optional<bool> is_plus;
  std::optional<std::vector<Avatar>> avatars;
};

auto AccountProfile::secure_avatar_url(std::string_view url)
    -> std::optional<std::string> {
  const auto safe = [](char value) {
    const auto byte = static_cast<unsigned char>(value);
    return byte > 0x20 && byte != 0x7F && value != '\\';
  };
  if (url.empty() || url.size() > avatar_url_max_bytes ||
      !std::ranges::all_of(url, safe))
    return std::nullopt;
  const auto parsed = net::http::StrictUrl::read(
      url, net::http::StrictUrl::Strictness::shown_link);
  if (!parsed) return std::nullopt;
  const bool secure = text::ieq(parsed->scheme(), secure_scheme);
  if (!secure && !text::ieq(parsed->scheme(), plain_scheme))
    return std::nullopt;
  if (secure) return std::string{url};
  if (parsed->port() && *parsed->port() != plain_default_port)
    return std::nullopt;
  std::string upgraded{secure_scheme};
  upgraded.append(scheme_separator)
      .append(parsed->host())
      .append(parsed->tail());
  return upgraded;
}

auto AccountProfile::avatar_url(const Reply& reply)
    -> Expected<std::optional<std::string>, AvatarFault> {
  if (!reply.avatars || reply.avatars->empty())
    return std::optional<std::string>{};
  const auto& avatars = *reply.avatars;
  const auto described = [](const Reply::Avatar& avatar) {
    return avatar.size && avatar.url;
  };
  if (!std::ranges::all_of(avatars, described))
    return unexpected(AvatarFault{bad_reply::InvalidSchema{}});
  const auto* chosen = &avatars.front();
  for (const auto size : preferred_avatar_sizes) {
    const auto sized =
        std::ranges::find(avatars, size, [](const Reply::Avatar& avatar) {
          return std::string_view{*avatar.size};
        });
    if (sized != avatars.end()) {
      chosen = &*sized;
      break;
    }
  }
  auto url = secure_avatar_url(*chosen->url);
  if (!url) return unexpected(AvatarFault{bad_reply::InvalidUrl{}});
  return url;
}

AccountProfile::AccountProfile(net::http::Client& client,
                               auth::AppToken token) noexcept
    : client_(client), token_(std::move(token)) {}

auto AccountProfile::details(AccountId account)
    -> Expected<ProfileDetails, DetailsError> {
  if (token_.refusal())
    return unexpected(DetailsError{net::http::Failure::invalid_request()});
  net::http::Request request{
      details_endpoint,
      std::format("{}{}/profiles", profiles_url, account.decimal())};
  request.authorize(token_).header("Accept", "*/*");
  auto reply = DetailsError::checked_reply<Reply>(
      client_.send(std::move(request)), token_.view());
  if (!reply) return unexpected(std::move(reply.error()));
  if (reply->online_id.value_or("").empty())
    return unexpected(DetailsError{bad_reply::InvalidSchema{}});
  auto avatar = avatar_url(reply.value());
  if (!avatar) return unexpected(DetailsError{avatar.error()});
  const auto plus = !reply->is_plus   ? PlusStatus::unknown
                    : *reply->is_plus ? PlusStatus::active
                                      : PlusStatus::absent;
  return ProfileDetails{std::move(*reply->online_id), std::move(avatar.value()),
                        plus};
}

}  // namespace libchiaking::psn::rest
