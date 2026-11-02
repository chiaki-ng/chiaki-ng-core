#include "tests/unit/core/consoles/adapters/titles/console_library_rig.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>

namespace consoles_test {
namespace {

using libchiaking::consoles::Generation;
using libchiaking::consoles::StartFailure;
using libchiaking::consoles::StartState;
using libchiaking::consoles::TitlesUnavailable;
using libchiaking::users::Unanswered;
using Asked = LibraryRig::Asked;
using Reason = Unanswered::Reason;
using Reply = LibraryRig::Reply;

class ConsoleLibraryAnswers : public ::testing::Test {
 protected:
  LibraryRig rig;
};

TEST_F(ConsoleLibraryAnswers, AreNeverAskedWhenAlreadyStopped) {
  auto stop = std::make_shared<test_support::StopState>();
  stop->stopped = true;
  const auto result = rig.run(stop);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().reason, Reason::cancelled);
  EXPECT_TRUE(rig.http.seen.empty());
}

TEST_F(ConsoleLibraryAnswers, AreCancelledWhenStoppedMidway) {
  auto stop = std::make_shared<test_support::StopState>();
  Reply reply{200, LibraryRig::clients_page(LibraryRig::ten_clients(0))};
  reply.during = [stop] { stop->stopped = true; };
  rig.clients(0, {reply});
  const auto result = rig.run(stop);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().reason, Reason::cancelled);
  EXPECT_EQ(rig.http.seen.size(), 1U);
}

TEST_F(ConsoleLibraryAnswers, AreSignedOutForAHomeUserWithoutARequest) {
  const auto home = rig.account.home_user(40);
  auto library = rig.library();
  const auto asked = library.asked(*home, {});
  ASSERT_FALSE(asked);
  EXPECT_EQ(asked.error().reason, Reason::signed_out);
  const auto kept = library.held_or_asked(*home, {});
  ASSERT_FALSE(kept);
  EXPECT_EQ(kept.error().reason, Reason::signed_out);
  const auto recent = rig.account.consoles.recently_played(*home, {});
  ASSERT_FALSE(recent);
  EXPECT_EQ(recent.error().reason, Reason::signed_out);
  EXPECT_TRUE(rig.http.seen.empty());
}

TEST_F(ConsoleLibraryAnswers, KeepAHomeUsersConsoleTitlesSignedOut) {
  const auto home = rig.account.home_user(40);
  const auto console = rig.account.paired(*home, LibraryRig::den, "Den");
  const auto titles = console->titles(*home, {});
  ASSERT_FALSE(titles);
  EXPECT_EQ(titles.error().reason, TitlesUnavailable::Reason::unanswered);
  ASSERT_TRUE(titles.error().unanswered);
  EXPECT_EQ(titles.error().unanswered->reason, Reason::signed_out);
  const auto wanted =
      LibraryRig::title("PPSA00001_00", "PPSA00001", "One", "https://img/a.png",
                        Generation::ps5, std::nullopt, false);
  const auto started = console->start(wanted, *home, nullptr, {});
  ASSERT_FALSE(started);
  EXPECT_EQ(started.error().reason, StartFailure::Reason::unanswered);
  ASSERT_TRUE(started.error().unanswered);
  EXPECT_EQ(started.error().unanswered->reason, Reason::signed_out);
  EXPECT_EQ(started.error().reached, StartState::pending);
  EXPECT_TRUE(rig.http.seen.empty());
}

