/// @file
/// @brief Declares cache::StreamableListRecord, the stored form of one
/// region's StreamableList.
///
/// Only complete lists read through the account's own service are stored;
/// public-store fallbacks and partial walks are never persisted. A record
/// is served for 24 hours and stored at streamable/{country}-{language}.
#pragma once

#include "src/core/catalog/adapters/cache/record_format.hpp"

#include <libchiaking/detail/catalog/store_locale.hpp>
#include <libchiaking/detail/catalog/streamable_list.hpp>
#include <libchiaking/platform/storage/record_store.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::catalog::cache {

/// @brief One region's stored streamable list.
///
/// Built only by of, so the list is always complete and from the account's
/// service. Immutable; may be shared between threads.
class StreamableListRecord {
 public:
  /// @brief Records are keyed by store region.
  using Key = StoreLocale;

  /// @brief The stored form of one list entry. Plain data.
  struct Listed {
    /// @brief The product id.
    std::string product;
    /// @brief The display name.
    std::string name;
    /// @brief The platform.
    Platform platform{};
    /// @brief The streaming service.
    ServiceGeneration service{};
    /// @brief What the list says about streaming it.
    ListStreamability streamability{};
    /// @brief The concept id, when known.
    std::optional<std::string> concept_id;
    /// @brief The portrait cover URL.
    std::string cover;
    /// @brief The landscape cover URL.
    std::string landscape_cover;
    /// @brief The store page URL, when known.
    std::optional<std::string> concept_page;
    /// @brief How the store page was found; required with a page.
    std::optional<StorePage::Source> concept_page_source;
    /// @brief Other products the title answers to.
    std::vector<std::string> aliases;
    /// @brief Where the PS Plus inclusion came from.
    PlusInclusion::Source inclusion_source{};
    /// @brief The lowest including tier; absent when not included.
    std::optional<users::Tier> inclusion_tier;
  };

  /// @brief The stored body. Plain data.
  struct Body {
    /// @brief The locale the store answered in.
    std::string settled_locale;
    /// @brief How the list was obtained.
    ListOrigin origin{};
    /// @brief Whether every shelf and page was read.
    bool complete{};
    /// @brief When the list was built; wall-clock milliseconds.
    std::int64_t fetched_at_ms{};
    /// @brief The streamable titles.
    std::vector<Listed> titles;
    /// @brief Included titles the store does not list for streaming.
    std::vector<Listed> included_but_not_streamable;
    /// @brief Title numbers flagged as not streamable.
    std::vector<std::string> marked_unstreamable;
    /// @brief How many PS4 products the copy check asked about.
    std::optional<std::int64_t> streaming_copies_asked;
    /// @brief How many of them PS answered.
    std::optional<std::int64_t> streaming_copies_answered;
  };

  /// @brief The record kind and first name segment.
  static constexpr std::string_view kind = "streamable";
  /// @brief The schema version.
  static constexpr std::int64_t schema = 2;
  /// @brief How long a list is served after it was built.
  static constexpr std::chrono::hours lifetime{24};
  /// @brief How long the record is kept after it was written.
  static constexpr auto kept_for = lifetime;

  /// @brief Wraps a list for storing.
  ///
  /// @param list The list.
  /// @return The record, or std::nullopt when the list is incomplete or
  ///     came from the fallback public store.
  [[nodiscard]] static auto of(StreamableList list)
      -> std::optional<StreamableListRecord>;
  /// @brief Reads a stored body back.
  ///
  /// @param body The stored body.
  /// @param region The region the record is stored under.
  /// @return The record; std::nullopt when the settled locale, an entry or
  ///     a title number does not read, the list is refused, or it is
  ///     incomplete or not from the account's service. Inconsistent copy
  ///     check counts are dropped, not refused.
  [[nodiscard]] static auto from_body(const Body& body,
                                      const StoreLocale& region)
      -> std::optional<StreamableListRecord>;
  /// @brief The record's name.
  ///
  /// @param region The region.
  /// @return streamable/{country}-{language}, lower-case.
  [[nodiscard]] static auto name_of(const StoreLocale& region)
      -> std::optional<platform::RecordName>;

  /// @brief The record's key.
  ///
  /// @return The list's region.
  auto key() const noexcept -> const StoreLocale& { return list_.region(); }
  /// @brief The list.
  ///
  /// @return The list, valid while the record lives.
  auto list() const noexcept -> const StreamableList& { return list_; }
  /// @brief The stored body.
  ///
  /// @return The body.
  auto body() const -> Body;

