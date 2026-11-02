#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/internet_start_rig.hpp"
#include "tests/unit/core/play/takion_defaults.hpp"

#include <gtest/gtest.h>

namespace play_test {
namespace {

using internet_starts::TranscribedStart;

class InternetStartTranscript : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!TakionDefaults::consoles())
      GTEST_SKIP() << "the build pins non-default console Takion versions";
  }
};

TEST_F(InternetStartTranscript, MatchesTheScriptedPs5Start) {
  TranscribedStart start;
  ASSERT_NO_FATAL_FAILURE(start.play());
  EXPECT_TRUE(start.matches_golden());
}

}  // namespace
}  // namespace play_test
