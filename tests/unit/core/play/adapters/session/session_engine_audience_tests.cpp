#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/session/session_engine_rig.hpp"

#include "libchiaking/play_types.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <vector>

namespace play_test {
namespace {

namespace play = libchiaking::play;
using namespace std::chrono_literals;
using libchiaking::testing::ScriptedSession;
using Waits = std::vector<std::chrono::milliseconds>;

class CappedAudience final : public play::SharedAudience {
 public:
  CappedAudience(ScriptedSession& session, std::chrono::milliseconds first,
                 std::chrono::milliseconds later)
      : session_(session), first_(first), later_(later) {}

  void stream_started(const play::SharedStream&, play::ReceiveKeys) override {}
  void encrypted_datagram(const play::SharedDatagram&) noexcept override {}
  void feedback(const libchiaking::controllers::ControllerFeedback&) override {}
  void guest_seated(libchiaking::controllers::SeatNumber) override {}
  void guest_seat_refused(const play::SeatRefusal&) override {}
  void guest_unseated(libchiaking::controllers::SeatNumber) override {}
  void became_unshareable(play::NotShareable) override {}

  auto service(std::chrono::milliseconds wait)
      -> std::chrono::milliseconds override {
    offered_.push_back(wait);
    return offered_.size() == 1 ? first_ : later_;
  }

  void session_ended() override {
    ++ends_;
    ended_before_stop_ = session_.count("stop") == 0;
  }

  auto offered() const -> const Waits& { return offered_; }
  auto ends() const -> int { return ends_; }
  auto ended_before_stop() const -> bool { return ended_before_stop_; }

 private:
  ScriptedSession& session_;
  std::chrono::milliseconds first_;
  std::chrono::milliseconds later_;
  Waits offered_;
  int ends_{};
  bool ended_before_stop_{};
};

class SessionEngineAudience : public ::testing::Test {
 protected:
  static auto every_turn() -> Waits { return {8ms, 8ms, 8ms}; }

  static auto waits_shared_with(std::chrono::milliseconds cap) -> Waits {
    SessionEngineRig h;
    CappedAudience capped{*h.session, cap, cap};
    auto sharing = h.runner->share()->begin(capped);
    EXPECT_TRUE(sharing);
    h.streaming();
    h.then([](ScriptedSession&) {});
    const auto outcome = h.run();
    EXPECT_TRUE(outcome.succeeded());
    EXPECT_EQ(capped.offered(), every_turn());
    EXPECT_EQ(capped.ends(), 1);
    return h.session->waits;
  }
};

TEST_F(SessionEngineAudience, IsServicedEveryTurnAndToldWhenTheLoopEnds) {
  SessionEngineRig h;
  CappedAudience audience{*h.session, 2ms, 0ms};
  auto sharing = h.runner->share()->begin(audience);
  EXPECT_TRUE(sharing);
  h.streaming();
  h.then([](ScriptedSession&) {});
  const auto outcome = h.run();
  EXPECT_TRUE(outcome.succeeded());
  EXPECT_EQ(audience.offered(), every_turn());
  EXPECT_EQ(h.session->waits, (Waits{2ms}));
  EXPECT_EQ(audience.ends(), 1);
  EXPECT_TRUE(audience.ended_before_stop());
  const auto late = h.runner->share()->begin(audience);
  ASSERT_FALSE(late);
  EXPECT_EQ(late.error(), play::NotShareable::session_ended);
}

TEST_F(SessionEngineAudience, ShortensTheLoopsWaitWhenItAsksForLess) {
  EXPECT_EQ(waits_shared_with(6ms), (Waits{6ms, 6ms, 6ms}));
  EXPECT_EQ(waits_shared_with(3ms), (Waits{3ms, 3ms, 3ms}));
}

TEST_F(SessionEngineAudience, NeverLengthensTheLoopsWait) {
  EXPECT_EQ(waits_shared_with(50ms), every_turn());
}

TEST_F(SessionEngineAudience, IsTheOnlyOneServicedWhileItShares) {
  SessionEngineRig h;
  CappedAudience quick{*h.session, 3ms, 3ms};
  CappedAudience slow{*h.session, 6ms, 6ms};
  auto sharing = h.runner->share()->begin(quick);
  EXPECT_TRUE(sharing);
  const auto second = h.runner->share()->begin(slow);
  ASSERT_FALSE(second);
  EXPECT_EQ(second.error(), play::NotShareable::already_shared);
  const auto repeated = h.runner->share()->begin(quick);
  ASSERT_FALSE(repeated);
  EXPECT_EQ(repeated.error(), play::NotShareable::already_shared);
  h.streaming();
  h.then([](ScriptedSession&) {});
  const auto outcome = h.run();
  EXPECT_TRUE(outcome.succeeded());
  EXPECT_EQ(quick.offered(), every_turn());
  EXPECT_TRUE(slow.offered().empty());
  EXPECT_EQ(h.session->waits, (Waits{3ms, 3ms, 3ms}));
  EXPECT_EQ(quick.ends(), 1);
  EXPECT_EQ(slow.ends(), 0);
  CappedAudience late{*h.session, 1ms, 1ms};
  const auto refused = h.runner->share()->begin(late);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), play::NotShareable::session_ended);
  EXPECT_EQ(late.ends(), 0);
}

TEST_F(SessionEngineAudience, KeepsTheIdleWaitForAGenerousAudience) {
  SessionEngineRig alone;
  CappedAudience only{*alone.session, 50ms, 50ms};
  auto generous = alone.runner->share()->begin(only);
  EXPECT_TRUE(generous);
  alone.streaming();
  EXPECT_TRUE(alone.run().succeeded());
  EXPECT_EQ(alone.session->waits, (Waits{8ms, 8ms}));
}

}  // namespace
}  // namespace play_test
