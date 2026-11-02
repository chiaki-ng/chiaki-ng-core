#include "src/core/utils/diagnostics/api_name.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/session_client_rig.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>

namespace play_test {
namespace {

namespace platform = libchiaking::platform;
namespace psn_remote_play = libchiaking::psn::remote_play;
using psn_remote_play::SessionView;

class SessionClientStatus : public ::testing::Test {
 protected:
  static inline const libchiaking::psn::auth::BearerToken token{
      std::string_view{"SYNTHETIC_TEST_TOKEN"}};
  static constexpr std::string_view id = "01234567-89ab-cdef-0123-456789abcdef";

  session_client_rig::StatusHttp http;
  session_client_rig::SwitchableRandom random;
  psn_remote_play::SessionClient client{
      http, test_support::LiveProviders::steady(), random};
};

TEST_F(SessionClientStatus, ReadsTheSessionInEitherView) {
  for (const auto view : {SessionView::status, SessionView::detailed}) {
    SCOPED_TRACE(view == SessionView::detailed ? "detailed" : "status");
    EXPECT_TRUE(client.check_session(token, id, view, {}));
    EXPECT_EQ(http.last.method, platform::HttpMethod::get);
    EXPECT_EQ(http.last.label, "psn.session_manager.observe_sessions");
    EXPECT_EQ(libchiaking::diagnostics::ApiName{http.last}.view(),
              "PSN Read Remote Play session");
    EXPECT_EQ(http.last.url.view(),
              std::string{"https://web.np.playstation.com/api/sessionManager/"
                          "v1/remotePlaySessions"} +
                  (view == SessionView::detailed ? "?view=v1.0" : ""));
    EXPECT_EQ(http.last.headers.size(), 2U);
    session_client_rig::Headers::expect(http.last, 0, "Authorization",
                                        "Bearer SYNTHETIC_TEST_TOKEN");
    session_client_rig::Headers::expect(
        http.last, 1, "X-PSN-SESSION-MANAGER-SESSION-IDS", id);
    EXPECT_TRUE(http.last.body.empty());
    EXPECT_EQ(http.last.total_timeout,
              libchiaking::net::http::default_total_timeout);
  }
}

TEST_F(SessionClientStatus, RefusesAnInvalidOrStoppedCheckWithoutSending) {
  EXPECT_FALSE(client.check_session(
      libchiaking::psn::auth::BearerToken{std::string_view{}}, id,
      SessionView::status, {}));
  EXPECT_FALSE(
      client.check_session(token, "invalid\r\n", SessionView::status, {}));
  EXPECT_FALSE(client.check_session(
      token, id, SessionView::status,
      platform::StopToken{std::make_shared<test_support::Stopped>()}));
  EXPECT_EQ(http.calls, 0U);
}

TEST_F(SessionClientStatus, ReportsARefusalStatus) {
  http.status = 503;
  auto failed = client.check_session(token, id, SessionView::status, {});
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().kind,
            psn_remote_play::SessionErrorKind::http_status);
  EXPECT_EQ(failed.error().status, 503);
}

}  // namespace
}  // namespace play_test
