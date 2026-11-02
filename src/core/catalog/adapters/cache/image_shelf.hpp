/// @file
/// @brief Declares cache::ImageShelf, the size-bounded store of downloaded
/// store images (covers, key art, screenshots) kept between runs.
///
/// Each image is one record under image/{hash of its URL}; a separate
/// image-index record lists every image with its size and when it was
/// stored and last used, so the least recently used images can be evicted
/// once the shelf exceeds its byte budget.
#pragma once

#include "src/core/catalog/adapters/cache/cache.hpp"
#include "src/core/catalog/model/unanswered.hpp"

#include <libchiaking/detail/catalog/content_url.hpp>
#include <libchiaking/detail/expected.hpp>
#include <libchiaking/platform/runtime/threading.hpp>
#include <libchiaking/platform/storage/record_store.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::catalog::cache {

/// @brief The images kept on disk, evicted least recently used first once
/// they exceed a byte budget.
///
/// The index is read lazily on first use; blobs it does not list are erased
/// then. Every public member is safe to call from any thread: the entries
/// are guarded by one lock and index writes by another, and an older index
/// snapshot is never written over a newer one. Calls block on disk I/O,
/// some of it while the entries lock is held. The cache must outlive the
/// shelf. Not copyable.
class ImageShelf {
 public:
  /// @brief Wall-clock time in milliseconds since the Unix epoch.
  using Time = Cache::Time;
  /// @brief An image's bytes, shared so callers can keep them without a
  /// copy.
  using Bytes = std::shared_ptr<const std::vector<std::byte>>;

  /// @brief One kept image as cache inspection reports it. Plain data.
  struct Held {
    /// @brief The image's URL.
    ImageRef image;
    /// @brief Its size in bytes.
    std::uint64_t bytes{};
    /// @brief When it was stored.
    Time stored_at{};
    /// @brief When it was last served.
    Time used_at{};
  };

  /// @brief One image as the stored index lists it; incomplete rows are
  /// skipped on load. Plain data.
  struct Indexed {
    /// @brief The image's URL.
    std::optional<std::string> url;
    /// @brief Its size in bytes.
    std::optional<std::int64_t> bytes;
    /// @brief When it was stored; wall-clock milliseconds.
    std::optional<std::int64_t> stored_at_ms;
    /// @brief When it was last served; wall-clock milliseconds.
    std::optional<std::int64_t> used_at_ms;
  };

  /// @brief The stored index. Plain data.
  struct Index {
    /// @brief The index schema; an index of another version is ignored.
    std::optional<std::int64_t> schema;
    /// @brief Every kept image.
    std::optional<std::vector<Indexed>> images;
  };

  /// @brief The first name segment of image blobs.
  static constexpr std::string_view shelf = "image";
  /// @brief The record name of the index.
  static constexpr std::string_view index_name = "image-index";
  /// @brief The index schema version.
  static constexpr std::int64_t schema = 1;
  /// @brief The default budget, 256 MiB.
  static constexpr std::uint64_t default_budget = 256ULL * 1024ULL * 1024ULL;
  /// @brief How much a last-used time must advance before serving an image
  /// rewrites the index.
  static constexpr std::chrono::hours use_granularity{1};

  /// @brief Opens the shelf; nothing is read until first use.
  ///
  /// @param cache Where blobs and the index live. Must outlive the shelf.
  /// @param threading Makes the shelf's locks. Must outlive the shelf.
  /// @param budget The most bytes kept; the image just stored is never
  ///     evicted, so one image larger than the budget is still kept.
  ImageShelf(Cache& cache, platform::ThreadingProvider& threading,
             std::uint64_t budget = default_budget);
  /// @brief Not copyable: the shelf owns its locks and index.
  ImageShelf(const ImageShelf&) = delete;
  /// @brief Not copyable: the shelf owns its locks and index.
  auto operator=(const ImageShelf&) -> ImageShelf& = delete;

