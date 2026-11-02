#include "src/core/net/protocol/remote_play/console_reply.hpp"
#include "src/core/net/protocol/remote_play/ctrl_codec.hpp"
#include "src/core/net/protocol/remote_play/pairing/response.hpp"
#include "src/core/utils/json/json.hpp"
#include "src/core/utils/text/base64.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace remote_play_test {

struct ConsoleReplyScript {
  std::string kind;
  std::string family;
  std::size_t framed_length{};
  std::string raw_base64;
  std::uint16_t status{};
  std::vector<std::array<std::string, 2>> headers;
};

namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
using remote_play::ConsoleReply;

class ConsoleReplyFixtures : public ::testing::Test {
 protected:
  void SetUp() override {
    std::ifstream file{LIBCHIAKING_CONSOLE_REPLY_FIXTURES
                       "/console_replies.jsonl"};
    ASSERT_TRUE(file);
    for (std::string line; std::getline(file, line);) {
      if (line.empty()) continue;
      auto reply = libchiaking::json::read<ConsoleReplyScript>(line);
      ASSERT_TRUE(reply) << line;
      replies.push_back(std::move(reply.value()));
    }
  }

  static auto bytes(std::string_view text) -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
  }

  static auto raw(const ConsoleReplyScript& reply)
      -> std::optional<std::string> {
    std::string decoded(reply.raw_base64.size(), '\0');
    const auto size = libchiaking::text::base64::decode_into(
        reply.raw_base64, std::as_writable_bytes(std::span{decoded}),
        libchiaking::text::base64::Alphabet::standard);
    if (!size) return std::nullopt;
    decoded.resize(*size);
    return decoded;
  }

  static auto closed(const ConsoleReplyScript& reply, std::string text)
      -> std::string {
    if (reply.kind == "pairing_payload")
      text += "\r\n";
    else if (reply.kind == "search")
      text += "\n";
    return text;
  }

  static auto read(const ConsoleReplyScript& reply, std::string_view text)
      -> libchiaking::Expected<ConsoleReply, ConsoleReply::Refusal> {
    if (reply.kind == "pairing_payload") return ConsoleReply::fields(text);
    return ConsoleReply::response(text);
  }

  std::vector<ConsoleReplyScript> replies;
};

TEST_F(ConsoleReplyFixtures, EveryRecordedReplyReadsWithTrimmedValues) {
  ASSERT_EQ(replies.size(), 10U);
  for (const auto& reply : replies) {
    const auto text = raw(reply);
    ASSERT_TRUE(text) << reply.kind;
    EXPECT_EQ(text->size(), reply.framed_length) << reply.kind;
    const auto framed = closed(reply, *text);
    const auto parsed = read(reply, framed);
    ASSERT_TRUE(parsed) << reply.kind;
    EXPECT_EQ(parsed->status(), reply.status) << reply.kind;
    const auto headers = parsed->headers();
    ASSERT_EQ(headers.size(), reply.headers.size()) << reply.kind;
    for (const auto& [header, expected] :
         std::views::zip(headers, reply.headers)) {
      EXPECT_EQ(header.name, expected[0]) << reply.kind;
      EXPECT_EQ(header.value, expected[1]) << reply.kind;
    }
  }
}

TEST_F(ConsoleReplyFixtures, APaddedContentLengthSizesThePairingBody) {
  for (const auto& reply : replies) {
    if (reply.kind != "pairing") continue;
    const auto text = raw(reply);
    ASSERT_TRUE(text);
    EXPECT_NE(text->find("Content-Length:        193"), std::string::npos);
    const auto parsed = ConsoleReply::response(*text);
    ASSERT_TRUE(parsed);
    EXPECT_TRUE(std::ranges::any_of(
        parsed->headers(), [](const ConsoleReply::Header& header) {
          return header.name == "Content-Length" && header.value == "193";
        }));
    const auto header =
        remote_play::RegistrationResponseCodec::parse_header(bytes(*text));
    ASSERT_TRUE(header);
    EXPECT_EQ(header->status, 200);
    EXPECT_EQ(header->header_size, reply.framed_length);
    EXPECT_EQ(header->content_size, 193U);
  }
}

