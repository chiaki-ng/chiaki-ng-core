/// @file
/// @brief Declares cache::RefusalRecord, the stored form of one account's
/// remembered streaming refusals.
///
/// Stored at user/{account}/refusals. A record older than
/// StreamRefusal::lifetime is discarded on read.
#pragma once

#include "src/core/catalog/adapters/cache/record_format.hpp"

#include <libchiaking/detail/catalog/stream_refusal.hpp>
#include <libchiaking/detail/users/account_id.hpp>
#include <libchiaking/platform/storage/record_store.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::catalog::cache {

/// @brief One account's stored refusals, at most one per product, all for
/// that account.
///
/// Immutable; may be shared between threads.
class RefusalRecord {
 public:
  /// @brief Records are keyed by account.
  using Key = AccountId;

  /// @brief The stored form of one StreamRefusal. Plain data.
  struct Refused {
    /// @brief The product refused.
    std::string product;
    /// @brief How PS refused.
    StreamRefusal::Kind kind{};
    /// @brief When; wall-clock milliseconds.
    std::int64_t refused_at_ms{};
    /// @brief The tier at the time, when known.
    std::optional<users::Tier> tier;
    /// @brief Whether the title was held at the time.
    StreamRefusal::Holding::State ownership{};
    /// @brief The entitlement it was held as; required when held.
    std::optional<std::string> ownership_id;
  };

  /// @brief The stored body. Plain data.
  struct Body {
    /// @brief The refusals, ordered by product.
    std::vector<Refused> refusals;
  };

  /// @brief The record kind and last name segment.
  static constexpr std::string_view kind = "refusals";
  /// @brief The schema version.
  static constexpr std::int64_t schema = 2;
  /// @brief How long the record is kept after it was written.
  static constexpr auto kept_for = StreamRefusal::lifetime;

  /// @brief Builds a record from refusals.
  ///
  /// @param account The account.
  /// @param refusals The refusals, in any order.
  /// @return The record with refusals sorted by product, or std::nullopt
  ///     when two share a product or one is for another account.
  [[nodiscard]] static auto of(AccountId account,
                               std::vector<StreamRefusal> refusals)
      -> std::optional<RefusalRecord>;
  /// @brief Reads a stored body back.
  ///
  /// @param body The stored body.
  /// @param account The account the record is stored under.
  /// @return The record; std::nullopt when a product or held entitlement
  ///     id does not parse, a held refusal lacks its entitlement, or of
  ///     refuses the set.
  [[nodiscard]] static auto from_body(const Body& body, AccountId account)
      -> std::optional<RefusalRecord>;
  /// @brief The record's name.
  ///
  /// @param account The account.
  /// @return user/{account}/refusals.
  [[nodiscard]] static auto name_of(AccountId account)
      -> std::optional<platform::RecordName>;

  /// @brief The record's key.
  ///
  /// @return The account.
  auto key() const noexcept -> AccountId { return account_; }
  /// @brief The refusals.
  ///
  /// @return The refusals ordered by product, valid while the record
  ///     lives.
  auto refusals() const noexcept -> std::span<const StreamRefusal> {
    return refusals_;
  }
  /// @brief The stored body.
  ///
  /// @return The body.
  auto body() const -> Body;

  /// @brief Compares account and refusals.
  friend auto operator==(const RefusalRecord&, const RefusalRecord&)
      -> bool = default;

 private:
  /// @brief Keeps checked refusals.
  ///
  /// @param account The account.
  /// @param refusals The sorted, distinct refusals.
  RefusalRecord(AccountId account,
                std::vector<StreamRefusal> refusals) noexcept;

  /// @brief The stored form of a refusal.
  ///
  /// @param refusal The refusal.
  /// @return Its stored form.
  static auto refused(const StreamRefusal& refusal) -> Refused;
  /// @brief A stored holding read back.
  ///
  /// @param wire The stored refusal.
  /// @return The holding, or std::nullopt when held without a valid
  ///     entitlement id.
  [[nodiscard]] static auto holding_of(const Refused& wire)
      -> std::optional<StreamRefusal::Holding>;

  /// @brief The account.
  AccountId account_;
  /// @brief The refusals ordered by product.
  std::vector<StreamRefusal> refusals_;
};

}  // namespace libchiaking::catalog::cache

/// @brief How a stored refusal is spelled in JSON.
template <>
struct glz::meta<libchiaking::catalog::cache::RefusalRecord::Refused> {
  /// @brief The type described.
  using T = libchiaking::catalog::cache::RefusalRecord::Refused;
  /// @brief The JSON keys product, kind, refusedAtMs, tier, ownership and
  /// ownershipId.
  static constexpr auto value =
      glz::object("product", &T::product, "kind", &T::kind, "refusedAtMs",
                  &T::refused_at_ms, "tier", &T::tier, "ownership",
                  &T::ownership, "ownershipId", &T::ownership_id);
};

/// @brief How a refusal record body is spelled in JSON.
template <>
struct glz::meta<libchiaking::catalog::cache::RefusalRecord::Body> {
  /// @brief The type described.
  using T = libchiaking::catalog::cache::RefusalRecord::Body;
  /// @brief The JSON keys refusals.
  static constexpr auto value = glz::object("refusals", &T::refusals);
};
