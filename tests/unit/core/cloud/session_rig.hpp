#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/play/adapters/links/cloud/server_reservation.hpp"
#include "src/core/users/ports/play_calls.hpp"
#include "src/core/utils/diagnostics/trace.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/play/signed_in_player.hpp"
#include "tests/support/waiter.hpp"
#include "tests/unit/core/cloud/rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace cloud_test {

class Chain {
 public:
  Chain(platform::HttpProvider& provider, const platform::StopToken& stop,
        const libchiaking::diagnostics::Trace* trace = nullptr)
      : provider_(provider),
        stop_(stop),
        trace_(trace),
        http_(provider, clock_, stop, trace ? trace->sink() : nullptr),
        client_(http_) {
    waiter_.answer = false;
  }

  Chain(const Chain&) = delete;
  auto operator=(const Chain&) -> Chain& = delete;

  auto open(gk::LaunchSpec spec) -> libchiaking::Expected<void, gk::Error> {
    auto opened = play::ServerReservation::open(
        client_, {waiter_, trace_}, std::move(spec), CloudPlatform::ps5);
    if (!opened) return libchiaking::unexpected(opened.error());
    session_.emplace(std::move(opened.value()));
    return {};
  }

  auto grant(gk::LaunchSpec spec, std::string_view duid)
      -> libchiaking::Expected<void, gk::Error> {
    auto opened = open(std::move(spec));
    if (!opened) return opened;
    player_.emplace(
        test_support::play::SignedInPlayer::saved("synthetic-token", duid));
    return grant(*player_->user);
  }

  auto grant(gk::LaunchSpec spec, libchiaking::users::PsUser& user)
      -> libchiaking::Expected<void, gk::Error> {
    auto opened = open(std::move(spec));
    if (!opened) return opened;
    return grant(user);
  }

  auto provision(gk::LaunchSpec spec, std::string_view duid,
                 play::ServerReservation::Probes probes,
                 std::string_view forced, int bitrate_kbps)
      -> libchiaking::Expected<play::ServerSlot, gk::Error> {
    auto granted = grant(std::move(spec), duid);
    if (!granted) return libchiaking::unexpected(granted.error());
    return session_->provision(probes, forced, {}, bitrate_kbps);
  }

  auto session() -> play::ServerReservation& { return session_.value(); }

 private:
  auto grant(libchiaking::users::PsUser& user)
      -> libchiaking::Expected<void, gk::Error> {
    libchiaking::net::http::Client account{provider_, clock_, stop_};
    libchiaking::users::PlayCalls calls{user};
    auto signed_in = session_->sign_in(calls, account);
    if (!signed_in) return signed_in;
    return session_->lock();
  }

  platform::HttpProvider& provider_;
  platform::StopToken stop_;
  const libchiaking::diagnostics::Trace* trace_;
  platform::SteadyClock clock_;
  test_support::FakeWaiter waiter_;
  libchiaking::net::http::Client http_;
  gk::Client client_;
  std::optional<play::ServerReservation> session_;
  std::optional<test_support::play::SignedInPlayer> player_;
};

class GatewayProvider final : public platform::HttpProvider {
 public:
  static auto session_key(const platform::HttpRequest& request)
      -> std::string_view {
    EXPECT_GT(request.headers.size(), std::size_t{4});
    return request.headers.size() > 4 ? request.headers[4].value.view()
                                      : std::string_view{};
  }

