/// @file
/// @brief Declares NativeFiles, the file operations behind the stock record
/// store: whole-file reads and crash-safe whole-file replacement.
///
/// posix.cpp and win32.cpp each define it; only one is built.
#pragma once

#include <libchiaking/platform/storage/record_store.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>

namespace libchiaking::platform::storage {

/// @brief The system's file calls as the record store needs them.
///
/// All members are static and block on disk I/O. They may be called from any
/// thread; two writers replacing the same file race, the last rename
/// winning. Files are opened so that child processes do not inherit them.
class NativeFiles {
 public:
  /// @brief Reads a whole file and when it was last written.
  ///
  /// @param path The file.
  /// @return Its bytes and last-modified wall-clock time, to the second on
  ///     POSIX.
  /// @retval StorageError::not_found The file does not exist.
  /// @retval StorageError::read_failed It exists but could not be read in
  ///     full.
  [[nodiscard]] static auto read(const std::filesystem::path& path)
      -> Expected<StoredRecord, StorageError>;

  /// @brief Replaces a file's contents so that a reader, or a crash, sees
  /// either the old contents or the new, never a mixture.
  ///
  /// Writes a temporary file beside the target, flushes it to disk and
  /// renames it over the target, then on POSIX flushes the directory too.
  /// On Windows the rename is retried for up to about 200 ms while another
  /// process holds the target open. A failure leaves the old file intact
  /// and removes the temporary one.
  ///
  /// @param target The file to create or replace; its directory must
  ///     exist.
  /// @param data The new contents.
  /// @return Nothing once the new contents are durable.
  /// @retval StorageError::write_failed Any step failed.
  [[nodiscard]] static auto replace(const std::filesystem::path& target,
                                    std::span<const std::byte> data)
      -> Expected<void, StorageError>;

  /// @brief Creates one directory, readable only by the user on POSIX
  /// (mode 0700).
  ///
  /// @param directory The directory; its parent must exist.
  /// @return True when it was created; false when it already exists or
  ///     could not be made.
  [[nodiscard]] static bool create_directory(
      const std::filesystem::path& directory) noexcept;

  /// @brief Flushes a directory's entries to disk, so that a rename in it
  /// survives a crash.
  ///
  /// @param directory The directory.
  /// @return True when flushed; always true on Windows, where the rename
  ///     is already written through.
  [[nodiscard]] static bool sync_directory(
      const std::filesystem::path& directory) noexcept;

  /// @brief Reads when a file was last written, without reading it.
  ///
  /// @param path The file.
  /// @return The wall-clock time, or nothing when the file cannot be
  ///     examined.
  [[nodiscard]] static auto modified_at(
      const std::filesystem::path& path) noexcept
      -> std::optional<std::chrono::system_clock::time_point>;

 private:
  /// @brief The system-specific helpers, defined beside each
  /// implementation.
  class Native;
};

}  // namespace libchiaking::platform::storage
