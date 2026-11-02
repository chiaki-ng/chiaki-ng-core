#include "src/core/play/adapters/datacenters/datacenter_ranking.hpp"
#include "tests/unit/core/cloud/rig.hpp"

#include <gtest/gtest.h>

#include <memory>

namespace cloud_test {
namespace {

using Reason = gk::Error::Reason;
using Stage = gk::Error::Stage;

class ReservationSelect : public ::testing::Test {
 protected:
  static auto make_choice() -> play::DatacenterRanking::Chosen {
    gk::Ping ping;
    ping.name = "dc1";
    ping.public_ip = "192.0.2.1";
    ping.rtt_ms = 10;
    ping.rtts_ms = {10};
    ping.selected_port = 9295;
    play::DatacenterRanking::Chosen choice;
    choice.selected = ping;
    choice.ping_results = {ping};
    return choice;
  }

  SelectProvider provider;
  test_support::FakeWaiter waiter = GkRig::recording_waiter();
  std::shared_ptr<test_support::StopState> stop =
      std::make_shared<test_support::StopState>();
  SessionHarness harness{provider, waiter, SessionSeed{}, stop};
  const play::DatacenterRanking::Chosen choice = make_choice();
};

TEST_F(ReservationSelect, SendsNothingWhileOpening) {
  EXPECT_EQ(provider.calls, 0U);
  EXPECT_EQ(harness.session().config_key().view(), "synthetic-key");
}

TEST_F(ReservationSelect, RefusesAChoiceWithoutPingResults) {
  const auto result =
      harness.session().select(play::DatacenterRanking::Chosen{});
  EXPECT_TRUE(
      GkRig::failed(result, Stage::select, Reason::invalid_ping_results, 5));
  ASSERT_FALSE(result);
  EXPECT_TRUE(result.error().is_invalid_request());
  EXPECT_EQ(provider.calls, 0U);
  EXPECT_EQ(harness.session().config_key().view(), "synthetic-key");
}

TEST_F(ReservationSelect, TakesTheSelectedPort) {
  const auto result = harness.session().select(choice);
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().selected_port, 8975);
  EXPECT_EQ(provider.calls, 1U);
  EXPECT_EQ(harness.session().config_key().view(), "synthetic-key");
}

TEST_F(ReservationSelect, KeepsTheChosenPortAndTakesTheRotatedKey) {
  provider.body = "bad json";
  provider.headers = {{"X-Gaikai-Session", "new-key"}};
  const auto result = harness.session().select(choice);
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().selected_port, 9295);
  EXPECT_EQ(provider.calls, 1U);
  EXPECT_EQ(harness.session().config_key().view(), "new-key");
}

TEST_F(ReservationSelect, KeepsTheKeyWhenRejected) {
  provider.body = "bad json";
  provider.headers = {{"X-Gaikai-Session", "new-key"}};
  ASSERT_TRUE(harness.session().select(choice));
  provider.status = 500;
  EXPECT_TRUE(GkRig::failed(harness.session().select(choice), Stage::select,
                            Reason::rejected, 5));
  EXPECT_EQ(provider.calls, 2U);
  EXPECT_EQ(harness.session().config_key().view(), "new-key");
}

TEST_F(ReservationSelect, KeepsTheKeyWhenCancelled) {
  provider.body = "bad json";
  provider.headers = {{"X-Gaikai-Session", "new-key"}};
  ASSERT_TRUE(harness.session().select(choice));
  provider.error = platform::HttpError::cancelled;
  const auto result = harness.session().select(choice);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().stage(), Stage::select);
  EXPECT_TRUE(result.error().is_cancelled());
  EXPECT_EQ(result.error().detail(), 0U);
  EXPECT_EQ(provider.calls, 2U);
  EXPECT_EQ(harness.session().config_key().view(), "new-key");
}

TEST_F(ReservationSelect, SendsNothingOnceStopped) {
  stop->stopped = true;
  const auto result = harness.session().select(choice);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().stage(), Stage::select);
  EXPECT_TRUE(result.error().is_cancelled());
  EXPECT_EQ(result.error().detail(), 0U);
  EXPECT_EQ(provider.calls, 0U);
}

TEST_F(ReservationSelect, RefusesAnUnauthorizedSpecBeforeSending) {
  SessionSeed unauthorized_seed;
  unauthorized_seed.until = SessionSeed::Until::started;
  SessionHarness unauthorized{provider, waiter, unauthorized_seed};
  EXPECT_TRUE(unauthorized.session().authorized_spec().view().empty());
  const auto result = unauthorized.session().select(choice);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().stage(), Stage::select);
  EXPECT_TRUE(result.error().is_invalid_request());
  EXPECT_EQ(result.error().detail(), 5U);
  EXPECT_EQ(provider.calls, 0U);
}

}  // namespace
}  // namespace cloud_test
