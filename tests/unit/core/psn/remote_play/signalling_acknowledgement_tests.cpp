#include "tests/unit/core/psn/remote_play/rig.hpp"
#include "tests/unit/core/psn/remote_play/signalling_bodies.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace psn_test {
namespace {

using libchiaking::psn::remote_play::PsnServicePhase;
using libchiaking::psn::remote_play::PushErrorKind;
using Bodies = SignallingBodies;

class SignallingAcknowledgement : public ::testing::Test {
 protected:
  void leaves_the_first_offer_and_acknowledges_a_repeat() {
    ASSERT_TRUE(rig.start());
    rig.connection.push(Bodies::route_offer);
    ASSERT_TRUE(rig.push.receive_notification(rig.connection, {}));
    EXPECT_EQ(rig.http.calls, 0U);
    rig.connection.push(Bodies::route_offer);
    ASSERT_TRUE(RealTime::eventually([&] { return rig.http.calls == 1; }));
  }

  void acknowledges_through_every_phase() {
    rig.connection.push(Bodies::route_result);
    rig.connection.push(Bodies::route_offer);
    rig.connection.push(Bodies::route_accept);
    EXPECT_TRUE(rig.push.receive_notification(rig.connection, {}));
    EXPECT_TRUE(rig.push.receive_notification(rig.connection, {}));
    ASSERT_TRUE(RealTime::eventually([&] { return rig.http.calls == 2; }));
    rig.push.service_phase(999, PsnServicePhase::data);
    rig.push.service_phase(7, PsnServicePhase::data);
    rig.connection.push(Bodies::data_offer);
    EXPECT_TRUE(rig.push.receive_notification(rig.connection, {}));
    rig.connection.push(Bodies::data_offer);
    ASSERT_TRUE(RealTime::eventually([&] { return rig.http.calls == 3; }));
    rig.push.service_phase(7, PsnServicePhase::active);
    for (unsigned i = 0; i < 80; ++i) rig.connection.push(Bodies::data_offer);
    ASSERT_TRUE(RealTime::eventually([&] { return rig.http.calls == 83; }));
    EXPECT_FALSE(rig.push.service_failure());
    rig.connection.push(Bodies::other_session_deleted);
    rig.connection.push(Bodies::data_offer);
    ASSERT_TRUE(RealTime::eventually([&] { return rig.http.calls == 84; }));
    EXPECT_FALSE(rig.push.service_failure());
    rig.connection.push(Bodies::session_deleted);
    ASSERT_TRUE(RealTime::eventually(
        [&] { return rig.push.service_failure().has_value(); }));
    EXPECT_EQ(rig.push.service_failure()->kind, PushErrorKind::session_deleted);
    rig.push.stop_service();
    EXPECT_EQ(rig.connection.closes, 0U);
    EXPECT_TRUE(rig.connection.close());
    EXPECT_EQ(rig.connection.closes, 1U);
  }

  ServiceRig<> rig;
};

TEST_F(SignallingAcknowledgement, AnswersRepeatedOffersInEveryPhase) {
  ASSERT_NO_FATAL_FAILURE(leaves_the_first_offer_and_acknowledges_a_repeat());
  acknowledges_through_every_phase();
}

TEST_F(SignallingAcknowledgement, CarriesOnAfterARefusedAcknowledgement) {
  rig.http.fail = true;
  ASSERT_NO_FATAL_FAILURE(leaves_the_first_offer_and_acknowledges_a_repeat());
  RealTime::settle();
  EXPECT_FALSE(rig.push.service_failure());
  acknowledges_through_every_phase();
}

TEST_F(SignallingAcknowledgement, StopsDuringABlockedAcknowledgement) {
  rig.http.block = true;
  ASSERT_NO_FATAL_FAILURE(leaves_the_first_offer_and_acknowledges_a_repeat());
  rig.push.stop_service();
  EXPECT_EQ(rig.connection.closes, 0U);
}

TEST_F(SignallingAcknowledgement, AnswersANewControlOfferOnce) {
  ASSERT_TRUE(rig.start());
  rig.connection.push(Bodies::route_offer);
  ASSERT_TRUE(rig.push.receive_notification(rig.connection, {}));
  ASSERT_NE(Bodies::route_offer.find(R"(\"reqId\":2)"), std::string_view::npos);
  rig.connection.push(Bodies::route_offer_numbered("9"));
  rig.connection.push(Bodies::route_result);
  ASSERT_TRUE(RealTime::eventually([&] { return rig.http.calls == 1; }));
  EXPECT_TRUE(rig.received_with("RESULT"));
  RealTime::settle();
  EXPECT_EQ(rig.http.calls, 1U);
  rig.push.stop_service();
}

}  // namespace
}  // namespace psn_test
