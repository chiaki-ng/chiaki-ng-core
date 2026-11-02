#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/users/adapters/cloud_streaming_cache.hpp"
#include "src/core/users/adapters/ps_user_core.hpp"
#include "src/core/users/ports/catalog_calls.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/psn/fakes.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/support/users/signed_in.hpp"
#include "tests/unit/core/users/rig.hpp"

#include <libchiaking/platform/runtime/stop_token.hpp>
#include <libchiaking/users.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace users_test {

class CloudStreamingRig {
 public:
  using Reply = test_support::psn::Reply;
  using CloudStreaming = libchiaking::users::CloudStreaming;
  using Freshness = libchiaking::users::CloudStreamingCache::Freshness;
  using Remedy = CloudStreaming::Remedy;

  static constexpr std::string_view benefits_url =
      "https://m.np.playstation.com/api/subscriptions/v2/users/me/premium/"
      "features/"
      "cloudgame";
  static constexpr std::string_view services_url =
      "https://m.np.playstation.com/api/subscriptions/v2/users/me/services/"
      "pssubscriptions?days=1&useTiers=true";
  static constexpr std::string_view granted =
      R"({"authorization":true,"justification":"OK","cloudgameOwnerInfos":[)"
      R"({"ownerAccountId":"100","tierLabel":"TIER_30","benefitPlanIds":["PLAN-A","PLAN-B"],"hasEstBenefit":true},)"
      R"({"ownerAccountId":"200","tierLabel":"TIER_30","benefitPlanIds":["PLAN-C"],"hasSgodBenefit":true}]})";
  static constexpr std::string_view denied =
      R"({"authorization":false,"cloudgameOwnerInfos":[]})";

  static auto plus(std::string_view tier,
                   std::string_view status = "SUBSCRIBED", bool active = true)
      -> std::string {
    return R"({"type":"PSPLUS","state":{"active":)" +
           std::string{active ? "true" : "false"} + R"(,"status":")" +
           std::string{status} + R"("},"currentTier":{"tierLabel":")" +
           std::string{tier} + R"("}})";
  }

  static auto refused_with(std::string benefits_body,
                           std::vector<Reply> service_replies)
      -> CloudStreaming {
    CloudStreamingRig rig;
    rig.benefits({Reply{200, std::move(benefits_body)}});
    rig.services(std::move(service_replies));
    auto verdict = rig.ask();
    EXPECT_EQ(rig.asked(services_url), 1U);
    EXPECT_TRUE(rig.fake.unmatched.empty());
    return verdict;
  }

  static auto worded_by(std::string services_body) -> std::optional<Remedy> {
    return refused_with(std::string{denied},
                        {Reply{200, std::move(services_body)}})
        .remedy();
  }

  void benefits(std::vector<Reply> replies) {
    fake.on("GET", std::string{benefits_url}, std::move(replies));
  }

  void services(std::vector<Reply> replies) {
    fake.on("GET", std::string{services_url}, std::move(replies));
  }

  auto ask(Freshness freshness = Freshness::read_afresh) -> CloudStreaming {
    return libchiaking::users::CatalogCalls{*user}.cloud_streaming(client,
                                                                   freshness);
  }

  auto asked(std::string_view url) const -> std::size_t {
    return static_cast<std::size_t>(
        std::ranges::count(fake.seen, url, &test_support::psn::Seen::url));
  }

  test_support::FakeClock clock;
  test_support::FakeClock wall;
  test_support::psn::FakeHttp fake;
  std::shared_ptr<test_support::StopState> state =
      std::make_shared<test_support::StopState>();
  libchiaking::platform::StopToken stop{state};
  libchiaking::net::http::Client client{fake, Runtime::steady(), stop};
  test_support::users::SignedIn signed_in{fake, clock, wall};
  std::shared_ptr<libchiaking::users::PsUser> user =
      signed_in.as("NPSSO-A", "app", std::chrono::system_clock::time_point{});
};

}  // namespace users_test
