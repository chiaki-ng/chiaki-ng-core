#include "src/core/play/adapters/cloud_launch/launch_failure.hpp"
#include "src/core/play/adapters/links/cloud/services/old_service_launch.hpp"
#include "src/core/psn/gk/error.hpp"

#include <gtest/gtest.h>

#include <string>

namespace cloud_test {
namespace {

namespace play = libchiaking::play;
using Error = libchiaking::psn::gk::Error;
using Reason = Error::Reason;
using Stage = Error::Stage;

class LaunchFailureDetail : public ::testing::Test {
 protected:
  static auto detail(const Error& error) -> std::string {
    return std::string{play::LaunchFailure::reservation(error, "").detail()};
  }
};

TEST_F(LaunchFailureDetail, IsTheBodyOfARejection) {
  EXPECT_EQ(detail(Error::rejected(Stage::start, Reason::rejected, 403,
                                   "reject body")),
            "reject body");
  EXPECT_EQ(detail(Error::rejected(Stage::authorize, Reason::rejected, 403,
                                   "reject body")),
            "reject body");
}

TEST_F(LaunchFailureDetail, IsEmptyWithoutARejectionBody) {
  EXPECT_TRUE(
      detail(Error{Stage::lock, Reason::lock_unavailable, 403}).empty());
  EXPECT_TRUE(detail(Error{Stage::authorize, Reason::transport}).empty());
  EXPECT_TRUE(detail(Error{Stage::identity, Reason::cancelled}).empty());
}

TEST_F(LaunchFailureDetail, IsEmptyForAMissingTitleItDescribes) {
  const play::OldServiceLaunch::Error not_found{
      play::OldServiceLaunch::Error::Stage::title,
      play::OldServiceLaunch::Error::Cause::not_found, 404};
  EXPECT_TRUE(play::LaunchFailure::title(not_found).detail().empty());
  EXPECT_TRUE(play::LaunchFailure::title(not_found).describe().starts_with(
      "Game not found"));
}

}  // namespace
}  // namespace cloud_test
