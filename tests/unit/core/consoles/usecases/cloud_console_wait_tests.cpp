#include "tests/support/clock.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/support/waiter.hpp"
#include "tests/unit/core/consoles/usecases/invitations_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace consoles_test {
namespace {

using libchiaking::consoles::InvitationJoinFailure;
using libchiaking::consoles::usecases::Invitations;
using Reason = InvitationJoinFailure::Reason;
using Rig = InvitationsRig;

class CloudConsoleWait : public ::testing::Test {
 protected:
  static constexpr std::string_view home_only =
      "{\"clients\":[{\"duid\":\"AAAA\",\"device\":{\"name\":\"Den\"}}]}";
  static constexpr std::string_view with_cloud =
      "{\"clients\":[{\"duid\":\"AAAA\",\"device\":{\"name\":\"Den\"}},"
      "{\"duid\":\"CCCC\",\"deviceProperties\":[\"CLDK\"]}]}";
  static constexpr int poll_ms = static_cast<int>(
      std::chrono::milliseconds{Invitations::cloud_console_poll}.count());

  CloudConsoleWait() {
    rig.account.http.on(
        "GET", std::string{Rig::sessions_url},
        {{200, Rig::sessions({Rig::session(Rig::session_a,
                                           Rig::player("222", "Friend"))})}});
  }

  void lists_the_cloud_console_after(std::size_t listings) {
    std::vector<Rig::Reply> replies(listings,
                                    Rig::Reply{200, std::string{home_only}});
    replies.emplace_back(200, std::string{with_cloud});
    rig.account.http.on("GET", std::string{Rig::clients_url},
                        std::move(replies));
  }

  void lists_only_the_home_console() {
    rig.account.http.on("GET", std::string{Rig::clients_url},
                        {{200, std::string{home_only}}});
  }

  void completes_the_command() {
    rig.account.http.on(
        "POST", std::string{Rig::commands_url},
        {{202, "{\"commandId\":" + Rig::quoted(Rig::command_id) + "}"}});
    rig.account.http.on(
        "GET", Rig::poll_url(),
        {{200, "{\"commands\":[{\"commandStatus\":\"completed\"}]}"}});
  }

  auto join() -> libchiaking::Expected<void, InvitationJoinFailure> {
    waiter.stop_state = stop.get();
    return Invitations{rig.account.http, clock, &rig.account.log, waiter}
        .join_in_cloud(*rig.user, Rig::session_a, nullptr,
                       libchiaking::platform::StopToken{stop});
  }

  auto listings() const -> int {
    return rig.account.http.requests_to(Rig::clients_url);
  }

  Rig rig;
  test_support::FakeClock clock;
  test_support::FakeWaiter waiter{&clock};
  std::shared_ptr<test_support::StopState> stop{
      std::make_shared<test_support::StopState>()};
};

TEST_F(CloudConsoleWait, JoinsOnceTheCloudConsoleIsListed) {
  lists_the_cloud_console_after(2);
  completes_the_command();
  EXPECT_TRUE(join());
  EXPECT_EQ(listings(), 3);
  EXPECT_EQ(waiter.waits, (std::vector<int>{poll_ms, poll_ms}));
  EXPECT_TRUE(rig.account.log.contains(
      "[INVITES] join waiting for cloud console (attempt 2)"));
  const auto post =
      std::ranges::find(rig.account.http.seen, std::string{"POST"},
                        &test_support::psn::Seen::method);
  ASSERT_NE(post, rig.account.http.seen.end());
  EXPECT_NE(post->body.find("\"duid\":\"CCCC\""), std::string::npos);
}

TEST_F(CloudConsoleWait, GivesUpAtTheDeadlineWithoutSending) {
  lists_only_the_home_console();
  const auto started = clock.now_us;
  const auto joined = join();
  ASSERT_FALSE(joined);
  EXPECT_EQ(joined.error().reason, Reason::no_cloud_console);
  EXPECT_EQ(listings(), Invitations::cloud_console_deadline /
                                Invitations::cloud_console_poll +
                            1);
  EXPECT_EQ(std::chrono::microseconds{static_cast<std::int64_t>(clock.now_us -
                                                                started)},
            Invitations::cloud_console_deadline);
  EXPECT_EQ(rig.account.http.count("/commands"), 0);
}

TEST_F(CloudConsoleWait, EndsWhenStoppedDuringAWait) {
  lists_only_the_home_console();
  waiter.stop_on_wait = 1;
  const auto joined = join();
  ASSERT_FALSE(joined);
  EXPECT_EQ(joined.error().reason, Reason::cancelled);
  EXPECT_EQ(listings(), 1);
  EXPECT_EQ(rig.account.http.count("/commands"), 0);
}

TEST_F(CloudConsoleWait, IsNotTriedAgainWhenPsnDoesNotAnswer) {
  rig.account.http.on("GET", std::string{Rig::clients_url}, {{404, "{}"}});
  const auto joined = join();
  ASSERT_FALSE(joined);
  EXPECT_EQ(joined.error().reason, Reason::unanswered);
  EXPECT_EQ(listings(), 1);
  EXPECT_TRUE(waiter.waits.empty());
}

}  // namespace
}  // namespace consoles_test