  auto perform(const platform::HttpRequest& request, const platform::StopToken&)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    if (preflight_expected && preflight_calls == 0) {
      ++preflight_calls;
      EXPECT_NE(request.url.view().find("authorizeCheck"), std::string::npos);
      platform::HttpResponse response;
      response.status_code = reject_preflight ? 403 : 204;
      if (!server_date.empty())
        response.headers.push_back(
            {"Date", libchiaking::SecretString{server_date}});
      return response;
    }
    ++calls;
    platform::HttpResponse response;
    response.status_code = 200;
    std::string_view body;
    const auto url = request.url.view();
    switch (calls) {
      case 1:
        EXPECT_NE(url.find("/client_ids"), std::string::npos);
        body =
            R"({"gkClientId":"gk-id","ps3GkClientId":"ps3-id","streamServerClientId":"server-id"})";
        break;
      case 2:
        EXPECT_TRUE(url.ends_with("/config")) << url;
        body = R"({"configKey":"config-key"})";
        break;
      case 3:
        EXPECT_NE(url.find("/sessions/start"), std::string::npos);
        body = R"({"sessionId":"synthetic-session"})";
        response.headers.push_back(
            {"X-Gaikai-Session", libchiaking::SecretString{"start-key"}});
        break;
      case 4:
      case 5:
        EXPECT_EQ(request.method, platform::HttpMethod::get);
        response.status_code = 302;
        response.headers.push_back(
            {"Location",
             libchiaking::SecretString{
                 calls == 4
                     ? (request.url.view().find("grc-response") !=
                                std::string_view::npos
                            ? std::string{"https://psnow.playstation.com/app/"
                                          "2.2.0/133/5cdcc037d/"
                                          "grc-response.html"}
                            : std::string{"gaikai://local"}) +
                           "?code=CLIENT"
                     : (request.url.view().find("grc-response") !=
                                std::string_view::npos
                            ? std::string{"https://psnow.playstation.com/app/"
                                          "2.2.0/133/5cdcc037d/"
                                          "grc-response.html"}
                            : std::string{"gaikai://local"}) +
                           "?code=SERVER"}});
        break;
      case 6:
        EXPECT_NE(url.find("/authorize"), std::string::npos);
        EXPECT_EQ(session_key(request), "start-key");
        response.headers.push_back(
            {"X-Gaikai-Session", libchiaking::SecretString{"authorize-key"}});
        break;
      case 7:
        EXPECT_NE(url.find("/lock?forceLogout=true"), std::string::npos);
        EXPECT_EQ(session_key(request), "authorize-key");
        body = R"({"lockAcquired":true,"pollFrequency":5})";
        break;
      case 8:
        EXPECT_TRUE(url.ends_with("/datacenters")) << url;
        EXPECT_EQ(session_key(request), "authorize-key");
        body =
            R"([{"dataCenter":"dc1","publicIp":"192.0.2.1","port":2053,"maxBandwidth":20000}])";
        response.headers.push_back(
            {"X-Gaikai-Session", libchiaking::SecretString{"datacenter-key"}});
        break;
      case 9:
        EXPECT_TRUE(url.ends_with("/datacenters/select")) << url;
        EXPECT_EQ(session_key(request), "datacenter-key");
        body = R"({"port":8975})";
        response.headers.push_back(
            {"X-Gaikai-Session", libchiaking::SecretString{"select-key"}});
        break;
      case 10:
        EXPECT_TRUE(url.ends_with("/allocate")) << url;
        EXPECT_EQ(session_key(request), "select-key");
        body =
            R"({"launchSlot":{"publicIp":"203.0.113.5","port":9295,"privateIp":"10.0.0.7"},"handshakeKey":"synthetic-handshake","launchSpecification":"synthetic-launch","sessionId":"synthetic-session"})";
        response.headers.push_back(
            {"X-Gaikai-Session", libchiaking::SecretString{"allocation-key"}});
        break;
      default:
        ADD_FAILURE() << "unexpected request " << calls << " to " << url;
    }
    if (calls == fail_at) response.status_code = 500;
    for (char character : body)
      response.body.push_back(static_cast<std::byte>(character));
    return response;
  }

  unsigned calls{};
  unsigned fail_at{};
  unsigned preflight_calls{};
  bool preflight_expected{};
  bool reject_preflight{};
  std::string server_date;
};

}  // namespace cloud_test
