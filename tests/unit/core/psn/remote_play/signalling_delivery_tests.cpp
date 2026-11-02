#include "tests/unit/core/psn/remote_play/rig.hpp"
#include "tests/unit/core/psn/remote_play/signalling_bodies.hpp"

#include <gtest/gtest.h>

namespace psn_test {
namespace {

using libchiaking::psn::remote_play::PsnServicePhase;
using libchiaking::psn::remote_play::PushErrorKind;
using Bodies = SignallingBodies;

class SignallingDelivery : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(rig.start()); }

  void TearDown() override { rig.push.stop_service(); }

  ServiceRig<> rig;
};

TEST_F(SignallingDelivery, KeepsEveryResultBeyondTheInboxLimitInOrder) {
  for (unsigned i = 0; i < 200; ++i) rig.connection.push(Bodies::route_result);
  rig.connection.push(Bodies::route_accept);
  ASSERT_TRUE(RealTime::eventually([&] { return rig.connection.drained(); }));
  RealTime::settle();
  EXPECT_FALSE(rig.push.service_failure());
  for (unsigned i = 0; i < 200; ++i)
    ASSERT_TRUE(rig.push.receive_notification(rig.connection, {})) << i;
  EXPECT_TRUE(rig.received_with("ACCEPT"));
}

TEST_F(SignallingDelivery, HoldsAnEarlyDataOfferForTheDataPhase) {
  rig.connection.push(Bodies::route_offer);
  EXPECT_TRUE(rig.push.receive_notification(rig.connection, {}));
  rig.connection.push(Bodies::route_accept);
  EXPECT_TRUE(rig.push.receive_notification(rig.connection, {}));
  rig.connection.push(Bodies::route_offer);
  rig.connection.push(Bodies::data_offer);
  ASSERT_TRUE(RealTime::eventually([&] { return rig.connection.drained(); }));
  RealTime::settle();
  EXPECT_EQ(rig.http.calls, 0U);
  rig.push.service_phase(7, PsnServicePhase::data);
  EXPECT_TRUE(rig.received_with(Bodies::data_offer_id, TimedStop::token()));
  EXPECT_FALSE(rig.push.service_failure());
}

TEST_F(SignallingDelivery, PassesAnUndecodableMessageOn) {
  const auto bounded = TimedStop::token();
  rig.connection.push(Bodies::undecodable);
  EXPECT_TRUE(rig.received_with("not-json", bounded));
  rig.connection.push(Bodies::route_offer);
  EXPECT_TRUE(rig.push.receive_notification(rig.connection, bounded));
  rig.connection.push(Bodies::undecodable);
  rig.connection.push(Bodies::route_accept);
  EXPECT_TRUE(rig.received_with("ACCEPT", bounded));
  EXPECT_FALSE(rig.push.service_failure());
}

TEST_F(SignallingDelivery, LeavesATerminateToTheWaitingStage) {
  const auto bounded = TimedStop::token();
  rig.connection.push(Bodies::terminate);
  EXPECT_TRUE(rig.received_with("TERMINATE", bounded));
  EXPECT_FALSE(rig.push.service_failure());
  rig.connection.push(Bodies::route_offer);
  EXPECT_TRUE(rig.push.receive_notification(rig.connection, bounded));
  rig.push.service_phase(7, PsnServicePhase::data);
  rig.connection.push(Bodies::data_offer);
  EXPECT_TRUE(rig.push.receive_notification(rig.connection, bounded));
  rig.push.service_phase(7, PsnServicePhase::active);
  rig.connection.push(Bodies::terminate);
  rig.connection.push(Bodies::session_deleted);
  ASSERT_TRUE(RealTime::eventually(
      [&] { return rig.push.service_failure().has_value(); }));
  EXPECT_EQ(rig.push.service_failure()->kind, PushErrorKind::session_deleted);
}

TEST_F(SignallingDelivery, DrainsTheQueueBeforeReportingTheFailure) {
  rig.connection.push(Bodies::route_result);
  rig.connection.push(Bodies::session_deleted);
  ASSERT_TRUE(RealTime::eventually(
      [&] { return rig.push.service_failure().has_value(); }));
  EXPECT_TRUE(rig.received_with("RESULT"));
  const auto failed = rig.push.receive_notification(rig.connection, {});
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().kind, PushErrorKind::session_deleted);
}

TEST_F(SignallingDelivery, HoldsADataOfferThatBeatsTheControlAccept) {
  rig.connection.push(Bodies::route_offer);
  EXPECT_TRUE(rig.push.receive_notification(rig.connection, {}));
  rig.connection.push(Bodies::data_offer);
  rig.connection.push(Bodies::route_result);
  rig.connection.push(Bodies::route_accept);
  ASSERT_TRUE(RealTime::eventually([&] { return rig.http.calls == 1; }));
  EXPECT_TRUE(rig.received_with("RESULT"));
  EXPECT_TRUE(rig.received_with("ACCEPT"));
  rig.push.service_phase(7, PsnServicePhase::data);
  EXPECT_TRUE(rig.received_with(Bodies::data_offer_id, TimedStop::token()));
  rig.connection.push(Bodies::data_offer);
  ASSERT_TRUE(RealTime::eventually([&] { return rig.http.calls == 2; }));
  EXPECT_FALSE(rig.push.service_failure());
}

TEST_F(SignallingDelivery, DoesNotTakeARepeatedControlOfferForTheDataOffer) {
  const auto bounded = TimedStop::token();
  rig.connection.push(Bodies::route_offer);
  EXPECT_TRUE(rig.push.receive_notification(rig.connection, bounded));
  rig.connection.push(Bodies::route_accept);
  EXPECT_TRUE(rig.received_with("ACCEPT", bounded));
  rig.push.service_phase(7, PsnServicePhase::data);
  rig.connection.push(Bodies::route_offer);
  rig.connection.push(Bodies::data_offer);
  EXPECT_TRUE(rig.received_with(Bodies::data_offer_id, bounded));
  EXPECT_FALSE(rig.push.service_failure());
}

TEST(SignallingStop, InterruptsTheBlockedReceive) {
  ServiceRig<InterruptibleConnection> rig;
  ASSERT_TRUE(rig.start());
  ASSERT_TRUE(RealTime::eventually([&] {
    libchiaking::platform::Lock lock{rig.connection.mutex};
    return rig.connection.entered;
  }));
  rig.push.quiesce_service();
  rig.push.stop_service();
  libchiaking::platform::Lock lock{rig.connection.mutex};
  EXPECT_TRUE(rig.connection.released_by_interrupt);
  EXPECT_EQ(rig.connection.interrupts, 1U);
}

}  // namespace
}  // namespace psn_test
