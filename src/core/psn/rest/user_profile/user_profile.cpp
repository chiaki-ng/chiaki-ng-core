#include "src/core/psn/rest/user_profile/user_profile.hpp"

#include "src/core/utils/json/wire_dialect.hpp"

#include "src/core/net/transport/http/strict_url.hpp"
#include "src/core/net/transport/http/url.hpp"
#include "src/core/psn/online_id.hpp"
#include "src/core/utils/text/ascii.hpp"
#include "src/core/utils/text/base64.hpp"
#include "src/core/utils/text/hex.hpp"

#include "src/core/utils/json/ps_reply.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace libchiaking::psn::rest {

struct UserProfile::BaseUrls {
  std::optional<std::string> url;
};

struct UserProfile::WakeBody {
  struct Data {
    using keys = json::ps::CamelCaseKeys;
    std::string_view client_type{"Windows"};
    SecretString data1;
    SecretString data2;
    std::int64_t room_id{};
    std::string_view protocol_ver{"10.0"};
    std::string_view session_id;
  };
  using keys = json::ps::CamelCaseKeys;
  Data data;
  std::string_view data_type_suffix{"remotePlay"};
};

auto UserProfile::base_url_authority(std::string_view url)
    -> std::optional<std::string_view> {
  if (url.size() > url_max_bytes) return std::nullopt;
  const auto parsed = net::http::StrictUrl::read(
      url, net::http::StrictUrl::Strictness::request_target);
  if (!parsed || !text::ieq(parsed->scheme(), secure_scheme))
    return std::nullopt;
  const auto path_character = [](char value) {
    const auto byte = static_cast<unsigned char>(value);
    return byte > 0x20 && byte != 0x7F && value != '\\' && value != '?' &&
           value != '#';
  };
  const auto path = parsed->tail();
  if (!path.empty() &&
      (path.front() != '/' || !std::ranges::all_of(path, path_character)))
    return std::nullopt;
  return parsed->authority();
}

auto UserProfile::wake_body(
    const SessionId& session,
    std::span<const std::byte, psn_custom_data_size> data1,
    std::span<const std::byte, psn_custom_data_size> data2)
    -> Expected<SecretBytes, Fault<bad_request::InvalidMessage>> {
  WakeBody wake{.data = {.session_id = session.view()}};
  text::base64::append(wake.data.data1, data1,
                       text::base64::Alphabet::standard);
  text::base64::append(wake.data.data2, data2,
                       text::base64::Alphabet::standard);
  auto body = json::ps::write(wake);
  if (!body)
    return unexpected(
        Fault<bad_request::InvalidMessage>{bad_request::InvalidMessage{}});
  return SecretBytes{body->bytes()};
}

UserProfile::UserProfile(net::http::Client& client,
                         auth::BearerToken token) noexcept
    : client_(client), token_(std::move(token)) {}

auto UserProfile::send(net::http::Endpoint endpoint, std::string_view url,
                       std::string_view host, SecretBytes body)
    -> Expected<net::http::Response, ExchangeError> {
  net::http::Request request{endpoint, url};
  request.authorize(token_)
      .header("Host", host)
      .header("Connection", "Keep-Alive")
      .body(content_type, std::move(body))
      .header("User-Agent", user_agent);
  return ExchangeError::checked(client_.send(std::move(request)),
                                token_.view());
}

auto UserProfile::profile_base_url()
    -> Expected<std::string, ProfileBaseUrlError> {
  auto response =
      send(profile_base_url_endpoint, base_urls, profile_host, SecretBytes{});
  if (!response) return unexpected(ProfileBaseUrlError{response.error()});
  auto reply = json::WireDialect::read<BaseUrls>(response.value().body());
  if (!reply) return unexpected(ProfileBaseUrlError::parsed(reply.error()));
  if (!reply->url)
    return unexpected(ProfileBaseUrlError{bad_reply::InvalidSchema{}});
  if (!base_url_authority(*reply->url))
    return unexpected(ProfileBaseUrlError{bad_reply::InvalidUrl{}});
  return std::move(*reply->url);
}

auto UserProfile::wake_ps4(
    std::string_view profile_base_url, std::string_view online_id,
    const SessionId& session,
    std::span<const std::byte, psn_custom_data_size> data1,
    std::span<const std::byte, psn_custom_data_size> data2)
    -> Expected<void, WakePs4Error> {
  const auto authority = base_url_authority(profile_base_url);
  if (!authority || profile_base_url.back() == '/')
    return unexpected(WakePs4Error{bad_request::InvalidUrl{}});
  if (!psn::OnlineId::parse(online_id))
    return unexpected(WakePs4Error{bad_request::InvalidOnlineId{}});
  net::http::Query url{
      std::format("{}/v1/users/{}/remoteConsole/wakeUp", profile_base_url,
                  net::http::PercentEncoding::component(online_id))};
  url.add_verbatim("platform", "PS4");
  auto body = wake_body(session, data1, data2);
  if (!body) return unexpected(WakePs4Error{body.error()});
  auto sent =
      send(wake_endpoint, url.view(), *authority, std::move(body.value()));
  if (!sent) return unexpected(WakePs4Error{sent.error()});
  return {};
}

}  // namespace libchiaking::psn::rest
