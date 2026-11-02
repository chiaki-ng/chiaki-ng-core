#include <libchiaking/platform/storage/default.hpp>

#include "native_files.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace libchiaking::platform {
class DefaultStorageProvider::Implementation {
  using Files = storage::NativeFiles;
  using Path = std::filesystem::path;

 public:
  explicit Implementation(const std::string& root) : root_(path_of(root)) {}

  [[nodiscard]] auto read(const RecordName& name) const
      -> Expected<StoredRecord, StorageError> {
    if (root_.empty()) return unexpected(StorageError::read_failed);
    return Files::read(record_path(name));
  }

  [[nodiscard]] auto write_atomic(const RecordName& name,
                                  std::span<const std::byte> data) const
      -> Expected<void, StorageError> {
    const auto target = record_path(name);
    if (root_.empty() || !ensure_directory(target.parent_path()))
      return unexpected(StorageError::write_failed);
    return Files::replace(target, data);
  }

  [[nodiscard]] auto erase(const RecordName& name) const
      -> Expected<void, StorageError> {
    if (root_.empty()) return unexpected(StorageError::remove_failed);
    const auto target = record_path(name);
    std::error_code error;
    if (!std::filesystem::remove(target, error))
      return unexpected(error ? StorageError::remove_failed
                              : StorageError::not_found);
    if (!Files::sync_directory(target.parent_path()))
      return unexpected(StorageError::remove_failed);
    return {};
  }

  [[nodiscard]] auto erase_prefix(const RecordName& prefix) const
      -> Expected<void, StorageError> {
    if (root_.empty()) return unexpected(StorageError::remove_failed);
    std::error_code error;
    if (!std::filesystem::remove(record_path(prefix), error) && error &&
        error != std::errc::not_a_directory)
      return unexpected(StorageError::remove_failed);
    const auto tree = subtree(prefix);
    if (!remove_tree(tree)) return unexpected(StorageError::remove_failed);
    (void)Files::sync_directory(tree.parent_path());
    return {};
  }

  [[nodiscard]] auto list(const RecordName& prefix) const
      -> Expected<std::vector<RecordListing>, StorageError> {
    if (root_.empty()) return unexpected(StorageError::read_failed);
    std::vector<RecordListing> held;
    const auto single = record_path(prefix);
    std::error_code error;
    if (std::filesystem::is_regular_file(single, error))
      if (auto listing = listing_of(prefix, single))
        held.push_back(std::move(*listing));
    if (!list_tree(subtree(prefix), prefix.text(), held))
      return unexpected(StorageError::read_failed);
    std::ranges::sort(held, {}, &RecordListing::name);
    return held;
  }

  [[nodiscard]] auto erase_legacy_file(std::string_view file_name) const
      -> Expected<void, StorageError> {
    if (!RecordStore::is_legacy_file_name(file_name))
      return unexpected(StorageError::invalid_argument);
    if (root_.empty()) return unexpected(StorageError::remove_failed);
    std::error_code error;
    if (!std::filesystem::remove(root_ / path_of(file_name), error))
      return error ? Expected<void, StorageError>{unexpected(
                         StorageError::remove_failed)}
                   : Expected<void, StorageError>{};
    (void)Files::sync_directory(root_);
    return {};
  }

 private:
  static constexpr std::string_view record_suffix = ".record";

  static auto path_of(std::string_view utf8) -> Path {
    return Path{std::u8string{utf8.begin(), utf8.end()}};
  }

  template <class Unit>
  static auto ascii_name(std::basic_string_view<Unit> native)
      -> std::optional<std::string> {
    if (!std::ranges::all_of(native,
                             [](Unit unit) { return unit > 0 && unit < 0x80; }))
      return std::nullopt;
    std::string name;
    name.reserve(native.size());
    for (const Unit unit : native) name.push_back(static_cast<char>(unit));
    return name;
  }

  auto subtree(const RecordName& name) const -> Path {
    return (root_ / path_of(name.text())).make_preferred();
  }

  auto record_path(const RecordName& name) const -> Path {
    return (root_ / path_of(name.text() + std::string{record_suffix}))
        .make_preferred();
  }

