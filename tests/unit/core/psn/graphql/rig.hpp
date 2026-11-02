#pragma once

#include "src/core/net/transport/http/url.hpp"
#include "src/core/psn/auth/app_token_slot.hpp"
#include "src/core/psn/graphql/operation.hpp"
#include "src/core/psn/graphql/sender.hpp"
#include "src/core/psn/graphql/stats.hpp"
#include "tests/unit/core/psn/session_rig.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace psn_test {

class GraphqlRig : public SessionRig {
 public:
  using Auth = libchiaking::psn::graphql::Auth;
  using Endpoint = libchiaking::net::http::Endpoint;
  using GraphqlError = libchiaking::psn::graphql::GraphqlError;
  using Kind = GraphqlError::Kind;
  using Operation = libchiaking::psn::graphql::Operation;
  using Sent =
      libchiaking::Expected<libchiaking::psn::graphql::Result, GraphqlError>;

  static constexpr std::int64_t wall_start_ms = 1'900'000'000'000;
  static constexpr Operation probe{
      "metSubscriptionBenefitsQuery",
      "f5bf4ed3645ecc3a1063f75992d154a294c2c19b5150774f73c501fc834e368b",
      libchiaking::psn::graphql::web_endpoint,
      "psn.graphql.web.metSubscriptionBenefitsQuery",
      Auth::none,
      1};

  GraphqlRig()
      : SessionRig{static_cast<std::uint64_t>(wall_start_ms) * 1000U} {}

  static auto with_auth(Operation operation, Auth auth) -> Operation {
    operation.auth = auth;
    return operation;
  }

  static auto url_of(const Operation& operation, std::string_view variables)
      -> std::string {
    const auto extensions =
        R"({"persistedQuery":{"version":)" + std::to_string(operation.version) +
        R"(,"sha256Hash":")" + std::string{operation.sha256} + R"("}})";
    return std::string{operation.endpoint} + "?operationName=" +
           libchiaking::net::http::PercentEncoding::form(operation.name) +
           "&variables=" +
           libchiaking::net::http::PercentEncoding::form(variables) +
           "&extensions=" +
           libchiaking::net::http::PercentEncoding::form(extensions);
  }

  static auto retrying(int retries, int delay_ms = 0) -> Endpoint {
    Endpoint endpoint;
    endpoint.retry.retries = retries;
    endpoint.retry.delay = std::chrono::milliseconds{delay_ms};
    return endpoint;
  }

  template <class Value>
  static auto kind_of(const libchiaking::Expected<Value, GraphqlError>& result)
      -> std::optional<Kind> {
    if (result) return std::nullopt;
    return result.error().kind();
  }

  static auto run(std::vector<Reply> replies, Endpoint endpoint = {}) -> Sent {
    GraphqlRig rig;
    rig.http.on("GET", url_of(probe, "{}"), std::move(replies));
    return rig.send(probe, endpoint);
  }

  void supply(std::string_view token_text, std::int64_t expires_at_ms = 0) {
    slot.supply(libchiaking::psn::auth::AppToken{token_text},
                std::chrono::system_clock::time_point{
                    std::chrono::milliseconds{expires_at_ms}});
  }

  auto send(const Operation& operation, Endpoint endpoint = {},
            libchiaking::psn::auth::AppTokenSlot* credentials = nullptr)
      -> Sent {
    if (endpoint.label.empty()) endpoint.label = operation.label;
    libchiaking::psn::graphql::Sender sender{client, &stats};
    const auto variables = libchiaking::psn::graphql::Variables::none();
    if (credentials == nullptr)
      return sender.send_public(operation, endpoint, variables, {});
    return sender.send_authenticated(operation, endpoint, variables, {},
                                     *credentials);
  }

  auto app_token_uses() -> std::optional<std::uint64_t> {
    const auto tokens = snapshot().tokens;
    if (tokens.empty()) return std::nullopt;
    return tokens.front().usage.uses;
  }

  libchiaking::psn::auth::AppTokenSlot& slot = app_token();
  libchiaking::psn::graphql::Stats stats{threading(), wall};
};

}  // namespace psn_test
