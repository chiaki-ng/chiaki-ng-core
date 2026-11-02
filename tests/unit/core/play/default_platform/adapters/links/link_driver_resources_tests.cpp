#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/default_platform/adapters/links/link_driver_rig.hpp"

#include <fcntl.h>
#include <sys/resource.h>

#include <gtest/gtest.h>

#include <string>

namespace play_test {
namespace {

using Rig = LinkDriverRig;
using RealStream = Rig::RealStream;
using StreamError = Rig::StreamError;
using libchiaking::net::protocol::remote_play::FailureStage;

class LinkDriverResources : public ::testing::Test {
 protected:
  static auto highest_descriptor() noexcept -> int {
    int highest = 0;
    for (int descriptor = 0; descriptor < 4096; ++descriptor)
      if (::fcntl(descriptor, F_GETFD) != -1) highest = descriptor;
    return highest;
  }
};

TEST_F(LinkDriverResources, ReportsDescriptorExhaustionAtSessionInit) {
  api_test::LoopbackListener listener;
  RealStream opened{RealStream::local(listener.port())};
  rlimit original{};
  ASSERT_EQ(::getrlimit(RLIMIT_NOFILE, &original), 0);
  rlimit lowered = original;
  lowered.rlim_cur = static_cast<rlim_t>(highest_descriptor() + 1);
  ASSERT_EQ(::setrlimit(RLIMIT_NOFILE, &lowered), 0);
  const auto started = opened->start();
  ASSERT_EQ(::setrlimit(RLIMIT_NOFILE, &original), 0);

  EXPECT_TRUE(Rig::refused(started, StreamError::resource_exhausted));
  const auto& failure = opened->failure();
  ASSERT_TRUE(failure);
  EXPECT_EQ(failure->result(), StreamError::resource_exhausted);
  EXPECT_EQ(failure->stage(), FailureStage::session_init);
  EXPECT_EQ(failure->route(), libchiaking::play::Venue::home());
  EXPECT_NE(failure->sentence().find("file descriptors"), std::string::npos);
  EXPECT_TRUE(opened->stop());
}

}  // namespace
}  // namespace play_test
