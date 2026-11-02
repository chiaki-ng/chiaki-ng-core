#include "libchiaking/platform/net/default.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/process_threading.hpp"
#include "tests/support/random.hpp"
#include "tests/unit/core/cloud/session_rig.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <type_traits>

namespace cloud_test {
namespace {

using Reason = gk::Error::Reason;
using Stage = gk::Error::Stage;

static_assert(!std::is_copy_constructible_v<play::ServerSlot>);
static_assert(std::is_move_constructible_v<play::ServerSlot>);

class AllocatingProvider final : public platform::HttpProvider {
 public:
  auto perform(const platform::HttpRequest& request, const platform::StopToken&)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    ++calls;
    const std::string expected_key = calls == 1   ? "initial-key"
                                     : calls == 2 ? "datacenter-key"
                                                  : "select-key";
    EXPECT_EQ(GatewayProvider::session_key(request), expected_key);
    platform::HttpResponse response;
    response.status_code = calls == 3 ? allocate_status : 200;
    std::string_view body;
    const auto url = request.url.view();
    switch (calls) {
      case 1:
        EXPECT_TRUE(url.ends_with("/datacenters")) << url;
        response.headers.push_back(
            {"X-Gaikai-Session", libchiaking::SecretString{"datacenter-key"}});
        body =
            R"([{"dataCenter":"dc1","publicIp":"192.0.2.1","port":2053,"maxBandwidth":20000}])";
        break;
      case 2:
        EXPECT_TRUE(url.ends_with("/datacenters/select")) << url;
        response.headers.push_back(
            {"X-Gaikai-Session", libchiaking::SecretString{"select-key"}});
        body = R"({"port":8975})";
        break;
      case 3:
        EXPECT_TRUE(url.ends_with("/allocate")) << url;
        EXPECT_NE(request.body.view().find("\"port\":8975"),
                  std::string_view::npos);
        response.headers.push_back(
            {"X-Gaikai-Session", libchiaking::SecretString{"allocated-key"}});
        body =
            R"({"launchSlot":{"publicIp":"203.0.113.5","port":9295,"privateIp":"10.0.0.7"},"handshakeKey":"synthetic-handshake","launchSpecification":"synthetic-launch","sessionId":"synthetic-session"})";
        break;
      default:
        ADD_FAILURE() << "unexpected request " << calls << " to " << url;
    }
    for (char character : body)
      response.body.push_back(static_cast<std::byte>(character));
    return response;
  }

  std::uint16_t allocate_status{200};
  unsigned calls{};
};

class ReservationProvision : public ::testing::Test {
 protected:
  void SetUp() override {
    ping_waiter.answer = false;
    allocate_waiter.answer = false;
  }

  static auto locked() -> SessionSeed {
    SessionSeed seed;
    seed.session_id = "synthetic-session";
    seed.config_key = "initial-key";
    seed.until = SessionSeed::Until::locked;
    return seed;
  }

