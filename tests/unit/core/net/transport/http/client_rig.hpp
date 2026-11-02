#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/psn/fakes.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/support/waiter.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>

namespace http_test {

struct TestBearerKind {
  static constexpr std::string_view label = "test bearer";
  static constexpr libchiaking::net::http::CredentialPlacement placement{
      "Authorization", "Bearer "};
  static constexpr auto credential =
      libchiaking::platform::HttpCredential::account_token;
};

struct TestCookieKind {
  static constexpr std::string_view label = "test cookie";
  static constexpr libchiaking::net::http::CredentialPlacement placement{
      "Cookie", "npsso="};
  static constexpr auto credential =
      libchiaking::platform::HttpCredential::sign_in_cookie;
};

struct TestCodeKind {
  static constexpr std::string_view label = "test code";
};

using TestBearer = libchiaking::net::http::Credential<TestBearerKind>;
using TestCookie = libchiaking::net::http::Credential<TestCookieKind>;
using TestCode = libchiaking::net::http::Credential<TestCodeKind>;

class ClientRig : public ::testing::Test {
 public:
  static constexpr std::string_view url =
      "https://m.np.playstation.com/api/example";
  static constexpr libchiaking::net::http::Endpoint plain{.label =
                                                              "test.plain"};
  static constexpr libchiaking::net::http::Endpoint posted{
      .label = "test.post", .method = libchiaking::net::http::Method::post};

  static auto at(std::string_view suffix) -> std::string {
    return std::string{url} + std::string{suffix};
  }

 protected:
  test_support::psn::FakeHttp fake;
  test_support::FakeWaiter waiter;
  test_support::FakeLog log;
  std::shared_ptr<test_support::StopState> state =
      std::make_shared<test_support::StopState>();
  libchiaking::platform::StopToken stop{state};
  libchiaking::platform::SteadyClock clock;
  libchiaking::net::http::Client client{fake, clock, stop, &log, &waiter};
};

}  // namespace http_test
