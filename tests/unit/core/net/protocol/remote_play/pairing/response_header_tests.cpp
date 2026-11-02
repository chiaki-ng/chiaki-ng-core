#include "src/core/net/protocol/remote_play/pairing/response.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace remote_play_test {
namespace {

using libchiaking::net::protocol::remote_play::RegistrationResponseCodec;
using libchiaking::net::protocol::remote_play::ResponseHeaderError;

class RegistrationResponseHeader : public ::testing::Test {
 protected:
  static auto parse(std::string_view text) {
    return RegistrationResponseCodec::parse_header(std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(text.data()), text.size()});
  }

  static auto with_content_length(std::string_view value) -> std::string {
    return std::string("HTTP/1.1 200 OK\r\nContent-Length:") +
           std::string{value} + "\r\n\r\n";
  }

  static auto with_status_line(std::string_view line) -> std::string {
    return std::string{line} + "Content-Length: 4\r\n\r\n";
  }

  const std::string valid200 =
      "HTTP/1.1 200 OK\r\n"
      "Content-Length: 12\r\n"
      "X-Unknown: whatever\r\n"
      "\r\n";
};

TEST_F(RegistrationResponseHeader, ReadsAContentLengthAroundBlanksAndZeros) {
  for (const std::string_view length :
       {" 193", "193", " 0193", "        193", "\t193", " \t 193", " 193 ",
        " 193\t"}) {
    const auto header = with_content_length(length);
    const auto parsed = parse(header);
    ASSERT_TRUE(parsed) << length;
    EXPECT_EQ(parsed.value().status, 200U) << length;
    EXPECT_EQ(parsed.value().content_size, 193U) << length;
    EXPECT_EQ(parsed.value().header_size, header.size()) << length;
  }
}

TEST_F(RegistrationResponseHeader, RefusesAContentLengthThatIsNotADecimal) {
  for (const std::string_view invalid :
       {" -26", " -8", "   ", "   -64", " x1", "   +8", " 8 3", " 8x", " 0x8",
        " 200abc"})
    EXPECT_FALSE(parse(with_content_length(invalid))) << invalid;
}

TEST_F(RegistrationResponseHeader, AcceptsLenientStatusLines) {
  for (const std::string_view line :
       {"HTTP/1.1 0200 OK\r\n", "HTTP/1.1 200\r\n", "HTTP/1.1 200 OK\n"}) {
    const auto parsed = parse(with_status_line(line));
    ASSERT_TRUE(parsed) << line;
    EXPECT_EQ(parsed.value().status, 200U) << line;
    EXPECT_EQ(parsed.value().content_size, 4U) << line;
  }
}

TEST_F(RegistrationResponseHeader, RefusesMalformedStatusLines) {
  for (const std::string_view line :
       {"HTTP/1.1  200 OK\r\n", "HTTP/1.1 +200\r\n", "HTTP/1.1 200OK\n"})
    EXPECT_FALSE(parse(with_status_line(line))) << line;
}

TEST_F(RegistrationResponseHeader, ReadsAnUnusualStatusAndItsReason) {
  const auto parsed =
      parse("HTTP/1.1 999 Odd\r\nRP-Application-Reason: 80108b12\r\n\r\n");
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed.value().status, 999U);
  EXPECT_EQ(parsed.value().application_reason,
            std::optional<std::uint32_t>{0x80108b12U});
  const auto odd = parse("HTTP/1.1 99 x\r\n\r\n");
  ASSERT_TRUE(odd);
  EXPECT_EQ(odd.value().status, 99U);
}

TEST_F(RegistrationResponseHeader, ReportsAnOutOfRangeStatusAsZero) {
  const auto huge = parse("HTTP/1.1 70000\r\n\r\n");
  ASSERT_TRUE(huge);
  EXPECT_EQ(huge.value().status, 0U);
  const auto negative = parse("HTTP/1.1 -200\r\n\r\n");
  ASSERT_TRUE(negative);
  EXPECT_EQ(negative.value().status, 0U);
}

