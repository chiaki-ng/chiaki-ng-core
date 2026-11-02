#include <libchiaking/play_types.hpp>

#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace play_test {
namespace {

namespace play = libchiaking::play;
using libchiaking::controllers::SeatNumber;
using play::EndReason;
using play::SeatRefusal;
using Kind = SeatRefusal::Kind;

class SeatRefusals : public ::testing::Test {
 protected:
  const SeatNumber third = *SeatNumber::create(2);
};

TEST_F(SeatRefusals, CarriesTheCauseOfARequestNeverSent) {
  const auto unsent =
      SeatRefusal::not_sent(third, EndReason::Cause::invalid_state);
  EXPECT_EQ(unsent.kind(), Kind::not_sent);
  EXPECT_EQ(unsent.seat(), third);
  EXPECT_EQ(unsent.cause(), EndReason::Cause::invalid_state);
  EXPECT_FALSE(unsent.console_status());
  EXPECT_EQ(SeatRefusal::not_sent(third, EndReason::Cause::resource_exhausted)
                .cause(),
            EndReason::Cause::resource_exhausted);
}

TEST_F(SeatRefusals, CarriesTheConsoleStatus) {
  const auto console = SeatRefusal::by_console(third, 0x21);
  EXPECT_EQ(console.kind(), Kind::refused_by_console);
  EXPECT_EQ(console.console_status(), std::uint8_t{0x21});
  EXPECT_FALSE(console.cause());
}

TEST_F(SeatRefusals, NamesTheStatusesItKnows) {
  const auto attempts =
      SeatRefusal::by_console(third, SeatRefusal::passcode_attempts_status);
  EXPECT_EQ(attempts.kind(), Kind::passcode_attempts_used);
  EXPECT_FALSE(attempts.console_status());
  EXPECT_FALSE(attempts.cause());
  const auto timed_out =
      SeatRefusal::by_console(third, SeatRefusal::timed_out_status);
  EXPECT_EQ(timed_out.kind(), Kind::timed_out);
  EXPECT_FALSE(timed_out.cause());
  EXPECT_FALSE(timed_out.console_status());
  EXPECT_EQ(timed_out.seat(), third);
}

TEST_F(SeatRefusals, NamesAnAccountTheConsoleDoesNotKnow) {
  const auto unknown = SeatRefusal::account_not_on_console(third);
  EXPECT_EQ(unknown.kind(), Kind::account_not_on_console);
  EXPECT_EQ(unknown.seat(), third);
  EXPECT_FALSE(unknown.cause());
  EXPECT_FALSE(unknown.console_status());
}

TEST_F(SeatRefusals, HasNoSeatWhenNoneIsFree) {
  const auto full = SeatRefusal::no_free_seat();
  EXPECT_EQ(full.kind(), Kind::no_free_seat);
  EXPECT_FALSE(full.seat());
  EXPECT_FALSE(full.cause());
}

TEST_F(SeatRefusals, ComparesBySeatKindAndStatus) {
  const auto console = SeatRefusal::by_console(third, 0x21);
  const auto attempts =
      SeatRefusal::by_console(third, SeatRefusal::passcode_attempts_status);
  const auto timed_out =
      SeatRefusal::by_console(third, SeatRefusal::timed_out_status);
  EXPECT_EQ(console, SeatRefusal::by_console(third, 0x21));
  EXPECT_NE(console, SeatRefusal::by_console(third, 0x22));
  EXPECT_NE(console, SeatRefusal::by_console(SeatNumber::first(), 0x21));
  EXPECT_NE(attempts, timed_out);
}

}  // namespace
}  // namespace play_test
