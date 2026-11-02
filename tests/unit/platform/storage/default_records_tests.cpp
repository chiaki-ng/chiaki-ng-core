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

class DefaultStorageRecords : public StorageRig {
 protected:
  void SetUp() override {
    StorageRig::SetUp();
    ASSERT_TRUE(std::filesystem::create_directories(temporary.path() + "/store",
                                                    filesystem_error));
  }

  auto seed() -> ::testing::AssertionResult {
    if (!store.write_atomic(library, payload) ||
        !store.write_atomic(library, replacement))
      return ::testing::AssertionFailure() << "the library was not written";
    if (!store.write_atomic(refusals, payload) ||
        !store.write_atomic(neighbour, payload) ||
        !store.write_atomic(whole_user, payload) ||
        !store.write_atomic(shared, payload) ||
        !store.write_atomic(flat, std::vector<std::byte>{}))
      return ::testing::AssertionFailure() << "a record was not written";
    return ::testing::AssertionSuccess();
  }

  [[nodiscard]] auto under_root() const -> std::filesystem::path {
    return std::filesystem::path(root);
  }

  const std::string root = temporary.path() + "/records/store";
  DefaultStorageProvider storage{root};
  RecordStore& store = storage;
  const RecordName library = named("user/42/library");
  const RecordName refusals = named("user/42/refusals");
  const RecordName neighbour = named("user/421/library");
  const RecordName whole_user = named("user/42");
  const RecordName shared = named("streamable/gb-en");
  const RecordName flat = named("flat");
  const std::vector<std::byte> payload{std::byte{0x00}, std::byte{0xff},
                                       std::byte{0x42}, std::byte{0x00}};
  const std::vector<std::byte> replacement{std::byte{0xde}, std::byte{0xad}};
};

TEST_F(DefaultStorageRecords, AMissingRecordIsNotFound) {
  const auto missing = store.read(library);
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error(), StorageError::not_found);
  const auto never_written = store.erase(library);
  ASSERT_FALSE(never_written);
  EXPECT_EQ(never_written.error(), StorageError::not_found);
  EXPECT_TRUE(store.erase_prefix(whole_user));
}

TEST_F(DefaultStorageRecords, AWriteLandsAsAFileUnderTheRoot) {
  ASSERT_TRUE(store.write_atomic(library, payload));
  const auto stored = store.read(library);
  ASSERT_TRUE(stored);
  EXPECT_EQ(stored.value().data, payload);
  EXPECT_TRUE(modified_time_is_sane(stored.value().modified_at));
  const auto written_path = under_root() / "user" / "42" / "library.record";
  EXPECT_TRUE(std::filesystem::is_regular_file(written_path, filesystem_error));
  EXPECT_EQ(std::filesystem::file_size(written_path, filesystem_error),
            payload.size());
  EXPECT_TRUE(no_temporary_files_below(root));
}

TEST_F(DefaultStorageRecords, AWriteReplacesTheRecordWithoutLeftovers) {
  ASSERT_TRUE(store.write_atomic(library, payload));
  ASSERT_TRUE(store.write_atomic(library, replacement));
  const auto replaced = store.read(library);
  ASSERT_TRUE(replaced);
  EXPECT_EQ(replaced.value().data, replacement);
  EXPECT_TRUE(no_temporary_files_below(root));
}

TEST_F(DefaultStorageRecords, EmptyAndParentRecordsAreKept) {
  ASSERT_TRUE(seed());
  const auto empty = store.read(flat);
  ASSERT_TRUE(empty);
  EXPECT_TRUE(empty.value().data.empty());
  EXPECT_TRUE(std::filesystem::is_regular_file(under_root() / "flat.record",
                                               filesystem_error));
  const auto user_record = store.read(whole_user);
  ASSERT_TRUE(user_record);
  EXPECT_EQ(user_record.value().data, payload);
}

