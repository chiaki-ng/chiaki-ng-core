#include "src/core/play/adapters/links/cloud/cloud_messaging.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string_view>
#include <vector>

namespace play_test {
namespace {

namespace play = libchiaking::play;
using play::CloudNotice;
using play::CloudNoticeKind;
using play::RecordingBanner;
using play::TrophyGrade;

class CloudNotices : public ::testing::Test {
 protected:
  static auto bytes(std::string_view text) -> std::vector<std::byte> {
    std::vector<std::byte> body;
    for (const auto character : text)
      body.push_back(static_cast<std::byte>(character));
    body.push_back(std::byte{0});
    return body;
  }
};

TEST_F(CloudNotices, NameASavedScreenshot) {
  const auto saved = play::CloudMessaging::notice(bytes(
      R"({"userId":268435456,"userIdCoupon":0,"isLogged":false,"payload":{"useCaseId":"NUC9","priority":1,"isImmediate":false,"toastOverwriteType":"No","viewTemplateType":"IconOnlyToast","channelType":"ServiceFeedback","viewData":{"icon":{"type":"Predefined","parameters":{"icon":"photo_storing_done"}}},"createdDateTime":"2026-10-18T09:19:14.442Z"}})"));
  ASSERT_TRUE(saved);
  EXPECT_EQ(saved->kind, CloudNoticeKind::screenshot_saved);
  EXPECT_TRUE(saved->title.empty());
}

TEST_F(CloudNotices, NameASavedRecording) {
  const auto clip = play::CloudMessaging::notice(
      bytes(R"({"payload":{"useCaseId":"NUC7"}})"));
  ASSERT_TRUE(clip);
  EXPECT_EQ(clip->kind, CloudNoticeKind::recording_saved);
}

TEST_F(CloudNotices, NameAnEarnedTrophyWithItsGradeAndWords) {
  const auto trophy = play::CloudMessaging::notice(bytes(
      R"({"payload":{"rawData":{"useCaseId":"NUC55","viewData":{"message":{"body":"Hello"},"subMessage":{"body":"Say hi"}},"telemetryData":{"parameters":{"trophyGrade":"Gold","trophyId":"3"}}}}})"));
  ASSERT_TRUE(trophy);
  EXPECT_EQ(*trophy, (CloudNotice{CloudNoticeKind::trophy_earned,
                                  TrophyGrade::gold, "Hello", "Say hi"}));
  const auto bare = play::CloudMessaging::notice(
      bytes(R"({"payload":{"useCaseId":"NUC55"}})"));
  ASSERT_TRUE(bare);
  EXPECT_EQ(bare->kind, CloudNoticeKind::trophy_earned);
  EXPECT_TRUE(bare->title.empty());
}

TEST_F(CloudNotices, NameTheTimeLeftOnATrial) {
  const auto trial = play::CloudMessaging::notice(bytes(
      R"({"userId":254,"isLogged":false,"payload":{"viewTemplateType":"InteractiveToastTemplateB","useCaseId":"NUC559","associatedTitle":{"npTitleId":"PPSA17005_00"},"channelType":"ServiceFeedback","toastOverwriteType":"No","isImmediate":false,"priority":1,"viewData":{"icon":{"type":"AssociatedTitle"},"message":{"body":"You have 5 minutes remaining on your game trial."},"subMessage":{"body":"%titleName%","placeHolders":{"titleName":{"objectType":"Title","npTitleId":"PPSA17005_00"}}},"actions":[{"actionName":"View Game","actionType":"DeepLink","defaultFocus":true,"parameters":{"actionUrl":"psgamehub:main?titleId=PPSA17005"}}]},"createdDateTime":"2026-10-21T02:34:30.713Z"}})"));
  ASSERT_TRUE(trial);
  EXPECT_EQ(
      *trial,
      (CloudNotice{CloudNoticeKind::trial_time_left, TrophyGrade::bronze,
                   "You have 5 minutes remaining on your game trial.", ""}));
  const auto unworded = play::CloudMessaging::notice(
      bytes(R"({"payload":{"useCaseId":"NUC559"}})"));
  ASSERT_TRUE(unworded);
  EXPECT_EQ(unworded->kind, CloudNoticeKind::trial_time_left);
  EXPECT_TRUE(unworded->title.empty());
}

TEST_F(CloudNotices, IgnoreUnknownUseCasesAndBrokenJson) {
  EXPECT_FALSE(play::CloudMessaging::notice(
      bytes(R"({"payload":{"useCaseId":"NUC75"}})")));
  EXPECT_FALSE(play::CloudMessaging::notice(bytes("not json")));
}

TEST_F(CloudNotices, ShowTheRecordingBannerFromBeginUntilCancel) {
  const auto begun = play::CloudMessaging::recording_banner(bytes(
      R"({"type":"ShareRecording","data":{"itemType":"ShareRecording","template":"PersistentToast","priority":"Normal","eventType":"begin","announceOnce":true,"platformViews":{"accessibility":{"viewData":{"text":"<say-as interpret-as=\"time\" format=\"ms\">00:10</say-as>"}}},"viewData":{"icons":["control_rec"],"text":"00:10","minimal":true}}})"));
  ASSERT_TRUE(begun);
  EXPECT_EQ(*begun, (RecordingBanner{true, "00:10"}));
  const auto ended = play::CloudMessaging::recording_banner(bytes(
      R"({"type":"ShareRecording","data":{"itemType":"ShareRecording","template":"PersistentToast","eventType":"cancel"}})"));
  ASSERT_TRUE(ended);
  EXPECT_EQ(*ended, RecordingBanner{});
}

TEST_F(CloudNotices, IgnoreBannersOtherThanTheRecording) {
  EXPECT_FALSE(play::CloudMessaging::recording_banner(bytes(
      R"({"type":"RemotePlay","data":{"itemType":"RemotePlay","eventType":"cancel"}})")));
}

}  // namespace
}  // namespace play_test
