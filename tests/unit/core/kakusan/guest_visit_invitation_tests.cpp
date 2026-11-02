#include "src/core/kakusan/adapters/protocol_refusal.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/guest_visit_stage.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace kakusan_test {
namespace {

using namespace std::chrono_literals;

class GuestVisitInvitation : public GuestVisitTest {
 protected:
  Stage stage;
};

TEST_F(GuestVisitInvitation, MustBePresentAndFitABlock) {
  const std::string oversized(manual::max_block_size + 1, 'A');
  EXPECT_TRUE(refused(stage.make({}), Refusal::Kind::invalid_argument));
  EXPECT_TRUE(refused(stage.make(oversized), Refusal::Kind::invalid_text));
  EXPECT_TRUE(stage.events.empty());
  EXPECT_TRUE(stage.waits.empty());
}

TEST_F(GuestVisitInvitation, NamesTheHostAndItsSeats) {
  const auto invitation = stage.root.read(stage.invitation());
  ASSERT_TRUE(invitation);
  EXPECT_EQ(invitation.value().text(), stage.invitation());
  EXPECT_EQ(invitation.value().summary().host_hint,
            Identities::hint_of(TestPlatform::identity(0x10).key()));
  EXPECT_EQ(invitation.value().summary().seats.capacity, 4U);
  EXPECT_EQ(invitation.value().summary().recognition.kind,
            libchiaking::kakusan::Recognition::Kind::unknown);
}

class GuestVisitDefaults : public GuestVisitTest {};

TEST_F(GuestVisitDefaults, PaceTheVisit) {
  const GuestVisit::Pacing pacing;
  EXPECT_EQ(pacing.idle_wait, 8ms);
  EXPECT_EQ(pacing.admitted_wait, 2ms);
  EXPECT_EQ(pacing.leave_deadline, 1500ms);
}

TEST_F(GuestVisitDefaults, NumberTheOutcomesAndRefusals) {
  EXPECT_EQ(VisitOutcome::cancelled_code, 2);
  EXPECT_EQ(VisitOutcome::refusal_base, 5000);
  EXPECT_EQ(VisitOutcome::media_base, 4800);
  EXPECT_EQ(GuestVisit::diagnostic_limit, 512U);
  EXPECT_EQ(libchiaking::kakusan::ProtocolRefusal::of(manual::Error::invalid_argument).code(), 1);
  EXPECT_EQ(libchiaking::kakusan::ProtocolRefusal::of(manual::Error::invalid_block).code(), 3);
  EXPECT_EQ(libchiaking::kakusan::ProtocolRefusal::of(manual::Error::no_path).code(), 9);
  EXPECT_EQ(libchiaking::kakusan::ProtocolRefusal::of(manual::Error::internal).code(), 19);
}

}  // namespace
}  // namespace kakusan_test
