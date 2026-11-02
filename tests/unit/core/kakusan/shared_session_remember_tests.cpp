#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/shared_session_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <optional>

namespace kakusan_test {
namespace {

using libchiaking::kakusan::Recognition;
using libchiaking::kakusan::RememberRefusal;
using libchiaking::kakusan::SavedDevice;
using libchiaking::kakusan::SharedSession;
using manual::GuestId;

class SharedSessionRemembered : public SharedSessionTest {};

TEST_F(SharedSessionRemembered, GuestIsRecognisedAndCanTakeOverANickname) {
  const std::chrono::system_clock::time_point long_ago{
      std::chrono::seconds{1000}};
  const auto guest_key = TestPlatform::identity(0x45).key();
  const auto other_key = TestPlatform::identity(0x77).key();
  ShareRig rig{ShareRig::Where::home,
               std::nullopt,
               {SavedDevice{guest_key, "Pat", long_ago, long_ago},
                SavedDevice{other_key, "Sam", long_ago, long_ago}}};
  ASSERT_NO_FATAL_FAILURE(rig.play());
  ASSERT_NO_FATAL_FAILURE(rig.stream());
  const auto early = rig.share->remember(GuestId{}, "Pat");
  ASSERT_FALSE(early);
  EXPECT_EQ(early.error().kind, RememberRefusal::Kind::unknown_device);
  ASSERT_NO_FATAL_FAILURE(rig.seated(0x45));
  const auto* identified = rig.recorder.find<SharedSession::PlayerIdentified>();
  ASSERT_NE(identified, nullptr);
  EXPECT_EQ(identified->recognition.kind, Recognition::Kind::recognised);
  ASSERT_TRUE(identified->recognition.device);
  EXPECT_EQ(identified->recognition.device->nickname, "Pat");
  const auto before = rig.store.saved();
  const auto seen = std::ranges::find(before, guest_key, &SavedDevice::key);
  ASSERT_NE(seen, before.end());
  EXPECT_GT(seen->last_seen, long_ago);
  EXPECT_EQ(seen->first_verified, long_ago);

  const auto taken = rig.share->remember(rig.guest, "s\xc3\xa1m");
  ASSERT_FALSE(taken);
  EXPECT_EQ(taken.error().kind, RememberRefusal::Kind::nickname_taken);
  ASSERT_TRUE(taken.error().holder);
  EXPECT_EQ(taken.error().holder->key, other_key);
  EXPECT_EQ(rig.store.saved().size(), 2U);
  const auto invalid = rig.share->remember(rig.guest, "\t");
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error().kind, RememberRefusal::Kind::invalid_nickname);
  const auto replaced =
      rig.share->remember(rig.guest, "sam", libchiaking::kakusan::Replace::yes);
  ASSERT_TRUE(replaced);
  EXPECT_EQ(replaced.value().nickname, "sam");
  EXPECT_EQ(replaced.value().key, guest_key);
  EXPECT_EQ(replaced.value().first_verified, long_ago);
  const auto saved = rig.store.saved();
  ASSERT_EQ(saved.size(), 1U);
  EXPECT_EQ(saved.front().key, guest_key);
  EXPECT_EQ(saved.front().nickname, "sam");
  EXPECT_EQ(rig.root.remembered().recognise(other_key).kind,
            Recognition::Kind::unknown);
  EXPECT_EQ(rig.root.remembered().recognise(other_key, "Sam").kind,
            Recognition::Kind::changed_key);
}

TEST_F(SharedSessionRemembered, KeepsTheOldHolderWhenTheTakeOverIsNotSaved) {
  const std::chrono::system_clock::time_point long_ago{
      std::chrono::seconds{1000}};
  const auto guest_key = TestPlatform::identity(0x45).key();
  const auto other_key = TestPlatform::identity(0x77).key();
  ShareRig rig{ShareRig::Where::home,
               std::nullopt,
               {SavedDevice{guest_key, "Pat", long_ago, long_ago},
                SavedDevice{other_key, "Sam", long_ago, long_ago}}};
  ASSERT_NO_FATAL_FAILURE(rig.play());
  ASSERT_NO_FATAL_FAILURE(rig.stream());
  ASSERT_NO_FATAL_FAILURE(rig.seated(0x45));
  rig.store.refuses_saves = true;
  const auto refused =
      rig.share->remember(rig.guest, "sam", libchiaking::kakusan::Replace::yes);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().kind, RememberRefusal::Kind::store_unavailable);
  EXPECT_EQ(rig.root.remembered().recognise(other_key).kind,
            Recognition::Kind::recognised);
  const auto saved = rig.store.saved();
  EXPECT_NE(std::ranges::find(saved, other_key, &SavedDevice::key),
            saved.end());
}

