#include "src/core/psn/rest/session_manager/session_manager.hpp"
#include "src/core/utils/json/json.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/psn/rest/rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace psn_test {
namespace {

using libchiaking::psn::rest::Cause;
using libchiaking::psn::rest::CreatedSession;
namespace bad_reply = libchiaking::psn::rest::bad_reply;
namespace bad_request = libchiaking::psn::rest::bad_request;
using libchiaking::psn::rest::SessionId;
using libchiaking::psn::rest::SessionManager;
using HttpError = libchiaking::platform::HttpError;
using Reply = RestRig::Reply;

class RemotePlaySessionCreation : public ::testing::Test {
 protected:
  static constexpr std::string_view sessions_url =
      "https://web.np.playstation.com/api/sessionManager/v1/remotePlaySessions";
  static constexpr std::string_view created_reply =
      R"({"remotePlaySessions":[{"sessionId":"12345678-90ab-4cde-8f01-23456789abcd","members":[{"accountId":"123456789"}]}]})";
  static constexpr std::array<std::byte, 16> zero{};
  static constexpr std::array<std::byte, 16> mixed{
      std::byte{0x01}, std::byte{0x23}, std::byte{0x45}, std::byte{0x67},
      std::byte{0x89}, std::byte{0xab}, std::byte{0xff}, std::byte{0xff},
      std::byte{0xff}, std::byte{0xff}, std::byte{0xde}, std::byte{0xad},
      std::byte{0xbe}, std::byte{0xef}, std::byte{0x42}, std::byte{0x24}};

  static auto created_from(std::string_view body) -> libchiaking::Expected<
      CreatedSession, libchiaking::psn::rest::SessionManager::CreateError> {
    RestRig rig;
    rig.fake.on("POST", std::string{sessions_url},
                {Reply{200, std::string{body}}});
    SessionManager manager{rig.http, RestRig::bearer()};
    return manager.create_remote_play_session(zero);
  }

  static auto create_response(std::string_view id, std::string_view account)
      -> std::string {
    return std::string{R"({"remotePlaySessions":[{"sessionId":)"} +
           std::string{id} + R"(,"members":[{"accountId":)" +
           std::string{account} + R"(}]}]})";
  }

  template <class Reason>
  static auto create_error(std::string_view body) -> bool {
    const auto created = created_from(body);
    return !created && created.error().template is<Reason>();
  }

  static auto valid_session(std::string_view account) -> std::string {
    return create_response(R"("12345678-90ab-4cde-8f01-23456789abcd")",
                           account);
  }

  void creates_with(const std::array<std::byte, 16>& random,
                    std::string_view uuid) {
    rig.fake.on("POST", std::string{sessions_url},
                {Reply{201, std::string{created_reply}}});
    SessionManager manager{rig.http, RestRig::bearer()};
    const auto created = manager.create_remote_play_session(random);
    ASSERT_TRUE(created);
    EXPECT_EQ(created.value().push_context_id, uuid);
    EXPECT_EQ(created.value().session_id,
              "12345678-90ab-4cde-8f01-23456789abcd");
    EXPECT_EQ(created.value().account_id, 123456789);
    ASSERT_EQ(rig.fake.seen.size(), 1U);
    const auto& seen = rig.fake.seen.front();
    EXPECT_EQ(
        seen.body,
        std::string{
            R"({"remotePlaySessions":[{"members":[{"accountId":"me","deviceUniqueId":"me","platform":"me","pushContexts":[{"pushContextId":")"} +
            std::string{uuid} + R"("}]}]}]})");
    EXPECT_EQ(RestRig::headers_of(seen),
              (std::vector<std::string>{
                  "Authorization: Bearer SYNTHETIC_TEST_TOKEN",
                  "Content-Type: application/json; charset=utf-8"}));
    EXPECT_EQ(seen.total_timeout, std::chrono::seconds{30});
    EXPECT_EQ(seen.connect_timeout, std::chrono::seconds{10});
  }

  RestRig rig;
};

TEST_F(RemotePlaySessionCreation, SendsAZeroPushContextExactly) {
  creates_with(zero, "00000000-0000-4000-8000-000000000000");
}

TEST_F(RemotePlaySessionCreation, SendsAMixedPushContextExactly) {
  creates_with(mixed, "01234567-89ab-4fff-bfff-deadbeef4224");
}

TEST_F(RemotePlaySessionCreation, ReadsANumericAccountId) {
  const auto number = created_from(create_response(
      R"("12345678-90ab-4cde-8f01-23456789abcd")", "123456789012345678"));
  ASSERT_TRUE(number);
  EXPECT_EQ(number.value().session_id, "12345678-90ab-4cde-8f01-23456789abcd");
  EXPECT_EQ(number.value().account_id, 123456789012345678LL);
}

TEST_F(RemotePlaySessionCreation, ReadsATextAccountIdBeyondDoublePrecision) {
  const auto text = created_from(valid_session(R"("9007199254740993")"));
  ASSERT_TRUE(text);
  EXPECT_EQ(text.value().account_id, 9007199254740993LL);
}

TEST_F(RemotePlaySessionCreation, ReadsNegativeAndZeroPaddedAccountIds) {
  const auto negative = created_from(valid_session("-42"));
  ASSERT_TRUE(negative);
  EXPECT_EQ(negative.value().account_id, -42);
  const auto quoted = created_from(valid_session(R"("-9223372036854775808")"));
  ASSERT_TRUE(quoted);
  EXPECT_EQ(quoted.value().account_id, -9223372036854775807LL - 1);
  const auto padded = created_from(valid_session("0042"));
  ASSERT_TRUE(padded);
  EXPECT_EQ(padded.value().account_id, 42);
}

