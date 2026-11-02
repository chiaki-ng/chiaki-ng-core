#include "tests/support/enum_printing.hpp"
#include "tests/unit/platform/storage/rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace platform_storage_test {
namespace {

using libchiaking::platform::DefaultStorageProvider;
using libchiaking::platform::RecordName;
using libchiaking::platform::RecordStore;
using libchiaking::platform::StorageError;

class DefaultStorageLegacyFiles : public StorageRig {
 protected:
  void SetUp() override {
    StorageRig::SetUp();
    ASSERT_TRUE(store.write_atomic(flat, payload));
    ASSERT_TRUE(write_file(tags, payload));
    ASSERT_TRUE(write_file(catalog, payload));
  }

  const std::string root = temporary.path() + "/legacy/store";
  DefaultStorageProvider storage{root};
  RecordStore& store = storage;
  const std::vector<std::byte> payload{std::byte{0x01}, std::byte{0x02}};
  const RecordName flat = named("flat");
  const std::filesystem::path tags =
      std::filesystem::path(root) / "ps_plus_store_tags_v2.json";
  const std::filesystem::path catalog =
      std::filesystem::path(root) / "unified_catalog_v4.json";
};

TEST_F(DefaultStorageLegacyFiles, AreErasedByNameAndOnlyOnce) {
  EXPECT_TRUE(std::filesystem::is_regular_file(tags, filesystem_error));
  EXPECT_TRUE(store.erase_legacy_file("ps_plus_store_tags_v2.json"));
  EXPECT_FALSE(std::filesystem::exists(tags, filesystem_error));
  EXPECT_TRUE(store.erase_legacy_file("ps_plus_store_tags_v2.json"));
  EXPECT_TRUE(store.erase_legacy_file("never_written.json"));
  EXPECT_TRUE(std::filesystem::is_regular_file(catalog, filesystem_error));
  EXPECT_TRUE(store.read(flat));
}

TEST_F(DefaultStorageLegacyFiles, RefuseNamesThatAreNotPlainLegacyFiles) {
  for (const char* refused : {"", ".hidden", "../unified_catalog_v4.json",
                              "user/42", "a\\b", "c:d", "flat.record"}) {
    const auto result = store.erase_legacy_file(refused);
    ASSERT_FALSE(result) << '"' << refused << '"';
    EXPECT_EQ(result.error(), StorageError::invalid_argument)
        << '"' << refused << '"';
  }
  EXPECT_TRUE(std::filesystem::is_regular_file(catalog, filesystem_error));
  EXPECT_TRUE(store.read(flat));
}

TEST_F(DefaultStorageLegacyFiles, CannotBeRemovedFromARootlessStore) {
  DefaultStorageProvider rootless{""};
  RecordStore& nowhere = rootless;
  const auto unreachable = nowhere.erase_legacy_file("unified_catalog_v4.json");
  ASSERT_FALSE(unreachable);
  EXPECT_EQ(unreachable.error(), StorageError::remove_failed);
}

}  // namespace
}  // namespace platform_storage_test
