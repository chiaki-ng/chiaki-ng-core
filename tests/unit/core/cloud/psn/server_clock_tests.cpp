#include "src/core/psn/server_clock.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/stop_states.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace cloud_test {
namespace {

namespace psn = libchiaking::psn;
namespace platform = libchiaking::platform;

class ServerClockDates : public ::testing::Test {
 protected:
  static auto parse(std::string_view text) -> std::optional<std::int64_t> {
    return psn::ServerClock::parse_http_date(text);
  }
};

TEST_F(ServerClockDates, ParseToExactMilliseconds) {
  EXPECT_EQ(parse("Tue, 06 Oct 2026 08:00:00 GMT"), 1'791'273'600'000);
  EXPECT_EQ(parse("Thu, 01 Jan 1970 00:00:00 GMT"), 0);
  EXPECT_EQ(parse("Sat, 29 Feb 2028 23:59:59 UTC"), 1'835'481'599'000);
}

TEST_F(ServerClockDates, IgnoreCaseAndSingleDigitDays) {
  EXPECT_EQ(parse("tue, 6 oct 2026 08:00:00 gmt"), 1'791'273'600'000);
}

TEST_F(ServerClockDates, RefuseImpossibleDates) {
  EXPECT_FALSE(parse(""));
  EXPECT_FALSE(parse("Tue, 31 Feb 2026 08:00:00 GMT"));
  EXPECT_FALSE(parse("Sun, 29 Feb 2026 08:00:00 GMT"));
  EXPECT_FALSE(parse("Thu, 31 Apr 2026 08:00:00 GMT"));
  EXPECT_FALSE(parse("Tue, 06 Foo 2026 08:00:00 GMT"));
  EXPECT_FALSE(parse("Tue, 06 Oct 2026 24:00:00 GMT"));
  EXPECT_FALSE(parse("Tue, 06 Oct 1969 08:00:00 GMT"));
}

TEST_F(ServerClockDates, RefuseOtherZonesAndFormats) {
  EXPECT_FALSE(parse("Tue, 06 Oct 2026 08:00:00 PST"));
  EXPECT_FALSE(parse("Tue, 06 Oct 2026 08:00:00 +0000"));
  EXPECT_FALSE(parse("Tue, 06 Oct 2026 08:00 GMT"));
  EXPECT_FALSE(parse("Tue, 06 Oct 2026 08:00:00 GMT trailing"));
  EXPECT_FALSE(parse("2026-10-06T08:00:00Z"));
}

TEST(ServerClockHosts, AreReadOnlyFromPlainHttpsUrls) {
  EXPECT_EQ(psn::ServerClock::url_host("https://ca.account.sony.com/api/authz"),
            "ca.account.sony.com");
  EXPECT_EQ(psn::ServerClock::url_host("HTTPS://cc.prod.gaikai.com:443/v1?x=1"),
            "cc.prod.gaikai.com");
  EXPECT_TRUE(
      psn::ServerClock::url_host("https://user@cc.prod.gaikai.com/v1").empty());
  EXPECT_TRUE(
      psn::ServerClock::url_host("http://ca.account.sony.com/").empty());
  EXPECT_TRUE(psn::ServerClock::url_host("gaikai://local?code=X").empty());
}

TEST(ServerClockHosts, TrustOnlyPlayStationDomains) {
  EXPECT_TRUE(psn::ServerClock::is_trusted_host("ca.account.sony.com"));
  EXPECT_TRUE(psn::ServerClock::is_trusted_host("psnow.playstation.com"));
  EXPECT_TRUE(
      psn::ServerClock::is_trusted_host("commerce.api.np.km.playstation.net"));
  EXPECT_TRUE(psn::ServerClock::is_trusted_host("cc.prod.gaikai.com"));
  EXPECT_TRUE(
      psn::ServerClock::is_trusted_host("id.sonyentertainmentnetwork.com"));
  EXPECT_TRUE(psn::ServerClock::is_trusted_host("SONY.COM"));
  EXPECT_FALSE(psn::ServerClock::is_trusted_host("notsony.com"));
  EXPECT_FALSE(psn::ServerClock::is_trusted_host("sony.com.example.net"));
  EXPECT_FALSE(psn::ServerClock::is_trusted_host("example.com"));
  EXPECT_FALSE(psn::ServerClock::is_trusted_host(""));
}

class ServerClock : public ::testing::Test {
 protected:
  static auto exchange(std::string url, std::string final_url, std::string name,
                       std::string value)
      -> std::pair<platform::HttpRequest, platform::HttpResponse> {
    platform::HttpRequest request;
    request.url.assign(url);
    platform::HttpResponse response;
    response.final_url.assign(final_url);
    response.headers.push_back(
        {"Content-Type", libchiaking::SecretString{"application/json"}});
    response.headers.push_back(
        {std::move(name), libchiaking::SecretString{value}});
    return {std::move(request), std::move(response)};
  }

  void observe(std::string url, std::string final_url, std::string name,
               std::string value) {
    auto [request, response] = exchange(std::move(url), std::move(final_url),
                                        std::move(name), std::move(value));
    clock.observe(request, response);
  }

