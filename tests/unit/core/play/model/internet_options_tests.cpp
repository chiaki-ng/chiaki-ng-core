#include <libchiaking/play_types.hpp>

#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace play_test {
namespace {

using libchiaking::play::InternetOptions;

TEST(InternetOptions, GuessesSeventyFivePortsWithinAMinuteByDefault) {
  const InternetOptions standard;
  EXPECT_FALSE(standard.forces_port_guessing());
  EXPECT_EQ(standard.port_guesses(), 75u);
  EXPECT_EQ(standard.start_timeout(), std::chrono::seconds{60});
  EXPECT_EQ(*InternetOptions::create(false, 75), standard);
}

TEST(InternetOptions, ClampsThePortGuesses) {
  EXPECT_EQ(InternetOptions::create(true, 0)->port_guesses(),
            InternetOptions::minimum_port_guesses);
  EXPECT_EQ(InternetOptions::create(true, 4)->port_guesses(), 5u);
  EXPECT_EQ(InternetOptions::create(true, 60)->port_guesses(), 60u);
  EXPECT_EQ(InternetOptions::create(true, 151)->port_guesses(),
            InternetOptions::maximum_port_guesses);
  EXPECT_TRUE(InternetOptions::create(true, 75)->forces_port_guessing());
}

TEST(InternetOptions, RefusesAStartTimeoutThatIsNotPositive) {
  EXPECT_EQ(InternetOptions::create(false, 75, std::chrono::seconds{30})
                ->start_timeout(),
            std::chrono::seconds{30});
  EXPECT_FALSE(InternetOptions::create(false, 75, std::chrono::seconds{0}));
  EXPECT_FALSE(InternetOptions::create(false, 75, std::chrono::seconds{-1}));
}

}  // namespace
}  // namespace play_test