TEST_F(RemotePlaySessionCreation, ReadsAnAccountIdOfAnotherKindAsUnusable) {
  for (const std::string_view other : {"true", "null", "{}", "[]"})
    EXPECT_TRUE(create_error<bad_reply::InvalidSchema>(valid_session(other)))
        << other;
}

TEST_F(RemotePlaySessionCreation, ReadsReorderedAndIgnoredFields) {
  const auto reordered = created_from(
      R"({"ignored":{"note":"😀","items":[null,true,1.5e3]},"remotePlaySessions":[{"members":[{"accountId":42,"nickname":"hi\nthere"}],"sessionId":"12345678-90ab-4cde-8f01-23456789abcd"}]})");
  ASSERT_TRUE(reordered);
  EXPECT_EQ(reordered.value().account_id, 42);
}

TEST_F(RemotePlaySessionCreation, KeepsLooselyShapedSessionIds) {
  for (const std::string_view id : {"12345678-90AB-4CDE-8F01-23456789ABCD",
                                    "12345678-90ab-4cde-8f01-23456789abcg",
                                    "123456789-0ab-4cde-8f01-23456789abcd"}) {
    const auto decoded =
        created_from(create_response("\"" + std::string(id) + "\"", "1"));
    ASSERT_TRUE(decoded) << id;
    EXPECT_EQ(decoded.value().session_id, id);
    EXPECT_TRUE(SessionId::parse(id)) << id;
  }
}

TEST_F(RemotePlaySessionCreation, RejectsUnreadableReplies) {
  EXPECT_TRUE(create_error<bad_reply::InvalidJson>("{"));
  EXPECT_TRUE(create_error<bad_reply::InvalidSchema>("{}x"));
  EXPECT_TRUE(
      create_error<bad_reply::InvalidSchema>(R"({"remotePlaySessions":[]})"));
  EXPECT_TRUE(
      create_error<bad_reply::InvalidSchema>(create_response("12", "1")));
  EXPECT_TRUE(create_error<bad_reply::InvalidSchema>(
      R"({"remotePlaySessions":[],"remotePlaySessions":[]})"));
  EXPECT_TRUE(create_error<bad_reply::InvalidSchema>(R"({"note":"\uD800"})"));
}

TEST_F(RemotePlaySessionCreation, RejectsMalformedSessionIds) {
  EXPECT_TRUE(create_error<bad_reply::InvalidSessionId>(
      create_response(R"("12345678-90ab-4cde-8f01-23456789abc")", "1")));
  EXPECT_TRUE(create_error<bad_reply::InvalidSessionId>(
      create_response(R"("12345678-90ab-4cde-8f01-23456789abcde")", "1")));
  EXPECT_TRUE(create_error<bad_reply::InvalidSessionId>(
      create_response(R"("12345678/90ab-4cde-8f01-23456789abcd")", "1")));
  EXPECT_TRUE(create_error<bad_reply::InvalidSessionId>(
      create_response(R"("12345678-90ab-4cde-8f01-23456789abc\"")", "1")));
}

TEST_F(RemotePlaySessionCreation, RejectsMalformedAccountIds) {
  EXPECT_TRUE(create_error<bad_reply::InvalidAccountId>(
      valid_session("9223372036854775808")));
  EXPECT_TRUE(create_error<bad_reply::InvalidAccountId>(valid_session("1.5")));
  EXPECT_TRUE(create_error<bad_reply::InvalidAccountId>(
      valid_session(R"("not-a-number")")));
}

TEST_F(RemotePlaySessionCreation, RejectsOversizedAndDeepReplies) {
  EXPECT_TRUE(create_error<bad_reply::TooLarge>(
      std::string(libchiaking::json::max_document_bytes + 1, ' ')));
  std::string deep(66, '[');
  deep.append(66, ']');
  EXPECT_TRUE(create_error<bad_reply::TooDeep>(deep));
}

TEST_F(RemotePlaySessionCreation, TypesARefusalAndATransportFailure) {
  rig.fake.on("POST", std::string{sessions_url},
              {Reply{401, "{}"}, Reply::transport()});
  SessionManager manager{rig.http, RestRig::bearer()};
  const auto refused = manager.create_remote_play_session(zero);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().cause(), Cause::status);
  EXPECT_EQ(refused.error().http_status(), 401);
  EXPECT_TRUE(refused.error().auth_refused());
  const auto failed = manager.create_remote_play_session(zero);
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().cause(), Cause::failure);
  ASSERT_TRUE(failed.error().failure());
  EXPECT_EQ(failed.error().failure()->error(), HttpError::disconnected);
}

TEST_F(RemotePlaySessionCreation, TypesAnOversizedReply) {
  rig.fake.on("POST", std::string{sessions_url},
              {Reply{200, std::string(65, ' ')}});
  auto limited = rig.http.limited(64);
  SessionManager capped{limited, RestRig::bearer()};
  const auto oversized = capped.create_remote_play_session(zero);
  ASSERT_FALSE(oversized);
  ASSERT_TRUE(oversized.error().failure());
  EXPECT_EQ(oversized.error().failure()->error(),
            HttpError::response_too_large);
}

}  // namespace
}  // namespace psn_test
