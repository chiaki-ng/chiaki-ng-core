#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "tests/support/enum_printing.hpp"
#include "tests/unit/platform/storage/rig.hpp"

#include <libchiaking/platform/runtime/threading.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace platform_storage_test {
namespace {

using libchiaking::platform::DefaultStorageProvider;
using libchiaking::platform::RecordStore;

class DefaultStorageContention : public StorageRig {
 protected:
  [[nodiscard]] static auto no_temporary_files(const std::string& directory)
      -> bool {
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
      if (entry.path().filename().string().starts_with(".tmp-")) return false;
    }
    return true;
  }
};

TEST_F(DefaultStorageContention, AWriteWaitsOutABriefSharingViolation) {
  const std::string root = temporary.path() + "/store";
  DefaultStorageProvider storage{root};
  ASSERT_TRUE(std::filesystem::create_directories(root, filesystem_error));
  RecordStore& store = storage;
  const auto flat = named("flat");
  const std::vector<std::byte> payload{std::byte{0x00}, std::byte{0xff}};
  const std::vector<std::byte> replacement{std::byte{0xde}, std::byte{0xad}};
  ASSERT_TRUE(store.write_atomic(flat, payload));
  const auto target = (std::filesystem::path(root) / "flat.record").wstring();
  const DWORD attributes = ::GetFileAttributesW(target.c_str());
  ASSERT_NE(attributes, INVALID_FILE_ATTRIBUTES);
  EXPECT_EQ(attributes & FILE_ATTRIBUTE_TEMPORARY, 0U);
  const HANDLE blocker =
      ::CreateFileW(target.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(blocker, INVALID_HANDLE_VALUE);
  libchiaking::platform::DefaultThreadingProvider threading;
  const libchiaking::platform::SteadyClock clock;
  auto releaser = libchiaking::platform::Thread::start(
      threading, "releaser", libchiaking::platform::ThreadRole::unspecified,
      [&threading, &clock, blocker] {
        libchiaking::platform::Sleeper sleeper{threading, clock};
        static_cast<void>(sleeper.sleep_for(std::chrono::milliseconds{60}));
        static_cast<void>(::CloseHandle(blocker));
      });
  ASSERT_TRUE(releaser);
  const auto contended = store.write_atomic(flat, replacement);
  static_cast<void>(releaser.value().join());
  EXPECT_TRUE(contended);
  const auto after_contention = store.read(flat);
  ASSERT_TRUE(after_contention);
  EXPECT_EQ(after_contention.value().data, replacement);
  EXPECT_TRUE(no_temporary_files(root));
}

}  // namespace
}  // namespace platform_storage_test
#endif
