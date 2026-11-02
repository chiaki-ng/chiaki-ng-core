#include "src/core/psn/rest/commerce/commerce_client.hpp"

#include "src/core/net/transport/http/status.hpp"
#include "src/core/net/transport/http/url.hpp"
#include "src/core/utils/text/ascii.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <iterator>
#include <string>
#include <utility>
#include <variant>

namespace libchiaking::psn::rest {

auto CommerceClient::document(const net::http::Response& response)
    -> Expected<json::ps::Tolerant<Listed>, CommerceError> {
  if (!net::HttpStatus{response.status()}.is_success())
    return unexpected(CommerceError::from_status(response.status()));
  auto parsed = response.document<json::ps::Tolerant<Listed>>();
  if (!parsed)
    return unexpected(
        CommerceError{CommerceError::Reason::malformed, response.status()});
  return std::move(parsed.value());
}

auto CommerceClient::names(const Text& field, std::string_view id) -> bool {
  return field && text::c_string(*field) == id;
}

auto CommerceClient::matching(const net::http::Response& response,
                              std::string_view id)
    -> std::optional<glz::raw_json> {
  auto parsed = response.document<json::ps::Tolerant<Looked>>();
  if (!parsed || !parsed.value()) return std::nullopt;
  const auto& object = *parsed.value();
  if (object.entitlements) {
    const auto found = std::ranges::find_if(
        *object.entitlements, [id](const glz::raw_json& entry) {
          const auto identified = json::ps::read_reply<Identified>(entry.str);
          return entry.str.starts_with('{') && identified &&
                 (names(identified->id, id) ||
                  names(identified->product_id, id));
        });
    return found == object.entitlements->end()
               ? std::nullopt
               : std::optional<glz::raw_json>{*found};
  }
  if (!object.id || text::c_string(*object.id).empty()) return std::nullopt;
  return glz::raw_json{std::string{text::c_string(response.body())}};
}

CommerceError::CommerceError(net::http::Failure failure, int requests) noexcept
    : reason_(failure.is_cancelled() ? Reason::cancelled : Reason::unavailable),
      requests_(requests),
      failure_(failure) {}

CommerceError::CommerceError(Reason reason, std::uint16_t http_status,
                             int requests) noexcept
    : reason_(reason), http_status_(http_status), requests_(requests) {}

auto CommerceError::from_status(std::uint16_t http_status) noexcept
    -> CommerceError {
  if (net::HttpStatus{http_status}.is_auth_refused())
    return {Reason::auth_refused, http_status};
  if (net::HttpStatus{http_status} == net::HttpStatus::not_found)
    return {Reason::not_found, http_status};
  if (net::HttpStatus{http_status}.is_client_error())
    return {Reason::rejected, http_status};
  return {Reason::unavailable, http_status};
}

auto CommerceError::name() const noexcept -> std::string_view {
  switch (reason_) {
    case Reason::cancelled:
      return "cancelled";
    case Reason::unavailable:
      return "unavailable";
    case Reason::auth_refused:
      return "auth_refused";
    case Reason::not_found:
      return "not_found";
    case Reason::rejected:
      return "rejected";
    case Reason::malformed:
      return "malformed";
    case Reason::truncated:
      return "truncated";
  }
  return "unavailable";
}

CommerceClient::CommerceClient(net::http::Client client,
                               std::string_view user_agent) noexcept
    : client_(std::move(client)), user_agent_(user_agent) {}

auto CommerceClient::internal_entitlements(
    const auth::BearerToken& bearer, const EntitlementPageObserver& observe)
    -> Expected<Entitlements, CommerceError> {
  Entitlements result;
  int page = 0;
  const auto build = [&](int start) {
    net::http::Query url{entitlements_url};
    url.add("fields", "game_meta,sku_meta")
        .add("entitlement_type", "5")
        .add("start", std::to_string(start))
        .add("size", std::to_string(commerce_entitlements_page_size));
    net::http::Request request{entitlements_endpoint, std::move(url)};
    request.authorize(bearer).header("Accept", "application/json");
    return request;
  };
  const auto consume =
      [&](const net::http::Response& response) -> Expected<int, CommerceError> {
    ++page;
    auto parsed = document(response);
    if (observe) {
      if (parsed)
        observe(page, response.status());
      else
        observe(page, unexpected(parsed.error()));
    }
    if (!parsed) return unexpected(parsed.error());
    const auto& listed = parsed.value();
    if (!listed || !listed->entitlements) return 0;
    std::ranges::copy(*listed->entitlements, std::back_inserter(result.items));
    return static_cast<int>(listed->entitlements->size());
  };
  auto paged = client_.pages(entitlement_paging, build, consume);
  if (!paged) {
    const auto& failed = paged.error();
    if (const auto* failure = std::get_if<net::http::Failure>(&failed.cause)) {
      CommerceError error{*failure, failed.requests};
      if (observe) observe(failed.requests, unexpected(error));
      return unexpected(error);
    }
    const auto& refused = std::get<CommerceError>(failed.cause);
    return unexpected(CommerceError{refused.reason(), refused.http_status(),
                                    failed.requests});
  }
  if (paged.value().truncated)
    return unexpected(CommerceError{CommerceError::Reason::truncated,
                                    paged.value().last_status,
                                    paged.value().requests});
  result.requests = paged.value().requests;
  result.last_status = paged.value().last_status;
  return result;
}

auto CommerceClient::internal_entitlement(const auth::BearerToken& bearer,
                                          std::string_view id)
    -> Expected<EntitlementLookup, CommerceError> {
  net::http::Query url{
      std::format("{}/{}", entitlements_url,
                  net::http::PercentEncoding::component_truncated(
                      id, commerce_entitlement_id_capacity))};
  url.add("fields", "game_meta,sku_meta");
  net::http::Request request{entitlement_endpoint, std::move(url)};
  request.authorize(bearer);
  if (!user_agent_.empty()) request.header("User-Agent", user_agent_);
  request.header("Accept", "application/json");
  auto sent = client_.send(std::move(request));
  if (!sent) return unexpected(CommerceError{sent.error()});
  const auto& response = sent.value();
  const auto status = response.status();
  if (net::HttpStatus{status} == net::HttpStatus::not_found)
    return EntitlementLookup{
        .owned = false, .http_status = status, .entitlement = std::nullopt};
  if (!net::HttpStatus{status}.is_success())
    return unexpected(CommerceError::from_status(status));
  if (net::HttpStatus{status} != net::HttpStatus::ok)
    return unexpected(CommerceError{CommerceError::Reason::rejected, status});
  return EntitlementLookup{
      .owned = true,
      .http_status = status,
      .entitlement = matching(response, id),
  };
}

}  // namespace libchiaking::psn::rest
