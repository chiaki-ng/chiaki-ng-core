#include "src/core/psn/rest/session_manager/session_manager.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/psn/rest/rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace psn_test {
namespace {

using libchiaking::psn::rest::Cause;
using libchiaking::psn::rest::ConsoleFamily;
namespace bad_reply = libchiaking::psn::rest::bad_reply;
namespace bad_request = libchiaking::psn::rest::bad_request;
using libchiaking::psn::rest::SessionManager;
using Reply = RestRig::Reply;

class RemotePlaySessionMessage : public ::testing::Test {
 protected:
  RemotePlaySessionMessage() {
    rig.fake.on("POST",
                "https://web.np.playstation.com/api/sessionManager/v1/"
                "remotePlaySessions/" +
                    std::string{RestRig::session_text} + "/sessionMessage",
                {Reply{204, ""}});
    for (std::size_t index = 0; index < sequence_uid.size(); ++index)
      sequence_uid[index] = static_cast<std::byte>(index);
  }

  RestRig rig;
  SessionManager manager{rig.http, RestRig::bearer()};
  std::array<std::byte, 32> zero_uid{};
  std::array<std::byte, 32> sequence_uid{};
};

TEST_F(RemotePlaySessionMessage, WrapsAResultInTheEnvelopeExactly) {
  EXPECT_TRUE(manager.send_session_message(
      RestRig::session(RestRig::session_text),
      R"({"action":"RESULT","reqId":7,"error":0,"connRequest":{}})",
      {12, zero_uid, ConsoleFamily::ps5}));
  ASSERT_EQ(rig.fake.seen.size(), 1U);
  EXPECT_EQ(
      rig.fake.seen[0].body,
      "{\"channel\":\"remote_play:1\",\"payload\":\"ver=1.0, type=text, body="
      "{\\\"action\\\":\\\"RESULT\\\",\\\"reqId\\\":7,\\\"error\\\":0,"
      "\\\"connRequest\\\":{}}\",\"to\":[{\"accountId\":\"12\","
      "\"deviceUniqueId\":"
      "\"0000000000000000000000000000000000000000000000000000000000000000\","
      "\"platform\":\"PS5\"}]}");
  EXPECT_EQ(RestRig::headers_of(rig.fake.seen[0]),
            (std::vector<std::string>{
                "Authorization: Bearer SYNTHETIC_TEST_TOKEN",
                "Content-Type: application/json; charset=utf-8"}));
}

TEST_F(RemotePlaySessionMessage, EscapesATerminateForAPs4Exactly) {
  EXPECT_TRUE(manager.send_session_message(
      RestRig::session(RestRig::session_text),
      R"({"action":"TERMINATE","reqId":65535,"error":1,"connRequest":{"addr":"a\"b\\c\n\u0001"}})",
      {-42, sequence_uid, ConsoleFamily::ps4}));
  ASSERT_EQ(rig.fake.seen.size(), 1U);
  EXPECT_EQ(
      rig.fake.seen[0].body,
      "{\"channel\":\"remote_play:1\",\"payload\":\"ver=1.0, type=text, body="
      "{\\\"action\\\":\\\"TERMINATE\\\",\\\"reqId\\\":65535,\\\"error\\\":1,"
      "\\\"connRequest\\\":{\\\"addr\\\":"
      "\\\"a\\\\\\\"b\\\\\\\\c\\\\n\\\\u0001\\\"}}\","
      "\"to\":[{\"accountId\":\"-42\","
      "\"deviceUniqueId\":"
      "\"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\","
      "\"platform\":\"PS4\"}]}");
}

TEST_F(RemotePlaySessionMessage, RefusesAnOversizedMessageUnsent) {
  const auto too_large = manager.send_session_message(
      RestRig::session(RestRig::session_text), std::string(33U * 1024U, 'x'),
      {12, zero_uid, ConsoleFamily::ps5});
  ASSERT_FALSE(too_large);
  EXPECT_TRUE(too_large.error().is<bad_request::InvalidMessage>());
  EXPECT_TRUE(rig.fake.seen.empty());
}

}  // namespace
}  // namespace psn_test