TEST_F(DefaultStorageRecords,
       ListingWalksTheTreeInNameOrderAndSkipsOtherFiles) {
  ASSERT_TRUE(seed());
  ASSERT_TRUE(write_file(under_root() / "unified_catalog_v4.json", payload));

  const auto under_user = store.list(whole_user);
  ASSERT_TRUE(under_user);
  ASSERT_EQ(under_user.value().size(), 3U);
  EXPECT_EQ(under_user.value()[0].name, whole_user);
  EXPECT_EQ(under_user.value()[1].name, library);
  EXPECT_EQ(under_user.value()[2].name, refusals);
  EXPECT_EQ(under_user.value()[1].bytes, replacement.size());
  EXPECT_EQ(under_user.value()[2].bytes, payload.size());
  EXPECT_TRUE(modified_time_is_sane(under_user.value()[1].modified_at));
  const auto every_user = store.list(named("user"));
  ASSERT_TRUE(every_user);
  ASSERT_EQ(every_user.value().size(), 4U);
  EXPECT_EQ(every_user.value()[3].name, neighbour);
  const auto one_record = store.list(shared);
  ASSERT_TRUE(one_record);
  ASSERT_EQ(one_record.value().size(), 1U);
  EXPECT_EQ(one_record.value()[0].name, shared);
  const auto nothing_there = store.list(named("absent"));
  ASSERT_TRUE(nothing_there);
  EXPECT_TRUE(nothing_there.value().empty());
}

TEST_F(DefaultStorageRecords, ErasingAPrefixLeavesNeighboursAndForeignFiles) {
  ASSERT_TRUE(seed());
  const auto catalog = under_root() / "unified_catalog_v4.json";
  ASSERT_TRUE(write_file(catalog, payload));
  ASSERT_TRUE(store.erase_prefix(whole_user));
  for (const auto& erased : {library, refusals, whole_user}) {
    const auto result = store.read(erased);
    ASSERT_FALSE(result) << erased.text();
    EXPECT_EQ(result.error(), StorageError::not_found) << erased.text();
  }
  const auto kept_neighbour = store.read(neighbour);
  ASSERT_TRUE(kept_neighbour);
  EXPECT_EQ(kept_neighbour.value().data, payload);
  EXPECT_TRUE(store.read(shared));
  EXPECT_EQ(std::filesystem::file_size(catalog, filesystem_error),
            payload.size());
  EXPECT_FALSE(
      std::filesystem::exists(under_root() / "user" / "42", filesystem_error));
  EXPECT_TRUE(
      std::filesystem::exists(under_root() / "user" / "421", filesystem_error));
  EXPECT_TRUE(store.erase_prefix(whole_user));
}

TEST_F(DefaultStorageRecords, ErasingARecordTwiceReportsNotFound) {
  ASSERT_TRUE(store.write_atomic(library, payload));
  ASSERT_TRUE(store.erase(library));
  const auto erased_twice = store.erase(library);
  ASSERT_FALSE(erased_twice);
  EXPECT_EQ(erased_twice.error(), StorageError::not_found);
}

TEST_F(DefaultStorageRecords, ErasingTopLevelPrefixesEmptiesTheStore) {
  ASSERT_TRUE(seed());
  ASSERT_TRUE(store.read(shared));
  ASSERT_TRUE(store.read(neighbour));
  EXPECT_TRUE(store.erase_prefix(named("streamable")));
  EXPECT_FALSE(store.read(shared));
  EXPECT_TRUE(store.erase_prefix(named("user")));
  EXPECT_FALSE(store.read(neighbour));
  EXPECT_TRUE(no_temporary_files_below(root));
}

TEST_F(DefaultStorageRecords, ARootlessStoreCannotWrite) {
  DefaultStorageProvider rootless{""};
  RecordStore& nowhere = rootless;
  const auto unwritable = nowhere.write_atomic(library, payload);
  ASSERT_FALSE(unwritable);
  EXPECT_EQ(unwritable.error(), StorageError::write_failed);
}

}  // namespace
}  // namespace platform_storage_test
