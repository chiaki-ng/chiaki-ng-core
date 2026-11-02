/// @file
/// @brief Declares InMemoryRecordStore, a record store kept in memory.
#pragma once

#include "libchiaking/platform/runtime/threading.hpp"
#include "libchiaking/platform/storage/record_store.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::testing {

/// @brief Stands in for platform::RecordStore without touching a filesystem.
///
/// Records live in a map for the life of the object, so anything the library
/// caches or persists through a platform::RecordStore can be inspected and is
/// gone when the test ends. Seed it by calling write_atomic() directly, run the
/// code under test, then read back names(), read() and the counters.
///
/// Failures are scripted with fail_writes and fail_lists. Every record is
/// stamped with modified_at at the moment it is written, which lets a test
/// write records of different ages by changing modified_at between writes.
///
/// @note The store's own functions lock internally. The public data members are
///     plain fields; set them before the code under test runs and read them
///     after it has stopped. No call blocks beyond taking that lock.
/// @ingroup api_testing
class InMemoryRecordStore final : public platform::RecordStore {
 public:
  /// @brief Creates an empty store.
  ///
  /// @param threading Makes the lock that guards the store; must outlive it.
  explicit InMemoryRecordStore(platform::ThreadingProvider& threading)
      : mutex_(threading) {}

  /// @brief Returns a stored record and counts the attempt.
  ///
  /// @param name The record to fetch.
  /// @return A copy of the record's bytes and the time it was written.
  /// @retval platform::StorageError::not_found No record has that name. The
  ///     attempt is still added to reads.
  [[nodiscard]] auto read(const platform::RecordName& name)
      -> Expected<platform::StoredRecord, platform::StorageError> override {
    const platform::Lock lock{mutex_};
    ++reads;
    const auto found = records_.find(name);
    if (found == records_.end())
      return unexpected(platform::StorageError::not_found);
    return found->second;
  }

  /// @brief Stores a record, replacing any record of the same name.
  ///
  /// The record's modification time is the value of modified_at at this
  /// moment, not the wall clock.
  ///
  /// @param name The record to create or replace.
  /// @param data The bytes to keep; they are copied.
  /// @return Nothing on success, after adding one to writes.
  /// @retval platform::StorageError::write_failed fail_writes is set. Nothing
  ///     is stored and writes is not incremented.
  [[nodiscard]] auto write_atomic(const platform::RecordName& name,
                                  std::span<const std::byte> data)
      -> Expected<void, platform::StorageError> override {
    const platform::Lock lock{mutex_};
    if (fail_writes) return unexpected(platform::StorageError::write_failed);
    ++writes;
    records_.insert_or_assign(
        name,
        platform::StoredRecord{std::vector<std::byte>{data.begin(), data.end()},
                               modified_at});
    return {};
  }

  /// @brief Removes one record.
  ///
  /// @param name The record to remove.
  /// @return Nothing on success.
  /// @retval platform::StorageError::not_found No record has that name.
  /// @note fail_writes does not affect erasing.
  [[nodiscard]] auto erase(const platform::RecordName& name)
      -> Expected<void, platform::StorageError> override {
    const platform::Lock lock{mutex_};
    if (records_.erase(name) == 0)
      return unexpected(platform::StorageError::not_found);
    return {};
  }

  /// @brief Removes every record whose name lies under a prefix.
  ///
  /// @param prefix The name to match with platform::RecordName::is_under.
  /// @return Always succeeds, including when nothing matched.
  [[nodiscard]] auto erase_prefix(const platform::RecordName& prefix)
      -> Expected<void, platform::StorageError> override {
    const platform::Lock lock{mutex_};
    std::erase_if(records_, [&prefix](const auto& record) {
      return record.first.is_under(prefix);
    });
    return {};
  }

  /// @brief Lists the records under a prefix.
  ///
  /// @param prefix The name to match with platform::RecordName::is_under.
  /// @return One listing per matching record with its name, size in bytes and
  ///     modification time, in name order.
  /// @retval platform::StorageError::read_failed fail_lists is set.
  [[nodiscard]] auto list(const platform::RecordName& prefix)
      -> Expected<std::vector<platform::RecordListing>,
                  platform::StorageError> override {
    const platform::Lock lock{mutex_};
    if (fail_lists) return unexpected(platform::StorageError::read_failed);
    std::vector<platform::RecordListing> held;
    for (const auto& [name, record] : records_)
      if (name.is_under(prefix))
        held.push_back({name, record.data.size(), record.modified_at});
    return held;
  }

  /// @brief Removes a name from legacy_files.
  ///
  /// @param file_name The legacy file to remove.
  /// @return Nothing on success, whether or not the name was present.
  /// @retval platform::StorageError::invalid_argument The name is not one
  ///     platform::RecordStore::is_legacy_file_name accepts. legacy_files is
  ///     left alone.
  [[nodiscard]] auto erase_legacy_file(std::string_view file_name)
      -> Expected<void, platform::StorageError> override {
    const platform::Lock lock{mutex_};
    if (!platform::RecordStore::is_legacy_file_name(file_name))
      return unexpected(platform::StorageError::invalid_argument);
    const auto found = legacy_files.find(file_name);
    if (found != legacy_files.end()) legacy_files.erase(found);
    return {};
  }

  /// @brief Lists the name of every record held, for assertions.
  ///
  /// @return A snapshot in name order. Unlike list() it is not affected by
  ///     fail_lists.
  auto names() -> std::vector<platform::RecordName> {
    const platform::Lock lock{mutex_};
    std::vector<platform::RecordName> held;
    for (const auto& [name, record] : records_) held.push_back(name);
    return held;
  }

  /// @brief When true, write_atomic() fails with
  /// platform::StorageError::write_failed.
  bool fail_writes{};
  /// @brief When true, list() fails with platform::StorageError::read_failed.
  bool fail_lists{};
  /// @brief Number of read() calls so far, including ones that found nothing.
  std::size_t reads{};
  /// @brief Number of write_atomic() calls that stored a record.
  std::size_t writes{};
  /// @brief Modification time given to records written from now on.
  ///
  /// Defaults to the clock's epoch. Records already written keep the time
  /// they were given.
  std::chrono::system_clock::time_point modified_at{};
  /// @brief The legacy files that "exist" for erase_legacy_file() to remove.
  ///
  /// A test inserts names here to play an installation that still has files
  /// from an older layout, then checks which ones the library cleaned up.
  std::set<std::string, std::less<>> legacy_files{};

 private:
  platform::Mutex mutex_;
  std::map<platform::RecordName, platform::StoredRecord> records_;
};

}  // namespace libchiaking::testing
