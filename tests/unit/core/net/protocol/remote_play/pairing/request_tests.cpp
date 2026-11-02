#include "src/core/net/protocol/remote_play/pairing/request.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace remote_play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
using remote_play::HeaderError;
using remote_play::RegistrationRequestCodec;
using remote_play::RegistrationTarget;

TEST(RegistrationRequestHeader, PostsAPs4Version8RequestWithoutAVersion) {
  const auto result =
      RegistrationRequestCodec::encode_header(RegistrationTarget::ps4_8, 160);
  ASSERT_TRUE(result);
  EXPECT_EQ(
      result.value(),
      "POST /sce/rp/regist HTTP/1.1\r\n HTTP/1.1\r\nHOST: 10.0.2.15\r\n"
      "User-Agent: remoteplay Windows\r\nConnection: close\r\nContent-Length: "
      "160\r\n"
      "\r\n");
}

TEST(RegistrationRequestHeader, PostsAPs4Version9RequestToTheOldPath) {
  const auto result =
      RegistrationRequestCodec::encode_header(RegistrationTarget::ps4_9, 320);
  ASSERT_TRUE(result);
  EXPECT_EQ(
      result.value(),
      "POST /sce/rp/regist HTTP/1.1\r\n HTTP/1.1\r\nHOST: 10.0.2.15\r\n"
      "User-Agent: remoteplay Windows\r\nConnection: close\r\nContent-Length: "
      "320\r\n"
      "RP-Version: 9.0\r\n"
      "\r\n");
}

TEST(RegistrationRequestHeader, PostsAPs4Version10RequestToTheSessionPath) {
  const auto result =
      RegistrationRequestCodec::encode_header(RegistrationTarget::ps4_10, 400);
  ASSERT_TRUE(result);
  EXPECT_EQ(
      result.value(),
      "POST /sie/ps4/rp/sess/rgst HTTP/1.1\r\n HTTP/1.1\r\nHOST: 10.0.2.15\r\n"
      "User-Agent: remoteplay Windows\r\nConnection: close\r\nContent-Length: "
      "400\r\n"
      "RP-Version: 10.0\r\n"
      "\r\n");
}

TEST(RegistrationRequestHeader, PostsAnUnknownPs4ToThePs4PathWithoutAVersion) {
  const auto result = RegistrationRequestCodec::encode_header(
      RegistrationTarget::ps4_unknown, 400);
  ASSERT_TRUE(result);
  EXPECT_EQ(
      result.value(),
      "POST /sie/ps4/rp/sess/rgst HTTP/1.1\r\n HTTP/1.1\r\nHOST: 10.0.2.15\r\n"
      "User-Agent: remoteplay Windows\r\nConnection: close\r\nContent-Length: "
      "400\r\n"
      "\r\n");
}

TEST(RegistrationRequestHeader, PostsAPs5RequestToItsHost) {
  const auto result = RegistrationRequestCodec::encode_header(
      RegistrationTarget::ps5_1, 1024, "::1");
  ASSERT_TRUE(result);
  EXPECT_EQ(
      result.value(),
      "POST /sie/ps5/rp/sess/rgst HTTP/1.1\r\n HTTP/1.1\r\nHOST: ::1\r\n"
      "User-Agent: remoteplay Windows\r\nConnection: close\r\nContent-Length: "
      "1024\r\n"
      "RP-Version: 1.0\r\n"
      "\r\n");
}

TEST(RegistrationRequestHeader, NamesTheHostAndTheWholeContentLength) {
  const auto result = RegistrationRequestCodec::encode_header(
      RegistrationTarget::ps4_9, 999, "192.168.1.42");
  ASSERT_TRUE(result);
  EXPECT_NE(result.value().find("HOST: 192.168.1.42\r\n"), std::string::npos);
  EXPECT_NE(result.value().find("Content-Length: 999\r\n"), std::string::npos);
  EXPECT_EQ(result.value().find("Content-Length: 99\r\n"), std::string::npos);
}

TEST(RegistrationRequestHeader, RefusesAnUnknownTarget) {
  const auto result = RegistrationRequestCodec::encode_header(
      static_cast<RegistrationTarget>(200), 16);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), HeaderError::invalid_target);
}

