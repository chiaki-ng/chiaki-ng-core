#include "tests/unit/core/consoles/adapters/titles/title_start_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace consoles_test {
namespace {

using libchiaking::consoles::InstalledTitle;
using libchiaking::consoles::StartFailure;
using libchiaking::consoles::StartState;
using libchiaking::users::Unanswered;
using Reply = StartHarness::Reply;
using State = StartState;
using Why = StartFailure::Reason;

TEST(TitleStartRequest, PostsTheExactCommandAndPolls) {
  for (const std::string_view title :
       {std::string_view{"PPSA01234_00"}, std::string_view{" PPSA01234 "}}) {
    StartHarness harness;
    harness.title_id = title;
    harness.answers_post(StartHarness::accepted());
    harness.answers_polls({StartHarness::status("completed", "command")});
    const auto result = harness.run();
    ASSERT_TRUE(result) << title;
    EXPECT_EQ(result.value(), State::completed) << title;
    ASSERT_FALSE(harness.http.seen.empty()) << title;
    const auto& post = harness.http.seen.front();
    EXPECT_EQ(post.method, "POST") << title;
    EXPECT_EQ(post.url, StartHarness::commands_url) << title;
    EXPECT_EQ(post.body,
              "{\"commandDetail\":{\"platform\":\"PS5\",\"duid\":"
              "\"F1E2D3C4B5A6\","
              "\"commandType\":\"launchTitle\",\"parameters\":{\"titleIds\":["
              "\"PPSA01234\"]}}}")
        << title;
    EXPECT_EQ(post.header("Authorization"),
              "Bearer " + std::string{StartHarness::token})
        << title;
    EXPECT_EQ(post.header("Content-Type"), "application/json") << title;
    EXPECT_EQ(post.header("Accept"), "application/json") << title;
    const auto& poll = harness.http.seen.back();
    EXPECT_EQ(poll.method, "GET") << title;
    EXPECT_EQ(poll.url, StartHarness::poll_url()) << title;
    EXPECT_TRUE(poll.body.empty()) << title;
    EXPECT_EQ(poll.header("Authorization"),
              "Bearer " + std::string{StartHarness::token})
        << title;
    EXPECT_EQ(harness.progress,
              (StartHarness::States{State::pending, State::completed}))
        << title;
  }
}

TEST(TitleStartRequest, EscapesTheBodyAndKeepsTheDuidVerbatim) {
  StartHarness harness;
  harness.duid = " a\"b ";
  harness.title_id = "CUSA1";
  harness.answers_post({202, "{\"commandId\":\" a b/c \"}"});
  harness.http.on("GET",
                  std::string{StartHarness::commands_url} +
                      "?platform=PS5&commandIds=a%20b%2Fc&includeFields="
                      "actions,commandDetail&commandStatus=executing,completed",
                  {StartHarness::status("completed")});
  const auto result = harness.run();
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value(), State::completed);
  EXPECT_TRUE(harness.http.unmatched.empty());
  ASSERT_FALSE(harness.http.seen.empty());
  EXPECT_EQ(harness.http.seen.front().body,
            "{\"commandDetail\":{\"platform\":\"PS5\",\"duid\":\" a\\\"b \","
            "\"commandType\":\"launchTitle\",\"parameters\":{\"titleIds\":["
            "\"CUSA1\"]}}"
            "}");
}