  test_support::FakeClock monotonic{0};
  psn::ServerClock clock{monotonic};
};

TEST_F(ServerClock, KnowsNothingBeforeADate) {
  monotonic.now_us = 7'000'000;
  EXPECT_FALSE(clock.now_ms());
}

TEST_F(ServerClock, FollowsTheMonotonicSourceFromTheLastPsDate) {
  monotonic.now_us = 7'000'000;
  observe("https://ca.account.sony.com/x", "", "date",
          " Tue, 06 Oct 2026 08:00:00 GMT\r");
  EXPECT_EQ(clock.now_ms(), 1'791'273'600'000);
  monotonic.now_us += 90'500'000;
  EXPECT_EQ(clock.now_ms(), 1'791'273'690'500);
  observe("https://cc.prod.gaikai.com/v1/start", "", "Date",
          "Tue, 06 Oct 2026 09:00:00 GMT");
  EXPECT_EQ(clock.now_ms(), 1'791'277'200'000);
}

TEST_F(ServerClock, ForgetsTheDateOnReset) {
  monotonic.now_us = 7'000'000;
  observe("https://ca.account.sony.com/x", "", "date",
          " Tue, 06 Oct 2026 08:00:00 GMT\r");
  ASSERT_TRUE(clock.now_ms());
  clock.reset();
  EXPECT_FALSE(clock.now_ms());
}

TEST_F(ServerClock, IgnoresAForeignHost) {
  observe("https://example.com/", "", "Date", "Tue, 06 Oct 2026 08:00:00 GMT");
  EXPECT_FALSE(clock.now_ms());
}

TEST_F(ServerClock, IgnoresPlainHttp) {
  observe("http://ca.account.sony.com/", "", "Date",
          "Tue, 06 Oct 2026 08:00:00 GMT");
  EXPECT_FALSE(clock.now_ms());
}

TEST_F(ServerClock, IgnoresARedirectToAForeignHost) {
  observe("https://ca.account.sony.com/", "https://example.com/landing", "Date",
          "Tue, 06 Oct 2026 08:00:00 GMT");
  EXPECT_FALSE(clock.now_ms());
}

TEST_F(ServerClock, IgnoresAMalformedDate) {
  observe("https://ca.account.sony.com/", "", "Date", "soon");
  EXPECT_FALSE(clock.now_ms());
}

TEST_F(ServerClock, IgnoresOtherDateHeaders) {
  observe("https://ca.account.sony.com/", "", "Last-Modified",
          "Tue, 06 Oct 2026 08:00:00 GMT");
  EXPECT_FALSE(clock.now_ms());
}

class FixedDateHttp final : public platform::HttpProvider {
 public:
  auto perform(const platform::HttpRequest&, const platform::StopToken&)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    ++calls;
    platform::HttpResponse response;
    response.status_code = 403;
    response.headers.push_back(
        {"Date", libchiaking::SecretString{"Tue, 06 Oct 2026 08:00:00 GMT"}});
    return response;
  }

  unsigned calls{};
};

TEST_F(ServerClock, IsFedByTheHttpWrapperEvenOnARefusal) {
  FixedDateHttp inner;
  psn::ServerClockHttp http{inner, clock};
  platform::HttpRequest request;
  request.url.assign("https://psnow.playstation.com/api");
  const auto response = http.perform(request, platform::StopToken{});
  ASSERT_TRUE(response);
  EXPECT_EQ(response.value().status_code, 403);
  EXPECT_EQ(inner.calls, 1U);
  EXPECT_EQ(clock.now_ms(), 1'791'273'600'000);
}

class RecordedReply final : public platform::HttpProvider {
 public:
  auto perform(const platform::HttpRequest&, const platform::StopToken& stop)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    saw_stop = stop.stop_requested();
    platform::HttpResponse response;
    response.status_code = 200;
    return response;
  }

  bool saw_stop{};
};

TEST_F(ServerClock, ForwardsTheStopTokenThroughTheHttpWrapper) {
  RecordedReply reply;
  psn::ServerClockHttp http{reply, clock};
  static_cast<void>(http.perform(
      {}, platform::StopToken{std::make_shared<test_support::Stopped>()}));
  EXPECT_TRUE(reply.saw_stop);
  static_cast<void>(http.perform({}, {}));
  EXPECT_FALSE(reply.saw_stop);
}

TEST(StopSource, CancelsItsTokensOnce) {
  platform::StopSource source;
  const auto token = source.token();
  EXPECT_TRUE(token);
  EXPECT_FALSE(token.stop_requested());
  EXPECT_FALSE(source.stop_requested());
  EXPECT_TRUE(source.request_stop());
  EXPECT_FALSE(source.request_stop());
  EXPECT_TRUE(token.stop_requested());
  EXPECT_TRUE(source.token().stop_requested());
}

TEST(StopSource, StartsUncancelled) {
  EXPECT_FALSE(platform::StopSource{}.token().stop_requested());
}

}  // namespace
}  // namespace cloud_test