TEST_F(RegistrationResponseHeader, RefusesAMissingOrZeroStatus) {
  EXPECT_FALSE(parse("HTTP/1.1 OK 200\r\n\r\n"));
  EXPECT_FALSE(parse("HTTP/1.1 0\r\n\r\n"));
}

TEST_F(RegistrationResponseHeader, RefusesABareLineBreakInsideTheHeader) {
  const auto parsed =
      parse("HTTP/1.1 200 OK\r\nX: a\nContent-Length: 5\rY: b\r\n\r\n");
  ASSERT_FALSE(parsed);
  EXPECT_EQ(parsed.error(), ResponseHeaderError::invalid);
}

TEST_F(RegistrationResponseHeader, AcceptsAnErrorStatusWithoutContent) {
  const auto bare = parse("HTTP/1.1 403\r\n\r\n");
  ASSERT_TRUE(bare);
  EXPECT_EQ(bare.value().status, 403U);
  EXPECT_EQ(bare.value().content_size, 0U);
}

TEST_F(RegistrationResponseHeader, ReadsAContentLengthWithALeadingZero) {
  for (const std::string_view value : {"016", "16"}) {
    const auto parsed =
        parse(std::string("HTTP/1.1 200 OK\r\nContent-Length: ") +
              std::string{value} + "\r\n\r\n");
    ASSERT_TRUE(parsed) << value;
    EXPECT_EQ(parsed.value().content_size, 16U) << value;
  }
}

TEST_F(RegistrationResponseHeader, RefusesAHexadecimalContentLength) {
  EXPECT_FALSE(parse("HTTP/1.1 200 OK\r\nContent-Length: 0x10\r\n\r\n"));
  EXPECT_FALSE(parse("HTTP/1.1 200 OK\r\nContent-Length: 0x\r\n\r\n"));
}

TEST_F(RegistrationResponseHeader, MatchesHeaderNamesCaseSensitively) {
  EXPECT_FALSE(parse("HTTP/1.1 200 OK\r\ncontent-length: 4\r\n\r\n"));
}

TEST_F(RegistrationResponseHeader, ReadsAValidSuccessHeader) {
  const auto parsed = parse(valid200);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed.value().status, 200U);
  EXPECT_FALSE(parsed.value().application_reason);
  EXPECT_EQ(parsed.value().header_size, valid200.size());
  EXPECT_EQ(parsed.value().content_size, 12U);
}

TEST_F(RegistrationResponseHeader, CallsEveryShorterPrefixIncomplete) {
  for (std::size_t n = 0; n < valid200.size(); ++n) {
    const auto parsed = parse(std::string_view{valid200}.substr(0, n));
    ASSERT_FALSE(parsed) << n << " bytes";
    EXPECT_EQ(parsed.error(), ResponseHeaderError::incomplete) << n << " bytes";
  }
}

TEST_F(RegistrationResponseHeader, IgnoresBinaryBodyBytesAfterTheHeader) {
  std::string with_body = valid200 + std::string(12, '\0');
  with_body.back() = '\xFF';
  const auto parsed = parse(with_body);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed.value().header_size, valid200.size());
}

TEST_F(RegistrationResponseHeader, ReadsTheReasonOfARefusal) {
  const auto parsed =
      parse("HTTP/1.1 403 Forbidden\r\nRP-Application-Reason: 1f\r\n\r\n");
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed.value().status, 403U);
  ASSERT_TRUE(parsed.value().application_reason);
  EXPECT_EQ(parsed.value().application_reason.value(), 0x1fU);
  EXPECT_EQ(parsed.value().content_size, 0U);
}

TEST_F(RegistrationResponseHeader, NeedsContentInASuccess) {
  EXPECT_FALSE(parse("HTTP/1.1 200 OK\r\n\r\n"));
  EXPECT_FALSE(parse("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n"));
}

