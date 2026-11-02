/// @file
/// @brief Declares DefaultStorageProvider, the stock file-backed RecordStore.
#pragma once

#include <libchiaking/platform/storage/record_store.hpp>

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::platform {

/// @brief Stock RecordStore that keeps each record as a file under one
/// directory.
///
/// It depends only on the standard library's file system support and the
/// operating system's file API. Each RecordName maps to
/// a file named after it with a ".record" suffix, in subdirectories matching
/// the name's leading segments. Writes go to a temporary file in the same
/// directory that is then moved over the target, so a failed write leaves the
/// old record in place. Directories are created on first write, readable and
/// writable only by the owner on POSIX systems.
///
/// Every call blocks on file I/O. Calls on one instance may come from several
/// threads at once; writes to the same record race, and the last rename wins.
/// @ingroup api_platform
class DefaultStorageProvider final : public RecordStore {
 public:
  /// @brief Binds the store to a directory without touching the file system.
  ///
  /// @param root_directory Directory that holds the records. It need not
  ///     exist yet. With an empty string every write and every RecordName
  ///     operation fails.
  explicit DefaultStorageProvider(std::string root_directory);
  /// @brief Releases the store; files stay on disk.
  ~DefaultStorageProvider() override;

  /// @brief Not copyable; users hold the store by reference.
  DefaultStorageProvider(const DefaultStorageProvider&) = delete;
  /// @brief Not copy-assignable; users hold the store by reference.
  auto operator=(const DefaultStorageProvider&)
      -> DefaultStorageProvider& = delete;
  /// @brief Not movable; users hold the store by reference.
  DefaultStorageProvider(DefaultStorageProvider&&) = delete;
  /// @brief Not move-assignable; users hold the store by reference.
  auto operator=(DefaultStorageProvider&&) -> DefaultStorageProvider& = delete;

  /// @brief Reads a named record's file. See RecordStore::read.
  ///
  /// @param name Record to read.
  /// @return The file's bytes and its modification time.
  /// @retval StorageError::not_found The file does not exist.
  /// @retval StorageError::read_failed The file could not be read, or the
  ///     root directory is empty.
  [[nodiscard]] auto read(const RecordName& name)
      -> Expected<StoredRecord, StorageError> override;
  /// @brief Creates or replaces a named record's file, creating directories
  /// as needed. See RecordStore::write_atomic.
  ///
  /// @param name Record to write.
  /// @param data New contents.
  /// @return Nothing once the new file is in place.
  /// @retval StorageError::write_failed The directory or file could not be
  ///     written; any previous file is untouched.
  [[nodiscard]] auto write_atomic(const RecordName& name,
                                  std::span<const std::byte> data)
      -> Expected<void, StorageError> override;
  /// @brief Deletes a named record's file. See RecordStore::erase.
  ///
  /// @param name Record to delete.
  /// @return Nothing once the file is gone.
  /// @retval StorageError::not_found The file did not exist.
  /// @retval StorageError::remove_failed The file could not be deleted, or
  ///     the root directory is empty.
  [[nodiscard]] auto erase(const RecordName& name)
      -> Expected<void, StorageError> override;
  /// @brief Deletes a named record and the whole directory tree beneath it.
  /// See RecordStore::erase_prefix.
  ///
  /// @param prefix Subtree to delete.
  /// @return Nothing on success, including when nothing existed.
  /// @retval StorageError::remove_failed Something could not be deleted, or
  ///     the root directory is empty.
  /// @note Every file in the subtree's directory is deleted, not only record
  ///     files.
  [[nodiscard]] auto erase_prefix(const RecordName& prefix)
      -> Expected<void, StorageError> override;
  /// @brief Lists the record files at or beneath a name. See
  /// RecordStore::list.
  ///
  /// @param prefix Subtree to list.
  /// @return One entry per record file, sorted by name. Files without the
  ///     record suffix, and files whose path does not form a valid
  ///     RecordName, are skipped.
  /// @retval StorageError::read_failed A directory could not be read, or the
  ///     root directory is empty.
  [[nodiscard]] auto list(const RecordName& prefix)
      -> Expected<std::vector<RecordListing>, StorageError> override;
  /// @brief Deletes a stray file directly in the root directory. See
  /// RecordStore::erase_legacy_file.
  ///
  /// @param file_name Bare file name that passes
  ///     RecordStore::is_legacy_file_name.
  /// @return Nothing on success, including when the file does not exist.
  /// @retval StorageError::invalid_argument The name is not acceptable.
  /// @retval StorageError::remove_failed The file could not be deleted, or
  ///     the root directory is empty.
  [[nodiscard]] auto erase_legacy_file(std::string_view file_name)
      -> Expected<void, StorageError> override;

 private:
  class Implementation;
  std::unique_ptr<Implementation> implementation_;
};

}  // namespace libchiaking::platform
