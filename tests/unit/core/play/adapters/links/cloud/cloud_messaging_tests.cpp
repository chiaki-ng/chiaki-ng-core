#include "src/core/play/adapters/links/cloud/cloud_messaging.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace play_test {
namespace {

namespace play = libchiaking::play;

class CloudMessaging : public ::testing::Test {
 protected:
  static auto words(std::span<const std::byte> body, std::size_t count)
      -> std::vector<std::uint32_t> {
    std::vector<std::uint32_t> values;
    for (std::size_t index = 0; index < count; ++index) {
      std::uint32_t value{};
      for (std::size_t shift = 0; shift < 4; ++shift)
        value |= static_cast<std::uint32_t>(
                     std::to_integer<std::uint8_t>(body[index * 4 + shift]))
                 << (shift * 8);
      values.push_back(value);
    }
    return values;
  }

  static auto handshake(std::uint32_t type,
                        std::vector<std::uint32_t> addresses)
      -> std::vector<std::byte> {
    std::vector<std::byte> body(play::CloudMessaging::sync_size);
    addresses.insert(addresses.begin(), type);
    for (std::size_t index = 0; index < addresses.size(); ++index)
      for (std::size_t shift = 0; shift < 4; ++shift)
        body[index * 4 + shift] =
            static_cast<std::byte>((addresses[index] >> (shift * 8)) & 0xffU);
    return body;
  }
};

TEST_F(CloudMessaging, WritesHeadersInCanonicalKeyOrder) {
  EXPECT_EQ(
      play::CloudMessaging::data_header(12, 1, 3),
      R"({"action":3,"dataHeader":{"addr":12,"id":3,"version":1},"msgType":1,"pid":0,"sender":1})");
}

TEST_F(CloudMessaging, OpensByListingOurAddresses) {
  play::CloudMessaging messaging{{7, 8, 13}};
  const auto opened = messaging.open();
  EXPECT_EQ(opened.metadata, play::CloudMessaging::data_header(4, 1, 1));
  ASSERT_EQ(opened.data.size(), play::CloudMessaging::sync_size);
  EXPECT_EQ(words(opened.data, 5),
            (std::vector<std::uint32_t>{1, 7, 8, 13, 0}));
  EXPECT_TRUE(messaging.allows(4));
  EXPECT_FALSE(messaging.allows(12));
}

TEST_F(CloudMessaging, AcknowledgesTheServicesTheServerLists) {
  play::CloudMessaging messaging{{7, 8, 13}};
  static_cast<void>(messaging.open());
  const auto listed = messaging.receive(
      play::CloudMessaging::data_header(4, 1, 9), handshake(1, {5, 12}));
  ASSERT_EQ(listed.replies.size(), 1U);
  EXPECT_FALSE(listed.message);
  EXPECT_EQ(listed.replies[0].metadata,
            play::CloudMessaging::data_header(4, 1, 2));
  ASSERT_GE(listed.replies[0].data.size(), 16U);
  EXPECT_EQ(words(listed.replies[0].data, 4),
            (std::vector<std::uint32_t>{2, 5, 12, 0}));
  EXPECT_TRUE(messaging.allows(12));
  EXPECT_TRUE(messaging.allows(5));
  EXPECT_FALSE(messaging.allows(13));
}

TEST_F(CloudMessaging, ListsOurAddressesAgainOnlyWhenTheServerMissedSome) {
  play::CloudMessaging messaging{{7, 8, 13}};
  static_cast<void>(messaging.open());
  static_cast<void>(messaging.receive(
      play::CloudMessaging::data_header(4, 1, 9), handshake(1, {5, 12})));
  EXPECT_TRUE(messaging
                  .receive(play::CloudMessaging::data_header(4, 1, 10),
                           handshake(2, {13, 7, 8}))
                  .replies.empty());
  const auto partial = messaging.receive(
      play::CloudMessaging::data_header(4, 1, 11), handshake(2, {7}));
  ASSERT_EQ(partial.replies.size(), 1U);
  ASSERT_GE(partial.replies[0].data.size(), 16U);
  EXPECT_EQ(words(partial.replies[0].data, 4),
            (std::vector<std::uint32_t>{1, 7, 8, 13}));
}

TEST_F(CloudMessaging, TellsMessagesFromResults) {
  play::CloudMessaging messaging{{7}};
  const std::vector<std::byte> text{std::byte{'h'}, std::byte{'i'}};
  const auto notice =
      messaging.receive(play::CloudMessaging::data_header(7, 2, 40), text);
  ASSERT_TRUE(notice.message);
  EXPECT_EQ(notice.message->address, 7U);
  EXPECT_EQ(notice.message->version, 2U);
  EXPECT_EQ(notice.message->id, 40U);
  EXPECT_EQ(notice.message->body, text);
  const auto answered = messaging.receive(
      R"({"action":3,"msgType":2,"pid":0,"resultHeader":{"addr":12,"id":5,"result":0,"version":1},"sender":2})",
      {});
  ASSERT_TRUE(answered.result);
  EXPECT_EQ(answered.result->address, 12U);
  EXPECT_EQ(answered.result->id, 5U);
  EXPECT_EQ(answered.result->result, 0U);
}

TEST_F(CloudMessaging, IgnoresAStranger) {
  play::CloudMessaging messaging{{7}};
  const std::vector<std::byte> text{std::byte{'h'}, std::byte{'i'}};
  const auto stranger = messaging.receive("not json", text);
  EXPECT_FALSE(stranger.message);
  EXPECT_FALSE(stranger.result);
  EXPECT_TRUE(stranger.replies.empty());
}

TEST_F(CloudMessaging, CarriesTheAccountAndThePressInACreatePress) {
  const auto body = play::CloudMessaging::capture_body(
      0x0102030405060708ULL, play::CloudMessaging::CapturePress::double_tap);
  ASSERT_EQ(body.size(), play::CloudMessaging::capture_size);
  EXPECT_EQ(words(body, 4),
            (std::vector<std::uint32_t>{0x05060708, 0x01020304, 79, 0}));
}

TEST_F(CloudMessaging, NumbersEachMessageItSends) {
  const auto body = play::CloudMessaging::capture_body(
      0x0102030405060708ULL, play::CloudMessaging::CapturePress::double_tap);
  play::CloudMessaging messaging{{}};
  const auto first = messaging.message(12, 1, body);
  const auto second = messaging.message(12, 1, body);
  EXPECT_EQ(first.metadata, play::CloudMessaging::data_header(12, 1, 1));
  EXPECT_EQ(second.metadata, play::CloudMessaging::data_header(12, 1, 2));
}

}  // namespace
}  // namespace play_test