TEST_F(RegistrationResponseHeader, RefusesAnOverflowingContentLength) {
  EXPECT_FALSE(
      parse("HTTP/1.1 200 OK\r\nContent-Length: 99999999999999999999\r\n\r\n"));
}

TEST_F(RegistrationResponseHeader, KeepsTheFirstContentLength) {
  const auto first = parse(
      "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nContent-Length: 9\r\n\r\n");
  ASSERT_TRUE(first);
  EXPECT_EQ(first.value().content_size, 4U);
}

TEST_F(RegistrationResponseHeader, KeepsTheLastReason) {
  const auto last = parse(
      "HTTP/1.1 400 x\r\nRP-Application-Reason: 1\r\nRP-Application-Reason: "
      "2\r\n\r\n");
  ASSERT_TRUE(last);
  EXPECT_EQ(last.value().application_reason, std::optional<std::uint32_t>{2U});
}

TEST_F(RegistrationResponseHeader, DropsAReasonWiderThanThirtyTwoBits) {
  const auto wide =
      parse("HTTP/1.1 400 x\r\nRP-Application-Reason: 100000000\r\n\r\n");
  ASSERT_TRUE(wide);
  EXPECT_FALSE(wide.value().application_reason);
}

TEST_F(RegistrationResponseHeader, RefusesHttp10) {
  EXPECT_FALSE(parse("HTTP/1.0 200 x\r\n\r\n"));
}

TEST_F(RegistrationResponseHeader, RefusesAHeaderLineWithoutAColon) {
  EXPECT_FALSE(parse("HTTP/1.1 404 x\r\nBadLine\r\n\r\n"));
}

TEST_F(RegistrationResponseHeader, RefusesANulInAHeaderValue) {
  const std::string header =
      std::string("HTTP/1.1 404 x\r\nRP-Application-Reason: 1\r\nX-A: a") +
      '\0' + "b\r\nRP-Application-Reason: 2\r\n\r\n";
  const auto parsed = parse(header);
  ASSERT_FALSE(parsed);
  EXPECT_EQ(parsed.error(), ResponseHeaderError::invalid);
}

TEST_F(RegistrationResponseHeader, GivesUpOnAnUnfinishedHeaderAt1500Bytes) {
  const std::string unfinished(1500, 'A');
  const auto too_large = parse(unfinished);
  ASSERT_FALSE(too_large);
  EXPECT_EQ(too_large.error(), ResponseHeaderError::too_large);
  const auto waiting = parse(std::string_view{unfinished}.substr(0, 1499));
  ASSERT_FALSE(waiting);
  EXPECT_EQ(waiting.error(), ResponseHeaderError::incomplete);
}

TEST_F(RegistrationResponseHeader, AcceptsAResponseOfExactly1500Bytes) {
  const std::string header = "HTTP/1.1 200 x\r\nContent-Length: 1460\r\n\r\n";
  ASSERT_EQ(header.size() + 1460, 1500U);
  const auto parsed = parse(header);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed.value().header_size, header.size());
  EXPECT_EQ(parsed.value().content_size, 1460U);
}

TEST_F(RegistrationResponseHeader, RefusesAResponseOneByteOver1500) {
  const auto parsed = parse("HTTP/1.1 200 x\r\nContent-Length: 1461\r\n\r\n");
  ASSERT_FALSE(parsed);
  EXPECT_EQ(parsed.error(), ResponseHeaderError::too_large);
}

TEST_F(RegistrationResponseHeader, RefusesAContentLengthThatWrapsPastTheLimit) {
  for (const std::string_view length :
       {" 18446744073709551615", " 18446744073709551600"}) {
    const auto parsed = parse(with_content_length(length));
    ASSERT_FALSE(parsed) << length << " gave a body of "
                         << parsed.value().content_size;
    EXPECT_EQ(parsed.error(), ResponseHeaderError::too_large) << length;
  }
}

}  // namespace
}  // namespace remote_play_test
