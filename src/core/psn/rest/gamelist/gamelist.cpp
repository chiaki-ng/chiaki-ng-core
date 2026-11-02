#include "src/core/psn/rest/gamelist/gamelist.hpp"

#include "src/core/net/transport/http/url.hpp"
#include "src/core/utils/json/ps_reply.hpp"
#include "src/core/utils/text/ascii.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace libchiaking::psn::rest {

struct Gamelist::Reply {
  struct Title {
    using keys = json::ps::CamelCaseKeys;
    json::ps::Tolerant<std::string> title_id;
    json::ps::Tolerant<std::string> localized_name;
    json::ps::Tolerant<std::string> name;
    json::ps::Tolerant<std::string> localized_image_url;
    json::ps::Tolerant<std::string> image_url;
    json::ps::Tolerant<std::string> category;
    json::ps::Tolerant<std::string> last_played_date_time;
    json::ps::Tolerant<std::string> first_played_date_time;
    json::ps::Tolerant<std::string> play_duration;
    json::ps::Tolerant<json::ps::LenientNumber<std::uint32_t>> play_count;
  };
  using keys = json::ps::CamelCaseKeys;
  std::optional<std::vector<json::ps::Tolerant<Title>>> titles;
  json::ps::Tolerant<json::ps::LenientNumber<std::size_t>> next_offset;
  json::ps::Tolerant<json::ps::LenientNumber<std::size_t>> total_item_count;
};

auto Gamelist::first_present(json::ps::Tolerant<std::string>& preferred,
                             json::ps::Tolerant<std::string>& fallback)
    -> std::string {
  if (preferred && !text::trim(*preferred).empty())
    return std::move(*preferred);
  return std::move(fallback).value_or(std::string{});
}

Gamelist::Gamelist(net::http::Client& client, auth::BearerToken token) noexcept
    : client_(client), token_(std::move(token)) {}

auto Gamelist::played_titles(std::size_t offset)
    -> Expected<PlayedTitlesPage, PlayedTitlesError> {
  net::http::Query url{titles_url};
  url.add_verbatim("limit", std::to_string(played_titles_page_size))
      .add_verbatim("offset", std::to_string(offset));
  net::http::Request request{played_titles_endpoint, std::move(url)};
  request.authorize(token_).header("Accept", "application/json");
  auto reply = PlayedTitlesError::checked_reply<Reply>(
      client_.send(std::move(request)), token_.view());
  if (!reply) return unexpected(std::move(reply.error()));
  if (!reply->titles)
    return unexpected(PlayedTitlesError{bad_reply::InvalidSchema{}});
  PlayedTitlesPage page;
  page.received = reply->titles->size();
  if (reply->next_offset) page.next_offset = reply->next_offset->value();
  if (reply->total_item_count) page.total = reply->total_item_count->value();
  for (auto& slot : *reply->titles) {
    if (!slot) continue;
    auto& title = *slot;
    std::optional<std::uint32_t> play_count;
    if (title.play_count) play_count = title.play_count->value();
    page.titles.push_back(PlayedTitle{
        .title_id = std::move(title.title_id).value_or(""),
        .name = first_present(title.localized_name, title.name),
        .image_url = first_present(title.localized_image_url, title.image_url),
        .category = std::move(title.category).value_or(""),
        .last_played = std::move(title.last_played_date_time).value_or(""),
        .first_played = std::move(title.first_played_date_time).value_or(""),
        .play_duration = std::move(title.play_duration).value_or(""),
        .play_count = play_count});
  }
  return page;
}

}  // namespace libchiaking::psn::rest
