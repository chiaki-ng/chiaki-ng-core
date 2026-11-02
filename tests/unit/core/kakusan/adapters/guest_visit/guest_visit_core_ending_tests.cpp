#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/adapters/guest_visit/guest_visit_core_rig.hpp"

#include <gtest/gtest.h>

namespace kakusan_test {
namespace {

namespace control = libchiaking::net::kakusan::control;

class GuestVisitCoreEnding : public GuestVisitCoreTest {
 protected:
  void SetUp() override { rig.activate(); }

  Rig rig;
};

TEST_F(GuestVisitCoreEnding, StopsTheStreamWhenTheHostEnds) {
  rig.apply(rig.host.handle(
      libchiaking::kakusan::Cancel{rig.now, control::Reason::user}));
  rig.run(20);
  expect_stopped_by_peer(rig, control::Reason::user);
}

TEST_F(GuestVisitCoreEnding, StopsTheStreamWhenTheSeatIsRevoked) {
  rig.apply(rig.host.handle(libchiaking::kakusan::CapacityRevoked{rig.now}));
  rig.run(20);
  expect_stopped_by_peer(rig, control::Reason::capacity);
}

TEST_F(GuestVisitCoreEnding, StopsTheStreamWhenTheTransportFails) {
  rig.wire_link().sides[1].events.push_back(
      libchiaking::net::kakusan::transport_event::Failed{Error::no_path});
  rig.step();
  EXPECT_EQ(rig.guest->state(), Progress::gone);
  EXPECT_EQ(rig.count<GuestVisit::Core::StreamStopped>(), 1U);
  expect_silent_rumble(rig);
  EXPECT_EQ(rig.last_state().reason, control::Reason::network);
  EXPECT_FALSE(rig.last_state().by_peer);
  EXPECT_FALSE(rig.last_state().error);
  EXPECT_FALSE(rig.guest->next_deadline_us());
  EXPECT_TRUE(rig.wire_link().sides[1].closed);
}

}  // namespace
}  // namespace kakusan_test
