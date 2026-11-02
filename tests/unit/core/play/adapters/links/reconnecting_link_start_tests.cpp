#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/reconnecting_link_rig.hpp"
#include "tests/support/live_providers.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

namespace play_test {

using test_support::LiveProviders;
namespace {

namespace play = libchiaking::play;
namespace rp = libchiaking::net::protocol::remote_play;
using reconnecting_links::FakeLink;
using reconnecting_links::Rig;
using reconnecting_links::Script;
using reconnecting_links::Watcher;

class ReconnectingLinkUnopened : public ::testing::Test {
 protected:
  void SetUp() override {
    rig_.script().refuse_openings({rp::StreamError::provider_unavailable});
    started_ = rig_.link().start();
  }

  auto rig() -> Rig& { return rig_; }
  auto started() const -> const play::Link::Result& { return started_; }

 private:
  Rig rig_;
  play::Link::Result started_;
};

TEST_F(ReconnectingLinkUnopened, FailsTheLinkWithTheOpenersError) {
  ASSERT_FALSE(started());
  EXPECT_EQ(started().error(), rp::StreamError::provider_unavailable);
  EXPECT_EQ(rig().link().state(), rp::StreamState::failed);
  EXPECT_EQ(rig().sink().states(), std::vector{rp::StreamState::failed});
  ASSERT_TRUE(rig().link().failure());
  EXPECT_EQ(rig().link().failure()->result(),
            rp::StreamError::provider_unavailable);
}

TEST_F(ReconnectingLinkUnopened, RefusesToPumpOrStartAgain) {
  const auto pumped = rig().link().pump_once();
  ASSERT_FALSE(pumped);
  EXPECT_EQ(pumped.error(), rp::StreamError::invalid_state);
  EXPECT_TRUE(rig().link().stop());
  EXPECT_EQ(rig().sink().states().size(), 1U);
  const auto again = rig().link().start();
  ASSERT_FALSE(again);
  EXPECT_EQ(again.error(), rp::StreamError::invalid_state);
}

TEST(ReconnectingLinkStart, NeedsAnEventSink) {
  Script script;
  libchiaking::testing::ManualClock clock;
  Watcher watcher;
  play::ReconnectingLink orphan(
      play::Where::home,
      [&]() -> libchiaking::Expected<std::unique_ptr<play::Link>,
                                     rp::StreamError> {
        return std::unique_ptr<play::Link>{std::make_unique<FakeLink>(script)};
      },
      {}, clock, LiveProviders::threading(), watcher);
  const auto refused = orphan.start();
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), rp::StreamError::invalid_configuration);
  EXPECT_EQ(orphan.state(), rp::StreamState::idle);
}

TEST(ReconnectingLinkStart, StartsOnlyOnce) {
  Rig rig;
  EXPECT_TRUE(rig.link().start());
  const auto twice = rig.link().start();
  ASSERT_FALSE(twice);
  EXPECT_EQ(twice.error(), rp::StreamError::invalid_state);
}

}  // namespace
}  // namespace play_test
