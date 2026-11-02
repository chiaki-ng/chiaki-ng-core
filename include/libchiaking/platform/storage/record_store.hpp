/// @file
/// @brief Declares RecordStore, the persistent store addressed by hierarchical
/// names, the RecordName that addresses it, and the StorageError its calls
/// report.
#pragma once

#include <libchiaking/detail/expected.hpp>

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::platform {

/// @brief Reason a storage operation failed.
enum class StorageError : std::uint8_t {
  /// @brief No such record exists.
  not_found,
  /// @brief The record identifier or file name is not acceptable.
  invalid_argument,
  /// @brief The record exists but could not be read, or a listing could not
  /// be completed.
  read_failed,
  /// @brief The new contents could not be stored; any previous contents are
  /// intact.
  write_failed,
  /// @brief The record could not be removed.
  remove_failed,
  /// @brief The backend does not offer this operation.
  unsupported,
};

/// @brief The contents of one record as returned by a read.
struct StoredRecord {
  /// @brief The complete bytes last written to the record.
  std::vector<std::byte> data;
  /// @brief Wall-clock time of the last successful write, as the backend
  /// recorded it.
  std::chrono::system_clock::time_point modified_at;
};

/// @brief A validated, slash-separated name for a record in a RecordStore.
///
/// A name is one or more segments joined by separator. Each segment is 1 to
/// segment_limit characters drawn from lower-case ASCII letters, digits, hyphen
/// and underscore, and is not a reserved Windows device name such as "con" or
/// "com1". The whole name is at most limit characters. These rules make every
/// name safe to use as a relative path on any supported system, so an instance
/// can only be obtained from parse() or child().
class RecordName {
 public:
  /// @brief Longest whole name, in characters.
  static constexpr std::size_t limit = 160;
  /// @brief Longest single segment, in characters.
  static constexpr std::size_t segment_limit = 48;
  /// @brief Character that joins segments.
  static constexpr char separator = '/';

  /// @brief Validates text as a record name.
  ///
  /// @param text Candidate name, for example "catalog/ps5/titles".
  /// @return The name, or std::nullopt when the text is empty, too long, or
  ///     contains an empty or invalid segment.
  [[nodiscard]] static auto parse(std::string_view text)
      -> std::optional<RecordName>;

  /// @brief The name as text, with segments joined by separator.
  ///
  /// @return The validated text, for example "catalog/ps5/titles". Valid
  ///     while the name exists.
  auto text() const noexcept -> const std::string& { return text_; }
  /// @brief Whether this name is the given prefix or lies beneath it.
  ///
  /// Comparison is by whole segments: "a/bc" is not under "a/b".
  ///
  /// @param prefix Name of a subtree.
  /// @return True when this name equals @p prefix or extends it by further
  ///     segments.
  auto is_under(const RecordName& prefix) const noexcept -> bool;
  /// @brief Builds the name one segment below this one.
  ///
  /// @param segment Segment to append, subject to the segment rules.
  /// @return The longer name, or std::nullopt when the segment is invalid or
  ///     the result would exceed limit.
  [[nodiscard]] auto child(std::string_view segment) const
      -> std::optional<RecordName>;

  /// @brief Whether two names are the same text.
  friend auto operator==(const RecordName&, const RecordName&)
      -> bool = default;
  /// @brief Orders names by their text.
  friend auto operator<=>(const RecordName&, const RecordName&)
      -> std::strong_ordering = default;

 private:
  explicit RecordName(std::string_view text) : text_(text) {}

  static auto valid_segment(std::string_view segment) noexcept -> bool;

  std::string text_;
};

/// @brief One entry returned by RecordStore::list.
struct RecordListing {
  /// @brief Full name of the record.
  RecordName name;
  /// @brief Size of the record's contents in bytes.
  std::uint64_t bytes{};
  /// @brief Wall-clock time of the last successful write.
  std::chrono::system_clock::time_point modified_at{};
};

/// @brief Persistent key-value store addressed by RecordName.
///
/// Implemented by the host frontend, or by DefaultStorageProvider. The frontend
/// passes it to the library as ChiakiNg::Stores::records, where it backs the
/// catalogue's caches; it is held by reference and must outlive the library
/// handle. Names form a tree, so a whole group of records can be listed or
/// erased by prefix.
///
/// Implementations must be safe for concurrent calls: the library reads,
/// writes, lists and erases records from several threads at once on one
/// store. Calls on different records must not interfere. Calls on the same
/// record are serialised as if made one after another: of two writes, the one
/// that completes last wins, and a write is never torn, so a concurrent read
/// sees the whole old contents or the whole new contents. A listing taken
/// during writes may include or omit the records being written.
/// DefaultStorageProvider meets this.
/// @ingroup api_platform
class RecordStore {
 public:
  /// @brief Not copyable; users hold the store by reference.
  RecordStore(const RecordStore&) = delete;
  /// @brief Not copy-assignable; users hold the store by reference.
  auto operator=(const RecordStore&) -> RecordStore& = delete;

 protected:
  /// @brief Constructs the base of an implementation.
  RecordStore() = default;

 public:
  /// @brief Destroys the store. The library never deletes a store it was
  /// given.
  virtual ~RecordStore() = default;

  /// @brief Reads a whole record.
  ///
  /// @param name Record to read.
  /// @return The record's bytes and modification time.
  /// @retval StorageError::not_found No record has that name.
  /// @retval StorageError::read_failed The record could not be read.
  [[nodiscard]] virtual auto read(const RecordName& name)
      -> Expected<StoredRecord, StorageError> = 0;
  /// @brief Creates or replaces a record in one step.
  ///
  /// A reader sees either the old contents or the new, never a mixture, and a
  /// failed write leaves any previous record intact.
  ///
  /// @param name Record to write.
  /// @param data New contents. Not retained after the call.
  /// @return Nothing once the new contents are stored.
  /// @retval StorageError::write_failed The contents could not be stored.
  [[nodiscard]] virtual auto write_atomic(const RecordName& name,
                                          std::span<const std::byte> data)
      -> Expected<void, StorageError> = 0;
  /// @brief Removes one record.
  ///
  /// @param name Record to remove.
  /// @return Nothing once the record is gone.
  /// @retval StorageError::not_found No record has that name.
  /// @retval StorageError::remove_failed The record could not be removed.
  [[nodiscard]] virtual auto erase(const RecordName& name)
      -> Expected<void, StorageError> = 0;
  /// @brief Removes a record and everything beneath it.
  ///
  /// @param prefix Name of the subtree to remove.
  /// @return Nothing on success, including when nothing was stored there.
  /// @retval StorageError::remove_failed Something could not be removed; part
  ///     of the subtree may already be gone.
  [[nodiscard]] virtual auto erase_prefix(const RecordName& prefix)
      -> Expected<void, StorageError> = 0;
  /// @brief Lists the records at or beneath a name.
  ///
  /// @param prefix Name of the subtree to list.
  /// @return One entry per record, sorted by name; the record named exactly
  ///     @p prefix is included if it exists. Empty when nothing is stored
  ///     there.
  /// @retval StorageError::read_failed The subtree could not be read.
  [[nodiscard]] virtual auto list(const RecordName& prefix)
      -> Expected<std::vector<RecordListing>, StorageError> = 0;
  /// @brief Removes a file left in the store's location by an older storage
  /// layout.
  ///
  /// @param file_name Bare file name that passes is_legacy_file_name().
  /// @return Nothing on success, including when no such file exists.
  /// @retval StorageError::invalid_argument @p file_name does not pass
  ///     is_legacy_file_name().
  /// @retval StorageError::remove_failed The file exists but could not be
  ///     removed.
  [[nodiscard]] virtual auto erase_legacy_file(std::string_view file_name)
      -> Expected<void, StorageError> = 0;

  /// @brief Whether a file name is acceptable to erase_legacy_file().
  ///
  /// The rules keep the call inside the store's own directory and away from
  /// current records.
  ///
  /// @param file_name Candidate name.
  /// @return True for a non-empty name of at most 96 characters that does not
  ///     start with a dot, contains no slash, backslash, colon or NUL, and
  ///     does not end in ".record".
  static auto is_legacy_file_name(std::string_view file_name) noexcept -> bool;
};

}  // namespace libchiaking::platform
