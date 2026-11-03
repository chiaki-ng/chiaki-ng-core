#include "src/core/psn/rest/kamaji/kamaji_client.hpp"

#include "src/core/net/transport/http/status.hpp"
#include "src/core/net/transport/http/url.hpp"
#include "src/core/psn/legacy_cloud_stream_endpoints.hpp"
#include "src/core/psn/rest/error_reply.hpp"
#include "src/core/psn/user_agents.hpp"
#include "src/core/utils/json/ps_reply.hpp"
#include "src/core/utils/text/ascii.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <format>
#include <limits>
#include <ranges>
#include <system_error>
#include <utility>
#include <variant>

namespace libchiaking::psn::rest {

struct KamajiClient::Header {
  std::optional<std::string> status_code;
};

struct KamajiClient::Acknowledgement {
  std::optional<Header> header;
};

struct KamajiClient::SessionReply {
  struct Data {
    std::optional<std::string> country;
    std::optional<std::string> language;
  };
  std::optional<Header> header;
  std::optional<Data> data;
};

struct KamajiClient::StoresReply {
  struct Data {
    std::optional<std::string> base_url;
  };
  std::optional<Header> header;
  std::optional<Data> data;
};

struct KamajiClient::PreviewReply {
  struct Item {
    std::optional<std::string> sku_id;
  };
  struct Cart {
    std::optional<glz::raw_json> total_price_value;
    std::optional<glz::raw_json> total_price;
    std::optional<std::vector<Item>> items;
  };
  struct Data {
    std::optional<Cart> cart;
  };
  std::optional<Header> header;
  std::optional<Data> data;
};

auto KamajiClient::accepted(const std::optional<Header>& header) -> bool {
  return header && header->status_code == kamaji_status_ok;
}

template <class Reply>
auto KamajiClient::reply_of(const net::http::Response& response) -> Reply {
  return json::ps::read_reply<Reply>(response.body()).value_or(Reply{});
}

auto KamajiClient::count(const json::ps::Number& value) -> int {
  constexpr auto largest = std::numeric_limits<int>::max();
  constexpr auto smallest = std::numeric_limits<int>::min();
  if (const auto* whole = std::get_if<std::uint64_t>(&value.value()))
    return static_cast<int>(
        std::min<std::uint64_t>(*whole, static_cast<std::uint64_t>(largest)));
  if (const auto* whole = std::get_if<std::int64_t>(&value.value()))
    return static_cast<int>(
        std::clamp<std::int64_t>(*whole, smallest, largest));
  return 0;
}

auto KamajiClient::page_of(const json::ps::Tolerant<PageReply>& document)
    -> ContainerPage {
  ContainerPage page;
  if (!document) return page;
  if (const auto& total = document->total_results) {
    const auto number = json::ps::Number::from_token(total->str);
    page.total_results = number ? count(*number) : 0;
  }
  if (document->links)
    std::ranges::copy_if(
        *document->links, std::back_inserter(page.links),
        [](const glz::raw_json& link) { return link.str.starts_with('{'); });
  return page;
}

auto KamajiClient::page_in(const net::http::Response& response)
    -> Expected<ContainerPage, StoreError> {
  return document<json::ps::Tolerant<PageReply>>(response).transform(page_of);
}

auto KamajiClient::preview_of(const net::http::Response& response)
    -> Expected<CheckoutPreview, StoreError> {
  const auto status = response.status();
  const auto reply = reply_of<PreviewReply>(response);
  const auto code =
      reply.header ? reply.header->status_code.value_or("") : std::string{};
  if (code == kamaji_status_ineligible)
    return unexpected(StoreError{StoreError::Reason::ineligible, status});
  if (net::HttpStatus{status} != net::HttpStatus::ok ||
      (!code.empty() && code != kamaji_status_ok))
    return unexpected(StoreError{StoreError::Reason::refused, status});
  if (!reply.data || !reply.data->cart)
    return unexpected(StoreError{StoreError::Reason::malformed, status});
  const auto& cart = *reply.data->cart;
  const auto scalar_text =
      [](const glz::raw_json& raw) -> std::optional<std::string> {
    if (raw.str.starts_with('"')) {
      auto text = json::ps::read_reply<std::string>(raw.str);
      return text ? std::optional{std::move(text.value())} : std::nullopt;
    }
    if (!raw.str.empty() &&
        (raw.str.front() == '-' || text::ascii_digit(raw.str.front())))
      return raw.str;
    return std::nullopt;
  };
  double amount{};
  if (cart.total_price_value && cart.total_price_value->str != "null") {
    const auto price = scalar_text(*cart.total_price_value);
    if (!price)
      return unexpected(StoreError{StoreError::Reason::malformed, status});
    const auto* end = price->data() + price->size();
    const auto [parsed_end, error] =
        std::from_chars(price->data(), end, amount);
    if (error != std::errc{} || parsed_end != end)
      return unexpected(StoreError{StoreError::Reason::malformed, status});
  }
  CheckoutPreview preview;
  if (amount != 0) {
    if (cart.total_price)
      preview.display_price = scalar_text(*cart.total_price).value_or("");
    return preview;
  }
  preview.free = true;
  if (cart.items && !cart.items->empty())
    preview.sku = cart.items->front().sku_id.value_or("");
  return preview;
}

KamajiClient::KamajiClient(net::http::Client client) noexcept
    : client_(std::move(client)) {}

auto KamajiClient::with_same_session(net::http::Client client) const
    -> KamajiClient {
  KamajiClient sharing{std::move(client)};
  sharing.session_ = session_.clone();
  return sharing;
}

auto KamajiClient::send(net::http::Request request)
    -> Expected<net::http::Response, StoreError> {
  auto sent = client_.send(std::move(request));
  if (!sent) return unexpected(StoreError{sent.error()});
  return std::move(sent.value());
}

void KamajiClient::adopt_session(const net::http::Response& response) {
  for (const auto& header : response.headers()) {
    if (!text::ieq(header.name, "Set-Cookie")) continue;
    const auto cookie =
        header.value.view().substr(0, header.value.view().find(';'));
    const auto equals = cookie.find('=');
    if (equals == std::string_view::npos ||
        text::trim(cookie.substr(0, equals)) != session_cookie)
      continue;
    const auto value = text::trim(cookie.substr(equals + 1));
    if (value.size() > kamaji_session_cookie_capacity) continue;
    session_ =
        auth::SessionCookie{SecretString{value}, kamaji_session_credential};
    return;
  }
}

auto KamajiClient::create_session(const auth::AuthCode& code,
                                  std::string_view duid)
    -> Expected<KamajiSession, StoreError> {
  net::http::Form form;
  form.add("code", code.view())
      .add("client_id", legacy_cloud_stream_client_id)
      .add("duid", duid);
  if (form.view().size() >= kamaji_session_body_capacity) {
    client_.log(platform::LogSeverity::warning,
                std::format("[HTTP] {} request too large; not sent",
                            kamaji_create_session_label));
    return unexpected(StoreError{net::http::Failure::invalid_request()});
  }
  net::http::Request request{create_session_endpoint,
                             std::format("{}/user/session", api_url)};
  request
      .body("text/plain;charset=UTF-8",
            SecretBytes{std::move(form).take().bytes()})
      .header("User-Agent", app_user_agent)
      .header("X-Alt-Referer", legacy_cloud_stream_redirect_uri)
      .header("Origin", legacy_cloud_stream_origin)
      .header("Referer", legacy_cloud_stream_referer)
      .header("Accept", "*/*")
      .carries(code);
  auto sent = send(std::move(request));
  if (!sent) return unexpected(sent.error());
  const auto& response = sent.value();
  adopt_session(response);
  auto reply = reply_of<SessionReply>(response);
  auto data = std::move(reply.data).value_or(SessionReply::Data{});
  return KamajiSession{
      .http_status = response.status(),
      .accepted = accepted(reply.header),
      .country = std::move(data.country).value_or(""),
      .language = std::move(data.language).value_or(""),
  };
}

auto KamajiClient::session_get(const net::http::Endpoint& endpoint,
                               net::http::Query url)
    -> Expected<net::http::Response, StoreError> {
  net::http::Request request{endpoint, std::move(url)};
  request.header("User-Agent", app_user_agent)
      .authorize(session_)
      .header("Origin", legacy_cloud_stream_origin)
      .header("Referer", legacy_cloud_stream_referer)
      .header("Accept", "application/json");
  return send(std::move(request));
}

auto KamajiClient::stores() -> Expected<std::string, StoreError> {
  auto sent =
      session_get(stores_endpoint,
                  net::http::Query{std::format("{}/user/stores", api_url)});
  if (!sent) return unexpected(sent.error());
  const auto& response = sent.value();
  if (net::HttpStatus{response.status()} != net::HttpStatus::ok)
    return unexpected(
        StoreError{StoreError::Reason::refused, response.status()});
  auto reply = json::ps::read_reply<StoresReply>(response.body());
  if (!reply || !accepted(reply->header) || !reply->data ||
      reply->data->base_url.value_or("").empty())
    return unexpected(
        StoreError{StoreError::Reason::malformed, response.status()});
  return std::move(*reply->data->base_url);
}

auto KamajiClient::container(std::string_view base_url, int size)
    -> Expected<ContainerPage, StoreError> {
  net::http::Query url{base_url};
  url.add("size", std::to_string(size));
  auto sent = session_get(container_endpoint, std::move(url));
  if (!sent) return unexpected(sent.error());
  return page_in(sent.value());
}

auto KamajiClient::container_page(std::string_view country,
                                  std::string_view language,
                                  std::string_view container_id, int start,
                                  int size)
    -> Expected<ContainerPage, StoreError> {
  return start_container_page(country, language, container_id, start, size)
      .wait();
}

auto KamajiClient::start_container_page(std::string_view country,
                                        std::string_view language,
                                        std::string_view container_id,
                                        int start, int size) -> PendingPage {
  auto url = container_url(country, language, container_id);
  url.add("start", std::to_string(start)).add("size", std::to_string(size));
  net::http::Request request{container_endpoint, std::move(url)};
  request.header("Accept", "application/json")
      .header("User-Agent", app_user_agent);
  return PendingPage{client_.start(std::move(request))};
}

KamajiClient::PendingPage::PendingPage(net::http::Client::Pending sent) noexcept
    : sent_(std::move(sent)) {}

auto KamajiClient::PendingPage::wait() && -> Expected<ContainerPage,
                                                      StoreError> {
  auto sent = std::move(sent_).wait();
  if (!sent) return unexpected(StoreError{sent.error()});
  return page_in(sent.value());
}

auto KamajiClient::container_url(std::string_view country,
                                 std::string_view language,
                                 std::string_view container_id)
    -> net::http::Query {
  using net::http::PercentEncoding;
  net::http::Query url{
      std::format("{}{}/{}/19/{}", store_container_url,
                  PercentEncoding::component_truncated(
                      country, kamaji_locale_component_capacity),
                  PercentEncoding::component_truncated(
                      language, kamaji_locale_component_capacity),
                  PercentEncoding::component_truncated(
                      container_id, kamaji_title_id_capacity))};
  url.add("useOffers", "true").add("gkb", "1").add("gkb2", "1");
  return url;
}

auto KamajiClient::title_container_request(std::string_view country,
                                           std::string_view language,
                                           std::string_view title_id)
    -> net::http::Request {
  net::http::Request request{container_endpoint,
                             container_url(country, language, title_id)};
  request.header("Accept", "application/json")
      .header("User-Agent", browser_user_agent);
  return request;
}

auto KamajiClient::category(std::string_view category_url, int start, int size)
    -> Expected<ContainerPage, StoreError> {
  net::http::Query url{category_url};
  url.add("start", std::to_string(start)).add("size", std::to_string(size));
  net::http::Request request{category_endpoint, std::move(url)};
  request.header("Content-Type", "application/json")
      .header("Accept", "application/json")
      .header("User-Agent", browser_user_agent);
  auto sent = send(std::move(request));
  if (!sent) return unexpected(sent.error());
  return page_in(sent.value());
}

auto KamajiClient::checkout_request(const net::http::Endpoint& endpoint,
                                    std::string_view url,
                                    const auth::BearerToken& bearer) const
    -> net::http::Request {
  net::http::Request request{endpoint, url};
  request.authorize(bearer).header("User-Agent", app_user_agent);
  if (session_.empty())
    request.header("Accept", "application/json");
  else
    request.authorize(session_);
  request.credential_label(bearer.label());
  return request;
}

auto KamajiClient::buynow_preview(const auth::BearerToken& bearer,
                                  std::string_view sku)
    -> Expected<CheckoutPreview, StoreError> {
  net::http::Form form;
  form.add("sku", sku);
  auto request = checkout_request(
      buynow_preview_endpoint,
      std::format("{}/user/checkout/buynow/preview", api_url), bearer);
  request
      .body("application/x-www-form-urlencoded; charset=UTF-8",
            SecretBytes{std::move(form).take().bytes()})
      .header("Accept", "application/json, text/javascript, */*; q=0.01")
      .header("X-Requested-With", "XMLHttpRequest")
      .header("Origin", legacy_cloud_stream_origin)
      .header("Referer", legacy_cloud_stream_referer);
  auto sent = send(std::move(request));
  if (!sent) return unexpected(sent.error());
  adopt_session(sent.value());
  return preview_of(sent.value());
}

auto KamajiClient::buynow(const auth::BearerToken& bearer, std::string_view sku)
    -> Expected<void, StoreError> {
  net::http::Form form;
  form.add("sku", sku).add("skipEmail", "true");
  auto request = checkout_request(
      buynow_endpoint, std::format("{}/user/checkout/buynow", api_url), bearer);
  request.header("Accept", "application/json").form(std::move(form));
  auto sent = send(std::move(request));
  if (!sent) return unexpected(sent.error());
  adopt_session(sent.value());
  const auto& response = sent.value();
  const auto reply = json::ps::read_reply<Acknowledgement>(response.body());
  if (net::HttpStatus{response.status()} != net::HttpStatus::ok || !reply ||
      !accepted(reply->header))
    return unexpected(
        StoreError{StoreError::Reason::refused, response.status()});
  return {};
}

}  // namespace libchiaking::psn::rest