TEST_F(SharedSessionRemembered,
       RestoresTheGuestWhenTheOldHolderCannotBeRemoved) {
  const std::chrono::system_clock::time_point long_ago{
      std::chrono::seconds{1000}};
  const auto guest_key = TestPlatform::identity(0x45).key();
  const auto other_key = TestPlatform::identity(0x77).key();
  ShareRig rig{ShareRig::Where::home,
               std::nullopt,
               {SavedDevice{guest_key, "Pat", long_ago, long_ago},
                SavedDevice{other_key, "Sam", long_ago, long_ago}}};
  ASSERT_NO_FATAL_FAILURE(rig.play());
  ASSERT_NO_FATAL_FAILURE(rig.stream());
  ASSERT_NO_FATAL_FAILURE(rig.seated(0x45));
  const auto before = rig.store.saved();
  const auto earlier = std::ranges::find(before, guest_key, &SavedDevice::key);
  ASSERT_NE(earlier, before.end());
  rig.store.refuses_removing = other_key;
  const auto refused =
      rig.share->remember(rig.guest, "sam", libchiaking::kakusan::Replace::yes);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().kind, RememberRefusal::Kind::store_unavailable);
  EXPECT_EQ(rig.root.remembered().recognise(other_key).kind,
            Recognition::Kind::recognised);
  const auto saved = rig.store.saved();
  EXPECT_NE(std::ranges::find(saved, other_key, &SavedDevice::key),
            saved.end());
  const auto guest = std::ranges::find(saved, guest_key, &SavedDevice::key);
  ASSERT_NE(guest, saved.end());
  EXPECT_EQ(*guest, *earlier);
}

TEST_F(SharedSessionRemembered,
       ForgetsANewGuestWhenTheOldHolderCannotBeRemoved) {
  const std::chrono::system_clock::time_point long_ago{
      std::chrono::seconds{1000}};
  const auto guest_key = TestPlatform::identity(0x45).key();
  const auto other_key = TestPlatform::identity(0x77).key();
  ShareRig rig{ShareRig::Where::home,
               std::nullopt,
               {SavedDevice{other_key, "Sam", long_ago, long_ago}}};
  ASSERT_NO_FATAL_FAILURE(rig.play());
  ASSERT_NO_FATAL_FAILURE(rig.stream());
  ASSERT_NO_FATAL_FAILURE(rig.seated(0x45));
  rig.store.refuses_removing = other_key;
  const auto refused =
      rig.share->remember(rig.guest, "sam", libchiaking::kakusan::Replace::yes);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().kind, RememberRefusal::Kind::store_unavailable);
  EXPECT_EQ(rig.root.remembered().recognise(other_key).kind,
            Recognition::Kind::recognised);
  const auto saved = rig.store.saved();
  EXPECT_NE(std::ranges::find(saved, other_key, &SavedDevice::key),
            saved.end());
  EXPECT_EQ(std::ranges::find(saved, guest_key, &SavedDevice::key),
            saved.end());
}

}  // namespace
}  // namespace kakusan_test
