#include "src/core/psn/rest/session_manager/session_manager.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/psn/rest/rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace psn_test {
namespace {

using libchiaking::psn::rest::SessionId;
using libchiaking::psn::rest::SessionManager;
using libchiaking::psn::rest::SessionView;
using Reply = RestRig::Reply;

TEST(RemotePlaySessionId, AcceptsThirtySixLettersDigitsAndHyphens) {
  EXPECT_TRUE(SessionId::parse("01234567-89ab-4fff-bfff-deadbeef4224"));
  EXPECT_TRUE(SessionId::parse("01234567-89AB-4FFF-BFFF-DEADBEEF4224"));
  EXPECT_TRUE(SessionId::parse("0123456-789ab-4fff-bfff-deadbeef4224"));
}

TEST(RemotePlaySessionId, RejectsAnythingElse) {
  EXPECT_FALSE(SessionId::parse(""));
  EXPECT_FALSE(SessionId::parse("../../../1234/5678/90ab/cdef00000000"));
  EXPECT_FALSE(SessionId::parse("01234567-89ab-4fff-bfff-deadbeef422?"));
  EXPECT_FALSE(SessionId::parse("01234567-89ab-4fff-bfff-deadbeef422"));
  EXPECT_FALSE(SessionId::parse("01234567-89ab-4fff-bfff-deadbeef42%2"));
  EXPECT_FALSE(SessionId::parse("01234567-89ab-cdef-0123-456789abcde\""));
  EXPECT_FALSE(SessionId::parse("not-a-uuid"));
}

TEST(RemotePlaySessionUrls, NameTheSessionToLeaveAndObserve) {
  static constexpr std::string_view sessions_url =
      "https://web.np.playstation.com/api/sessionManager/v1/remotePlaySessions";
  RestRig rig;
  const std::string id = "01234567-89AB-4FFF-BFFF-DEADBEEF4224";
  rig.fake.on("DELETE", std::string{sessions_url} + "/" + id + "/members/me",
              {Reply{204, ""}});
  rig.fake.on("GET", std::string{sessions_url}, {Reply{200, "{}"}});
  rig.fake.on("GET", std::string{sessions_url} + "?view=v1.0",
              {Reply{200, "{}"}});
  SessionManager manager{rig.http, RestRig::bearer()};
  EXPECT_TRUE(manager.leave_session(RestRig::session(id)));
  EXPECT_TRUE(
      manager.observe_sessions(RestRig::session(id), SessionView::status));
  EXPECT_TRUE(
      manager.observe_sessions(RestRig::session(id), SessionView::detailed));
  EXPECT_TRUE(rig.fake.unmatched.empty());
  ASSERT_EQ(rig.fake.seen.size(), 3U);
  EXPECT_EQ(rig.fake.seen[0].method, "DELETE");
  EXPECT_TRUE(rig.fake.seen[0].body.empty());
  EXPECT_EQ(RestRig::headers_of(rig.fake.seen[0]),
            (std::vector<std::string>{
                "Authorization: Bearer SYNTHETIC_TEST_TOKEN",
                "Content-Type: application/json; charset=utf-8"}));
  for (std::size_t index = 1; index < 3; ++index)
    EXPECT_EQ(
        RestRig::headers_of(rig.fake.seen[index]),
        (std::vector<std::string>{"Authorization: Bearer SYNTHETIC_TEST_TOKEN",
                                  "X-PSN-SESSION-MANAGER-SESSION-IDS: " + id}))
        << index;
}

}  // namespace
}  // namespace psn_test
