#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/adapters/guest_visit/guest_visit_core_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace kakusan_test {
namespace {

namespace wire = libchiaking::net::kakusan;

class GuestVisitCoreInvitation : public GuestVisitCoreTest {
 protected:
  auto rejects(std::string_view block, std::uint64_t wall, Error error)
      -> ::testing::AssertionResult {
    const auto accepted =
        rig.guest->accept_invitation(block, Clock{rig.now, wall});
    if (accepted) return ::testing::AssertionFailure() << "it was accepted";
    if (accepted.error() != error)
      return ::testing::AssertionFailure()
             << "refused with " << wire::error_name(accepted.error());
    return ::testing::AssertionSuccess();
  }

  Rig rig;
};

TEST_F(GuestVisitCoreInvitation, IsRefusedOnceExpired) {
  EXPECT_TRUE(rejects(rig.invitation_block,
                      start_wall + wire::invitation_lifetime_seconds,
                      Error::expired));
  EXPECT_TRUE(rejects(rig.invitation_block,
                      start_wall + wire::invitation_lifetime_seconds + 5,
                      Error::expired));
}

TEST_F(GuestVisitCoreInvitation, IsRefusedWhenNotYetValidOrNotABlock) {
  EXPECT_TRUE(rejects(std::string{rig.sign(start_wall + 500).text.view()},
                      start_wall, Error::invalid_block));
  EXPECT_TRUE(rejects("RCP1:not-a-block", start_wall, Error::invalid_block));
  EXPECT_TRUE(rejects("hello", start_wall, Error::invalid_block));
}

TEST_F(GuestVisitCoreInvitation, IsRefusedWhenForged) {
  auto forged = rig.invitation_block;
  const auto middle = wire::block_prefix.size() +
                      (forged.size() - wire::block_prefix.size()) / 2;
  forged[middle] = forged[middle] == 'A' ? 'B' : 'A';
  EXPECT_FALSE(rig.guest->accept_invitation(forged, rig.clock()));
}

TEST_F(GuestVisitCoreInvitation, IsRefusedWhenItIsAReply) {
  Rig donor;
  donor.auto_import = false;
  donor.accept();
  donor.run_until([&] { return donor.reply.has_value(); });
  ASSERT_TRUE(donor.reply);
  EXPECT_TRUE(rejects(std::string{donor.reply->text.view()}, start_wall,
                      Error::invalid_block));
}

TEST_F(GuestVisitCoreInvitation, LeavesTheGuestWaitingUntilOneIsAccepted) {
  EXPECT_TRUE(rejects("hello", start_wall, Error::invalid_block));
  EXPECT_EQ(rig.guest->state(), Progress::waiting);
  EXPECT_FALSE(rig.guest->next_deadline_us());
  rig.accept();
  EXPECT_FALSE(rig.guest->accept_invitation(rig.invitation_block, rig.clock()));
  EXPECT_TRUE(rig.guest->next_deadline_us());
}

TEST_F(GuestVisitCoreInvitation, ExpiresWhenTheReplyIsNeverAnswered) {
  rig.auto_import = false;
  rig.accept();
  rig.run_until([&] { return rig.reply.has_value(); });
  EXPECT_EQ(rig.guest->state(), Progress::waiting);
  const auto deadline = rig.guest->next_deadline_us();
  ASSERT_TRUE(deadline);
  EXPECT_LE(*deadline, rig.now + wire::invitation_lifetime_seconds *
                                     libchiaking::microseconds_per_second);
  rig.run(wire::invitation_lifetime_seconds,
          libchiaking::microseconds_per_second);
  EXPECT_EQ(rig.guest->state(), Progress::gone);
  EXPECT_EQ(rig.last_state().error, Error::expired);
  EXPECT_FALSE(rig.guest->next_deadline_us());
  EXPECT_TRUE(rig.wire_link().sides[1].closed);
}

}  // namespace
}  // namespace kakusan_test
