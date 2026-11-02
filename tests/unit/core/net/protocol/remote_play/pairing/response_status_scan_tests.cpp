#include "src/core/net/protocol/remote_play/pairing/response.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace remote_play_test {
namespace {

using namespace std::string_view_literals;

using libchiaking::net::protocol::remote_play::RegistrationResponseCodec;
using libchiaking::net::protocol::remote_play::ResponseStatus;

class RegistrationStatusScan : public ::testing::Test {
 protected:
  static auto scan(std::string_view text) -> ResponseStatus {
    return RegistrationResponseCodec::scan_status(std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(text.data()), text.size()});
  }

  static auto reads(std::string_view text, std::uint16_t status,
                    std::optional<std::uint32_t> reason)
      -> testing::AssertionResult {
    const auto scanned = scan(text);
    if (scanned.status != status)
      return testing::AssertionFailure() << "status " << scanned.status;
    if (scanned.application_reason != reason)
      return testing::AssertionFailure()
             << "reason " << testing::PrintToString(scanned.application_reason);
    return testing::AssertionSuccess();
  }
};

TEST_F(RegistrationStatusScan, StopsAtTheFirstNul) {
  EXPECT_TRUE(
      reads("HTTP/1.1 403 F\r\nRP-Application-Reason: 80108b10\r\n\0"
            "RP-Application-Reason: 1\r\n"sv,
            403, 0x80108b10U));
  EXPECT_TRUE(
      reads("HTTP/1.1 4\0"
            "03 F\r\n"sv,
            0, std::nullopt));
}

TEST_F(RegistrationStatusScan, EndsALineAtABareCarriageReturn) {
  EXPECT_TRUE(
      reads("HTTP/1.1 403 F\r\nX-Other: y\rRP-Application-Reason: 80108b10\r",
            403, 0x80108b10U));
}

TEST_F(RegistrationStatusScan, SkipsLinesWithoutANameAndColon) {
  EXPECT_TRUE(
      reads("HTTP/1.1 403 F\r\nno colon here\r\n: nameless\r\n"
            "RP-Application-Reason: 80108b10\r\n",
            403, 0x80108b10U));
}

TEST_F(RegistrationStatusScan, ReadsATruncatedHeadUpToItsLastWholeLine) {
  EXPECT_TRUE(reads("HTTP/1.1 403 F\r\nRP-Application-Reason: 80108b10\r\nX-Pa",
                    403, 0x80108b10U));
  EXPECT_TRUE(reads("HTTP/1.1 403 F\r\nRP-Application-Reason: 80108b10", 403,
                    std::nullopt));
  EXPECT_TRUE(reads("HTTP/1.1 403", 0, std::nullopt));
}

TEST_F(RegistrationStatusScan, ReadsTheReasonBesideAnUnusableStatus) {
  EXPECT_TRUE(reads("HTTP/1.1 0 F\r\nRP-Application-Reason: 80108b10\r\n\r\n",
                    0, 0x80108b10U));
  EXPECT_TRUE(
      reads("HTTP/1.1 99999 F\r\nRP-Application-Reason: 80108b10\r\n\r\n", 0,
            0x80108b10U));
  EXPECT_TRUE(reads("HTTP/1.1 4o3 F\r\nRP-Application-Reason: 80108b10\r\n\r\n",
                    0, 0x80108b10U));
}

TEST_F(RegistrationStatusScan, ReportsASuccessWithoutAContentLength) {
  EXPECT_TRUE(reads("HTTP/1.1 200 OK\r\n\r\n", 200, std::nullopt));
  EXPECT_TRUE(reads("HTTP/1.1 200\n", 200, std::nullopt));
}

TEST_F(RegistrationStatusScan, KeepsTheLastReason) {
  EXPECT_TRUE(
      reads("HTTP/1.1 403 F\r\nRP-Application-Reason: 80108b10\r\n"
            "RP-Application-Reason: 80108b11\r\n\r\n",
            403, 0x80108b11U));
  EXPECT_TRUE(
      reads("HTTP/1.1 403 F\r\nRP-Application-Reason: 80108b10\r\n"
            "RP-Application-Reason: xyz\r\n\r\n",
            403, 0U));
}

TEST_F(RegistrationStatusScan, StopsAtTheBlankLineAndNeedsHttpOneOne) {
  EXPECT_TRUE(
      reads("HTTP/1.1 403 F\r\n\r\nRP-Application-Reason: 80108b10\r\n\r\n",
            403, std::nullopt));
  EXPECT_TRUE(reads("HTTP/1.0 403 F\r\nRP-Application-Reason: 80108b10\r\n\r\n",
                    0, std::nullopt));
  EXPECT_TRUE(reads("HTTP/1.1 403 F\r\nrp-application-reason: 80108b10\r\n\r\n",
                    403, std::nullopt));
}

}  // namespace
}  // namespace remote_play_test
