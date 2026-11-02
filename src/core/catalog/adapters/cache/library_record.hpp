/// @file
/// @brief Declares cache::LibraryRecord, the stored form of one account's
/// library: the owned titles of both services, the bundles, the concepts
/// learned for copies, and the raw PS5 entitlement scan.
///
/// Stored at user/{account}/library and served for 24 hours, so that a
/// restart does not have to rescan the account's entitlements.
#pragma once

#include "src/core/catalog/adapters/cache/record_format.hpp"
#include "src/core/catalog/model/library/owned_ps5_games.hpp"

#include <libchiaking/detail/catalog/library.hpp>
#include <libchiaking/detail/users/account_id.hpp>
#include <libchiaking/platform/storage/record_store.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::catalog::cache {

/// @brief One account's stored library.
///
/// Immutable; may be shared between threads.
class LibraryRecord {
 public:
  /// @brief Records are keyed by account.
  using Key = AccountId;

  /// @brief The stored form of an OwnedTitle. Plain data.
  struct Owned {
    /// @brief The entitlement id.
    std::string id;
    /// @brief The display name.
    std::string name;
    /// @brief The platform.
    Platform platform{};
    /// @brief The streaming service the copy belongs to.
    ServiceGeneration service{};
    /// @brief How the copy is held.
    HeldAs held_as{};
    /// @brief The release time, when the copy has one; wall-clock
    /// milliseconds.
    std::optional<std::int64_t> release_at_ms;
    /// @brief True when the copy is release-gated with no date.
    std::optional<bool> release_unannounced;
    /// @brief The product id, when known.
    std::optional<std::string> product;
    /// @brief The concept id, when known.
    std::optional<std::string> concept_id;
    /// @brief The cover URL; empty when none.
    std::string cover;
    /// @brief Whether the copy is known to stream.
    std::optional<bool> streams;
  };

  /// @brief The stored form of a Library::Bundle. Plain data.
  struct Bundled {
    /// @brief The product bought.
    std::string product;
    /// @brief The entitlements held under it, as product ids.
    std::vector<std::string> siblings;
  };

  /// @brief A concept learned for a copy. Plain data.
  struct Grouped {
    /// @brief The copy's entitlement id.
    std::string id;
    /// @brief The concept it belongs to.
    std::string concept_id;
  };

  /// @brief The stored form of a Ps5Entitlement, field for field. Plain
  /// data.
  struct Ps5Record {
    /// @brief The entitlement id.
    std::string id;
    /// @brief The product id, when known.
    std::optional<std::string> product;
    /// @brief The SKU id, when known.
    std::optional<std::string> sku;
    /// @brief The title id, when known.
    std::optional<std::string> title;
    /// @brief The concept id, when known.
    std::optional<std::string> concept_id;
    /// @brief The display name.
    std::string name;
    /// @brief The cover URL.
    std::string cover;
    /// @brief isActive, when sent.
    std::optional<bool> active;
    /// @brief entitlementType, when sent.
    std::optional<std::int64_t> entitlement_type;
    /// @brief featureType, when sent.
    std::optional<std::int64_t> feature_type;
    /// @brief packageType.
    std::string package_type;
    /// @brief streamingSupported, when sent.
    std::optional<bool> streaming_supported;
    /// @brief isPlayTimeFullyConsumed, when sent.
    std::optional<bool> play_time_used_up;
    /// @brief preorderCoreFlag was true.
    bool preorder_core{};
    /// @brief preorderPlaceholderFlag was true.
    bool preorder_placeholder{};
    /// @brief playabilityDate text, when sent.
    std::optional<std::string> playable_from;
    /// @brief activeDate text, when sent.
    std::optional<std::string> active_date;
  };

  /// @brief The stored form of OwnedPs5Games. Plain data.
  struct Ps5Games {
    /// @brief When the scan finished; wall-clock milliseconds.
    std::int64_t fetched_at_ms{};
    /// @brief How complete the scan was.
    LibraryState state{};
    /// @brief Records the scan read; negative makes the record
    /// unreadable.
    std::int64_t records_read{};
    /// @brief Revision restarts during the scan.
    std::int64_t revision_restarts{};
    /// @brief Every scanned record.
    std::vector<Ps5Record> records;
  };

