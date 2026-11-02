#include "libchiaking/kakusan_types.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/model/remembered_devices_rig.hpp"

#include <gtest/gtest.h>

namespace kakusan_test {
namespace {

using libchiaking::kakusan::DeviceStore;
using libchiaking::kakusan::Recognition;
using libchiaking::kakusan::RememberRefusal;

class RememberedDevicesLoaded : public RememberedDevicesTest {
 protected:
  void SetUp() override {
    book.store.devices.push_back(saved(0x20, "Ana"));
    book.store.devices.push_back(saved(0x30, "Ben"));
  }

  Book book;
};

TEST_F(RememberedDevicesTest,
       LoadsSavedDevicesWithTheFingerprintTheCoreComputes) {
  Book book;
  book.store.devices.push_back(saved(0x20, "Ana"));
  auto root = book.open();
  const auto all = root.remembered().all();
  ASSERT_EQ(all.size(), 1U);
  EXPECT_EQ(all.front().nickname, "Ana");
  EXPECT_EQ(all.front().key, key_of(0x20));
  EXPECT_EQ(all.front().fingerprint,
            TestPlatform::identity(0x20).fingerprint());
  EXPECT_EQ(all.front().first_verified, at(10));
  EXPECT_EQ(all.front().last_seen, at(20));
}

TEST_F(RememberedDevicesLoaded, RecogniseByKeyAndThenByExpectedNickname) {
  auto root = book.open();
  auto& devices = root.remembered();

  EXPECT_EQ(devices.recognise(key_of(0x40)).kind, Recognition::Kind::unknown);
  EXPECT_FALSE(devices.recognise(key_of(0x40)).device);
  const auto known = devices.recognise(key_of(0x20));
  EXPECT_EQ(known.kind, Recognition::Kind::recognised);
  ASSERT_TRUE(known.device);
  EXPECT_EQ(known.device->nickname, "Ana");
  const auto expected = devices.recognise(key_of(0x20), "ana");
  EXPECT_EQ(expected.kind, Recognition::Kind::recognised);
  ASSERT_TRUE(expected.device);
  EXPECT_EQ(expected.device->nickname, "Ana");
  const auto changed = devices.recognise(key_of(0x40), "ANA");
  EXPECT_EQ(changed.kind, Recognition::Kind::changed_key);
  ASSERT_TRUE(changed.device);
  EXPECT_EQ(changed.device->key, key_of(0x20));
  const auto other = devices.recognise(key_of(0x30), "Ana");
  EXPECT_EQ(other.kind, Recognition::Kind::changed_key);
  ASSERT_TRUE(other.device);
  EXPECT_EQ(other.device->nickname, "Ana");
  const auto unnamed = devices.recognise(key_of(0x30), "Nobody");
  EXPECT_EQ(unnamed.kind, Recognition::Kind::recognised);
  ASSERT_TRUE(unnamed.device);
  EXPECT_EQ(unnamed.device->nickname, "Ben");
  EXPECT_EQ(devices.recognise(key_of(0x40), "Nobody").kind,
            Recognition::Kind::unknown);
}

TEST_F(RememberedDevicesLoaded, RenameAndForgetThroughTheStore) {
  auto root = book.open();
  auto& devices = root.remembered();

  const auto invalid = devices.rename(key_of(0x20), " ");
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error().kind, RememberRefusal::Kind::invalid_nickname);
  const auto taken = devices.rename(key_of(0x20), "b\xc3\xa9n");
  ASSERT_FALSE(taken);
  EXPECT_EQ(taken.error().kind, RememberRefusal::Kind::nickname_taken);
  ASSERT_TRUE(taken.error().holder);
  EXPECT_EQ(taken.error().holder->key, key_of(0x30));
  const auto missing = devices.rename(key_of(0x50), "Cy");
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().kind, RememberRefusal::Kind::unknown_device);
  const auto same = devices.rename(key_of(0x20), "ANA");
  ASSERT_TRUE(same);
  EXPECT_EQ(same.value().nickname, "ANA");
  const auto renamed = devices.rename(key_of(0x20), "  Anna ");
  ASSERT_TRUE(renamed);
  EXPECT_EQ(renamed.value().nickname, "Anna");
  EXPECT_EQ(renamed.value().first_verified, at(10));
  EXPECT_EQ(book.store.saved().front().nickname, "Anna");
  const auto recognised = devices.recognise(key_of(0x20));
  ASSERT_TRUE(recognised.device);
  EXPECT_EQ(recognised.device->nickname, "Anna");

  EXPECT_TRUE(devices.forget(key_of(0x30)));
  EXPECT_EQ(devices.all().size(), 1U);
  EXPECT_EQ(book.store.saved().size(), 1U);
  EXPECT_EQ(devices.recognise(key_of(0x30)).kind, Recognition::Kind::unknown);
  EXPECT_TRUE(devices.forget(key_of(0x30)));
}

TEST_F(RememberedDevicesLoaded, KeepTheirNamesWhenTheStoreIsUnavailable) {
  auto root = book.open();
  auto& devices = root.remembered();
  ASSERT_TRUE(devices.rename(key_of(0x20), "Anna"));
  ASSERT_TRUE(devices.forget(key_of(0x30)));

  book.store.unavailable = true;
  const auto unsaved = devices.rename(key_of(0x20), "Zed");
  ASSERT_FALSE(unsaved);
  EXPECT_EQ(unsaved.error().kind, RememberRefusal::Kind::store_unavailable);
  const auto kept = devices.forget(key_of(0x20));
  ASSERT_FALSE(kept);
  EXPECT_EQ(kept.error(), DeviceStore::Error::unavailable);
  ASSERT_EQ(devices.all().size(), 1U);
  EXPECT_EQ(devices.all().front().nickname, "Anna");
}

}  // namespace
}  // namespace kakusan_test