  /// @brief Compares the lists.
  friend auto operator==(const StreamableListRecord&,
                         const StreamableListRecord&) -> bool = default;

 private:
  /// @brief Keeps a list that of has checked.
  ///
  /// @param list The list.
  explicit StreamableListRecord(StreamableList list) noexcept;

  /// @brief The stored form of a section of entries.
  ///
  /// @param listed The entries.
  /// @return Their stored forms in order.
  static auto entries(std::span<const StreamableList::Entry> listed)
      -> std::vector<Listed>;
  /// @brief The stored form of one entry.
  ///
  /// @param listed The entry.
  /// @return Its stored form.
  static auto entry(const StreamableList::Entry& listed) -> Listed;
  /// @brief A stored entry read back.
  ///
  /// @param wire The stored entry.
  /// @return The entry, or std::nullopt when an id, the page or the
  ///     inclusion does not read or the title is refused.
  [[nodiscard]] static auto entry(const Listed& wire)
      -> std::optional<StreamableList::Entry>;
  /// @brief A stored store page read back.
  ///
  /// @param wire The stored entry.
  /// @return An empty inner optional when no page is stored, the page when
  ///     it reads, std::nullopt when a stored page has no source or is not
  ///     acceptable.
  [[nodiscard]] static auto page_of(const Listed& wire)
      -> std::optional<std::optional<StorePage>>;
  /// @brief A stored inclusion read back.
  ///
  /// @param wire The stored entry.
  /// @return Not included when no tier is stored, else the tier's
  ///     inclusion, or std::nullopt when the tier does not map to one.
  [[nodiscard]] static auto inclusion_of(const Listed& wire)
      -> std::optional<PlusInclusion>;
  /// @brief A stored title number read back.
  ///
  /// @param text The stored text.
  /// @return The number, or std::nullopt unless the text is exactly a
  ///     title number.
  [[nodiscard]] static auto marked_number(const std::string& text)
      -> std::optional<TitleNumber>;
  /// @brief The stored copy check counts read back.
  ///
  /// @param body The stored body.
  /// @return The counts, or std::nullopt when either is missing, negative,
  ///     or more were answered than asked.
  static auto copies_of(const Body& body)
      -> std::optional<StreamableList::CopiesChecked>;

  /// @brief The list.
  StreamableList list_;
};

}  // namespace libchiaking::catalog::cache

/// @brief How a stored list entry is spelled in JSON.
template <>
struct glz::meta<libchiaking::catalog::cache::StreamableListRecord::Listed> {
  /// @brief The type described.
  using T = libchiaking::catalog::cache::StreamableListRecord::Listed;
  /// @brief The JSON keys product, name, platform, service, streamability,
  /// concept, cover, landscapeCover, conceptPage, conceptPageSource, aliases,
  /// inclusionSource and inclusionTier.
  static constexpr auto value = glz::object(
      "product", &T::product, "name", &T::name, "platform", &T::platform,
      "service", &T::service, "streamability", &T::streamability, "concept",
      &T::concept_id, "cover", &T::cover, "landscapeCover", &T::landscape_cover,
      "conceptPage", &T::concept_page, "conceptPageSource",
      &T::concept_page_source, "aliases", &T::aliases, "inclusionSource",
      &T::inclusion_source, "inclusionTier", &T::inclusion_tier);
};

/// @brief How a streamable list record body is spelled in JSON.
template <>
struct glz::meta<libchiaking::catalog::cache::StreamableListRecord::Body> {
  /// @brief The type described.
  using T = libchiaking::catalog::cache::StreamableListRecord::Body;
  /// @brief The JSON keys settledLocale, origin, complete, fetchedAtMs, titles,
  /// includedButNotStreamable, markedUnstreamable, streamingCopiesAsked and
  /// streamingCopiesAnswered.
  static constexpr auto value = glz::object(
      "settledLocale", &T::settled_locale, "origin", &T::origin, "complete",
      &T::complete, "fetchedAtMs", &T::fetched_at_ms, "titles", &T::titles,
      "includedButNotStreamable", &T::included_but_not_streamable,
      "markedUnstreamable", &T::marked_unstreamable, "streamingCopiesAsked",
      &T::streaming_copies_asked, "streamingCopiesAnswered",
      &T::streaming_copies_answered);
};