  /// @brief Serves a kept image.
  ///
  /// Marks it used now; the index is rewritten when that moves its stored
  /// last-used time by use_granularity or more. A listed image whose blob
  /// is gone is dropped from the index.
  ///
  /// @param image The image's URL.
  /// @return The bytes, or nullptr when the image is not kept or cannot be
  ///     read.
  auto find(const ImageRef& image) -> Bytes;
  /// @brief Stores an image, evicting least recently used ones beyond the
  /// budget.
  ///
  /// @param image The image's URL.
  /// @param bytes The downloaded bytes.
  /// @return The kept bytes; Unanswered::Reason::rejected when the bytes
  ///     are empty or larger than ImageHost::largest_bytes;
  ///     Unanswered::Reason::storage when the blob cannot be written.
  [[nodiscard]] auto keep(const ImageRef& image,
                          std::span<const std::byte> bytes)
      -> Expected<Bytes, Unanswered>;
  /// @brief Erases images no longer shown; ones not kept are ignored.
  ///
  /// Every image is dropped from memory and attempted on disk even when an
  /// earlier erase fails.
  ///
  /// @param images The images' URLs.
  /// @return Nothing when every blob was erased; otherwise the failure of the
  ///     last erase that failed, Unanswered::Reason::storage.
  [[nodiscard]] auto drop(std::span<const ImageRef> images)
      -> Expected<void, Unanswered>;
  /// @brief Every kept image.
  ///
  /// @return The images in hash order; possibly empty.
  auto held() -> std::vector<Held>;
  /// @brief Erases every image and the index.
  ///
  /// @return Nothing on success; Unanswered::Reason::storage when the store
  ///     failed. The in-memory shelf is empty either way.
  [[nodiscard]] auto clear() -> Expected<void, Unanswered>;

 private:
  /// @brief One kept image in memory.
  struct Entry {
    /// @brief The image's URL.
    ImageRef image;
    /// @brief Its size in bytes.
    std::uint64_t bytes{};
    /// @brief When it was stored.
    Time stored_at{};
    /// @brief When it was last served.
    Time used_at{};
    /// @brief The last-used time the stored index holds.
    Time used_at_on_disk{};
  };

  /// @brief A serialised index waiting to be written.
  struct IndexWrite {
    /// @brief Which snapshot this is; increases with each snapshot, 0 for
    /// one that could not be serialised.
    std::uint64_t generation{};
    /// @brief The index as JSON.
    std::string text;
  };

  /// @brief The blob key of an image.
  ///
  /// @param image The image's URL.
  /// @return The 64-bit FNV-1a hash of the URL as 16 hex digits.
  static auto hash_of(const ImageRef& image) -> std::string;
  /// @brief The blob name for a hash.
  ///
  /// @param hash The hash.
  /// @return image/{hash}, or std::nullopt when not a valid name.
  [[nodiscard]] static auto name_of(std::string_view hash)
      -> std::optional<platform::RecordName>;

  /// @brief Reads the stored index.
  ///
  /// @return Its rows, or an empty list when it is absent, unreadable or of
  ///     another schema.
  [[nodiscard]] auto index() -> std::vector<Indexed>;
  /// @brief Reads the index once and erases blobs it does not list. Caller
  /// holds mutex_.
  void load();
  /// @brief Evicts least recently used images until the total fits the
  /// budget. Caller holds mutex_.
  ///
  /// @param kept The hash of the image never to evict.
  void evict_beyond_budget(std::string_view kept);
  /// @brief Serialises the entries and marks their last-used times as
  /// stored. Caller holds mutex_.
  ///
  /// @return The next generation's index.
  auto snapshot() -> IndexWrite;
  /// @brief Writes an index unless a newer one was already written.
  ///
  /// @param index The snapshot.
  void write(const IndexWrite& index);
  /// @brief The bytes of every kept image. Caller holds mutex_.
  ///
  /// @return The total in bytes.
  auto total() const noexcept -> std::uint64_t;

  /// @brief Where blobs and the index live; not owned.
  Cache& cache_;
  /// @brief The most bytes kept.
  std::uint64_t budget_;
  /// @brief Guards loaded_, entries_ and generation_.
  platform::Mutex mutex_;
  /// @brief Whether the index has been read.
  bool loaded_{};
  /// @brief Kept images by hash.
  std::map<std::string, Entry, std::less<>> entries_;
  /// @brief The latest snapshot's generation.
  std::uint64_t generation_{};
  /// @brief Guards written_ and serialises index writes.
  platform::Mutex writing_;
  /// @brief The newest generation written.
  std::uint64_t written_{};
};

}  // namespace libchiaking::catalog::cache

/// @brief How an index row is spelled in JSON.
template <>
struct glz::meta<libchiaking::catalog::cache::ImageShelf::Indexed> {
  /// @brief The type described.
  using T = libchiaking::catalog::cache::ImageShelf::Indexed;
  /// @brief The JSON keys url, bytes, storedAtMs and usedAtMs.
  static constexpr auto value =
      glz::object("url", &T::url, "bytes", &T::bytes, "storedAtMs",
                  &T::stored_at_ms, "usedAtMs", &T::used_at_ms);
};

/// @brief How the image index is spelled in JSON.
template <>
struct glz::meta<libchiaking::catalog::cache::ImageShelf::Index> {
  /// @brief The type described.
  using T = libchiaking::catalog::cache::ImageShelf::Index;
  /// @brief The JSON keys schema and images.
  static constexpr auto value =
      glz::object("schema", &T::schema, "images", &T::images);
};
