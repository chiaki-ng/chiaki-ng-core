#include "src/core/catalog/adapters/cache/cache.hpp"
#include "src/core/catalog/adapters/cache/image_shelf.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/details_rig.hpp"

#include <libchiaking/platform/runtime/threading.hpp>
#include <libchiaking/testing/in_memory_record_store.hpp>
#include <libchiaking/testing/manual_clock.hpp>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace catalog_test {
namespace {

namespace cache = libchiaking::catalog::cache;
namespace platform = libchiaking::platform;
using libchiaking::catalog::ImageHost;
using libchiaking::catalog::ImageRef;
using Reason = libchiaking::catalog::Unanswered::Reason;

class SlowRecords final : public platform::RecordStore {
 public:
  auto read(const platform::RecordName& name)
      -> libchiaking::Expected<platform::StoredRecord,
                               platform::StorageError> override {
    return inner.read(name);
  }

  auto write_atomic(const platform::RecordName& name,
                    std::span<const std::byte> data)
      -> libchiaking::Expected<void, platform::StorageError> override {
    {
      platform::Lock lock{mutex_};
      if (slow_ && name.text().starts_with("image/")) {
        ++writing_;
        changed_.notify_all();
        changed_.wait(lock, [this] { return !slow_; });
        --writing_;
      }
    }
    return inner.write_atomic(name, data);
  }

  auto erase(const platform::RecordName& name)
      -> libchiaking::Expected<void, platform::StorageError> override {
    {
      const platform::Lock lock{mutex_};
      if (refusing_erase_)
        return libchiaking::unexpected(platform::StorageError::remove_failed);
    }
    return inner.erase(name);
  }

  auto erase_prefix(const platform::RecordName& prefix)
      -> libchiaking::Expected<void, platform::StorageError> override {
    return inner.erase_prefix(prefix);
  }

  auto list(const platform::RecordName& prefix)
      -> libchiaking::Expected<std::vector<platform::RecordListing>,
                               platform::StorageError> override {
    return inner.list(prefix);
  }

  auto erase_legacy_file(std::string_view file_name)
      -> libchiaking::Expected<void, platform::StorageError> override {
    return inner.erase_legacy_file(file_name);
  }

  void slow(bool slow) {
    const platform::Lock lock{mutex_};
    slow_ = slow;
    changed_.notify_all();
  }

  void refuse_erase() {
    const platform::Lock lock{mutex_};
    refusing_erase_ = true;
  }

  void wait_for_writing() {
    platform::Lock lock{mutex_};
    EXPECT_TRUE(changed_.wait_for(lock, std::chrono::seconds{5}, [this] {
      return writing_ > 0;
    })) << "no image was ever written";
  }

  platform::DefaultThreadingProvider threading;
  libchiaking::testing::InMemoryRecordStore inner{threading};

 private:
  platform::SteadyClock steady_;
  platform::Mutex mutex_{threading};
  platform::ConditionVariable changed_{threading, steady_};
  bool slow_{};
  bool refusing_erase_{};
  int writing_{};
};

class ImageShelfEviction : public ::testing::Test {
 protected:
  static constexpr std::uint64_t room_for_two = 10;

  void fill() {
    cache::ImageShelf shelf{cache, threading, room_for_two};
    ASSERT_TRUE(shelf.keep(logo, DetailsRig::bytes_of("logo")));
    wall.advance(std::chrono::minutes{1});
    ASSERT_TRUE(shelf.keep(hero, DetailsRig::bytes_of("hero")));
    wall.advance(std::chrono::hours{2});
    ASSERT_TRUE(shelf.find(logo));
    wall.advance(std::chrono::minutes{1});
    ASSERT_TRUE(shelf.keep(background, DetailsRig::bytes_of("back")));
  }