  /// @brief The stored body. Plain data.
  struct Body {
    /// @brief When the library was fetched or last amended; wall-clock
    /// milliseconds.
    std::int64_t fetched_at_ms{};
    /// @brief Every owned title of both services.
    std::vector<Owned> titles;
    /// @brief The bundles.
    std::vector<Bundled> bundles;
    /// @brief Concepts learned for copies; absent when none were.
    std::optional<std::vector<Grouped>> copy_concepts;
    /// @brief How complete PS5 ownership was.
    LibraryState ps5_ownership{};
    /// @brief The raw PS5 scan, when one was held.
    std::optional<Ps5Games> ps5_games;
  };

  /// @brief The record kind and last name segment.
  static constexpr std::string_view kind = "library";
  /// @brief The schema version.
  static constexpr std::int64_t schema = 5;
  /// @brief How long a library is served after it was fetched.
  static constexpr std::chrono::hours lifetime{24};
  /// @brief How long the record is kept on disk after it was written; an
  /// older one is discarded on read.
  static constexpr auto kept_for = lifetime;

  /// @brief Wraps a library for storing.
  ///
  /// @param library The library.
  /// @param ps5 The PS5 scan, when held.
  explicit LibraryRecord(Library library, std::optional<OwnedPs5Games> ps5 =
                                              std::nullopt) noexcept;

  /// @brief Reads a stored body back.
  ///
  /// @param body The stored body.
  /// @param account The account the record is stored under.
  /// @return The record; std::nullopt when any title, bundle, learned
  ///     concept or PS5 record does not read, two titles share an id, or
  ///     the PS5 counts are negative.
  [[nodiscard]] static auto from_body(const Body& body, AccountId account)
      -> std::optional<LibraryRecord>;
  /// @brief The record's name.
  ///
  /// @param account The account.
  /// @return user/{account}/library.
  [[nodiscard]] static auto name_of(AccountId account)
      -> std::optional<platform::RecordName>;

  /// @brief The record's key.
  ///
  /// @return The library's account.
  auto key() const noexcept -> AccountId { return library_.account(); }
  /// @brief The library.
  ///
  /// @return The library, valid while the record lives.
  auto library() const noexcept -> const Library& { return library_; }
  /// @brief The PS5 scan.
  ///
  /// @return The scan, or std::nullopt when none was stored.
  auto ps5_games() const noexcept -> const std::optional<OwnedPs5Games>& {
    return ps5_;
  }
  /// @brief The stored body.
  ///
  /// @return The body.
  auto body() const -> Body;

  /// @brief Compares the library and the scan.
  friend auto operator==(const LibraryRecord&, const LibraryRecord&)
      -> bool = default;

 private:
  /// @brief The stored form of an owned title.
  ///
  /// @param title The title.
  /// @return Its stored form.
  static auto owned(const OwnedTitle& title) -> Owned;
  /// @brief A stored owned title read back.
  ///
  /// @param wire The stored title.
  /// @return The title, or std::nullopt when an id, the release or the
  ///     title itself is refused.
  [[nodiscard]] static auto owned(const Owned& wire)
      -> std::optional<OwnedTitle>;
  /// @brief A stored release read back.
  ///
  /// @param wire The stored title.
  /// @return Unannounced when so marked with no time; unrestricted with
  ///     neither; the time when one is stored; std::nullopt when marked
  ///     unannounced with a time, marked false, or the time reads as
  ///     unrestricted.
  static auto release_of(const Owned& wire) -> std::optional<ReleaseDate>;
  /// @brief A stored bundle read back.
  ///
  /// @param wire The stored bundle.
  /// @return The bundle, or std::nullopt when a product id does not parse.
  [[nodiscard]] static auto bundle(const Bundled& wire)
      -> std::optional<Library::Bundle>;
  /// @brief A stored learned concept read back.
  ///
  /// @param wire The stored concept.
  /// @return The concept for the entitlement (no product), or std::nullopt
  ///     when an id does not parse.
  [[nodiscard]] static auto learned(const Grouped& wire)
      -> std::optional<Library::LearnedConcept>;
  /// @brief The stored form of a PS5 scan.
  ///
  /// @param games The scan.
  /// @return Its stored form.
  static auto ps5_part(const OwnedPs5Games& games) -> Ps5Games;
  /// @brief A stored PS5 scan read back.
  ///
  /// @param wire The stored scan.
  /// @return The scan, or std::nullopt when a record does not read or the
  ///     read count is negative.
  [[nodiscard]] static auto ps5_part(const Ps5Games& wire)
      -> std::optional<OwnedPs5Games>;
  /// @brief The stored form of a PS5 record.
  ///
  /// @param record The record.
  /// @return Its stored form.
  static auto ps5_record(const Ps5Entitlement& record) -> Ps5Record;
  /// @brief A stored PS5 record read back.
  ///
  /// @param wire The stored record.
  /// @return The record, or std::nullopt when a stored id does not parse.
  [[nodiscard]] static auto ps5_record(const Ps5Record& wire)
      -> std::optional<Ps5Entitlement>;

