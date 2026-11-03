/// @file
/// @brief The shared vocabulary of the catalog's cache records: the read
/// options, the millisecond time encoding, the helpers that turn stored text
/// back into checked ids, and the JSON spelling of the catalog enumerations.
///
/// Every record kind is stored as JSON inside a cache::Envelope. A record
/// whose text does not parse back into the model is discarded rather than
/// half-read, so each helper here reports a single bad value as a failure
/// of the whole record.
#pragma once

#include "libchiaking/detail/catalog/wall_time.hpp"

#include <libchiaking/detail/catalog/library.hpp>
#include <libchiaking/detail/catalog/owned_title.hpp>
#include <libchiaking/detail/catalog/platform.hpp>
#include <libchiaking/detail/catalog/plus_inclusion.hpp>
#include <libchiaking/detail/catalog/store_page.hpp>
#include <libchiaking/detail/catalog/stream_refusal.hpp>
#include <libchiaking/detail/catalog/streamable_list.hpp>
#include <libchiaking/detail/catalog/title.hpp>
#include <libchiaking/detail/users/membership.hpp>

#include "src/core/utils/json/json.hpp"
#include "src/core/utils/timing/wall_milliseconds.hpp"

#include <glaze/json.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::catalog::cache {

/// @brief The glaze options every record is read with: unknown keys are
/// ignored so newer writers stay readable, missing keys fail the read.
inline constexpr glz::opts record_reading{.error_on_unknown_keys = false,
                                          .error_on_missing_keys = true};

/// @brief Wall-clock time in milliseconds since the Unix epoch, as records
/// store it.
using Time = catalog::Time;

using timing::milliseconds_of;
using timing::time_of;

/// @brief Reads an optional stored id, telling "absent" from "invalid".
///
/// @tparam Id The checked id type.
/// @tparam Parse A callable taking the text and returning std::optional<Id>.
/// @param text The stored text, or std::nullopt when the field was absent.
/// @param parse The id's parser.
/// @return An empty inner optional when the field was absent, the id when it
///     parsed, and std::nullopt (a corrupt record) when it did not.
template <class Id, class Parse>
[[nodiscard]] auto stated(const std::optional<std::string>& text, Parse parse)
    -> std::optional<std::optional<Id>> {
  if (!text) return std::optional<Id>{};
  auto parsed = parse(*text);
  if (!parsed) return std::nullopt;
  return std::optional<Id>{std::move(*parsed)};
}

/// @brief Reads a list of stored ids, all or nothing.
///
/// @tparam Id The checked id type.
/// @tparam Parse A callable taking the text and returning std::optional<Id>.
/// @param texts The stored texts in order.
/// @param parse The id's parser.
/// @return The ids in order, or std::nullopt when any text does not parse.
template <class Id, class Parse>
[[nodiscard]] auto every(const std::vector<std::string>& texts, Parse parse)
    -> std::optional<std::vector<Id>> {
  std::vector<Id> all;
  all.reserve(texts.size());
  for (const auto& text : texts) {
    auto parsed = parse(text);
    if (!parsed) return std::nullopt;
    all.push_back(std::move(*parsed));
  }
  return all;
}

/// @brief Reads a list of stored objects, all or nothing.
///
/// @tparam Value The model type produced.
/// @tparam Read A callable taking one Wire and returning std::optional<Value>.
/// @tparam Wire The stored form of one object.
/// @param wires The stored objects in order.
/// @param read The object's reader.
/// @return The values in order, or std::nullopt when any object does not read.
template <class Value, class Read, class Wire>
[[nodiscard]] auto every_read(const std::vector<Wire>& wires, Read read)
    -> std::optional<std::vector<Value>> {
  std::vector<Value> all;
  all.reserve(wires.size());
  for (const auto& wire : wires) {
    auto value = read(wire);
    if (!value) return std::nullopt;
    all.push_back(std::move(*value));
  }
  return all;
}

/// @brief The stored texts of a range of ids.
///
/// @tparam Values A range of values with a text() member.
/// @param values The ids.
/// @return Each id's text in order.
template <std::ranges::input_range Values>
auto texts_of(const Values& values) -> std::vector<std::string> {
  std::vector<std::string> all;
  for (const auto& value : values) all.push_back(value.text());
  return all;
}

}  // namespace libchiaking::catalog::cache

