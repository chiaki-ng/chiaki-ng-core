#pragma once

#include "src/core/net/protocol/senkusha/lifecycle.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <variant>

namespace senkusha_test {

namespace lifecycle = libchiaking::net::protocol::senkusha::lifecycle_detail;

class LifecycleRun {
 public:
  LifecycleRun() = default;
  LifecycleRun(const LifecycleRun&) = delete;
  LifecycleRun& operator=(const LifecycleRun&) = delete;

  template <class Effect>
  auto effects() const -> std::size_t {
    std::size_t count{};
    for (const auto& effect : context.effects)
      count += std::holds_alternative<Effect>(effect) ? 1U : 0U;
    return count;
  }

  template <class Effect>
  auto last_effect() const -> const Effect* {
    if (context.effects.empty()) return nullptr;
    return std::get_if<Effect>(&context.effects.back());
  }

  void reach_enabling_echo(bool measure_bandwidth = false) {
    ASSERT_TRUE(machine.process(
        lifecycle::StartRequested{1, 1, 1, 12, {}, measure_bandwidth}));
    ASSERT_TRUE(machine.process(lifecycle::TakionConnected{1, 2, 1}));
    ASSERT_TRUE(machine.process(lifecycle::BangAccepted{1}));
  }

  void reach_measuring_rtt(bool measure_bandwidth = false) {
    ASSERT_NO_FATAL_FAILURE(reach_enabling_echo(measure_bandwidth));
    ASSERT_TRUE(machine.process(lifecycle::EchoEnabled{1}));
  }

  void reach_measuring_inbound_mtu(bool measure_bandwidth = false) {
    ASSERT_NO_FATAL_FAILURE(reach_measuring_rtt(measure_bandwidth));
    ASSERT_TRUE(machine.process(lifecycle::RttMeasured{1, {}}));
    ASSERT_TRUE(machine.process(lifecycle::EchoDisabled{1}));
    ASSERT_EQ(
        machine.state(),
        libchiaking::net::protocol::senkusha::State::measuring_inbound_mtu);
  }

  void reach_starting_outbound_mtu(bool measure_bandwidth = false) {
    ASSERT_NO_FATAL_FAILURE(reach_measuring_inbound_mtu(measure_bandwidth));
    ASSERT_TRUE(machine.process(lifecycle::InboundMtuMeasured{1, 1344}));
    ASSERT_EQ(
        machine.state(),
        libchiaking::net::protocol::senkusha::State::starting_outbound_mtu);
  }

  void reach_outbound_mtu_disabled(bool measure_bandwidth = false) {
    ASSERT_NO_FATAL_FAILURE(reach_starting_outbound_mtu(measure_bandwidth));
    ASSERT_TRUE(machine.process(lifecycle::OutboundMtuReady{1}));
    ASSERT_TRUE(machine.process(lifecycle::OutboundMtuMeasured{1, 1420, 1454}));
    ASSERT_TRUE(machine.process(lifecycle::OutboundMtuDisabled{1}));
  }

  void reach_awaiting_big_echo() {
    ASSERT_TRUE(machine.process(
        lifecycle::StartRequested{1, 1, 1, 9,
                                  libchiaking::net::protocol::senkusha::
                                      BigResponsePolicy::fragmented_big_echo}));
    ASSERT_TRUE(machine.process(lifecycle::TakionConnected{1, 2, 2'000'000}));
  }

  auto cleaned_up_once() const -> testing::AssertionResult {
    if (effects<lifecycle::BestEffortDisconnect>() != 1)
      return testing::AssertionFailure()
             << effects<lifecycle::BestEffortDisconnect>()
             << " best-effort disconnects";
    if (effects<lifecycle::StopTakion>() != 1)
      return testing::AssertionFailure()
             << effects<lifecycle::StopTakion>() << " Takion stops";
    if (effects<lifecycle::PublishFailure>() != 1)
      return testing::AssertionFailure()
             << effects<lifecycle::PublishFailure>() << " published failures";
    return testing::AssertionSuccess();
  }

  lifecycle::LifecycleContext context;
  lifecycle::Lifecycle machine{context};
};

}  // namespace senkusha_test
