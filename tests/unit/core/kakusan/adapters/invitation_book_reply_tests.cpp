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

class InvitationBookReplies : public InvitationBookTest {};

TEST_F(InvitationBookReplies, BindAValidReply) {
  InvitationBook book;
  EXPECT_TRUE(book.add(invitation, start_us));
  EXPECT_EQ(book.status(id_of(invitation)), InvitationStatus::offered);
  EXPECT_NE(book.find(id_of(invitation)), nullptr);
  EXPECT_EQ(book.pending_count(), 0);

  const auto imported =
      book.import_reply(reply, created_at + 30, start_us + second);
  ASSERT_TRUE(imported);
  EXPECT_EQ(imported.value(), ImportOutcome::bound);
  EXPECT_EQ(book.status(id_of(invitation)), InvitationStatus::bound);
  EXPECT_EQ(book.pending_count(), 1);

  const auto again =
      book.import_reply(reply, created_at + 31, start_us + 2 * second);
  ASSERT_TRUE(again);
  EXPECT_EQ(again.value(), ImportOutcome::already_bound);
  EXPECT_EQ(book.pending_count(), 1);
}

TEST_F(InvitationBookReplies, RefuseUnknownAndDuplicateInvitations) {
  InvitationBook book;
  const auto known = invite(1);
  const auto stranger = invite(2);
  EXPECT_TRUE(book.add(known, start_us));
  EXPECT_FALSE(book.add(known, start_us));
  EXPECT_EQ(book.tracked_count(), 1);

  EXPECT_TRUE(
      fails_with(book.import_reply(answer(stranger), created_at + 30, start_us),
                 Error::invalid_block));
  EXPECT_FALSE(book.status(id_of(stranger)));
  EXPECT_EQ(book.find(id_of(stranger)), nullptr);
  EXPECT_FALSE(book.fail(id_of(stranger), start_us));
  EXPECT_FALSE(book.finish(id_of(stranger)));
  EXPECT_TRUE(book.import_reply(answer(known), created_at + 30, start_us));
}

TEST_F(InvitationBookReplies, LeaveTheInvitationOfferedWhenBindingFails) {
  InvitationBook book;
  const auto offered = invite(1);
  EXPECT_TRUE(book.add(offered, start_us));

  EXPECT_TRUE(fails_with(book.import_reply(answer_with(offered,
                                                       [](ShortReply& body) {
                                                         body.invite_tag[0] ^=
                                                             std::byte{1};
                                                       }),
                                           created_at + 30, start_us),
                         Error::invalid_block));
  EXPECT_TRUE(fails_with(book.import_reply(answer_with(offered,
                                                       [](ShortReply& body) {
                                                         body.invite_tag[7] ^=
                                                             std::byte{0x80};
                                                       }),
                                           created_at + 30, start_us),
                         Error::invalid_block));

  EXPECT_EQ(book.status(id_of(offered)), InvitationStatus::offered);
  EXPECT_EQ(book.pending_count(), 0);
  const auto good =
      book.import_reply(answer(offered), created_at + 30, start_us);
  ASSERT_TRUE(good);
  EXPECT_EQ(good.value(), ImportOutcome::bound);
}

TEST_F(InvitationBookReplies, NeverReplaceTheFirstReply) {
  InvitationBook book;
  const auto offered = invite(1);
  EXPECT_TRUE(book.add(offered, start_us));
  const auto first = answer(offered, 1);
  const auto other = answer(offered, 2);
  EXPECT_NE(first.hash, other.hash);

  EXPECT_TRUE(book.import_reply(first, created_at + 30, start_us));
  EXPECT_TRUE(
      fails_with(book.import_reply(other, created_at + 30, start_us + second),
                 Error::invalid_block));
  const auto still =
      book.import_reply(first, created_at + 30, start_us + 2 * second);
  ASSERT_TRUE(still);
  EXPECT_EQ(still.value(), ImportOutcome::already_bound);
  EXPECT_TRUE(fails_with(
      book.import_reply(other, created_at + 30, start_us + 3 * second),
      Error::invalid_block));
  EXPECT_EQ(book.pending_count(), 1);
}

TEST_F(InvitationBookReplies, CannotFinishBeforeAReplyIsBound) {
  InvitationBook book;
  const auto offered = invite(1);
  EXPECT_TRUE(book.add(offered, start_us));
  const auto early = book.finish(id_of(offered));
  ASSERT_FALSE(early);
  EXPECT_EQ(early.error(), Error::invalid_block);
  EXPECT_EQ(book.status(id_of(offered)), InvitationStatus::offered);
}

TEST_F(InvitationBookReplies, StopAtThePendingLimit) {
  InvitationBook book;
  std::vector<SealedInvitation> offers;
  for (std::uint8_t tag = 1; tag <= 9; ++tag) {
    offers.push_back(invite(tag));
    EXPECT_TRUE(book.add(offers.back(), start_us));
  }
  for (std::size_t index = 0; index < max_pending_attempts; ++index) {
    const auto imported =
        book.import_reply(answer(offers[index]), created_at + 30, start_us);
    ASSERT_TRUE(imported);
    EXPECT_EQ(imported.value(), ImportOutcome::bound);
  }
  EXPECT_EQ(book.pending_count(), 8);

  const auto& ninth = offers[8];
  EXPECT_TRUE(
      fails_with(book.import_reply(answer(ninth), created_at + 30, start_us),
                 Error::no_capacity));
  EXPECT_EQ(book.status(id_of(ninth)), InvitationStatus::offered);
  EXPECT_EQ(book.pending_count(), 8);
  const auto again =
      book.import_reply(answer(offers[0]), created_at + 30, start_us);
  ASSERT_TRUE(again);
  EXPECT_EQ(again.value(), ImportOutcome::already_bound);

  EXPECT_TRUE(book.finish(id_of(offers[3])));
  EXPECT_EQ(book.pending_count(), 7);
  EXPECT_TRUE(book.import_reply(answer(ninth), created_at + 30, start_us));
  EXPECT_EQ(book.pending_count(), 8);
}

TEST_F(InvitationBookReplies, StopAtTheTrackedLimit) {
  InvitationBook book;
  for (std::size_t index = 0; index < max_tracked_invitations; ++index) {
    auto body = invitation.body.clone();
    body.seed[0] = static_cast<std::byte>(index);
    body.seed[1] = std::byte{0x7e};
    const auto sealed = ShortBlockCodec::seal(crypto, body);
    ASSERT_TRUE(sealed);
    EXPECT_TRUE(book.add(sealed.value(), start_us));
  }
  const auto extra = book.add(invite(200), start_us + second);
  ASSERT_FALSE(extra);
  EXPECT_EQ(extra.error(), Error::no_capacity);
  EXPECT_TRUE(book.add(invite(200), start_us + 600 * second));
  EXPECT_EQ(book.tracked_count(), 1);
}

}  // namespace
}  // namespace kakusan_test
