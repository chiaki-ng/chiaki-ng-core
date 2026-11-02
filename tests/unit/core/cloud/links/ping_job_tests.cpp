#include "src/core/net/transport/datagram/transport.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/random.hpp"
#include "tests/unit/core/cloud/links/ping_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <utility>

namespace cloud_test {
namespace {

namespace play = libchiaking::play;
namespace datagram = libchiaking::net::transport::datagram;
namespace takion = libchiaking::net::protocol::takion;
namespace senkusha = libchiaking::net::protocol::senkusha;

class TransportCounts {
 public:
  int sends{};
  int closes{};
};

class CountingTransport final : public datagram::DatagramTransport {
 public:
  explicit CountingTransport(TransportCounts& counts) : counts_(counts) {}

  auto send(std::span<const std::byte> data)
      -> takion::Expected<std::size_t, datagram::TransportError> override {
    ++counts_.sends;
    return data.size();
  }

  auto receive() -> takion::Expected<std::optional<datagram::Datagram>,
                                     datagram::TransportError> override {
    return std::optional<datagram::Datagram>{};
  }

  auto close() -> takion::Expected<void, datagram::TransportError> override {
    ++counts_.closes;
    return {};
  }

 private:
  TransportCounts& counts_;
};

class SenkushaPingJob : public ::testing::Test {
 protected:
  auto job(senkusha::ClientConfig config = {})
      -> std::unique_ptr<play::PingJob> {
    return play::PingJob::over(std::make_unique<CountingTransport>(counts),
                               clock, test_support::process_threading(), random,
                               std::move(config));
  }

  static auto has_failed(const std::optional<play::PingJob::Finished>& polled)
      -> bool {
    return polled && !*polled;
  }

  test_support::FakeClock clock{1'000};
  test_support::FixedRandom random;
  TransportCounts counts;
};

TEST_F(SenkushaPingJob, FailsAnInvalidConfigWithoutSending) {
  senkusha::ClientConfig invalid;
  invalid.policy.probe.count = 0;
  const auto invalid_job = job(std::move(invalid));
  EXPECT_TRUE(has_failed(invalid_job->poll()));
  EXPECT_EQ(counts.sends, 0);
  EXPECT_TRUE(has_failed(invalid_job->poll()));
  EXPECT_EQ(counts.sends, 0);
  EXPECT_EQ(counts.closes, 1);
}

TEST_F(SenkushaPingJob, FailsWithoutSendingOnceCancelled) {
  const auto cancelled_job = job();
  cancelled_job->cancel();
  cancelled_job->cancel();
  EXPECT_TRUE(has_failed(cancelled_job->poll()));
  EXPECT_EQ(counts.sends, 0);
  EXPECT_EQ(counts.closes, 1);
}

TEST_F(SenkushaPingJob, SendsUntilCancelledAndThenFails) {
  const auto active_job = job();
  EXPECT_FALSE(active_job->poll());
  EXPECT_GT(counts.sends, 0);
  EXPECT_EQ(counts.closes, 0);
  active_job->cancel();
  EXPECT_TRUE(has_failed(active_job->poll()));
  EXPECT_EQ(counts.closes, 1);
}

}  // namespace
}  // namespace cloud_test