  /// @brief The library.
  Library library_;
  /// @brief The PS5 scan, when held.
  std::optional<OwnedPs5Games> ps5_;
};

}  // namespace libchiaking::catalog::cache

/// @brief How a stored owned title is spelled in JSON.
template <>
struct glz::meta<libchiaking::catalog::cache::LibraryRecord::Owned> {
  /// @brief The type described.
  using T = libchiaking::catalog::cache::LibraryRecord::Owned;
  /// @brief The JSON keys id, name, platform, service, heldAs, releaseAtMs,
  /// releaseUnannounced, product, concept, cover and streams.
  static constexpr auto value = glz::object(
      "id", &T::id, "name", &T::name, "platform", &T::platform, "service",
      &T::service, "heldAs", &T::held_as, "releaseAtMs", &T::release_at_ms,
      "releaseUnannounced", &T::release_unannounced, "product", &T::product,
      "concept", &T::concept_id, "cover", &T::cover, "streams", &T::streams);
};

/// @brief How a stored bundle is spelled in JSON.
template <>
struct glz::meta<libchiaking::catalog::cache::LibraryRecord::Bundled> {
  /// @brief The type described.
  using T = libchiaking::catalog::cache::LibraryRecord::Bundled;
  /// @brief The JSON keys product and siblings.
  static constexpr auto value =
      glz::object("product", &T::product, "siblings", &T::siblings);
};

/// @brief How a stored learned concept is spelled in JSON.
template <>
struct glz::meta<libchiaking::catalog::cache::LibraryRecord::Grouped> {
  /// @brief The type described.
  using T = libchiaking::catalog::cache::LibraryRecord::Grouped;
  /// @brief The JSON keys id and concept.
  static constexpr auto value =
      glz::object("id", &T::id, "concept", &T::concept_id);
};

/// @brief How a stored PS5 entitlement is spelled in JSON.
template <>
struct glz::meta<libchiaking::catalog::cache::LibraryRecord::Ps5Record> {
  /// @brief The type described.
  using T = libchiaking::catalog::cache::LibraryRecord::Ps5Record;
  /// @brief The JSON keys id, product, sku, title, concept, name, cover,
  /// active, entitlementType, featureType, packageType, streamingSupported,
  /// preorderCore, preorderPlaceholder, playableFrom and activeDate.
  static constexpr auto value = glz::object(
      "id", &T::id, "product", &T::product, "sku", &T::sku, "title", &T::title,
      "concept", &T::concept_id, "name", &T::name, "cover", &T::cover, "active",
      &T::active, "entitlementType", &T::entitlement_type, "featureType",
      &T::feature_type, "packageType", &T::package_type, "streamingSupported",
      &T::streaming_supported, "playTimeUsedUp", &T::play_time_used_up,
      "preorderCore", &T::preorder_core, "preorderPlaceholder",
      &T::preorder_placeholder, "playableFrom", &T::playable_from, "activeDate",
      &T::active_date);
};

/// @brief How a stored PS5 scan is spelled in JSON.
template <>
struct glz::meta<libchiaking::catalog::cache::LibraryRecord::Ps5Games> {
  /// @brief The type described.
  using T = libchiaking::catalog::cache::LibraryRecord::Ps5Games;
  /// @brief The JSON keys fetchedAtMs, state, recordsRead, revisionRestarts and
  /// records.
  static constexpr auto value =
      glz::object("fetchedAtMs", &T::fetched_at_ms, "state", &T::state,
                  "recordsRead", &T::records_read, "revisionRestarts",
                  &T::revision_restarts, "records", &T::records);
};

/// @brief How a library record body is spelled in JSON.
template <>
struct glz::meta<libchiaking::catalog::cache::LibraryRecord::Body> {
  /// @brief The type described.
  using T = libchiaking::catalog::cache::LibraryRecord::Body;
  /// @brief The JSON keys fetchedAtMs, titles, bundles and copyConcepts.
  static constexpr auto value =
      glz::object("fetchedAtMs", &T::fetched_at_ms, "titles", &T::titles,
                  "bundles", &T::bundles, "copyConcepts", &T::copy_concepts,
                  "ps5Ownership", &T::ps5_ownership, "ps5Games", &T::ps5_games);
};
