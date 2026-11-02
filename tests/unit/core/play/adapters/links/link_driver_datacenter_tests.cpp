#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/link_driver_rig.hpp"
#include "tests/support/live_providers.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>

namespace play_test {

using test_support::LiveProviders;
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
using link_drivers::IdleSession;
using link_drivers::RecordingSink;
using link_drivers::Rig;
using remote_play::StreamError;
using namespace std::string_view_literals;

TEST(LinkDriverDatacenter, ForwardsAndLogsTheChoice) {
  Rig rig;
  EXPECT_TRUE(rig.driver().select_datacenter("lax1"));
  EXPECT_EQ(rig.session().selected, "lax1");
  EXPECT_TRUE(rig.logged("datacenter choice accepted: lax1"));
}

TEST(LinkDriverDatacenter, DoesNotLogAChoiceTheSessionRefuses) {
  Rig rig;
  const auto refused = rig.driver().select_datacenter("missing");
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), StreamError::invalid_configuration);
  EXPECT_FALSE(rig.logged("datacenter choice accepted: missing"));
}

class LinkDriverDatacenterNames : public ::testing::Test {
 protected:
  auto driver() -> libchiaking::play::LinkDriver& { return driver_; }

 private:
  RecordingSink events_;
  libchiaking::diagnostics::LogQueue log_{LiveProviders::steady()};
  libchiaking::play::LinkDriver driver_{std::make_unique<IdleSession>(),
                                        libchiaking::platform::StopSource{},
                                        LiveProviders::threading(), events_, log_};
};

TEST_F(LinkDriverDatacenterNames, AreRefusedWhenEmptyOrWithANul) {
  for (const auto name : {""sv, "dc\0"sv}) {
    SCOPED_TRACE(testing::PrintToString(std::string{name}));
    const auto refused = driver().select_datacenter(name);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error(), StreamError::invalid_argument);
  }
}

TEST_F(LinkDriverDatacenterNames, AreRefusedWhenOversized) {
  const std::string oversized(libchiaking::play::DatacenterName::limit + 1,
                              'd');
  const auto refused = driver().select_datacenter(oversized);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), StreamError::invalid_argument);
}

TEST_F(LinkDriverDatacenterNames, ReachTheSessionWhenWellFormed) {
  for (const auto name : {"dc1"sv, "Auto"sv}) {
    SCOPED_TRACE(name);
    const auto forwarded = driver().select_datacenter(name);
    ASSERT_FALSE(forwarded);
    EXPECT_EQ(forwarded.error(), StreamError::invalid_state);
  }
}

}  // namespace
}  // namespace play_test
