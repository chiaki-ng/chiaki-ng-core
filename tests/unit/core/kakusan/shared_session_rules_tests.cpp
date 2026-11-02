#include "src/core/kakusan/adapters/protocol_refusal.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/shared_session_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

namespace kakusan_test {
namespace {

using libchiaking::kakusan::EndReason;
using libchiaking::kakusan::Kakusan;
using libchiaking::kakusan::SharedSession;
using libchiaking::kakusan::SharingRules;
using manual::Error;
using manual::VerificationCode;
using namespace std::chrono_literals;

class SharedSessionSettings : public SharedSessionTest {};

TEST_F(SharedSessionSettings, RefuseImpossibleSeatCounts) {
  EXPECT_FALSE(SharingRules::create(0, 1));
  EXPECT_FALSE(SharingRules::create(4, 0));
  EXPECT_FALSE(SharingRules::create(2, 3));
  EXPECT_FALSE(SharingRules::create(5, 1));
}

TEST_F(SharedSessionSettings, RefuseUnusableStunServers) {
  Lan lan;
  test_support::SeededRandom random{1};
  Machine machine{lan, 1};
  MemoryStore store;
  libchiaking::platform::DefaultPlatformFactory platform{
      libchiaking::platform::PlatformProviders{
          .random = &random,
          .network = &machine,
          .crypto = &TestPlatform::crypto(),
          .secure_datagram = &TestPlatform::secure_datagram()}};
  const std::vector<std::string> crowded(Kakusan::most_stun_servers + 1,
                                         "stun.test:3478");
  const std::vector<std::string> blank{""};
  const std::vector<std::string> long_name{
      std::string(Kakusan::longest_stun_server + 1, 's')};
  for (const auto* servers : {&crowded, &blank, &long_name}) {
    const auto refused_servers =
        libchiaking::testing::ServiceBuilder::create<Kakusan>(platform, store,
                                                              *servers);
    ASSERT_FALSE(refused_servers);
    EXPECT_EQ(refused_servers.error(),
              Kakusan::Unavailable::invalid_stun_servers);
  }
  const std::vector<std::string> full(Kakusan::most_stun_servers,
                                      "192.0.2.1:3478");
  EXPECT_TRUE(libchiaking::testing::ServiceBuilder::create<Kakusan>(
      platform, store, full));
}

TEST_F(SharedSessionSettings, PaceAndLimitByDefault) {
  const SharedSession::Pacing pacing;
  EXPECT_EQ(pacing.admitted_wait, 2ms);
  EXPECT_EQ(pacing.tracked_players, 16U);
  EXPECT_EQ(SharedSession::diagnostic_limit, 512U);
}

TEST_F(SharedSessionSettings, TakeFourDigitVerificationCodes) {
  EXPECT_FALSE(VerificationCode::parse("12345"));
  EXPECT_FALSE(VerificationCode::parse("12a4"));
  EXPECT_FALSE(VerificationCode::parse(""));
  const auto code = VerificationCode::parse("1234");
  ASSERT_TRUE(code);
  EXPECT_EQ(code->digits(), "1234");
}

TEST_F(SharedSessionSettings, NameRefusalsAndEndings) {
  EXPECT_EQ(Refusal{Refusal::Kind::not_sharing}.name(), "NOT_SHARING");
  EXPECT_EQ(Refusal{Refusal::Kind::busy}.name(), "BUSY");
  EXPECT_EQ(Refusal{Refusal::Kind::cannot_be_shared}.name(),
            "CANNOT_BE_SHARED");
  EXPECT_EQ(libchiaking::kakusan::ProtocolRefusal::of(Error::no_path).name(), "UNREACHABLE");
  EXPECT_EQ(libchiaking::kakusan::ProtocolRefusal::of(Error::no_path).kind(), Refusal::Kind::unreachable);
  EXPECT_EQ(libchiaking::kakusan::ProtocolRefusal::of(Error::pairing_failed).kind(),
            Refusal::Kind::verification_failed);
  EXPECT_EQ(libchiaking::kakusan::ProtocolRefusal::of(Error::crypto_unavailable).kind(), Refusal::Kind::internal);
  EXPECT_EQ(libchiaking::kakusan::ProtocolRefusal::of(Error::pairing_failed).code(),
            manual::error_code(Error::pairing_failed));
  EXPECT_EQ(libchiaking::kakusan::ProtocolRefusal::of(Error::source_ended).code(),
            manual::error_code(Error::source_ended));
  EXPECT_EQ(EndReason{manual::Reason::source}, EndReason::Kind::session_ended);
  EXPECT_EQ(EndReason{manual::Reason::user}.name(), "LEFT");
}

}  // namespace
}  // namespace kakusan_test
