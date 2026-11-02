#pragma once

#include <libchiaking/platform/http/provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>
#include <libchiaking/users_types.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace test_support::play {

class ScriptedCloudPs final : public libchiaking::platform::HttpProvider {
 public:
  auto perform(const libchiaking::platform::HttpRequest& request,
               const libchiaking::platform::StopToken&)
      -> libchiaking::Expected<libchiaking::platform::HttpResponse,
                               libchiaking::platform::HttpError> override {
    ++requests;
    const auto url = request.url.view();
    const auto has = [url](std::string_view part) {
      return url.find(part) != std::string_view::npos;
    };
    if (has("/oauth/authorizeCheck")) return replied(204, {});
    if (has("client_id=bc6b0777-"))
      return redirected(
          "https://psnow.playstation.com/app/2.2.0/133/5cdcc037d/"
          "grc-response.html?code=ACCOUNT");
    if (has("response_type=token"))
      return redirected(
          "https://psnow.playstation.com/#access_token=SYNTHETIC");
    if (has("/oauth/authorize?"))
      return redirected(
          has("client_id=server-id")
              ? (request.url.view().find("grc-response") !=
                         std::string_view::npos
                     ? std::string{"https://psnow.playstation.com/app/2.2.0/"
                                   "133/5cdcc037d/grc-response.html"}
                     : std::string{"gaikai://local"}) +
                    "?code=SERVER"
              : (request.url.view().find("grc-response") !=
                         std::string_view::npos
                     ? std::string{"https://psnow.playstation.com/app/2.2.0/"
                                   "133/5cdcc037d/grc-response.html"}
                     : std::string{"gaikai://local"}) +
                    "?code=CLIENT");
    if (url.ends_with("/user/session"))
      return replied(200, R"({"header":{"status_code":"0x0000"}})",
                     {"Set-Cookie", "JSESSIONID=SYNTHETIC; Path=/"});
    if (has("/container/")) {
      ++lookups;
      return replied(
          200,
          R"({"default_sku":{"id":"SKU","entitlements":[{"id":"RESOLVED","license_type":4}]},"playable_platform":["PS4"]})");
    }
    if (url.ends_with("/accounts/me/attributes")) return replied(204, {});
    if (has("/internal_entitlements/")) return replied(200, {});
    if (has("/client_ids"))
      return replied(
          200,
          R"({"gkClientId":"gk-id","ps3GkClientId":"ps3-id","streamServerClientId":"server-id"})");
    if (url.ends_with("/config"))
      return replied(200, R"({"configKey":"config-key"})");
    if (has("/sessions/start")) {
      ++starts;
      if (no_game_refusals > 0) {
        --no_game_refusals;
        return replied(400, R"({"name":"noGameForEntitlementId"})");
      }
      if (other_refusals > 0) {
        --other_refusals;
        return replied(400, R"({"name":"somethingElse"})");
      }
      return replied(200, R"({"sessionId":"synthetic-session"})",
                     {"X-Gaikai-Session", "start-key"});
    }
    if (url.ends_with("/authorize"))
      return replied(200, {}, {"X-Gaikai-Session", "authorize-key"});
    if (has("/lock?forceLogout=true")) {
      ++locks;
      if (refused_locks > 0) {
        --refused_locks;
        return replied(
            401,
            R"({"httpCode":401,"componentCode":"002","errors":[{"eventCode":"002.0011","description":"Not authorized"}]})");
      }
      if (busy_locks > 0) {
        --busy_locks;
        return replied(200, R"({"lockAcquired":false,"pollFrequency":)" +
                                std::to_string(lock_poll_seconds) + "}");
      }
      return replied(200, R"({"lockAcquired":true,"pollFrequency":5})");
    }
    if (url.ends_with("/datacenters"))
      return replied(
          200,
          R"([{"dataCenter":"dc1","publicIp":"192.0.2.1","port":2053,"maxBandwidth":20000}])",
          {"X-Gaikai-Session", "datacenter-key"});
    if (url.ends_with("/datacenters/select"))
      return replied(200, R"({"port":8975})",
                     {"X-Gaikai-Session", "select-key"});
    if (url.ends_with("/allocate")) {
      if (!waits.empty()) {
        const auto body = waits.front();
        waits.erase(waits.begin());
        return replied(200, body);
      }
      return replied(
          200,
          R"({"launchSlot":{"publicIp":"203.0.113.5","port":9295,"privateIp":"10.0.0.7"},"handshakeKey":"synthetic-handshake","launchSpecification":"synthetic-launch","sessionId":"synthetic-session"})",
          {"X-Gaikai-Session", "allocation-key"});
    }
    if (has("/users/me/clients")) {
      ++console_lists;
      if (consoles_on_account) return replied(200, *consoles_on_account);
    }
    ++unscripted;
    return replied(404, {});
  }

  static auto account_listing(std::string_view duid_byte,
                              std::string_view features) -> std::string {
    std::string duid;
    for (std::size_t index = 0; index < libchiaking::psn_device_uid_size;
         ++index)
      duid += duid_byte;
    return R"({"clients":[{"duid":")" + duid +
           R"(","device":{"name":"Living Room","enabledFeatures":)" +
           std::string{features} + "}}]}";
  }

  std::vector<std::string> waits;
  std::optional<std::string> consoles_on_account;
  unsigned console_lists{};
  unsigned no_game_refusals{};
  unsigned other_refusals{};
  unsigned busy_locks{};
  unsigned lock_poll_seconds{1};
  unsigned refused_locks{};
  unsigned locks{};
  unsigned requests{};
  unsigned starts{};
  unsigned lookups{};
  unsigned unscripted{};

 private:
  static auto redirected(std::string_view location)
      -> libchiaking::Expected<libchiaking::platform::HttpResponse,
                               libchiaking::platform::HttpError> {
    return replied(302, {}, {"Location", location});
  }

  static auto replied(std::uint16_t status, std::string_view body,
                      std::pair<std::string_view, std::string_view> header = {})
      -> libchiaking::Expected<libchiaking::platform::HttpResponse,
                               libchiaking::platform::HttpError> {
    libchiaking::platform::HttpResponse response;
    response.status_code = status;
    if (!header.first.empty())
      response.headers.push_back({std::string{header.first},
                                  libchiaking::SecretString{header.second}});
    response.body.assign(body);
    return response;
  }
};

}  // namespace test_support::play
