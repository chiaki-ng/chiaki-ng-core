#include "src/core/net/protocol/remote_play/pairing/cipher.hpp"
#include "src/core/net/protocol/remote_play/pairing/material.hpp"
#include "src/core/net/protocol/remote_play/pairing/request.hpp"
#include "tests/unit/core/consoles/adapters/pairing/home_pairing_rig.hpp"

#include <libchiaking/platform/diagnostics/provider.hpp>
#include <libchiaking/testing/in_memory_network.hpp>
#include <libchiaking/testing/manual_clock.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace consoles_test {
namespace {

namespace platform = libchiaking::platform;
namespace remote_play = libchiaking::net::protocol::remote_play;
using libchiaking::consoles::HomePairing;
using Error = HomePairing::Error;
using Step = HomePairing::Step;
using Target = remote_play::RegistrationTarget;

class CapturingDiagnostics final : public platform::DiagnosticsProvider {
 public:
  void write(platform::LogRecord record) noexcept override {
    lines.emplace_back(record.severity, std::string(record.message));
  }

  auto has(platform::LogSeverity severity, std::string_view text) const
      -> bool {
    return std::ranges::any_of(lines, [&](const auto& line) {
      return line.first == severity && line.second == text;
    });
  }

  std::vector<std::pair<platform::LogSeverity, std::string>> lines;
};

class HomePairingBench : public ::testing::Test {
 protected:
  static auto make_request(Target target = Target::ps4_10,
                           bool with_account = true,
                           std::optional<std::string> online_id = std::nullopt,
                           std::uint32_t console_pin = 0)
      -> HomePairing::Request {
    return PairingTime::request(
        target, with_account ? PairingTime::account(0) : std::nullopt,
        std::move(online_id), 0x01020304, console_pin);
  }

  static auto door(platform::SocketEndpoint endpoint) -> HomePairing::Door {
    HomePairing::Door result;
    result.destination = endpoint;
    return result;
  }

  auto pairing_of(platform::DiagnosticsProvider* diagnostics = nullptr)
      -> HomePairing {
    return HomePairing(network, PairingTime::threading(), crypto, clock, random,
                       diagnostics);
  }

  auto hear_legacy_knock() -> platform::SocketEndpoint {
    const auto source = console.await_knock();
    EXPECT_EQ(console.knock.size(), 5U);
    if (console.knock.size() == 5) {
      EXPECT_EQ(console.knock[0], 'S');
      EXPECT_EQ(console.knock[3], '2');
      EXPECT_EQ(console.knock[4], '\0');
    }
    return source;
  }

  static auto pumped_until(HomePairing& pairing, Step expected)
      -> testing::AssertionResult {
    const auto deadline = PairingTime::now() + std::chrono::seconds{2};
    while (pairing.step() != expected && PairingTime::now() < deadline) {
      const auto step = pairing.pump_once();
      if (!step && pairing.step() != Step::failed)
        return testing::AssertionFailure()
               << "a pump failed with " << testing::PrintToString(step.error())
               << " without failing the pairing";
      if (pairing.step() == Step::failed)
        return testing::AssertionFailure() << "the pairing failed";
      PairingTime::pause(std::chrono::milliseconds{1});
    }
    if (pairing.step() != expected)
      return testing::AssertionFailure()
             << "stopped at " << testing::PrintToString(pairing.step());
    return testing::AssertionSuccess();
  }

  auto expected_request() -> std::vector<std::byte> {
    std::array<std::byte, 16> ambassador{};
    std::array<std::byte, 8> account_id{};
    for (std::size_t index = 0; index < ambassador.size(); ++index)
      ambassador[index] = static_cast<std::byte>(index);
    for (std::size_t index = 0; index < account_id.size(); ++index)
      account_id[index] = static_cast<std::byte>(index);
    const auto material = remote_play::RegistrationMaterial::derive_local(
                              Target::ps4_10, ambassador, 0x01020304)
                              .value();
    const auto payload = remote_play::RegistrationRequestCodec::encode_payload(
                             crypto, Target::ps4_10, material, account_id)
                             .value();
    const auto header = remote_play::RegistrationRequestCodec::encode_header(
                            Target::ps4_10, payload.size())
                            .value();
    auto expected = HomeConsole::bytes(header);
    expected.insert(expected.end(), payload.begin(), payload.end());
    return expected;
  }

