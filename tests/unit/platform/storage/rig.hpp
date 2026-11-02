#pragma once

#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/storage/default.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace platform_storage_test {

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    const auto base = std::filesystem::temp_directory_path();
    std::random_device device;
    const auto seed = (static_cast<std::uint64_t>(device()) << 32) |
                      static_cast<std::uint64_t>(device());
    for (int attempt = 0; attempt < 64; ++attempt) {
      const auto candidate =
          base / ("libchiaki-storage-" +
                  std::to_string(seed + static_cast<std::uint64_t>(attempt)));
      std::error_code error;
      if (std::filesystem::create_directory(candidate, error)) {
        path_ = candidate.string();
        return;
      }
    }
  }

  ~TemporaryDirectory() {
    if (!path_.empty()) {
      std::error_code error;
      static_cast<void>(std::filesystem::remove_all(path_, error));
    }
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  auto operator=(const TemporaryDirectory&) -> TemporaryDirectory& = delete;

  [[nodiscard]] auto path() const noexcept -> const std::string& {
    return path_;
  }

 private:
  std::string path_;
};

class StorageRig : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_FALSE(temporary.path().empty()); }

  [[nodiscard]] static auto named(std::string_view text)
      -> libchiaking::platform::RecordName {
    return libchiaking::platform::RecordName::parse(text).value();
  }

  [[nodiscard]] static auto modified_time_is_sane(
      std::chrono::system_clock::time_point modified_at) -> bool {
    const libchiaking::platform::SystemClock wall;
    const std::chrono::system_clock::time_point now{
        std::chrono::microseconds{wall.now_microseconds()}};
    return modified_at > now - std::chrono::hours(24) &&
           modified_at < now + std::chrono::hours(1);
  }

  [[nodiscard]] static auto no_temporary_files_below(
      const std::string& directory) -> bool {
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(directory)) {
      if (entry.path().filename().string().starts_with(".tmp-")) return false;
    }
    return true;
  }

  [[nodiscard]] static auto write_file(const std::filesystem::path& path,
                                       const std::vector<std::byte>& data)
      -> bool {
    std::FILE* file = std::fopen(path.string().c_str(), "wb");
    if (file == nullptr) return false;
    const bool written =
        std::fwrite(data.data(), 1, data.size(), file) == data.size();
    return std::fclose(file) == 0 && written;
  }

  TemporaryDirectory temporary;
  std::error_code filesystem_error;
};

}  // namespace platform_storage_test
