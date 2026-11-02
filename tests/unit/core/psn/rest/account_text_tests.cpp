#include "src/core/psn/rest/session_manager/player_sessions.hpp"
#include "src/core/psn/rest/subscriptions/subscriptions_client.hpp"
#include "src/core/psn/rest/user_notification/user_notifications.hpp"
#include "src/core/psn/rest/user_profile/friends.hpp"
#include "tests/unit/core/psn/rest/rig.hpp"

#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace psn_test {
namespace {

using libchiaking::psn::rest::Friends;
using libchiaking::psn::rest::PlayerSessions;
using libchiaking::psn::rest::SubscriptionsClient;
using libchiaking::psn::rest::UserNotifications;
using Reply = RestRig::Reply;

class AccountText : public ::testing::TestWithParam<std::string_view> {
 protected:
  using Read = std::function<std::vector<std::string>(RestRig&)>;

  static auto answered(const Read& read, const std::string& body)
      -> std::vector<std::string> {
    RestRig probe;
    read(probe);
    if (probe.fake.unmatched.empty()) {
      ADD_FAILURE() << "nothing was asked";
      return {};
    }
    RestRig rig;
    rig.fake.on("GET", probe.fake.unmatched.front(), {Reply{200, body}});
    return read(rig);
  }

  static auto notified(const std::vector<std::string>& ids)
      -> std::vector<std::string> {
    std::string list;
    for (const auto& id : ids)
      list += std::string{list.empty() ? "" : ","} +
              R"({"notificationId":"n","fromUser":{"accountId":)" + id + "}}";
    return answered(
        [](RestRig& rig) {
          std::vector<std::string> found;
          auto listed =
              UserNotifications{rig.http, RestRig::bearer()}.list("en");
          if (listed)
            for (const auto& entry : *listed)
              found.push_back(entry.from_account_id);
          return found;
        },
        R"({"notifications":[)" + list + "]}");
  }

  static auto played(const std::vector<std::string>& ids)
      -> std::vector<std::string> {
    std::string list;
    for (const auto& id : ids)
      list += std::string{list.empty() ? "" : ","} + R"({"accountId":)" + id +
              R"(,"onlineId":"o"})";
    return answered(
        [](RestRig& rig) {
          std::vector<std::string> found;
          const std::vector<std::string> sessions{"s-1"};
          auto read =
              PlayerSessions{rig.http, RestRig::bearer()}.read(sessions, "en");
          if (read)
            for (const auto& session : *read)
              for (const auto& player : session.players)
                found.push_back(player.account_id);
          return found;
        },
        R"({"playerSessions":[{"sessionId":"s-1","member":{"players":[)" +
            list + "]}}]}");
  }

  static auto befriended(const std::vector<std::string>& ids)
      -> std::vector<std::string> {
    std::string list;
    for (const auto& id : ids) list += (list.empty() ? "" : ",") + id;
    return answered(
        [](RestRig& rig) {
          auto page = Friends{rig.http, RestRig::bearer()}.page(0, "en");
          return page ? page->account_ids : std::vector<std::string>{};
        },
        R"({"friends":[)" + list + "]}");
  }

  static auto owned(const std::vector<std::string>& ids)
      -> std::vector<std::string> {
    std::string list;
    for (const auto& id : ids)
      list += std::string{list.empty() ? "" : ","} + R"({"ownerAccountId":)" +
              id + "}";
    return answered(
        [](RestRig& rig) {
          std::vector<std::string> found;
          auto feature =
              SubscriptionsClient{rig.http, RestRig::bearer()}.cloud_game();
          if (feature)
            for (const auto& owner : feature->owners)
              found.push_back(owner.owner_account_id);
          return found;
        },
        R"({"authorization":true,"cloudgameOwnerInfos":[)" + list + "]}");
  }

  static auto read(std::string_view site, const std::vector<std::string>& ids)
      -> std::vector<std::string> {
    if (site == "notifications") return notified(ids);
    if (site == "sessions") return played(ids);
    if (site == "friends") return befriended(ids);
    return owned(ids);
  }
};

TEST_P(AccountText, ReadsANumberExactlyAsDecimalText) {
  EXPECT_EQ(read(GetParam(), {"7", "18446744073709551615", "9007199254740993",
                              "222e0", "0"}),
            (std::vector<std::string>{"7", "18446744073709551615",
                                      "9007199254740993", "222", "0"}));
}

TEST_P(AccountText, DropsANumberThatIsNotAWholeUnsignedValue) {
  const auto found = read(GetParam(), {"-7", "1.5", "18446744073709551616",
                                       "true", "null", "{}", "\"8\""});
  const std::vector<std::string> kept{"8"};
  if (GetParam() == "friends")
    EXPECT_EQ(found, kept);
  else
    EXPECT_EQ(found, (std::vector<std::string>{"", "", "", "", "", "", "8"}));
}

TEST_P(AccountText, KeepsAStringUnvalidated) {
  EXPECT_EQ(read(GetParam(), {"\"007\"", "\"abc\"", "\"-1\""}),
            (std::vector<std::string>{"007", "abc", "-1"}));
}

TEST_P(AccountText, TrimsAStringOnlyWhereItAlwaysHas) {
  const auto found = read(GetParam(), {"\" \\t7\\n \""});
  ASSERT_EQ(found.size(), 1U);
  EXPECT_EQ(found.front(), GetParam() == "owners" ? " \t7\n " : "7");
}

INSTANTIATE_TEST_SUITE_P(EveryReply, AccountText,
                         ::testing::Values("notifications", "sessions",
                                           "friends", "owners"));

}  // namespace
}  // namespace psn_test
