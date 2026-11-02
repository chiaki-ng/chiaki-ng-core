#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/process_threading.hpp"
#include "tests/support/psn/fakes.hpp"
#include "tests/support/stop_states.hpp"

#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/stop_token.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include <memory>
#include <string>
#include <utility>

namespace psn_test {

class ClientRig {
 public:
  using Reply = test_support::psn::Reply;
  using StopToken = libchiaking::platform::StopToken;

  ClientRig() = default;
  ClientRig(const ClientRig&) = delete;
  auto operator=(const ClientRig&) -> ClientRig& = delete;

  static auto threading() -> libchiaking::platform::DefaultThreadingProvider& {
    return test_support::process_threading();
  }

  static auto steady_clock() -> const libchiaking::platform::SteadyClock& {
    static const libchiaking::platform::SteadyClock clock;
    return clock;
  }

  static auto redirect(std::string location, int status = 302) -> Reply {
    Reply reply{status, ""};
    reply.headers.push_back({"Location", std::move(location)});
    return reply;
  }

  static auto fired_stop() -> StopToken {
    return StopToken{std::make_shared<test_support::Stopped>()};
  }

  void stop() { state->stopped = true; }

  auto stopping_during(Reply reply) const -> Reply {
    reply.during = [kept = state] { kept->stopped = true; };
    return reply;
  }

  test_support::psn::FakeHttp http;
  std::shared_ptr<test_support::StopState> state =
      std::make_shared<test_support::StopState>();
  StopToken stop_token{state};
  test_support::FakeLog log;
  test_support::FakeWaiter waiter;
  libchiaking::net::http::Client client{http, steady_clock(), stop_token, &log,
                                        &waiter};
};

}  // namespace psn_test