TEST(TitleStartRequest, ClassifiesEveryFailedPost) {
  struct Case {
    Reply reply;
    Why why;
    Unanswered::Reason unanswered;
  };
  using PS = Unanswered::Reason;
  const std::vector<Case> cases{
      {{400,
        "{\"error\":{\"message\":\"Bad Request (body: "
        "platform)\",\"code\":2330625}}"},
       Why::refused,
       PS::unavailable},
      {{404, ""}, Why::refused, PS::unavailable},
      {{401, ""}, Why::unanswered, PS::store_access_lapsed},
      {{403, ""}, Why::unanswered, PS::store_access_lapsed},
      {{500, "boom"}, Why::unanswered, PS::unavailable},
      {{503, ""}, Why::unanswered, PS::unavailable},
      {{302, ""}, Why::unanswered, PS::unavailable},
      {{202, "{}"}, Why::unanswered, PS::malformed},
      {{200, "{\"commandId\":\"  \"}"}, Why::unanswered, PS::malformed},
      {{202, "nope"}, Why::unanswered, PS::malformed},
      {Reply::transport(), Why::unanswered, PS::unavailable},
  };
  for (std::size_t index = 0; index < cases.size(); ++index) {
    const auto& entry = cases[index];
    StartHarness harness;
    harness.account.renews_store_access(StartHarness::renewed_token);
    harness.answers_post(entry.reply);
    const auto result = harness.run();
    ASSERT_FALSE(result) << index;
    EXPECT_EQ(result.error().reason, entry.why) << index;
    ASSERT_TRUE(result.error().unanswered) << index;
    EXPECT_EQ(result.error().unanswered->reason, entry.unanswered) << index;
    EXPECT_EQ(result.error().reached, State::pending) << index;
    EXPECT_EQ(harness.polls(), 0) << index;
    EXPECT_TRUE(harness.progress.empty()) << index;
  }
}

TEST(TitleStartRequest, KeepsTheStatusOfARefusal) {
  StartHarness harness;
  harness.answers_post({400,
                        "{\"error\":{\"message\":\"Bad Request (body: "
                        "platform)\",\"code\":2330625}}"});
  const auto result = harness.run();
  ASSERT_FALSE(result);
  ASSERT_TRUE(result.error().unanswered);
  EXPECT_EQ(result.error().unanswered->http_status, 400);
}

TEST(TitleStartRequest, NeverLeaksTheTokenFromARefusal) {
  StartHarness harness;
  harness.answers_post(
      {400, "{\"echo\":\"Bearer " + std::string{StartHarness::token} +
                "\",\"again\":\"" + std::string{StartHarness::token} + "\"}"});
  const auto result = harness.run();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().reason, Why::refused);
  ASSERT_TRUE(result.error().unanswered);
  EXPECT_EQ(result.error().unanswered->http_status, 400);
}

TEST(TitleStartRequest, RefusesWithALongBody) {
  StartHarness harness;
  harness.answers_post({400, std::string(500, 'x')});
  const auto truncated = harness.run();
  ASSERT_FALSE(truncated);
  EXPECT_EQ(truncated.error().reason, Why::refused);
}

TEST(TitleStartRequest, RefusesWithControlCharacters) {
  StartHarness harness;
  harness.answers_post({400,
                        "a\nb\x01"
                        "c"});
  const auto cleaned = harness.run();
  ASSERT_FALSE(cleaned);
  EXPECT_EQ(cleaned.error().reason, Why::refused);
}

TEST(TitleStartRequest, AnswersAHomeUserSignedOutWithoutARequest) {
  StartHarness harness;
  harness.user = harness.account.home_user(40);
  const auto result = harness.run();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().reason, Why::unanswered);
  ASSERT_TRUE(result.error().unanswered);
  EXPECT_EQ(result.error().unanswered->reason, Unanswered::Reason::signed_out);
  EXPECT_EQ(result.error().reached, State::pending);
  EXPECT_TRUE(harness.progress.empty());
  EXPECT_TRUE(harness.http.seen.empty());
}

TEST(TitleStartRequest, RefusesABlankDuidWithoutARequest) {
  StartHarness harness;
  harness.duid = " ";
  const auto result = harness.run();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().reason, Why::not_on_account);
  EXPECT_EQ(result.error().reached, State::pending);
  EXPECT_TRUE(harness.progress.empty());
  EXPECT_TRUE(harness.http.seen.empty());
}

TEST(TitleStartRequest, NeedsAUsableTitleId) {
  EXPECT_FALSE(StartHarness::installed("_00"));
  EXPECT_FALSE(StartHarness::installed("  "));
  EXPECT_FALSE(StartHarness::installed(""));
  EXPECT_FALSE(
      StartHarness::installed(std::string(InstalledTitle::id_limit + 1, 'P')));
  EXPECT_FALSE(StartHarness::installed(std::string{"PPSA\00001_00", 10}));
}

}  // namespace
}  // namespace consoles_test