TEST_F(ConsoleLibraryAnswers, AreKeptPerUserAndNeverCross) {
  auto second = rig.account.user(8, LibraryRig::other_token);
  const auto first =
      rig.account.signed_in.users.find(libchiaking::AccountId{7});
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_NE(first, second);

  Reply mine{
      200,
      LibraryRig::clients_page({LibraryRig::client(
          "mine", "Mine", {LibraryRig::installed("PPSA00001_00", "PS5")})})};
  Reply theirs{200, LibraryRig::clients_page({LibraryRig::client(
                        "theirs", "Theirs",
                        {LibraryRig::installed("PPSA00002_00", "PS5")})})};
  rig.clients(0, {mine, theirs});
  rig.games(
      0,
      {{200, LibraryRig::gamelist({LibraryRig::game("PPSA00001_00", "One"),
                                   LibraryRig::game("PPSA00002_00", "Two")})}});

  auto library = rig.library();
  const auto one = library.asked(*first, {});
  ASSERT_TRUE(one);
  EXPECT_TRUE(LibraryRig::shelf(one, "mine"));
  EXPECT_FALSE(LibraryRig::shelf(one, "theirs"));
  ASSERT_EQ(rig.http.seen.size(), 2U);
  for (const auto& seen : rig.http.seen)
    EXPECT_EQ(seen.header("Authorization"),
              "Bearer " + std::string{LibraryRig::token})
        << seen.url;

  const auto two = library.held_or_asked(*second, {});
  ASSERT_TRUE(two);
  EXPECT_TRUE(LibraryRig::shelf(two, "theirs"));
  EXPECT_FALSE(LibraryRig::shelf(two, "mine"));
  ASSERT_EQ(rig.http.seen.size(), 4U);
  for (std::size_t index = 2; index < rig.http.seen.size(); ++index)
    EXPECT_EQ(rig.http.seen[index].header("Authorization"),
              "Bearer " + std::string{LibraryRig::other_token})
        << index;

  const auto kept_one = library.held_or_asked(*first, {});
  const auto kept_two = library.held_or_asked(*second, {});
  ASSERT_TRUE(kept_one);
  EXPECT_EQ(kept_one.value(), one.value());
  ASSERT_TRUE(kept_two);
  EXPECT_EQ(kept_two.value(), two.value());
  EXPECT_EQ(rig.http.seen.size(), 4U);

  const auto home = rig.account.home_user(9);
  const auto nobody = library.held_or_asked(*home, {});
  ASSERT_FALSE(nobody);
  EXPECT_EQ(nobody.error().reason, Reason::signed_out);
  EXPECT_EQ(rig.http.seen.size(), 4U);
}

TEST_F(ConsoleLibraryAnswers, ShareOneRequestForTheSameQuestionInFlight) {
  const auto watched = std::make_shared<test_support::WatchedStop>();
  const auto started = std::make_shared<std::atomic<bool>>(false);
  Reply slow{
      200,
      LibraryRig::clients_page({LibraryRig::client(
          "mine", "Mine", {LibraryRig::installed("PPSA00001_00", "PS5")})})};
  slow.during = [started, watched] {
    *started = true;
    while (watched->asked == 0) LibraryRig::pause(std::chrono::milliseconds{1});
  };
  rig.clients(0, {slow});
  rig.games(
      0,
      {{200, LibraryRig::gamelist({LibraryRig::game("PPSA00001_00", "One")})}});

  auto library = rig.library();
  std::optional<Asked> first;
  std::optional<Asked> second;
  auto leader = libchiaking::platform::Thread::start(
                    test_support::process_threading(), "test-leader",
                    libchiaking::platform::ThreadRole::unspecified,
                    [&] { first.emplace(library.asked(*rig.user, {})); })
                    .value();
  while (!*started) LibraryRig::pause(std::chrono::milliseconds{1});
  auto follower =
      libchiaking::platform::Thread::start(
          test_support::process_threading(), "test-follower",
          libchiaking::platform::ThreadRole::unspecified,
          [&] {
            second.emplace(library.asked(
                *rig.user, libchiaking::platform::StopToken{watched}));
          })
          .value();
  static_cast<void>(leader.join());
  static_cast<void>(follower.join());
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  ASSERT_TRUE(*first);
  ASSERT_TRUE(*second);
  EXPECT_EQ(first->value(), second->value());
  EXPECT_EQ(rig.http.count("/clients?"), 1);
  EXPECT_EQ(rig.http.count("/gamelist/"), 1);
}

}  // namespace
}  // namespace consoles_test