TEST(RegistrationRequestHeader, RefusesAnEmptyHost) {
  const auto result = RegistrationRequestCodec::encode_header(
      RegistrationTarget::ps4_8, 16, "");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), HeaderError::invalid_host);
}

TEST(RegistrationRequestHeader, RefusesAHostLongerThanAnAddress) {
  const std::string long_host(46, '1');
  const auto result = RegistrationRequestCodec::encode_header(
      RegistrationTarget::ps4_8, 16, long_host);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), HeaderError::invalid_host);
}

TEST(RegistrationRequestHeader, RefusesAHostWithWhitespaceOrLineBreaks) {
  for (std::string_view bad :
       {"10.0.2.15\r\nX: y", "a b", "10.0.2.15\n", "host\tname"}) {
    const auto result = RegistrationRequestCodec::encode_header(
        RegistrationTarget::ps4_8, 16, bad);
    ASSERT_FALSE(result) << bad;
    EXPECT_EQ(result.error(), HeaderError::invalid_host) << bad;
  }
}

TEST(RegistrationRequestHeader, RefusesAnEmptyPayload) {
  const auto result =
      RegistrationRequestCodec::encode_header(RegistrationTarget::ps4_8, 0);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), HeaderError::invalid_payload_size);
}

TEST(RegistrationRequestHeader, RefusesAPayloadOverOneKibibyte) {
  const auto result =
      RegistrationRequestCodec::encode_header(RegistrationTarget::ps4_8, 1025);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), HeaderError::invalid_payload_size);
}

TEST(RegistrationRequestHeader, DropsTheZoneOfALinkLocalHost) {
  auto result = RegistrationRequestCodec::encode_header(
      RegistrationTarget::ps5_1, 480, "fe80::1c2b:3aff:fe4d:5e6f%en0");
  ASSERT_TRUE(result);
  EXPECT_NE(result.value().find("HOST: fe80::1c2b:3aff:fe4d:5e6f\r\n"),
            std::string::npos);
  result = RegistrationRequestCodec::encode_header(
      RegistrationTarget::ps5_1, 480,
      "fe80:0000:0000:0000:1c2b:3aff:fe4d:5e6f%bridge100");
  ASSERT_TRUE(result);
  EXPECT_NE(
      result.value().find("HOST: fe80:0000:0000:0000:1c2b:3aff:fe4d:5e6f\r\n"),
      std::string::npos);
}

TEST(RegistrationRequestHeader, RefusesAMalformedZonedHost) {
  for (std::string_view bad : {"%en0", "fe80::1%en0\r\nX: y", "fe80::1%%"}) {
    const auto rejected = RegistrationRequestCodec::encode_header(
        RegistrationTarget::ps5_1, 16, bad);
    ASSERT_FALSE(rejected) << bad;
    EXPECT_EQ(rejected.error(), HeaderError::invalid_host) << bad;
  }
}

TEST(RegistrationRequestHeader, NeedsAPayloadLargerThanTheReferenceHeader) {
  const std::string head =
      "POST /sie/ps5/rp/sess/rgst HTTP/1.1\r\n HTTP/1.1\r\nHOST: ::1\r\n"
      "User-Agent: remoteplay Windows\r\nConnection: close\r\nContent-Length: ";
  const std::size_t version = std::string_view{"RP-Version: 1.0\r\n"}.size();
  bool saw_reject = false;
  bool saw_accept = false;
  for (std::size_t payload = 1; payload <= 400; ++payload) {
    const std::size_t current =
        head.size() + std::to_string(payload).size() + 2;
    const bool reference_ok =
        current < payload && current + version + 3 <= payload;
    const auto result = RegistrationRequestCodec::encode_header(
        RegistrationTarget::ps5_1, payload, "::1");
    EXPECT_EQ(result.has_value(), reference_ok) << "payload " << payload;
    if (!result) {
      EXPECT_EQ(result.error(), HeaderError::invalid_payload_size)
          << "payload " << payload;
    }
    saw_reject = saw_reject || !reference_ok;
    saw_accept = saw_accept || reference_ok;
  }
  EXPECT_TRUE(saw_reject);
  EXPECT_TRUE(saw_accept);
}

}  // namespace
}  // namespace remote_play_test
