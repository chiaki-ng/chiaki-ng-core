#include "src/core/net/nat/stun/public_servers.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/stop_states.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace nat_test {
namespace {

namespace stun = libchiaking::net::nat::stun;
namespace pf = libchiaking::platform;
using stun::PublicServers;
using FetchError = stun::StunHostListFetchError;

class HostListHttp final : public pf::HttpProvider {
 public:
  auto perform(const pf::HttpRequest& request, const pf::StopToken& stop)
      -> libchiaking::Expected<pf::HttpResponse, pf::HttpError> override {
    ++calls;
    EXPECT_FALSE(stop.stop_requested());
    EXPECT_EQ(request.method, pf::HttpMethod::get);
    EXPECT_EQ(request.url.view(), expected_url);
    EXPECT_TRUE(request.headers.empty());
    EXPECT_TRUE(request.body.empty());
    EXPECT_EQ(request.connect_timeout, std::chrono::seconds{10});
    EXPECT_EQ(request.total_timeout, std::chrono::seconds{10});
    EXPECT_EQ(request.redirect_policy, pf::HttpRedirectPolicy::follow_https);
    EXPECT_EQ(request.maximum_response_bytes,
              PublicServers::max_host_list_bytes);
    EXPECT_EQ(request.tls_configuration.trust_mode, pf::TlsTrustMode::system);
    if (error) return libchiaking::unexpected(*error);
    return response.clone();
  }

  void set_body(std::string_view body) {
    response.body.assign(std::string_view{body});
  }

  pf::HttpResponse response;
  std::string_view expected_url{PublicServers::host_list_url};
  std::optional<pf::HttpError> error;
  unsigned calls{};
};

class StunHostListFetch : public ::testing::Test {
 protected:
  void SetUp() override {
    http.response.status_code = 200;
    http.set_body("stun.example.test:3478\n");
  }

  auto fetch_error() -> std::optional<FetchError> {
    const auto fetched = PublicServers::fetch_host_list(http, {});
    if (fetched) return std::nullopt;
    return fetched.error();
  }

  HostListHttp http;
};

TEST_F(StunHostListFetch, GetsTheListOnceWithTheExpectedRequest) {
  const auto ok = PublicServers::fetch_host_list(http, {});
  ASSERT_TRUE(ok);
  ASSERT_EQ(ok.value().size(), 1U);
  EXPECT_EQ(http.calls, 1U);
  EXPECT_EQ(ok.value()[0].host, "stun.example.test");
  EXPECT_EQ(ok.value()[0].port, 3478);
}

TEST_F(StunHostListFetch, ReportsAFailedStatus) {
  http.response.status_code = 404;
  EXPECT_EQ(fetch_error(), FetchError::http_status);
}

TEST_F(StunHostListFetch, ReportsATransportFailureOrCancellation) {
  http.error = pf::HttpError::timed_out;
  EXPECT_EQ(fetch_error(), FetchError::transport);
  http.error = pf::HttpError::cancelled;
  EXPECT_EQ(fetch_error(), FetchError::cancelled);
}

TEST_F(StunHostListFetch, ReportsAListWithoutHosts) {
  http.set_body("\nnot-a-host-line\n");
  EXPECT_EQ(fetch_error(), FetchError::empty);
  http.response.body.clear();
  EXPECT_EQ(fetch_error(), FetchError::empty);
}

TEST_F(StunHostListFetch, ReportsAListThatDoesNotParse) {
  http.set_body(std::string(PublicServers::max_host_line_bytes + 1U, 'a'));
  EXPECT_EQ(fetch_error(), FetchError::parse);
  http.set_body(std::string(PublicServers::max_host_list_bytes + 1U, 'a'));
  EXPECT_EQ(fetch_error(), FetchError::parse);
}

TEST_F(StunHostListFetch, AsksNothingOnceStopped) {
  const pf::StopToken stopped{std::make_shared<test_support::Stopped>()};
  const auto before = http.calls;
  const auto pre_cancelled = PublicServers::fetch_host_list(http, stopped);
  ASSERT_FALSE(pre_cancelled);
  EXPECT_EQ(pre_cancelled.error(), FetchError::cancelled);
  EXPECT_EQ(http.calls, before);
}

TEST_F(StunHostListFetch, SaysWhetherTheIpv6ListShouldFollow) {
  bool ipv6_follows{};
  http.set_body("a.example:1\n");
  EXPECT_TRUE(PublicServers::fetch_host_list(http, {}, &ipv6_follows));
  EXPECT_TRUE(ipv6_follows);
  http.set_body("");
  EXPECT_FALSE(PublicServers::fetch_host_list(http, {}, &ipv6_follows));
  EXPECT_TRUE(ipv6_follows);
  http.set_body("a.example\n");
  EXPECT_FALSE(PublicServers::fetch_host_list(http, {}, &ipv6_follows));
  EXPECT_FALSE(ipv6_follows);
  http.response.status_code = 500;
  ipv6_follows = true;
  EXPECT_FALSE(PublicServers::fetch_host_list(http, {}, &ipv6_follows));
  EXPECT_FALSE(ipv6_follows);
}

TEST_F(StunHostListFetch, GetsTheIpv6ListWhereAnEmptyListIsFine) {
  http.expected_url = PublicServers::ipv6_host_list_url;
  http.set_body("[2001:db8::1]:3478\n");
  const auto ipv6 = PublicServers::fetch_ipv6_host_list(http, {});
  ASSERT_TRUE(ipv6);
  ASSERT_EQ(ipv6.value().size(), 1U);
  EXPECT_EQ(ipv6.value()[0].host, "2001:db8::1");
  http.response.body.clear();
  const auto ipv6_empty = PublicServers::fetch_ipv6_host_list(http, {});
  ASSERT_TRUE(ipv6_empty);
  EXPECT_TRUE(ipv6_empty.value().empty());
  http.response.status_code = 404;
  const auto ipv6_status = PublicServers::fetch_ipv6_host_list(http, {});
  ASSERT_FALSE(ipv6_status);
  EXPECT_EQ(ipv6_status.error(), FetchError::http_status);
}

}  // namespace
}  // namespace nat_test
