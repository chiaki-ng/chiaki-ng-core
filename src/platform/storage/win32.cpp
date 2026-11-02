#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "native_files.hpp"
#include "src/core/utils/memory/c_handle.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <utility>

namespace libchiaking::platform::storage {

class NativeFiles::Native {
 public:
  using Clock = std::chrono::system_clock;
  using OwnedHandle = memory::CHandle<void, ::CloseHandle>;

  static constexpr int temp_name_attempts = 64;
  static constexpr int replace_attempts = 20;
  static constexpr DWORD replace_retry_delay_ms = 10;
  static constexpr std::uint64_t filetime_unix_epoch_offset =
      116444736000000000ULL;
  static constexpr std::intmax_t filetime_ticks_per_second = 10'000'000;

  static bool missing(DWORD error) noexcept {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
  }

  static auto opened(HANDLE handle) noexcept -> OwnedHandle {
    return OwnedHandle{handle == INVALID_HANDLE_VALUE ? nullptr : handle};
  }

  class TemporaryFile {
   public:
    TemporaryFile() = default;
    ~TemporaryFile() {
      handle_.reset();
      if (!path_.empty()) (void)::DeleteFileW(path_.c_str());
    }
    TemporaryFile(const TemporaryFile&) = delete;
    auto operator=(const TemporaryFile&) -> TemporaryFile& = delete;

    [[nodiscard]] bool create(const std::filesystem::path& target) {
      std::random_device device;
      std::mt19937_64 engine{device()};
      for (int attempt = 0; attempt < temp_name_attempts; ++attempt) {
        std::wstring name = target.parent_path().native() + L"\\.tmp-" +
                            target.filename().native() + L"-" +
                            std::to_wstring(engine());
        handle_ =
            opened(::CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (handle_) {
          path_ = std::move(name);
          return true;
        }
        if (::GetLastError() != ERROR_FILE_EXISTS) return false;
      }
      return false;
    }

    [[nodiscard]] bool store(std::span<const std::byte> data) noexcept {
      return write_all(handle_.get(), data) &&
             ::FlushFileBuffers(handle_.get()) != 0 &&
             ::CloseHandle(handle_.release()) != 0;
    }

    [[nodiscard]] bool move_to(const std::filesystem::path& target) noexcept {
      for (int attempt = 0;; ++attempt) {
        if (::MoveFileExW(path_.c_str(), target.c_str(),
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) !=
            0) {
          path_.clear();
          return true;
        }
        const DWORD error = ::GetLastError();
        if ((error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED &&
             error != ERROR_LOCK_VIOLATION) ||
            attempt >= replace_attempts)
          return false;
        ::Sleep(replace_retry_delay_ms);
      }
    }

   private:
    OwnedHandle handle_;
    std::wstring path_;
  };

  [[nodiscard]] static bool write_all(
      HANDLE handle, std::span<const std::byte> data) noexcept {
    while (!data.empty()) {
      const DWORD chunk = static_cast<DWORD>(std::min(
          data.size(),
          static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
      DWORD written = 0;
      if (::WriteFile(handle, data.data(), chunk, &written, nullptr) == 0 ||
          written == 0)
        return false;
      data = data.subspan(written);
    }
    return true;
  }

  [[nodiscard]] static bool read_all(HANDLE handle,
                                     std::span<std::byte> data) noexcept {
    while (!data.empty()) {
      const DWORD chunk = static_cast<DWORD>(std::min(
          data.size(),
          static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
      DWORD received = 0;
      if (::ReadFile(handle, data.data(), chunk, &received, nullptr) == 0 ||
          received == 0)
        return false;
      data = data.subspan(received);
    }
    return true;
  }

  static Clock::time_point filetime_to_time_point(
      const FILETIME& time) noexcept {
    ULARGE_INTEGER value{};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    const std::int64_t ticks =
        value.QuadPart >= filetime_unix_epoch_offset
            ? static_cast<std::int64_t>(value.QuadPart -
                                        filetime_unix_epoch_offset)
            : -static_cast<std::int64_t>(filetime_unix_epoch_offset -
                                         value.QuadPart);
    using FiletimeDuration =
        std::chrono::duration<std::int64_t,
                              std::ratio<1, filetime_ticks_per_second>>;
    return Clock::time_point{
        std::chrono::duration_cast<Clock::duration>(FiletimeDuration{ticks})};
  }
};

auto NativeFiles::read(const std::filesystem::path& path)
    -> Expected<StoredRecord, StorageError> {
  auto file = Native::opened(
      ::CreateFileW(path.c_str(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!file)
    return unexpected(Native::missing(::GetLastError())
                          ? StorageError::not_found
                          : StorageError::read_failed);
  LARGE_INTEGER size{};
  if (::GetFileSizeEx(file.get(), &size) == 0 || size.QuadPart < 0 ||
      static_cast<std::uintmax_t>(size.QuadPart) >
          std::numeric_limits<std::size_t>::max())
    return unexpected(StorageError::read_failed);
  StoredRecord result;
  result.data.resize(static_cast<std::size_t>(size.QuadPart));
  FILETIME last_write{};
  if (!Native::read_all(file.get(), result.data) ||
      ::GetFileTime(file.get(), nullptr, nullptr, &last_write) == 0 ||
      ::CloseHandle(file.release()) == 0)
    return unexpected(StorageError::read_failed);
  result.modified_at = Native::filetime_to_time_point(last_write);
  return result;
}

auto NativeFiles::replace(const std::filesystem::path& target,
                          std::span<const std::byte> data)
    -> Expected<void, StorageError> {
  Native::TemporaryFile temporary;
  if (!temporary.create(target) || !temporary.store(data) ||
      !temporary.move_to(target))
    return unexpected(StorageError::write_failed);
  return {};
}

bool NativeFiles::create_directory(
    const std::filesystem::path& directory) noexcept {
  return ::CreateDirectoryW(directory.c_str(), nullptr) != 0;
}

bool NativeFiles::sync_directory(const std::filesystem::path&) noexcept {
  return true;
}

auto NativeFiles::modified_at(const std::filesystem::path& path) noexcept
    -> std::optional<std::chrono::system_clock::time_point> {
  WIN32_FILE_ATTRIBUTE_DATA found{};
  if (::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &found) == 0)
    return std::nullopt;
  return Native::filetime_to_time_point(found.ftLastWriteTime);
}

}  // namespace libchiaking::platform::storage