  static auto listing_of(const RecordName& name, const Path& file)
      -> std::optional<RecordListing> {
    std::error_code error;
    const auto size = std::filesystem::file_size(file, error);
    const auto modified = Files::modified_at(file);
    if (error || !modified) return std::nullopt;
    return RecordListing{name, static_cast<std::uint64_t>(size), *modified};
  }

  [[nodiscard]] static bool absent(const std::error_code& error) noexcept {
    return error == std::errc::no_such_file_or_directory ||
           error == std::errc::not_a_directory;
  }

  [[nodiscard]] static bool remove_tree(const Path& tree) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(tree, error);
    if (error) return absent(error);
    if (!std::filesystem::is_directory(status)) return true;
    std::filesystem::remove_all(tree, error);
    return !error || absent(error);
  }

  [[nodiscard]] static bool list_tree(const Path& directory,
                                      const std::string& relative,
                                      std::vector<RecordListing>& held) {
    std::error_code error;
    std::filesystem::directory_iterator entries{directory, error};
    if (error) return absent(error);
    for (const std::filesystem::directory_iterator end; entries != end;
         entries.increment(error)) {
      const auto& entry = *entries;
      const auto name =
          ascii_name(std::basic_string_view{entry.path().filename().native()});
      if (!name) continue;
      std::error_code status_error;
      const auto status = entry.symlink_status(status_error);
      if (status_error) {
        if (absent(status_error)) continue;
        return false;
      }
      if (std::filesystem::is_directory(status)) {
        if (!list_tree(entry.path(), relative + "/" + *name, held))
          return false;
        continue;
      }
      const std::string_view leaf{*name};
      if (!std::filesystem::is_regular_file(status) ||
          !leaf.ends_with(record_suffix))
        continue;
      const auto record = RecordName::parse(
          relative + "/" +
          std::string{leaf.substr(0, leaf.size() - record_suffix.size())});
      if (!record) continue;
      if (auto listing = listing_of(*record, entry.path()))
        held.push_back(std::move(*listing));
    }
    return !error;
  }

  [[nodiscard]] static bool ensure_directory(const Path& directory) {
    if (directory.empty()) return false;
    std::vector<Path> missing;
    std::error_code error;
    for (auto step = directory;
         !step.empty() && !std::filesystem::exists(step, error);
         step = step.parent_path()) {
      missing.push_back(step);
      if (step == step.parent_path()) break;
    }
    for (const auto& step : missing | std::views::reverse)
      (void)Files::create_directory(step);
    return std::filesystem::is_directory(directory, error);
  }

  Path root_;
};

DefaultStorageProvider::DefaultStorageProvider(std::string root_directory)
    : implementation_(std::make_unique<Implementation>(root_directory)) {}

DefaultStorageProvider::~DefaultStorageProvider() = default;

auto DefaultStorageProvider::read(const RecordName& name)
    -> Expected<StoredRecord, StorageError> {
  try {
    return implementation_->read(name);
  } catch (...) {
    return unexpected(StorageError::read_failed);
  }
}

auto DefaultStorageProvider::write_atomic(const RecordName& name,
                                          std::span<const std::byte> data)
    -> Expected<void, StorageError> {
  try {
    return implementation_->write_atomic(name, data);
  } catch (...) {
    return unexpected(StorageError::write_failed);
  }
}

auto DefaultStorageProvider::erase(const RecordName& name)
    -> Expected<void, StorageError> {
  try {
    return implementation_->erase(name);
  } catch (...) {
    return unexpected(StorageError::remove_failed);
  }
}

auto DefaultStorageProvider::erase_prefix(const RecordName& prefix)
    -> Expected<void, StorageError> {
  try {
    return implementation_->erase_prefix(prefix);
  } catch (...) {
    return unexpected(StorageError::remove_failed);
  }
}

auto DefaultStorageProvider::list(const RecordName& prefix)
    -> Expected<std::vector<RecordListing>, StorageError> {
  try {
    return implementation_->list(prefix);
  } catch (...) {
    return unexpected(StorageError::read_failed);
  }
}

auto DefaultStorageProvider::erase_legacy_file(std::string_view file_name)
    -> Expected<void, StorageError> {
  try {
    return implementation_->erase_legacy_file(file_name);
  } catch (...) {
    return unexpected(StorageError::remove_failed);
  }
}

}  // namespace libchiaking::platform
