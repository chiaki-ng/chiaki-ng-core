#include <libchiaking/consoles_types.hpp>
#include <libchiaking/play_types.hpp>

#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>

namespace play_test {
namespace {

namespace consoles = libchiaking::consoles;
namespace progress = libchiaking::progress;
using progress::Status;
using progress::Wait;
using progress::WaitReason;

class PairingProgress : public ::testing::Test {
 protected:
  class Quiet final : public consoles::PairingWatcher {};

  static auto answering() -> consoles::PairingProgress {
    return {.step = consoles::PairingStep::waiting_for_answer,
            .elapsed = std::chrono::milliseconds{300},
            .wait = Wait{WaitReason::console_answering}};
  }
};

TEST_F(PairingProgress, StartsOnItsFirstAttempt) {
  const consoles::PairingProgress fresh;
  EXPECT_EQ(fresh.step, consoles::PairingStep::starting);
  EXPECT_EQ(fresh.attempt, 1u);
  EXPECT_FALSE(fresh.wait);
  EXPECT_FALSE(fresh.route);
  EXPECT_NE(answering(), fresh);
}

TEST_F(PairingProgress, ReadsLikeTheOtherReports) {
  EXPECT_EQ(answering().status(),
            (Status{std::nullopt, 1, std::chrono::milliseconds{300},
                    Wait{WaitReason::console_answering}}));
}

TEST_F(PairingProgress, CanBeIgnoredByAWatcher) {
  Quiet quiet;
  consoles::PairingWatcher& watcher = quiet;
  watcher.on_stage(consoles::InternetPairingStage::pairing);
  watcher.on_progress(answering());
}

}  // namespace
}  // namespace play_test