  platform::SteadyClock steady;
  platform::DefaultNetworkProvider network{test_support::process_threading(),
                                           steady};
  test_support::FakeClock clock{1'000};
  test_support::FixedRandom random;
  test_support::FakeWaiter ping_waiter;
  test_support::FakeWaiter allocate_waiter;
  const play::ServerReservation::Probes probes{
      network, clock, test_support::process_threading(), random, ping_waiter};
  AllocatingProvider provider;
};

TEST_F(ReservationProvision, SelectsAndAllocatesAForcedDatacenter) {
  SessionHarness harness{provider, allocate_waiter, locked()};
  const auto result = harness.session().provision(probes, "dc1", {}, 10'000);
  ASSERT_TRUE(result);
  EXPECT_EQ(provider.calls, 3U);
  EXPECT_EQ(ping_waiter.calls(), 0);
  EXPECT_EQ(harness.session().service(), CloudService::cloud_stream);
  EXPECT_EQ(result.value().datacenter.selected_port, 8975);
  EXPECT_EQ(result.value().allocation.server_ip, "203.0.113.5");
  EXPECT_EQ(result.value().allocation.server_port, 9295);
  EXPECT_EQ(result.value().allocation.psn_wrapper_type, 7);
  EXPECT_EQ(result.value().allocation.handshake_key.view(),
            "synthetic-handshake");
  EXPECT_EQ(harness.session().config_key().view(), "allocated-key");
  ASSERT_TRUE(harness.session().lock_key());
  EXPECT_EQ(harness.session().lock_key()->view(), "initial-key");
}

TEST_F(ReservationProvision, StopsAtARefusedAllocation) {
  provider.allocate_status = 500;
  SessionHarness harness{provider, allocate_waiter, locked()};
  const auto result = harness.session().provision(probes, "dc1", {}, 10'000);
  ASSERT_FALSE(result);
  EXPECT_EQ(provider.calls, 3U);
  EXPECT_EQ(result.error().stage(), Stage::allocate);
  EXPECT_EQ(result.error().detail(), 4U);
}

TEST_F(ReservationProvision, StopsAtAnUnlistedDatacenter) {
  SessionHarness harness{provider, allocate_waiter, locked()};
  EXPECT_TRUE(
      GkRig::failed(harness.session().provision(probes, "missing", {}, 10'000),
                    Stage::select, Reason::datacenter_unavailable, 3));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(ReservationProvision, SendsNothingOnceStopped) {
  SessionHarness harness{provider, allocate_waiter, locked()};
  harness.stop();
  const auto result = harness.session().provision(probes, "dc1", {}, 10'000);
  ASSERT_FALSE(result);
  EXPECT_EQ(provider.calls, 0U);
  EXPECT_EQ(result.error().stage(), Stage::datacenters);
  EXPECT_TRUE(result.error().is_cancelled());
  EXPECT_EQ(result.error().detail(), 0U);
}

class ChainProvision : public ReservationProvision {
 protected:
  static constexpr std::string_view duid =
      "000000070041008000112233445566778899aabbccddeeff";

  static auto preauth() -> gk::LaunchSpec {
    return gk::LaunchSpec::cloud_stream(
               "synthetic-entitlement", gk::Language{"en-US"},
               gk::TimeZone::parse("UTC+00:00").value(), 1080)
        .value();
  }

  auto provision(Chain& chain) {
    return chain.provision(preauth(), duid, probes, "dc1", 10'000);
  }

  const platform::StopToken running;
  GatewayProvider gateway;
};

TEST_F(ChainProvision, OpensGrantsAndProvisionsInOneChain) {
  Chain provisioned{gateway, running};
  const auto result = provision(provisioned);
  ASSERT_TRUE(result);
  EXPECT_EQ(gateway.calls, 10U);
  EXPECT_EQ(ping_waiter.calls(), 0);
  EXPECT_EQ(result.value().datacenter.selected_port, 8975);
  EXPECT_EQ(result.value().allocation.server_ip, "203.0.113.5");
  EXPECT_EQ(result.value().allocation.handshake_key.view(),
            "synthetic-handshake");
  EXPECT_EQ(provisioned.session().config_key().view(), "allocation-key");
  ASSERT_TRUE(provisioned.session().lock_key());
  EXPECT_EQ(provisioned.session().lock_key()->view(), "authorize-key");
}

TEST_F(ChainProvision, StopsAtAGrantFailure) {
  gateway.fail_at = 2;
  Chain chain{gateway, running};
  const auto result = provision(chain);
  ASSERT_FALSE(result);
  EXPECT_EQ(gateway.calls, 2U);
  EXPECT_EQ(result.error().stage(), Stage::config);
}

TEST_F(ChainProvision, StopsAtAnAllocationFailure) {
  gateway.fail_at = 10;
  Chain chain{gateway, running};
  const auto result = provision(chain);
  ASSERT_FALSE(result);
  EXPECT_EQ(gateway.calls, 10U);
  EXPECT_EQ(result.error().stage(), Stage::allocate);
}

TEST_F(ChainProvision, SendsNothingOnceStopped) {
  Chain chain{gateway,
              platform::StopToken{std::make_shared<test_support::Stopped>()}};
  const auto result = provision(chain);
  ASSERT_FALSE(result);
  EXPECT_EQ(gateway.calls, 0U);
  EXPECT_EQ(result.error().stage(), Stage::client_ids);
}

}  // namespace
}  // namespace cloud_test