  libchiaking::testing::InMemoryNetwork network{PairingTime::threading(),
                                                PairingTime::steady()};
  libchiaking::testing::ManualClock clock;
  test_support::CountingRandom random;
  XorCrypto crypto;
  HomeConsole console{network, 0};
};

TEST_F(HomePairingBench, AWaitEndsOnAWake) {
  auto pairing = pairing_of();
  const auto woken_from = PairingTime::now();
  auto waker = libchiaking::platform::Thread::start(
                   PairingTime::threading(), "test-waker",
                   libchiaking::platform::ThreadRole::unspecified,
                   [&pairing] {
                     PairingTime::pause(std::chrono::milliseconds{30});
                     pairing.wake();
                   })
                   .value();
  pairing.wait(std::chrono::milliseconds{5000});
  const auto woken = PairingTime::now() - woken_from;
  static_cast<void>(waker.join());
  EXPECT_GE(woken, std::chrono::milliseconds{20});
  EXPECT_LT(woken, std::chrono::milliseconds{2000});
}

TEST_F(HomePairingBench, PairsOverLoopback) {
  auto pairing = pairing_of();
  ASSERT_TRUE(pairing.start(
      make_request(Target::ps4_10, true, std::nullopt, 0x00bc614e),
      door(console.endpoint())));
  const auto source = hear_legacy_knock();
  const auto waited_from = PairingTime::now();
  auto late_reply = libchiaking::platform::Thread::start(
                        PairingTime::threading(), "test-late-reply",
                        libchiaking::platform::ThreadRole::unspecified,
                        [&] {
                          PairingTime::pause(std::chrono::milliseconds{30});
                          console.answer(source, "RES2");
                        })
                        .value();
  pairing.wait(std::chrono::milliseconds{5000});
  const auto waited = PairingTime::now() - waited_from;
  static_cast<void>(late_reply.join());
  EXPECT_GE(waited, std::chrono::milliseconds{20});
  EXPECT_LT(waited, std::chrono::milliseconds{2000});
  ASSERT_TRUE(pumped_until(pairing, Step::settling));
  clock.now_us += 100'000;
  ASSERT_TRUE(pairing.pump_once());
  EXPECT_TRUE(pairing.step() == Step::connecting ||
              pairing.step() == Step::sending)
      << testing::PrintToString(pairing.step());

  const auto expected = expected_request();
  const auto deadline = PairingTime::now() + std::chrono::seconds{2};
  while (console.request.size() < expected.size() &&
         PairingTime::now() < deadline) {
    if (pairing.step() == Step::connecting || pairing.step() == Step::sending) {
      ASSERT_TRUE(pairing.pump_once());
    }
    if (console.accept()) {
      const auto part = console.read();
      if (!part) {
        EXPECT_EQ(part.error(), platform::NetError::would_block);
      }
    }
    PairingTime::pause(std::chrono::milliseconds{1});
  }
  ASSERT_TRUE(console.accept());
  EXPECT_EQ(console.request, expected);
  EXPECT_EQ(pairing.step(), Step::receiving);

  console.send(std::span<const std::byte>{HomeConsole::welcome(
      crypto, Target::ps4_10, 0x01020304, HomeConsole::ps4_facts)});
  ASSERT_TRUE(pumped_until(pairing, Step::paired));
  const auto result = pairing.take_result();
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().registration_key[0], std::byte{1});
  EXPECT_EQ(result.value().key[15], std::byte{0xff});
  EXPECT_EQ(result.value().console_pin, 0x00bc614eU);
  EXPECT_FALSE(pairing.take_result());
  EXPECT_TRUE(pairing.cancel());
}

TEST_F(HomePairingBench, CancelsDuringTheKnock) {
  auto pairing = pairing_of();
  ASSERT_TRUE(pairing.start(make_request(), door(console.endpoint())));
  static_cast<void>(hear_legacy_knock());
  EXPECT_TRUE(pairing.cancel());
  EXPECT_EQ(pairing.step(), Step::cancelled);
  EXPECT_EQ(pairing.error(), Error::cancelled);
  EXPECT_FALSE(pairing.take_result());
  EXPECT_TRUE(pairing.cancel());
}

