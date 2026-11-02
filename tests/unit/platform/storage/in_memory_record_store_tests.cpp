#include "tests/support/enum_printing.hpp"

#include <libchiaking/platform/platform.hpp>
#include <libchiaking/testing/in_memory_record_store.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

using libchiaking::platform::RecordName;
using libchiaking::platform::RecordStore;
using libchiaking::platform::StorageError;

class InMemoryRecordStore : public ::testing::Test {
 protected:
  void SetUp() override { memory.modified_at = written_at; }

  [[nodiscard]] static auto named(std::string_view text) -> RecordName {
    return RecordName::parse(text).value();
  }

  auto seed() -> ::testing::AssertionResult {
    if (!store.write_atomic(library, replacement) ||
        !store.write_atomic(refusals, expected) ||
        !store.write_atomic(other, expected) ||
        !store.write_atomic(shared, expected) ||
        !store.write_atomic(named("user/42"), expected))
      return ::testing::AssertionFailure() << "a record was not written";
    return ::testing::AssertionSuccess();
  }

  const std::chrono::system_clock::time_point written_at{
      std::chrono::seconds{345678}};
  libchiaking::platform::DefaultThreadingProvider threading;
  libchiaking::testing::InMemoryRecordStore memory{threading};
  RecordStore& store = memory;
  const RecordName library = named("user/42/library");
  const RecordName refusals = named("user/42/refusals");
  const RecordName other = named("user/421/library");
  const RecordName shared = named("streamable/gb-en");
  const std::vector<std::byte> expected{std::byte{0x01}, std::byte{0x00},
                                        std::byte{0x02}};
  const std::vector<std::byte> replacement{std::byte{0x10}};
};

TEST_F(InMemoryRecordStore, IsDestroyedThroughTheInterfaceAndNeverCopied) {
  EXPECT_TRUE(std::has_virtual_destructor_v<RecordStore>);
  EXPECT_FALSE(std::is_copy_constructible_v<RecordStore>);
  EXPECT_FALSE(std::is_copy_assignable_v<RecordStore>);
}

TEST_F(InMemoryRecordStore, AMissingRecordIsNotFound) {
  EXPECT_EQ(store.read(library).error(), StorageError::not_found);
  EXPECT_EQ(store.erase(library).error(), StorageError::not_found);
}

TEST_F(InMemoryRecordStore, KeepsItsOwnCopyOfWrittenBytes) {
  std::vector<std::byte> bytes = expected;
  ASSERT_TRUE(store.write_atomic(library, bytes));
  bytes[0] = std::byte{0xff};
  auto read = store.read(library);
  ASSERT_TRUE(read);
  EXPECT_EQ(read.value().data, expected);
  EXPECT_EQ(read.value().modified_at, written_at);
  read.value().data.clear();
  EXPECT_EQ(store.read(library).value().data, expected);
}

TEST_F(InMemoryRecordStore, AFailedWriteKeepsTheOldRecord) {
  ASSERT_TRUE(store.write_atomic(library, expected));
  memory.fail_writes = true;
  const auto failed =
      store.write_atomic(library, std::vector<std::byte>{std::byte{0x09}});
  memory.fail_writes = false;
  EXPECT_EQ(failed.error(), StorageError::write_failed);
  EXPECT_EQ(store.read(library).value().data, expected);
  ASSERT_TRUE(store.write_atomic(library, replacement));
  EXPECT_EQ(store.read(library).value().data, replacement);
}

TEST_F(InMemoryRecordStore, ListsInNameOrderUnderAPrefix) {
  ASSERT_TRUE(seed());
  const auto under = store.list(named("user/42"));
  const auto users = store.list(named("user"));
  const auto none = store.list(named("never/written"));
  ASSERT_TRUE(under);
  ASSERT_EQ(under.value().size(), 3U);
  EXPECT_EQ(under.value()[0].name, named("user/42"));
  EXPECT_EQ(under.value()[1].name, library);
  EXPECT_EQ(under.value()[1].bytes, replacement.size());
  EXPECT_EQ(under.value()[2].name, refusals);
  EXPECT_EQ(under.value()[2].bytes, expected.size());
  EXPECT_EQ(under.value()[2].modified_at, written_at);
  ASSERT_TRUE(users);
  ASSERT_EQ(users.value().size(), 4U);
  EXPECT_EQ(users.value()[3].name, other);
  ASSERT_TRUE(none);
  EXPECT_TRUE(none.value().empty());
}

TEST_F(InMemoryRecordStore, AFailedListIsAReadFailure) {
  ASSERT_TRUE(seed());
  memory.fail_lists = true;
  const auto unlisted = store.list(named("user"));
  memory.fail_lists = false;
  ASSERT_FALSE(unlisted);
  EXPECT_EQ(unlisted.error(), StorageError::read_failed);
}

TEST_F(InMemoryRecordStore, ErasingAPrefixLeavesNeighbours) {
  ASSERT_TRUE(seed());
  ASSERT_TRUE(store.erase_prefix(named("user/42")));
  EXPECT_EQ(store.read(library).error(), StorageError::not_found);
  EXPECT_EQ(store.read(refusals).error(), StorageError::not_found);
  EXPECT_EQ(store.read(named("user/42")).error(), StorageError::not_found);
  EXPECT_TRUE(store.read(other));
  EXPECT_TRUE(store.read(shared));
  EXPECT_EQ(memory.names().size(), 2U);
  EXPECT_TRUE(store.erase_prefix(named("user/42")));
  EXPECT_TRUE(store.erase_prefix(named("never/written")));
}

TEST_F(InMemoryRecordStore, ErasingARecordTwiceReportsNotFound) {
  ASSERT_TRUE(seed());
  EXPECT_TRUE(store.erase(shared));
  EXPECT_EQ(store.erase(shared).error(), StorageError::not_found);
}

TEST_F(InMemoryRecordStore, ErasesLegacyFilesByName) {
  memory.legacy_files = {"unified_catalog_v4.json", "settings.toml"};
  EXPECT_TRUE(store.erase_legacy_file("unified_catalog_v4.json"));
  EXPECT_TRUE(store.erase_legacy_file("unified_catalog_v4.json"));
  EXPECT_EQ(memory.legacy_files.size(), 1U);
  EXPECT_TRUE(memory.legacy_files.contains("settings.toml"));
}

TEST_F(InMemoryRecordStore, RefusesNamesThatAreNotLegacyFiles) {
  EXPECT_EQ(store.erase_legacy_file("user/42").error(),
            StorageError::invalid_argument);
  EXPECT_EQ(store.erase_legacy_file("flat.record").error(),
            StorageError::invalid_argument);
  EXPECT_TRUE(
      RecordStore::is_legacy_file_name("ps_plus_store_tags_failure.txt"));
  EXPECT_FALSE(RecordStore::is_legacy_file_name(""));
  EXPECT_FALSE(RecordStore::is_legacy_file_name(".."));
}

}  // namespace
