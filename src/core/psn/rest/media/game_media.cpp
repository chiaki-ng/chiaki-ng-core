#include "src/core/psn/rest/media/game_media.hpp"

#include "src/core/net/transport/http/percent_encoding.hpp"
#include "src/core/net/transport/http/url.hpp"
#include "src/core/utils/json/ps_reply.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace libchiaking::psn::rest {

struct GameMedia::Reply {
  struct Document {
    using keys = json::ps::CamelCaseKeys;
    json::ps::Tolerant<std::string> id;
    json::ps::Tolerant<std::string> download_url;
    json::ps::Tolerant<std::string> screenshot_url;
    json::ps::Tolerant<std::string> video_url;
    json::ps::Tolerant<std::string> file_type;
    json::ps::Tolerant<std::uint64_t> file_size;
  };
  std::optional<std::vector<json::ps::Tolerant<Document>>> ugc_document;
  using keys = json::ps::CamelCaseKeys;
};

namespace {

template <class Document>
auto best_address(Document& document) -> std::string {
  auto address = std::move(document.download_url).value_or(std::string{});
  if (address.empty())
    address = std::move(document.video_url).value_or(std::string{});
  if (address.empty())
    address = std::move(document.screenshot_url).value_or(std::string{});
  return address;
}

}  // namespace

GameMedia::GameMedia(net::http::Client& client,
                     auth::BearerToken token) noexcept
    : client_(client), token_(std::move(token)) {}

auto GameMedia::capture(std::string_view gallery_id)
    -> Expected<GalleryCapture, CaptureError> {
  net::http::Query url{gallery_url};
  url.add("ugcIds", gallery_id).add_verbatim("includeTokenizedUrls", "true");
  net::http::Request request{capture_endpoint, std::move(url)};
  request.authorize(token_).header("Accept", "application/json");
  auto reply = CaptureError::checked_reply<Reply>(
      client_.send(std::move(request)), token_.view());
  if (!reply) return unexpected(std::move(reply.error()));
  if (!reply->ugc_document)
    return unexpected(CaptureError{bad_reply::InvalidSchema{}});
  for (auto& slot : *reply->ugc_document) {
    if (!slot || (slot->id && *slot->id != gallery_id)) continue;
    auto& document = *slot;
    auto address = std::move(document.download_url).value_or(std::string{});
    if (address.empty())
      address = std::move(document.video_url).value_or(std::string{});
    if (address.empty())
      address = std::move(document.screenshot_url).value_or(std::string{});
    if (address.empty()) continue;
    return GalleryCapture{std::move(address),
                          std::move(document.file_type).value_or(std::string{}),
                          document.file_size.value_or(0)};
  }
  return unexpected(CaptureError{bad_reply::InvalidSchema{}});
}

auto GameMedia::download_address(std::string_view gallery_id)
    -> Expected<std::string, DownloadAddressError> {
  std::string url{ugc_url};
  url += net::http::PercentEncoding::component(gallery_id);
  url += "/url";
  net::http::Request request{address_endpoint, net::http::Query{url}};
  request.authorize(token_).header("Accept", "application/json");
  auto reply = DownloadAddressError::checked_reply<Reply::Document>(
      client_.send(std::move(request)), token_.view());
  if (!reply) return unexpected(std::move(reply.error()));
  auto address = best_address(*reply);
  if (address.empty())
    return unexpected(DownloadAddressError{bad_reply::InvalidSchema{}});
  return address;
}

}  // namespace libchiaking::psn::rest
