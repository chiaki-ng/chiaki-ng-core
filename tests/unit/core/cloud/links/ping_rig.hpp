#pragma once

#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/play/adapters/links/cloud/datacenter_survey.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/process_threading.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace cloud_test {

class ScriptedCounts {
 public:
  int polls{};
  int cancels{};
};

class ScriptedJob final : public libchiaking::play::PingJob {
 public:
  ScriptedJob(
      ScriptedCounts& counts, int terminal_poll,
      std::optional<libchiaking::net::protocol::senkusha::Result> result)
      : counts_(counts),
        terminal_poll_(terminal_poll),
        result_(std::move(result)) {}

  auto poll() -> std::optional<Finished> override {
    ++counts_.polls;
    if (counts_.polls < terminal_poll_) return std::nullopt;
    if (result_) return Finished{*result_};
    return Finished{libchiaking::unexpected(Failed{})};
  }

  void cancel() noexcept override { ++counts_.cancels; }

 private:
  ScriptedCounts& counts_;
  int terminal_poll_;
  std::optional<libchiaking::net::protocol::senkusha::Result> result_;
};

class PingRig {
 public:
  using Result = libchiaking::net::protocol::senkusha::Result;

  static auto make_result(std::int64_t rtt_us) -> Result {
    libchiaking::net::protocol::senkusha::RttStatistics rtt;
    rtt.raw_mean = std::chrono::microseconds{rtt_us};
    rtt.adjusted_mean = std::chrono::microseconds{rtt_us};
    const auto profile =
        libchiaking::net::protocol::takion::resolve_takion_wire_profile(9);
    EXPECT_TRUE(profile);
    return {9, rtt, 1454, 1254, profile.value()};
  }

  static auto job(ScriptedCounts& counts, int terminal_poll,
                  std::optional<Result> result)
      -> std::unique_ptr<libchiaking::play::PingJob> {
    return std::make_unique<ScriptedJob>(counts, terminal_poll,
                                         std::move(result));
  }

  static auto test_key() -> libchiaking::SecretString {
    libchiaking::SecretString key;
    key.assign("synthetic-lock-key");
    return key;
  }

  static auto monotonic() -> libchiaking::platform::SteadyClock& {
    static libchiaking::platform::SteadyClock clock;
    return clock;
  }

  static auto deadline_in(std::chrono::microseconds span) -> std::uint64_t {
    return monotonic().now_microseconds() +
           static_cast<std::uint64_t>(span.count());
  }

  static auto before(std::uint64_t deadline) -> bool {
    return monotonic().now_microseconds() < deadline;
  }

  static auto elapsed_since(std::uint64_t started)
      -> std::chrono::microseconds {
    return std::chrono::microseconds{
        static_cast<std::int64_t>(monotonic().now_microseconds() - started)};
  }

  static auto pause_for(std::chrono::nanoseconds span,
                        const libchiaking::platform::StopToken& stop = {})
      -> bool {
    return libchiaking::platform::Sleeper{test_support::process_threading(),
                                          monotonic()}
        .sleep_for(span, stop);
  }
};

}  // namespace cloud_test
