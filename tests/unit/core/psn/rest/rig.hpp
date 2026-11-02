#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/rest/session_manager/session_manager.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/psn/fakes.hpp"
#include "tests/support/stop_states.hpp"

#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/stop_token.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace psn_test {

class RestRig {
 public:
  using Reply = test_support::psn::Reply;
  using Seen = test_support::psn::Seen;

  static constexpr std::string_view token = "SYNTHETIC_TEST_TOKEN";
  static constexpr std::string_view session_text =
      "01234567-89ab-cdef-0123-456789abcdef";

  static auto bearer() -> libchiaking::psn::auth::BearerToken {
    return libchiaking::psn::auth::BearerToken{token};
  }

  static auto headers_of(const Seen& seen) -> std::vector<std::string> {
    std::vector<std::string> lines;
    for (const auto& header : seen.headers)
      lines.push_back(header.name + ": " + header.value);
    return lines;
  }

  static auto session(std::string_view text)
      -> libchiaking::psn::rest::SessionId {
    return libchiaking::psn::rest::SessionId::parse(text).value();
  }

  void stop() { state->stopped = true; }

  libchiaking::platform::SteadyClock clock;
  test_support::psn::FakeHttp fake;
  test_support::FakeWaiter waiter;
  std::shared_ptr<test_support::StopState> state =
      std::make_shared<test_support::StopState>();
  libchiaking::net::http::Client http{
      fake, clock, libchiaking::platform::StopToken{state}, nullptr, &waiter};
};

}  // namespace psn_test