  platform::DefaultThreadingProvider threading;
  libchiaking::testing::InMemoryRecordStore records{threading};
  libchiaking::testing::ManualClock wall;
  cache::Cache cache{records, wall};
  const ImageRef logo = DetailsRig::image(DetailsRig::concept_logo);
  const ImageRef hero = DetailsRig::image(DetailsRig::concept_hero);
  const ImageRef background = DetailsRig::image(DetailsRig::concept_background);
};

TEST_F(ImageShelfEviction, DropsTheLeastRecentlyUsed) {
  {
    cache::ImageShelf shelf{cache, threading, room_for_two};
    EXPECT_TRUE(shelf.keep(logo, DetailsRig::bytes_of("logo")));
    wall.advance(std::chrono::minutes{1});
    EXPECT_TRUE(shelf.keep(hero, DetailsRig::bytes_of("hero")));
    wall.advance(std::chrono::hours{2});
    const auto used = shelf.find(logo);
    ASSERT_TRUE(used);
    EXPECT_EQ(*used, DetailsRig::bytes_of("logo"));
    wall.advance(std::chrono::minutes{1});
    EXPECT_TRUE(shelf.keep(background, DetailsRig::bytes_of("back")));
    EXPECT_TRUE(shelf.find(logo));
    EXPECT_TRUE(shelf.find(background));
    EXPECT_FALSE(shelf.find(hero));
    EXPECT_EQ(shelf.held().size(), 2U);
  }
  EXPECT_EQ(records.names().size(), 3U);
}

TEST_F(ImageShelfEviction, IsRememberedWhenTheShelfReopens) {
  ASSERT_NO_FATAL_FAILURE(fill());
  cache::ImageShelf reopened{cache, threading, room_for_two};
  EXPECT_EQ(reopened.held().size(), 2U);
  EXPECT_FALSE(reopened.find(hero));
  const auto kept = reopened.find(background);
  ASSERT_TRUE(kept);
  EXPECT_EQ(*kept, DetailsRig::bytes_of("back"));
}

TEST_F(ImageShelfEviction, TidiesAStrayRecordWhenTheShelfOpens) {
  ASSERT_NO_FATAL_FAILURE(fill());
  const auto stray = platform::RecordName::parse("image/0000000000000000");
  ASSERT_TRUE(stray);
  ASSERT_TRUE(records.write_atomic(*stray, DetailsRig::bytes_of("stray")));
  cache::ImageShelf tidied{cache, threading, room_for_two};
  EXPECT_EQ(tidied.held().size(), 2U);
  EXPECT_FALSE(records.read(*stray));
}

TEST_F(ImageShelfEviction, DropsAndClearsWhatTheShelfHolds) {
  ASSERT_NO_FATAL_FAILURE(fill());
  cache::ImageShelf tidied{cache, threading, room_for_two};
  const std::array gone{logo};
  ASSERT_TRUE(tidied.drop(gone));
  EXPECT_EQ(tidied.held().size(), 1U);
  EXPECT_FALSE(tidied.find(logo));
  EXPECT_TRUE(tidied.clear());
  EXPECT_TRUE(tidied.held().empty());
  EXPECT_TRUE(records.names().empty());
}

TEST(ImageShelfDrops, ReportsAnEraseTheStoreRefuses) {
  SlowRecords records;
  libchiaking::testing::ManualClock wall;
  cache::Cache cache{records, wall};
  const ImageRef logo = DetailsRig::image(DetailsRig::concept_logo);
  cache::ImageShelf shelf{cache, records.threading, 1024};
  ASSERT_TRUE(shelf.keep(logo, DetailsRig::bytes_of("logo")));
  records.refuse_erase();
  const std::array gone{logo};
  const auto dropped = shelf.drop(gone);
  ASSERT_FALSE(dropped);
  EXPECT_EQ(dropped.error().reason(), Reason::storage);
  EXPECT_FALSE(shelf.find(logo));
}

TEST(ImageShelfSizeCap, RefusesAnImageTooLargeOrEmpty) {
  platform::DefaultThreadingProvider threading;
  libchiaking::testing::InMemoryRecordStore records{threading};
  libchiaking::testing::ManualClock wall;
  cache::Cache cache{records, wall};
  cache::ImageShelf shelf{cache, threading};
  const auto hero = DetailsRig::image(DetailsRig::concept_hero);
  const std::vector<std::byte> huge(ImageHost::largest_bytes + 1);
  const auto too_large = shelf.keep(hero, huge);
  ASSERT_FALSE(too_large);
  EXPECT_EQ(too_large.error().reason(), Reason::rejected);
  const auto nothing = shelf.keep(hero, {});
  EXPECT_FALSE(nothing);
  EXPECT_TRUE(shelf.held().empty());
  EXPECT_TRUE(records.names().empty());
}

TEST(ImageShelfWrites, LeaveAHeldImageFindableWhileAnotherIsWritten) {
  SlowRecords records;
  libchiaking::testing::ManualClock wall;
  cache::Cache cache{records, wall};
  cache::ImageShelf shelf{cache, records.threading};
  const auto logo = DetailsRig::image(DetailsRig::concept_logo);
  const auto hero = DetailsRig::image(DetailsRig::concept_hero);
  EXPECT_TRUE(shelf.keep(logo, DetailsRig::bytes_of("logo")));
  records.slow(true);
  std::optional<libchiaking::Expected<cache::ImageShelf::Bytes,
                                      libchiaking::catalog::Unanswered>>
      kept;
  auto writer = DetailsRig::started(records.threading, [&] {
    kept = shelf.keep(hero, DetailsRig::bytes_of("hero"));
  });
  records.wait_for_writing();
  const auto found = shelf.find(logo);
  EXPECT_TRUE(found);
  if (found) {
    EXPECT_EQ(*found, DetailsRig::bytes_of("logo"));
  }
  EXPECT_EQ(shelf.held().size(), 1U);
  records.slow(false);
  static_cast<void>(writer.join());
  ASSERT_TRUE(kept);
  EXPECT_TRUE(*kept);
  EXPECT_EQ(shelf.held().size(), 2U);

  cache::ImageShelf reopened{cache, records.threading};
  EXPECT_EQ(reopened.held().size(), 2U);
  EXPECT_TRUE(reopened.find(hero));
  EXPECT_TRUE(reopened.find(logo));
}

}  // namespace
}  // namespace catalog_test
