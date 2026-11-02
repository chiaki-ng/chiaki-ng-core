#include "src/core/play/adapters/links/link_stages.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

namespace play_test {
namespace {

using libchiaking::play::LinkStages;
using FailureStage = libchiaking::net::protocol::remote_play::FailureStage;
using Stage = libchiaking::consoles::InternetPairingStage;
using Step = libchiaking::consoles::PairingStep;

TEST(LinkStages, FollowTheSetupProgressIntoPairingStages) {
  EXPECT_EQ(LinkStages::pairing_stage(FailureStage::psn_open),
            Stage::opening_session);
  EXPECT_EQ(LinkStages::pairing_stage(FailureStage::psn_start),
            Stage::waking_console);
  EXPECT_EQ(LinkStages::pairing_stage(FailureStage::control_holepunch),
            Stage::connecting);
  EXPECT_EQ(LinkStages::pairing_stage(FailureStage::data_holepunch),
            Stage::connecting);
  EXPECT_EQ(LinkStages::pairing_stage(FailureStage::registration),
            Stage::pairing);
  EXPECT_EQ(LinkStages::pairing_stage(FailureStage::unknown), Stage::starting);
  EXPECT_EQ(LinkStages::pairing_stage(FailureStage::media), Stage::starting);
}

TEST(LinkStages, FollowTheSetupProgressIntoTypedPairingSteps) {
  EXPECT_EQ(LinkStages::pairing_step(FailureStage::psn_open),
            Step::opening_account_session);
  EXPECT_EQ(LinkStages::pairing_step(FailureStage::psn_start),
            Step::waking_console);
  EXPECT_EQ(LinkStages::pairing_step(FailureStage::control_holepunch),
            Step::reaching_console);
  EXPECT_EQ(LinkStages::pairing_step(FailureStage::data_holepunch),
            Step::reaching_console);
  EXPECT_EQ(LinkStages::pairing_step(FailureStage::registration),
            Step::sending_request);
  EXPECT_FALSE(LinkStages::pairing_step(FailureStage::media));
  EXPECT_FALSE(LinkStages::pairing_step(FailureStage::unknown));
}

}  // namespace
}  // namespace play_test
