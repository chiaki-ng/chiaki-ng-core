#include "native_files.hpp"

#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace libchiaking::platform::storage {

class NativeFiles::Native {
 public:
  using Clock = std::chrono::system_clock;

  static constexpr mode_t private_directory_mode = 0700;

  class Descriptor {
   public:
    Descriptor() noexcept = default;
    explicit Descriptor(int fd) noexcept : fd_(fd) {}

    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;

    Descriptor(Descriptor&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)) {}

    Descriptor& operator=(Descriptor&& other) noexcept {
      if (this != &other) {
        (void)close();
        fd_ = std::exchange(other.fd_, -1);
      }
      return *this;
    }

    ~Descriptor() { (void)close(); }

    auto get() const noexcept -> int { return fd_; }

    auto is_open() const noexcept -> bool { return fd_ >= 0; }

    [[nodiscard]] bool close() noexcept {
      if (!is_open()) return true;
      return ::close(std::exchange(fd_, -1)) == 0;
    }

   private:
    int fd_{-1};
  };

  class TemporaryFile {
   public:
    TemporaryFile() = default;

    ~TemporaryFile() {
      if (!path_.empty()) (void)::unlink(path_.c_str());
    }

    TemporaryFile(const TemporaryFile&) = delete;
    auto operator=(const TemporaryFile&) -> TemporaryFile& = delete;

    [[nodiscard]] bool create(const std::filesystem::path& target) {
      const std::string pattern = target.parent_path().native() + "/.tmp-" +
                                  target.filename().native() + "-XXXXXX";
      std::vector<char> buffer(pattern.begin(), pattern.end());
      buffer.push_back('\0');
      Descriptor created{create_unique(buffer.data())};
      if (!created.is_open()) return false;
      descriptor_ = std::move(created);
      path_.assign(buffer.data());
      return true;
    }

    [[nodiscard]] bool store(std::span<const std::byte> data) noexcept {
      return write_all(descriptor_.get(), data) && sync(descriptor_.get()) &&
             descriptor_.close();
    }

    [[nodiscard]] bool move_to(const std::filesystem::path& target) noexcept {
      if (::rename(path_.c_str(), target.c_str()) != 0) return false;
      path_.clear();
      return true;
    }

   private:
    Descriptor descriptor_;
    std::string path_;
  };

  [[nodiscard]] static bool write_all(
      int fd, std::span<const std::byte> data) noexcept {
    while (!data.empty()) {
      const ssize_t written = ::write(fd, data.data(), data.size());
      if (written < 0 && errno == EINTR) continue;
      if (written <= 0) return false;
      data = data.subspan(static_cast<std::size_t>(written));
    }
    return true;
  }

  [[nodiscard]] static bool read_all(int fd,
                                     std::span<std::byte> data) noexcept {
    while (!data.empty()) {
      const ssize_t received = ::read(fd, data.data(), data.size());
      if (received < 0 && errno == EINTR) continue;
      if (received <= 0) return false;
      data = data.subspan(static_cast<std::size_t>(received));
    }
    return true;
  }

  [[nodiscard]] static bool sync(int fd) noexcept {
    int result = 0;
    do {
      result = ::fsync(fd);
    } while (result != 0 && errno == EINTR);
    return result == 0;
  }

  [[nodiscard]] static int create_unique(char* pattern) noexcept {
#ifdef LIBCHIAKING_HAVE_MKOSTEMP
    return ::mkostemp(pattern, O_CLOEXEC);
#else
    const int fd = ::mkstemp(pattern);
    if (fd >= 0) (void)::fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
#endif
  }
};

auto NativeFiles::read(const std::filesystem::path& path)
    -> Expected<StoredRecord, StorageError> {
  Native::Descriptor file{::open(path.c_str(), O_RDONLY | O_CLOEXEC)};
  if (!file.is_open())
    return unexpected(errno == ENOENT ? StorageError::not_found
                                      : StorageError::read_failed);
  struct stat status{};
  if (::fstat(file.get(), &status) != 0 || status.st_size < 0 ||
      static_cast<std::uintmax_t>(status.st_size) >
          std::numeric_limits<std::size_t>::max())
    return unexpected(StorageError::read_failed);
  StoredRecord result;
  result.data.resize(static_cast<std::size_t>(status.st_size));
  if (!Native::read_all(file.get(), result.data) || !file.close())
    return unexpected(StorageError::read_failed);
  result.modified_at = Native::Clock::from_time_t(status.st_mtime);
  return result;
}

auto NativeFiles::replace(const std::filesystem::path& target,
                          std::span<const std::byte> data)
    -> Expected<void, StorageError> {
  Native::TemporaryFile temporary;
  if (!temporary.create(target) || !temporary.store(data) ||
      !temporary.move_to(target))
    return unexpected(StorageError::write_failed);
  (void)sync_directory(target.parent_path());
  return {};
}

bool NativeFiles::create_directory(
    const std::filesystem::path& directory) noexcept {
  return ::mkdir(directory.c_str(), Native::private_directory_mode) == 0;
}

bool NativeFiles::sync_directory(
    const std::filesystem::path& directory) noexcept {
  const Native::Descriptor opened{
      ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
  return opened.is_open() && Native::sync(opened.get());
}

auto NativeFiles::modified_at(const std::filesystem::path& path) noexcept
    -> std::optional<std::chrono::system_clock::time_point> {
  struct stat status{};
  if (::stat(path.c_str(), &status) != 0) return std::nullopt;
  return Native::Clock::from_time_t(status.st_mtime);
}

}  // namespace libchiaking::platform::storage
