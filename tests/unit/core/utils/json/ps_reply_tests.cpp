#include "src/core/utils/json/ps_reply.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace json_test {

namespace ps = libchiaking::json::ps;

class PsReply : public ::testing::Test {
 protected:
  struct LaunchSlot {
    using keys = ps::CamelCaseKeys;
    std::string public_ip;
    std::uint16_t port{};
  };

  struct QueueReply {
    using keys = ps::CamelCaseKeys;
    bool queued{true};
    bool data_migration{};
    ps::LenientNumber<std::uint32_t> poll_frequency;
    std::optional<ps::LenientNumber<std::uint32_t>> wait_time_estimate;
    std::optional<ps::LenientNumber<std::uint64_t>> max_bandwidth;
    LaunchSlot launch_slot;
  };

  struct Stamped {
    using keys = ps::CamelCaseKeys;
    std::chrono::sys_time<std::chrono::milliseconds> created_at;
  };

  struct Credentials {
    using keys = ps::CamelCaseKeys;
    std::string access_token;
    ps::LenientNumber<std::uint32_t> expires_in;
  };

  struct SecretReply {
    using keys = ps::CamelCaseKeys;
    libchiaking::SecretString handshake_key;
    std::optional<libchiaking::SecretString> session_id;
  };
};

namespace {

TEST_F(PsReply, ReadsCamelCaseKeysAndQuotedNumbers) {
  const auto reply = ps::read_reply<QueueReply>(
      R"({"queued":false,"dataMigration":true,"pollFrequency":"15","launchSlot":{"publicIp":"1.2.3.4","port":2053}})");
  ASSERT_TRUE(reply);
  EXPECT_FALSE(reply->queued);
  EXPECT_TRUE(reply->data_migration);
  EXPECT_EQ(reply->poll_frequency.value(), 15U);
  EXPECT_FALSE(reply->wait_time_estimate);
  EXPECT_FALSE(reply->max_bandwidth);
  EXPECT_EQ(reply->launch_slot.public_ip, "1.2.3.4");
  EXPECT_EQ(reply->launch_slot.port, 2053);
}

TEST_F(PsReply, ReadsNumbersBareOrAsText) {
  const auto reply = ps::read_reply<QueueReply>(
      R"({"pollFrequency":15,"waitTimeEstimate":"120","maxBandwidth":30000000})");
  ASSERT_TRUE(reply);
  EXPECT_EQ(reply->poll_frequency.value(), 15U);
  ASSERT_TRUE(reply->wait_time_estimate);
  EXPECT_EQ(reply->wait_time_estimate->value(), 120U);
  ASSERT_TRUE(reply->max_bandwidth);
  EXPECT_EQ(reply->max_bandwidth->value(), 30'000'000U);
}

TEST_F(PsReply, RefusesTextThatIsNotANumber) {
  EXPECT_FALSE(ps::read_reply<QueueReply>(R"({"pollFrequency":"soon"})"));
  EXPECT_FALSE(ps::read_reply<QueueReply>(R"({"pollFrequency":"15s"})"));
  EXPECT_FALSE(ps::read_reply<QueueReply>(R"({"pollFrequency":""})"));
  EXPECT_FALSE(ps::read_reply<QueueReply>(R"({"pollFrequency":"-1"})"));
  EXPECT_FALSE(ps::LenientNumber<std::uint32_t>::from_text("4294967296"));
}

TEST_F(PsReply, IntegerRefusesWhatInt64CannotHold) {
  const auto largest = ps::Number::from_token("9223372036854775807");
  ASSERT_TRUE(largest);
  EXPECT_EQ(largest->integer(), std::numeric_limits<std::int64_t>::max());
  const auto above = ps::Number::from_token("9223372036854775808");
  ASSERT_TRUE(above);
  EXPECT_FALSE(above->integer());
  const auto unsigned_max = ps::Number::from_token("18446744073709551615");
  ASSERT_TRUE(unsigned_max);
  EXPECT_FALSE(unsigned_max->integer());
  const auto smallest = ps::Number::from_token("-9223372036854775808");
  ASSERT_TRUE(smallest);
  EXPECT_EQ(smallest->integer(), std::numeric_limits<std::int64_t>::min());
  const auto fraction = ps::Number::from_token("1.5");
  ASSERT_TRUE(fraction);
  EXPECT_FALSE(fraction->integer());
}

TEST_F(PsReply, IgnoresUnknownKeys) {
  const auto reply = ps::read_reply<QueueReply>(
      R"({"pollFrequency":"3","futureField":{"nested":[1,2]},"queued":false})");
  ASSERT_TRUE(reply);
  EXPECT_EQ(reply->poll_frequency.value(), 3U);
  EXPECT_FALSE(reply->queued);
}

TEST_F(PsReply, RefusesMalformedJson) {
  EXPECT_FALSE(ps::read_reply<QueueReply>(R"({"queued":)"));
}

TEST_F(PsReply, ReadsATimestampWithAnOffsetAsUtc) {
  using namespace std::chrono;
  const auto reply = ps::read_reply<Stamped>(
      R"({"createdAt":"2026-10-16T09:30:15.250+02:00"})");
  ASSERT_TRUE(reply);
  const auto expected = sys_days{year{2026} / October / 16} + hours{7} +
                        minutes{30} + seconds{15} + milliseconds{250};
  EXPECT_EQ(reply->created_at, expected);
}

TEST_F(PsReply, SerialisesAValueIntoASecret) {
  const Credentials credentials{"abc", ps::LenientNumber<std::uint32_t>{3600}};
  const auto body = ps::write(credentials);
  ASSERT_TRUE(body);
  EXPECT_EQ(body->view(), R"({"accessToken":"abc","expiresIn":3600})");
}

TEST_F(PsReply, ReadsSecretsStraightIntoWipingText) {
  const auto reply = ps::read_reply<SecretReply>(
      R"({"handshakeKey":"a\/bé\"c","sessionId":null})");
  ASSERT_TRUE(reply);
  EXPECT_EQ(reply->handshake_key.view(), "a/b\xC3\xA9\"c");
  EXPECT_FALSE(reply->session_id);
  EXPECT_FALSE(ps::read_reply<SecretReply>(R"({"handshakeKey":"bad\x"})"));
  EXPECT_FALSE(ps::read_reply<SecretReply>(R"({"handshakeKey":7})"));
}

TEST_F(PsReply, WritesSecretsEscaped) {
  const auto body =
      ps::write(SecretReply{libchiaking::SecretString{"k\x01"}, {}});
  ASSERT_TRUE(body);
  EXPECT_EQ(body->view(), R"({"handshakeKey":"k\u0001"})");
}

}  // namespace
}  // namespace json_test
