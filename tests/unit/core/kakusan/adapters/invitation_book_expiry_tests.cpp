#include "src/core/kakusan/adapters/invitation_book.hpp"
#include "src/core/net/kakusan/short_block.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/adapters/invitation_book_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace kakusan_test {
namespace {

using namespace libchiaking::kakusan;
using namespace libchiaking::net::kakusan;

class InvitationBookExpiry : public InvitationBookTest {};

TEST_F(InvitationBookExpiry, ChecksTheInvitationWindowAlone) {
  EXPECT_TRUE(InvitationBook::check(invitation, created_at));
  EXPECT_TRUE(InvitationBook::check(invitation, expires_at - 1));
  const auto at_expiry = InvitationBook::check(invitation, expires_at);
  ASSERT_FALSE(at_expiry);
  EXPECT_EQ(at_expiry.error(), Error::expired);
  EXPECT_FALSE(InvitationBook::check(invitation, expires_at + 1000));

  EXPECT_TRUE(InvitationBook::check(invitation, created_at - 120));
  const auto too_early = InvitationBook::check(invitation, created_at - 121);
  ASSERT_FALSE(too_early);
  EXPECT_EQ(too_early.error(), Error::invalid_block);
  EXPECT_FALSE(InvitationBook::check(invitation, 0));
}

TEST_F(InvitationBookExpiry, EndsTheReplyWindowOnTheMonotonicClock) {
  InvitationBook book;
  const auto early = invite(1);
  const auto exact = invite(2);
  const auto late = invite(3);
  EXPECT_TRUE(book.add(early, start_us));
  EXPECT_TRUE(book.add(exact, start_us));
  EXPECT_TRUE(book.add(late, start_us));

  EXPECT_TRUE(book.import_reply(answer(early), created_at + 10,
                                start_us + 599 * second));
  EXPECT_TRUE(book.import_reply(answer(exact), created_at + 10,
                                start_us + 600 * second - 1));
  EXPECT_TRUE(fails_with(
      book.import_reply(answer(late), created_at + 10, start_us + 600 * second),
      Error::expired));
  EXPECT_EQ(book.status(id_of(late)), std::nullopt);
}

TEST_F(InvitationBookExpiry, EndsTheReplyWindowOnTheWallClock) {
  InvitationBook book;
  const auto before = invite(1);
  const auto at = invite(2);
  const auto after = invite(3);
  EXPECT_TRUE(book.add(before, start_us));
  EXPECT_TRUE(book.add(at, start_us));
  EXPECT_TRUE(book.add(after, start_us));

  EXPECT_TRUE(
      book.import_reply(answer(before), expires_at - 1, start_us + second));
  EXPECT_TRUE(
      fails_with(book.import_reply(answer(at), expires_at, start_us + second),
                 Error::expired));
  EXPECT_TRUE(fails_with(
      book.import_reply(answer(after), expires_at + 3600, start_us + second),
      Error::expired));
  EXPECT_EQ(book.status(id_of(at)), InvitationStatus::offered);
  EXPECT_TRUE(
      book.import_reply(answer(at), expires_at - 1, start_us + 2 * second));
}

TEST_F(InvitationBookExpiry, CannotBeExtendedByRollingTheWallClockBack) {
  InvitationBook book;
  std::uint8_t tag = 1;
  for (const auto& [wall, elapsed_s] :
       {std::pair<std::uint64_t, std::uint64_t>{created_at - 100, 600},
        {created_at, 601},
        {created_at + 1, 86400},
        {created_at - 120, 600}}) {
    const auto offered = invite(tag++);
    EXPECT_TRUE(book.add(offered, start_us));
    EXPECT_TRUE(fails_with(
        book.import_reply(answer(offered), wall, start_us + elapsed_s * second),
        Error::expired));
  }
}

TEST_F(InvitationBookExpiry, ToleratesTwoMinutesOfFutureCreation) {
  {
    InvitationBook book;
    const auto offered = invite(1);
    EXPECT_TRUE(book.add(offered, start_us));
    EXPECT_TRUE(book.import_reply(answer(offered), created_at - 120, start_us));
  }
  InvitationBook book;
  const auto offered = invite(1);
  EXPECT_TRUE(book.add(offered, start_us));
  EXPECT_TRUE(
      fails_with(book.import_reply(answer(offered), created_at - 121, start_us),
                 Error::invalid_block));
  EXPECT_EQ(book.status(id_of(offered)), InvitationStatus::offered);
}

TEST_F(InvitationBookExpiry, KeepsABoundReplyPastTheInvitationExpiry) {
  InvitationBook book;
  const auto offered = invite(1);
  EXPECT_TRUE(book.add(offered, start_us));
  const auto replied = answer(offered);
  EXPECT_TRUE(
      book.import_reply(replied, created_at + 30, start_us + 10 * second));
  const auto later =
      book.import_reply(replied, expires_at + 5000, start_us + 5000 * second);
  ASSERT_TRUE(later);
  EXPECT_EQ(later.value(), ImportOutcome::already_bound);
  EXPECT_EQ(book.pending_count(), 1);
  EXPECT_EQ(book.status(id_of(offered)), InvitationStatus::bound);
}

TEST_F(InvitationBookExpiry, KeepsConsumedInvitationsDeadUntilTheyExpire) {
  InvitationBook book;
  const auto failed = invite(1);
  const auto finished = invite(2);
  const auto cancelled = invite(3);
  for (const auto* entry : {&failed, &finished, &cancelled})
    EXPECT_TRUE(book.add(*entry, start_us));
  EXPECT_TRUE(
      book.import_reply(answer(failed), created_at + 30, start_us + second));
  EXPECT_TRUE(
      book.import_reply(answer(finished), created_at + 30, start_us + second));
  EXPECT_EQ(book.pending_count(), 2);

  EXPECT_TRUE(book.fail(id_of(failed), start_us + 2 * second));
  EXPECT_TRUE(book.finish(id_of(finished)));
  EXPECT_TRUE(book.fail(id_of(cancelled), start_us + 2 * second));
  EXPECT_EQ(book.pending_count(), 0);
  EXPECT_EQ(book.tracked_count(), 3);
  for (const auto* entry : {&failed, &finished, &cancelled})
    EXPECT_EQ(book.status(id_of(*entry)), InvitationStatus::consumed);

  for (const auto* entry : {&failed, &finished, &cancelled}) {
    EXPECT_TRUE(fails_with(book.import_reply(answer(*entry), created_at + 30,
                                             start_us + 3 * second),
                           Error::invalid_block));
    EXPECT_FALSE(book.add(*entry, start_us + 3 * second));
  }
  EXPECT_TRUE(book.finish(id_of(finished)));
  EXPECT_TRUE(book.fail(id_of(finished), start_us + 4 * second));

  book.expire(start_us + 600 * second - 1);
  EXPECT_EQ(book.tracked_count(), 3);
  book.expire(start_us + 600 * second);
  EXPECT_EQ(book.tracked_count(), 0);
  for (const auto* entry : {&failed, &finished, &cancelled}) {
    EXPECT_EQ(book.status(id_of(*entry)), std::nullopt);
    EXPECT_TRUE(fails_with(book.import_reply(answer(*entry), created_at + 30,
                                             start_us + 601 * second),
                           Error::invalid_block));
  }
}

TEST_F(InvitationBookExpiry, KeepsPendingAttemptsPastTheInvitationTimer) {
  InvitationBook book;
  const auto offered = invite(1);
  EXPECT_TRUE(book.add(offered, start_us));
  EXPECT_TRUE(book.import_reply(answer(offered), created_at + 30,
                                start_us + 10 * second));
  book.expire(start_us + 100000 * second);
  EXPECT_EQ(book.status(id_of(offered)), InvitationStatus::bound);
  EXPECT_TRUE(book.finish(id_of(offered)));
  EXPECT_EQ(book.status(id_of(offered)), InvitationStatus::consumed);
  book.expire(start_us + 100001 * second);
  EXPECT_EQ(book.tracked_count(), 0);
}

TEST_F(InvitationBookExpiry, PurgesUnrepliedInvitations) {
  InvitationBook book;
  const auto offered = invite(1);
  EXPECT_TRUE(book.add(offered, start_us));
  book.expire(start_us + 600 * second - 1);
  EXPECT_EQ(book.tracked_count(), 1);
  book.expire(start_us + 600 * second);
  EXPECT_EQ(book.tracked_count(), 0);
}

}  // namespace
}  // namespace kakusan_test
