/// @file
/// @brief Declares cache::ExclusionListRecord, the stored exclusion list.
#pragma once

#include "src/core/catalog/adapters/cache/cache.hpp"
#include "src/core/catalog/adapters/cache/record_format.hpp"
#include "src/core/catalog/model/cloud_stream/exclusions.hpp"

#include <libchiaking/platform/storage/record_store.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::catalog::cache {

/// @brief The stored exclusion list, one global record named by `kind`
/// and `scope`: the persisted last good snapshot of the excluded title ids.
///
/// The snapshot is persisted so that a failed refresh can still fall back to
/// it while it is less than 24 hours old. The record itself has no expiry;
/// freshness is judged by cloud_stream::ExclusionList::is_fresh_at.
/// Immutable; may be shared between threads.
class ExclusionListRecord {
 public:
  /// @brief The record belongs to no account or region.
  using Key = Everyone;

  /// @brief The stored body. Plain data.
  struct Body {
    /// @brief When the snapshot was fetched; wall-clock milliseconds.
    std::int64_t fetched_at_ms{};
    /// @brief The excluded title ids in the order the service returns them.
    std::vector<std::string> excluded_title_ids;
  };

  /// @brief The record kind and first name segment.
  static constexpr std::string_view kind = "exclusions";
  /// @brief The schema version.
  static constexpr std::int64_t schema = 1;

  /// @brief Wraps a snapshot for storing.
  ///
  /// @param list The snapshot.
  explicit ExclusionListRecord(cloud_stream::ExclusionList list) noexcept;

  /// @brief Reads a stored body back.
  ///
  /// @param body The stored body.
  /// @param key The global key.
  /// @return The record, or std::nullopt when any stored id does not parse.
  [[nodiscard]] static auto from_body(const Body& body, const Everyone& key)
      -> std::optional<ExclusionListRecord>;
  /// @brief The record's name.
  ///
  /// @param key The global key.
  /// @return The name made of `kind` and `scope`.
  [[nodiscard]] static auto name_of(const Everyone& key)
      -> std::optional<platform::RecordName>;

  /// @brief The record's key.
  ///
  /// @return The global key.
  auto key() const noexcept -> Everyone { return {}; }
  /// @brief The snapshot.
  ///
  /// @return The snapshot, valid while the record lives.
  auto list() const noexcept -> const cloud_stream::ExclusionList& {
    return list_;
  }
  /// @brief The stored body.
  ///
  /// @return The body.
  auto body() const -> Body;

  /// @brief Compares the snapshots.
  friend auto operator==(const ExclusionListRecord&, const ExclusionListRecord&)
      -> bool = default;

 private:
  /// @brief The second name segment.
  static constexpr std::string_view scope = "cloud";

  /// @brief The snapshot.
  cloud_stream::ExclusionList list_;
};

}  // namespace libchiaking::catalog::cache

/// @brief How an exclusion list record body is spelled in JSON.
template <>
struct glz::meta<libchiaking::catalog::cache::ExclusionListRecord::Body> {
  /// @brief The type described.
  using T = libchiaking::catalog::cache::ExclusionListRecord::Body;
  /// @brief The JSON keys fetchedAtMs and excludedTitleIds.
  static constexpr auto value =
      glz::object("fetchedAtMs", &T::fetched_at_ms, "excludedTitleIds",
                  &T::excluded_title_ids);
};