TEST_F(ConsoleReplyFixtures, CtrlRepliesCarryTheServerType) {
  for (const auto& reply : replies) {
    if (reply.kind != "ctrl") continue;
    const auto text = raw(reply);
    ASSERT_TRUE(text);
    const auto response = remote_play::CtrlCodec::parse_response(bytes(*text));
    ASSERT_TRUE(response);
    ASSERT_TRUE(response.value());
    EXPECT_FALSE(response.value()->prohibited());
    EXPECT_EQ(response.value()->header_size(), reply.framed_length);
    EXPECT_TRUE(response.value()->encrypted_server_type());
  }
}

TEST(ConsoleReplyParse, RefusesAnUnfinishedHeaderBlockAsIncomplete) {
  const auto parsed =
      ConsoleReply::response("HTTP/1.1 200 OK\r\nRP-Nonce: x\r\n");
  ASSERT_FALSE(parsed);
  EXPECT_EQ(parsed.error(), ConsoleReply::Refusal::incomplete);
}

TEST(ConsoleReplyParse, RefusesHttpOneZeroAsMalformed) {
  const auto parsed = ConsoleReply::response("HTTP/1.0 200 OK\r\n\r\n");
  ASSERT_FALSE(parsed);
  EXPECT_EQ(parsed.error(), ConsoleReply::Refusal::malformed);
}

TEST(ConsoleReplyParse, RefusesAHeaderWithoutAColonAsMalformed) {
  const auto parsed =
      ConsoleReply::response("HTTP/1.1 200 OK\r\nno colon\r\n\r\n");
  ASSERT_FALSE(parsed);
  EXPECT_EQ(parsed.error(), ConsoleReply::Refusal::malformed);
}

TEST(ConsoleReplyParse, RefusesAFieldWithoutANameAsMalformed) {
  const auto parsed = ConsoleReply::fields(":value\r\n\r\n");
  ASSERT_FALSE(parsed);
  EXPECT_EQ(parsed.error(), ConsoleReply::Refusal::malformed);
}

TEST(ConsoleReplyParse, TrimsFieldValuesAndStopsBeforeTheBody) {
  const auto parsed = ConsoleReply::fields("Name: \t value \t\r\n\r\nbody");
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->length(), 19U);
  ASSERT_EQ(parsed->headers().size(), 1U);
  EXPECT_EQ(parsed->headers()[0].value, "value");
}

TEST(ConsoleReplyParse, DropsFoldedContinuationLines) {
  const auto parsed = ConsoleReply::response(
      "HTTP/1.1 200 OK\r\nRP-Nonce: a\r\n folded\r\nRP-Version: 9\r\n\r\n");
  ASSERT_TRUE(parsed);
  ASSERT_EQ(parsed->headers().size(), 2U);
  EXPECT_EQ(parsed->headers()[0].name, "RP-Nonce");
  EXPECT_EQ(parsed->headers()[0].value, "a");
  EXPECT_EQ(parsed->headers()[1].name, "RP-Version");
}

TEST(ConsoleReplyParse, RefusesASpaceBeforeTheColonAsMalformed) {
  const auto parsed =
      ConsoleReply::response("HTTP/1.1 200 OK\r\nRP-Nonce : a\r\n\r\n");
  ASSERT_FALSE(parsed);
  EXPECT_EQ(parsed.error(), ConsoleReply::Refusal::malformed);
}

TEST(ConsoleReplyParse, RefusesALowerCaseProtocolAsMalformed) {
  const auto parsed = ConsoleReply::response("http/1.1 200 OK\r\n\r\n");
  ASSERT_FALSE(parsed);
  EXPECT_EQ(parsed.error(), ConsoleReply::Refusal::malformed);
}

TEST(ConsoleReplyParse, IgnoresControlBytesInTheReasonPhrase) {
  for (const std::string_view head :
       {std::string_view{"HTTP/1.1 403 a\x01b\r\n\r\n"},
        std::string_view{"HTTP/1.1 403 a\0b\r\n\r\n", 20}}) {
    const auto parsed = ConsoleReply::response(head);
    ASSERT_TRUE(parsed) << head;
    EXPECT_EQ(parsed->status(), 403U);
    EXPECT_EQ(parsed->length(), head.size());
  }
}

TEST(ConsoleReplyParse, KeepsHighBitBytesInValues) {
  const auto parsed = ConsoleReply::response(
      "HTTP/1.1 200 OK\r\nAP-Ssid: caf\xc3\xa9 \xff\r\n\r\n");
  ASSERT_TRUE(parsed);
  ASSERT_EQ(parsed->headers().size(), 1U);
  EXPECT_EQ(parsed->headers()[0].value, "caf\xc3\xa9 \xff");
}

}  // namespace
}  // namespace remote_play_test
