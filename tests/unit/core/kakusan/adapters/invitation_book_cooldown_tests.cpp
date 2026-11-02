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

class InvitationBookCooldown : public InvitationBookTest {};

TEST_F(InvitationBookCooldown, StartsAfterThreeAuthenticationFailures) {
  InvitationBook book;
  EXPECT_TRUE(book.pairing_allowed(0));
  book.record_authentication_failure(start_us);
  book.record_authentication_failure(start_us + 10 * second);
  EXPECT_TRUE(book.pairing_allowed(start_us + 11 * second));
  book.record_authentication_failure(start_us + 20 * second);
  const auto lifted = start_us + 320 * second;
  EXPECT_FALSE(book.pairing_allowed(start_us + 20 * second));
  EXPECT_FALSE(book.pairing_allowed(lifted - 1));
  EXPECT_TRUE(book.pairing_allowed(lifted));
  EXPECT_TRUE(book.pairing_allowed(lifted + 1));

  const auto fresh = invite(1);
  EXPECT_TRUE(book.add(fresh, start_us));
  EXPECT_TRUE(
      fails_with(book.import_reply(answer(fresh), created_at + 30, lifted - 1),
                 Error::pairing_failed));
  EXPECT_EQ(book.status(id_of(fresh)), InvitationStatus::offered);
  const auto next = invite(2);
  EXPECT_TRUE(book.add(next, start_us + 30 * second));
  EXPECT_TRUE(fails_with(
      book.import_reply(answer(next), created_at + 30, start_us + 100 * second),
      Error::pairing_failed));
  EXPECT_TRUE(book.import_reply(answer(fresh), created_at + 30, lifted));
}

TEST_F(InvitationBookCooldown, LeavesPendingAttemptsAlone) {
  InvitationBook book;
  const auto running = invite(1);
  EXPECT_TRUE(book.add(running, start_us));
  const auto replied = answer(running);
  EXPECT_TRUE(book.import_reply(replied, created_at + 30, start_us));
  for (int index = 0; index < 3; ++index)
    book.record_authentication_failure(
        start_us + static_cast<std::uint64_t>(index) * second);
  EXPECT_FALSE(book.pairing_allowed(start_us + 10 * second));
  const auto again =
      book.import_reply(replied, created_at + 30, start_us + 10 * second);
  ASSERT_TRUE(again);
  EXPECT_EQ(again.value(), ImportOutcome::already_bound);
  EXPECT_EQ(book.status(id_of(running)), InvitationStatus::bound);
}

TEST_F(InvitationBookCooldown, CountsFailuresWithinTenMinutes) {
  {
    InvitationBook book;
    book.record_authentication_failure(start_us);
    book.record_authentication_failure(start_us + second);
    book.record_authentication_failure(start_us + 600 * second - 1);
    EXPECT_FALSE(book.pairing_allowed(start_us + 600 * second - 1));
  }
  {
    InvitationBook book;
    book.record_authentication_failure(start_us);
    book.record_authentication_failure(start_us + second);
    book.record_authentication_failure(start_us + 600 * second);
    EXPECT_TRUE(book.pairing_allowed(start_us + 600 * second));
  }
  {
    InvitationBook book;
    book.record_authentication_failure(start_us);
    book.record_authentication_failure(start_us + 400 * second);
    book.record_authentication_failure(start_us + 800 * second);
    EXPECT_TRUE(book.pairing_allowed(start_us + 800 * second));
    book.record_authentication_failure(start_us + 801 * second);
    EXPECT_FALSE(book.pairing_allowed(start_us + 801 * second));
    EXPECT_FALSE(book.pairing_allowed(start_us + 1100 * second));
    EXPECT_TRUE(book.pairing_allowed(start_us + 1101 * second));
  }
  {
    InvitationBook book;
    for (int index = 0; index < 2; ++index)
      book.record_authentication_failure(start_us +
                                         static_cast<std::uint64_t>(index));
    EXPECT_TRUE(book.pairing_allowed(start_us + 2));
  }
}

TEST_F(InvitationBookCooldown, IsExtendedByFurtherFailures) {
  InvitationBook book;
  for (int index = 0; index < 3; ++index)
    book.record_authentication_failure(
        start_us + static_cast<std::uint64_t>(index) * second);
  EXPECT_FALSE(book.pairing_allowed(start_us + 301 * second));
  book.record_authentication_failure(start_us + 100 * second);
  EXPECT_FALSE(book.pairing_allowed(start_us + 399 * second));
  EXPECT_TRUE(book.pairing_allowed(start_us + 400 * second));
}

TEST_F(InvitationBookCooldown, CountsOnlyAuthenticationFailures) {
  InvitationBook book;
  for (std::uint8_t tag = 1; tag <= 3; ++tag) {
    const auto offered = invite(tag);
    EXPECT_TRUE(book.add(offered, start_us));
    EXPECT_TRUE(book.fail(id_of(offered), start_us + second));
  }
  EXPECT_TRUE(book.pairing_allowed(start_us + second));

  for (std::uint8_t tag = 11; tag <= 13; ++tag) {
    const auto offered = invite(tag);
    EXPECT_TRUE(book.add(offered, start_us));
    EXPECT_TRUE(book.import_reply(answer(offered), created_at + 30, start_us));
    EXPECT_TRUE(book.pairing_allowed(start_us));
    EXPECT_TRUE(book.fail_authentication(id_of(offered), start_us + second));
    EXPECT_EQ(book.status(id_of(offered)), InvitationStatus::consumed);
  }
  EXPECT_FALSE(book.pairing_allowed(start_us + second));
  EXPECT_TRUE(book.pairing_allowed(start_us + 301 * second));
  EXPECT_FALSE(book.fail_authentication(invite(99).body.invitation_id,
                                        start_us + 302 * second));
  EXPECT_TRUE(book.pairing_allowed(start_us + 302 * second));
}

}  // namespace
}  // namespace kakusan_test
