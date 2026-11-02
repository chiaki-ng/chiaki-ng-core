#include "src/core/catalog/adapters/cache/image_shelf.hpp"

#include "src/core/utils/bytes/endian.hpp"
#include "src/core/utils/text/hex.hpp"

#include <algorithm>
#include <array>
#include <numeric>
#include <utility>

namespace libchiaking::catalog::cache {

ImageShelf::ImageShelf(Cache& cache, platform::ThreadingProvider& threading,
                       std::uint64_t budget)
    : cache_(cache), budget_(budget), mutex_(threading), writing_(threading) {}

auto ImageShelf::hash_of(const ImageRef& image) -> std::string {
  constexpr std::uint64_t offset = 0xcbf29ce484222325ULL;
  constexpr std::uint64_t prime = 0x100000001b3ULL;
  const auto folded = std::accumulate(
      image.url().begin(), image.url().end(), offset,
      [](std::uint64_t hash, char unit) {
        return (hash ^ static_cast<unsigned char>(unit)) * prime;
      });
  std::array<std::byte, sizeof(std::uint64_t)> digest{};
  bytes::store_be<std::uint64_t>(digest, folded);
  std::string hex;
  text::append_hex(hex, digest);
  return hex;
}

auto ImageShelf::name_of(std::string_view hash)
    -> std::optional<platform::RecordName> {
  const auto images = platform::RecordName::parse(shelf);
  return images ? images->child(hash) : std::nullopt;
}

auto ImageShelf::total() const noexcept -> std::uint64_t {
  return std::accumulate(entries_.begin(), entries_.end(), std::uint64_t{},
                         [](std::uint64_t sum, const auto& entry) {
                           return sum + entry.second.bytes;
                         });
}

void ImageShelf::load() {
  if (loaded_) return;
  loaded_ = true;
  const auto on_disk = cache_.listed(shelf);
  if (!on_disk) return;
  for (const auto& indexed : index()) {
    auto image = ImageRef::at(indexed.url.value_or(""));
    if (!image || !indexed.stored_at_ms || !indexed.used_at_ms) continue;
    const auto hash = hash_of(*image);
    const auto name = name_of(hash);
    if (!name) continue;
    const auto blob = std::ranges::find(on_disk.value(), *name,
                                        &platform::RecordListing::name);
    if (blob != on_disk.value().end())
      entries_.insert_or_assign(
          hash,
          Entry{std::move(*image), blob->bytes, time_of(*indexed.stored_at_ms),
                time_of(*indexed.used_at_ms), time_of(*indexed.used_at_ms)});
  }
  for (const auto& listing : on_disk.value()) {
    const std::string_view text{listing.name.text()};
    const auto leaf =
        text.substr(text.rfind(platform::RecordName::separator) + 1);
    if (!entries_.contains(leaf)) (void)cache_.erase_named(listing.name);
  }
}

auto ImageShelf::index() -> std::vector<Indexed> {
  const auto name = platform::RecordName::parse(index_name);
  if (!name) return {};
  const auto stored = cache_.read_blob(*name);
  if (!stored || !stored.value()) return {};
  const auto& bytes = *stored.value();
  Index index{};
  if (json::read_into<record_reading>(
          index, std::string_view{reinterpret_cast<const char*>(bytes.data()),
                                  bytes.size()}) ||
      index.schema != schema)
    return {};
  return std::move(index.images).value_or(std::vector<Indexed>{});
}

auto ImageShelf::snapshot() -> IndexWrite {
  Index index{.schema = schema, .images = std::vector<Indexed>{}};
  index.images->reserve(entries_.size());
  for (auto& [hash, entry] : entries_) {
    index.images->push_back(
        {entry.image.url(), static_cast<std::int64_t>(entry.bytes),
         milliseconds_of(entry.stored_at), milliseconds_of(entry.used_at)});
    entry.used_at_on_disk = entry.used_at;
  }
  IndexWrite written{.generation = ++generation_};
  if (glz::write<glz::opts{}>(index, written.text)) return {};
  return written;
}

void ImageShelf::write(const IndexWrite& index) {
  const platform::Lock lock{writing_};
  if (index.generation <= written_) return;
  written_ = index.generation;
  if (const auto name = platform::RecordName::parse(index_name))
    (void)cache_.write_blob(
        *name, std::as_bytes(std::span{index.text.data(), index.text.size()}));
}

void ImageShelf::evict_beyond_budget(std::string_view kept) {
  for (auto held = total(); held > budget_;) {
    auto oldest = entries_.end();
    for (auto entry = entries_.begin(); entry != entries_.end(); ++entry)
      if (entry->first != kept &&
          (oldest == entries_.end() ||
           entry->second.used_at < oldest->second.used_at))
        oldest = entry;
    if (oldest == entries_.end()) return;
    if (const auto name = name_of(oldest->first))
      (void)cache_.erase_named(*name);
    held -= oldest->second.bytes;
    entries_.erase(oldest);
  }
}

auto ImageShelf::find(const ImageRef& image) -> Bytes {
  const auto hash = hash_of(image);
  const auto name = name_of(hash);
  if (!name) return nullptr;
  {
    const platform::Lock lock{mutex_};
    load();
    const auto found = entries_.find(hash);
    if (found == entries_.end() || found->second.image != image) return nullptr;
  }
  auto blob = cache_.read_blob(*name);
  std::optional<IndexWrite> index;
  Bytes kept;
  {
    const platform::Lock lock{mutex_};
    const auto found = entries_.find(hash);
    if (found == entries_.end() || found->second.image != image) return nullptr;
    if (!blob || !blob.value()) {
      entries_.erase(found);
      index = snapshot();
    } else {
      found->second.used_at = std::max(found->second.used_at, cache_.now());
      if (found->second.used_at - found->second.used_at_on_disk >=
          use_granularity)
        index = snapshot();
      kept = std::make_shared<const std::vector<std::byte>>(
          std::move(*blob.value()));
    }
  }
  if (index) write(*index);
  return kept;
}

auto ImageShelf::keep(const ImageRef& image, std::span<const std::byte> bytes)
    -> Expected<Bytes, Unanswered> {
  if (bytes.empty() || bytes.size() > ImageHost::largest_bytes)
    return unexpected(Unanswered{Unanswered::Reason::rejected});
  const auto hash = hash_of(image);
  const auto name = name_of(hash);
  {
    const platform::Lock lock{mutex_};
    load();
  }
  if (!name || !cache_.write_blob(*name, bytes))
    return unexpected(Unanswered{Unanswered::Reason::storage});
  auto kept = std::make_shared<const std::vector<std::byte>>(bytes.begin(),
                                                             bytes.end());
  IndexWrite index;
  {
    const platform::Lock lock{mutex_};
    const auto now = cache_.now();
    entries_.insert_or_assign(hash, Entry{image, bytes.size(), now, now, now});
    evict_beyond_budget(hash);
    index = snapshot();
  }
  write(index);
  return kept;
}

auto ImageShelf::drop(std::span<const ImageRef> images)
    -> Expected<void, Unanswered> {
  std::optional<IndexWrite> index;
  std::optional<Unanswered> failure;
  {
    const platform::Lock lock{mutex_};
    load();
    bool changed = false;
    for (const auto& image : images) {
      const auto hash = hash_of(image);
      const auto found = entries_.find(hash);
      if (found == entries_.end() || found->second.image != image) continue;
      if (const auto name = name_of(hash))
        if (auto erased = cache_.erase_named(*name); !erased)
          failure = erased.error();
      entries_.erase(found);
      changed = true;
    }
    if (changed) index = snapshot();
  }
  if (index) write(*index);
  if (failure) return unexpected(*failure);
  return {};
}

auto ImageShelf::held() -> std::vector<Held> {
  const platform::Lock lock{mutex_};
  load();
  std::vector<Held> all;
  all.reserve(entries_.size());
  for (const auto& [hash, entry] : entries_)
    all.push_back(
        Held{entry.image, entry.bytes, entry.stored_at, entry.used_at});
  return all;
}

auto ImageShelf::clear() -> Expected<void, Unanswered> {
  const platform::Lock lock{mutex_};
  loaded_ = true;
  entries_.clear();
  const platform::Lock writing{writing_};
  written_ = ++generation_;
  const auto index = platform::RecordName::parse(index_name);
  const auto erased = cache_.erase_shelf(shelf);
  const auto unlisted =
      index ? cache_.erase_named(*index) : Expected<void, Unanswered>{};
  if (!erased) return unexpected(erased.error());
  if (!unlisted) return unexpected(unlisted.error());
  return {};
}

}  // namespace libchiaking::catalog::cache