TEST_F(HomePairingBench, CancelsWhileSettlingAndRefusesARestart) {
  auto pairing = pairing_of();
  ASSERT_TRUE(pairing.start(make_request(), door(console.endpoint())));
  const auto source = hear_legacy_knock();
  console.answer(source, "RES2");
  ASSERT_TRUE(pumped_until(pairing, Step::settling));
  EXPECT_TRUE(pairing.cancel());
  EXPECT_EQ(pairing.step(), Step::cancelled);
  EXPECT_EQ(pairing.error(), Error::cancelled);
  EXPECT_FALSE(pairing.start(make_request(), door(console.endpoint())));
  EXPECT_FALSE(pairing.take_result());
}

TEST_F(HomePairingBench, LetsAnInvalidRequestBeReplacedBeforeTheAttempt) {
  auto pairing = pairing_of();
  auto bad = door(console.endpoint());
  bad.destination.port = 0;
  const auto rejected = pairing.start(make_request(), std::move(bad));
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error(), Error::invalid_request);
  EXPECT_EQ(pairing.step(), Step::idle);
  ASSERT_TRUE(pairing.start(make_request(), door(console.endpoint())));
  static_cast<void>(hear_legacy_knock());
  EXPECT_TRUE(pairing.cancel());
}

TEST_F(HomePairingBench, ReportsTheStatusAndReasonOfARefusal) {
  CapturingDiagnostics diagnostics;
  auto pairing = pairing_of(&diagnostics);
  EXPECT_FALSE(pairing.refusal().http_status);
  ASSERT_TRUE(pairing.start(make_request(), door(console.endpoint())));
  const auto source = hear_legacy_knock();
  console.answer(source, "RES2");
  ASSERT_TRUE(pumped_until(pairing, Step::settling));
  clock.now_us += 100'000;
  ASSERT_TRUE(pairing.pump_once());

  const auto deadline = PairingTime::now() + std::chrono::seconds{2};
  while ((!console.accept() || pairing.step() != Step::receiving) &&
         PairingTime::now() < deadline) {
    if (pairing.step() != Step::receiving) {
      ASSERT_TRUE(pairing.pump_once());
    }
    if (console.accept()) static_cast<void>(console.read());
    PairingTime::pause(std::chrono::milliseconds{1});
  }
  ASSERT_TRUE(console.accept());
  ASSERT_EQ(pairing.step(), Step::receiving);
  console.send(
      "HTTP/1.1 403 Forbidden\r\nRP-Application-Reason: 80108b09\r\n"
      "Content-Length: 0\r\n\r\n");
  const auto until = PairingTime::now() + std::chrono::seconds{2};
  while (pairing.step() == Step::receiving && PairingTime::now() < until) {
    static_cast<void>(pairing.pump_once());
    PairingTime::pause(std::chrono::milliseconds{1});
  }
  EXPECT_EQ(pairing.step(), Step::failed);
  EXPECT_EQ(pairing.error(), Error::refused);
  const auto refusal = pairing.refusal();
  EXPECT_EQ(refusal.http_status, std::optional<std::uint16_t>{403});
  EXPECT_EQ(refusal.ps_reason, std::optional<std::uint32_t>{0x80108b09U});
  EXPECT_TRUE(diagnostics.has(platform::LogSeverity::error,
                              "Regist received HTTP code 403"));
  EXPECT_TRUE(diagnostics.has(platform::LogSeverity::error,
                              "Reported Application Reason: 0x80108b09 (Regist "
                              "failed, probably invalid "
                              "PIN)"));
}

TEST_F(HomePairingBench, RequiresAnAccountIdForModernFamilies) {
  for (const auto target : {Target::ps4_10, Target::ps5_1}) {
    auto pairing = pairing_of();
    const auto rejected = pairing.start(make_request(target, false, "online"),
                                        door(console.endpoint()));
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error(), Error::invalid_request);
    EXPECT_EQ(pairing.step(), Step::idle);
  }
}

TEST_F(HomePairingBench, RequiresAnOnlineIdForALegacyPs4) {
  auto pairing = pairing_of();
  const auto rejected = pairing.start(make_request(Target::ps4_8, false),
                                      door(console.endpoint()));
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error(), Error::invalid_request);
}

TEST_F(HomePairingBench, KnocksForALegacyPs4WithAnOnlineId) {
  auto pairing = pairing_of();
  ASSERT_TRUE(pairing.start(make_request(Target::ps4_8, false, "online"),
                            door(console.endpoint())));
  static_cast<void>(hear_legacy_knock());
  EXPECT_TRUE(pairing.cancel());
}

}  // namespace
}  // namespace consoles_test