/// @brief How a PS Plus tier is spelled in a cache record.
template <>
struct glz::meta<libchiaking::users::Tier> {
  using enum libchiaking::users::Tier;
  /// @brief The stored names, which match the enumerator names.
  static constexpr auto value = glz::enumerate(
      "none", none, "essential", essential, "extra", extra, "premium", premium);
};

/// @brief How a library state is spelled in a cache record.
template <>
struct glz::meta<libchiaking::catalog::LibraryState> {
  using enum libchiaking::catalog::LibraryState;
  /// @brief The stored names, which match the enumerator names.
  static constexpr auto value = glz::enumerate(
      "fetched", fetched, "unavailable", unavailable, "partial", partial);
};

/// @brief How a platform is spelled in a cache record.
template <>
struct glz::meta<libchiaking::catalog::Platform> {
  using enum libchiaking::catalog::Platform;
  /// @brief The stored names, which match the enumerator names.
  static constexpr auto value =
      glz::enumerate("ps3", ps3, "ps4", ps4, "ps5", ps5);
};

/// @brief How a streaming service generation is spelled in a cache record.
template <>
struct glz::meta<libchiaking::catalog::ServiceGeneration> {
  using enum libchiaking::catalog::ServiceGeneration;
  /// @brief The stored names, which match the enumerator names.
  static constexpr auto value =
      glz::enumerate("old_service", old_service, "ps5_cloud", ps5_cloud);
};

/// @brief How what a list says about streaming is spelled in a cache record.
template <>
struct glz::meta<libchiaking::catalog::ListStreamability> {
  using enum libchiaking::catalog::ListStreamability;
  /// @brief The stored names, which match the enumerator names.
  static constexpr auto value =
      glz::enumerate("listed", listed, "unknown", unknown,
                     "marked_unstreamable", marked_unstreamable);
};

/// @brief How how a list was obtained is spelled in a cache record.
template <>
struct glz::meta<libchiaking::catalog::ListOrigin> {
  using enum libchiaking::catalog::ListOrigin;
  /// @brief The stored names, which match the enumerator names.
  static constexpr auto value =
      glz::enumerate("account_service", account_service, "public_store",
                     public_store, "regional_store", regional_store);
};

/// @brief How the source of a PS Plus inclusion is spelled in a cache record.
template <>
struct glz::meta<libchiaking::catalog::PlusInclusion::Source> {
  using enum libchiaking::catalog::PlusInclusion::Source;
  /// @brief The stored names, which match the enumerator names.
  static constexpr auto value =
      glz::enumerate("store_tags", store_tags, "store_lists", store_lists,
                     "old_service", old_service);
};

/// @brief How how a store page was found is spelled in a cache record.
template <>
struct glz::meta<libchiaking::catalog::StorePage::Source> {
  using enum libchiaking::catalog::StorePage::Source;
  /// @brief The stored names, which match the enumerator names.
  static constexpr auto value =
      glz::enumerate("product", product, "concept_page", concept_page);
};

/// @brief How how a copy is held is spelled in a cache record.
template <>
struct glz::meta<libchiaking::catalog::HeldAs> {
  using enum libchiaking::catalog::HeldAs;
  /// @brief The stored names, which match the enumerator names.
  static constexpr auto value =
      glz::enumerate("full_game", full_game, "trial_access", trial_access,
                     "disc_upgrade", disc_upgrade, "other_game", other_game);
};

/// @brief How the kind of a remembered refusal is spelled in a cache record.
template <>
struct glz::meta<libchiaking::catalog::StreamRefusal::Kind> {
  using enum libchiaking::catalog::StreamRefusal::Kind;
  /// @brief The stored names, which match the enumerator names.
  static constexpr auto value =
      glz::enumerate("no_game_for_entitlement", no_game_for_entitlement,
                     "refused_by_stream_server", refused_by_stream_server,
                     "trial_ended", trial_ended);
};

/// @brief How the holding state of a remembered refusal is spelled in a cache
/// record.
template <>
struct glz::meta<libchiaking::catalog::StreamRefusal::Holding::State> {
  using enum libchiaking::catalog::StreamRefusal::Holding::State;
  /// @brief The stored names, which match the enumerator names.
  static constexpr auto value =
      glz::enumerate("unknown", unknown, "not_held", not_held, "held", held);
};
