/// @file
/// @brief Declares TrialMemory, the store games whose Play First Trial the
/// signed-in store no longer offers each account, remembered in memory and
/// on disk, and cache::UsedTrialRecord, the form it is stored in.
///
/// The public store category keeps listing a trial after PS has deleted
/// the used trial from the account, while the signed-in store stops offering
/// it. The memory keeps what the signed-in store last said for each product,
/// with no age limit, so a store game is not labelled as offering a trial
/// the account has used.
#pragma once

#include "src/core/catalog/adapters/cache/cache.hpp"
#include "src/core/catalog/adapters/cache/record_format.hpp"
#include "src/core/catalog/model/unanswered.hpp"

#include <libchiaking/detail/catalog/product_id.hpp>
#include <libchiaking/detail/expected.hpp>
#include <libchiaking/detail/users/account_id.hpp>
#include <libchiaking/platform/runtime/threading.hpp>
#include <libchiaking/platform/storage/record_store.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::catalog::cache {

/// @brief One account's store games whose trial the account has used.
///
/// Stored at user/{account}/used_trials. The record has no age limit: only a
/// later store answer for a product removes it, so it never depends on the
/// device clock. Immutable; may be shared between threads.
class UsedTrialRecord {
 public:
  /// @brief Records are keyed by account.
  using Key = AccountId;

  /// @brief The stored body. Plain data.
  struct Body {
    /// @brief The products, ordered and distinct.
    std::vector<std::string> products;
  };

  /// @brief The record kind and last name segment.
  static constexpr std::string_view kind = "used_trials";
  /// @brief The schema version.
  static constexpr std::int64_t schema = 1;

  /// @brief Builds a record from products.
  ///
  /// @param account The account.
  /// @param products The products, in any order; repeats are dropped.
  /// @return The record with products sorted.
  [[nodiscard]] static auto of(AccountId account,
                               std::vector<ProductId> products)
      -> UsedTrialRecord;
  /// @brief Reads a stored body back.
  ///
  /// @param body The stored body.
  /// @param account The account the record is stored under.
  /// @return The record; std::nullopt when a product does not parse.
  [[nodiscard]] static auto from_body(const Body& body, AccountId account)
      -> std::optional<UsedTrialRecord>;
  /// @brief The record's name.
  ///
  /// @param account The account.
  /// @return user/{account}/used_trials.
  [[nodiscard]] static auto name_of(AccountId account)
      -> std::optional<platform::RecordName>;

  /// @brief The record's key.
  ///
  /// @return The account.
  auto key() const noexcept -> AccountId { return account_; }
  /// @brief The products.
  ///
  /// @return The products, ordered and distinct, valid while the record
  ///     lives.
  auto products() const noexcept -> std::span<const ProductId> {
    return products_;
  }
  /// @brief The stored body.
  ///
  /// @return The body.
  auto body() const -> Body;

  /// @brief Compares account and products.
  friend auto operator==(const UsedTrialRecord&, const UsedTrialRecord&)
      -> bool = default;

 private:
  /// @brief Keeps checked products.
  ///
  /// @param account The account.
  /// @param products The sorted, distinct products.
  UsedTrialRecord(AccountId account, std::vector<ProductId> products) noexcept;

  /// @brief The account.
  AccountId account_;
  /// @brief The products, ordered and distinct.
  std::vector<ProductId> products_;
};

}  // namespace libchiaking::catalog::cache

namespace libchiaking::catalog {

/// @brief Every account's used trials, loaded from disk on first use.
///
/// One lock guards all accounts; the first call for an account reads its
/// record from disk under that lock, so calls may block on disk I/O. Safe to
/// call from any thread. The cache must outlive the memory.
class TrialMemory {
 public:
  /// @brief One account's used trials with the revision they were read at.
  struct Used {
    /// @brief The products, ordered.
    std::vector<ProductId> products{};
    /// @brief A counter that changes whenever this account's used trials
    /// change; MyGames compares it to know when to rebuild.
    std::uint64_t revision{};

    /// @brief Compares products and revision.
    friend auto operator==(const Used&, const Used&) -> bool = default;
  };

  /// @brief Starts with nothing loaded.
  ///
  /// @param cache Where each account's record is kept. Must outlive this
  ///     object.
  /// @param threading Makes the lock that guards the loaded records. Must
  ///     outlive this object.
  TrialMemory(cache::Cache& cache, platform::ThreadingProvider& threading);

  /// @brief Records what the signed-in store said about a product's trial.
  ///
  /// A product the store no longer offers a trial of is remembered; one it
  /// offers again is forgotten. Nothing changes when the account's record
  /// could not be read or the change could not be written, so memory and
  /// disk always agree.
  ///
  /// @param account The account the store answered.
  /// @param product The product.
  /// @param offered Whether the store offered the account a trial.
  /// @return Nothing on success; Unanswered::Reason::storage when the
  ///     account's record could not be read or written.
  [[nodiscard]] auto note(AccountId account, const ProductId& product,
                          bool offered) -> Expected<void, Unanswered>;
  /// @brief The account's used trials.
  ///
  /// @param account The account.
  /// @return The products and revision; Unanswered::Reason::storage when
  ///     the record could not be read.
  [[nodiscard]] auto used(AccountId account) -> Expected<Used, Unanswered>;
  /// @brief Whether a product's trial is remembered as used.
  ///
  /// @param account The account.
  /// @param product The product.
  /// @return True when remembered; false otherwise, and when the record
  ///     could not be read.
  auto is_used(AccountId account, const ProductId& product) -> bool;
  /// @brief Forgets an account's used trials in memory and on disk.
  ///
  /// @param account The account.
  /// @return Nothing on success; Unanswered::Reason::storage when the
  ///     record could not be erased.
  [[nodiscard]] auto forget(AccountId account) -> Expected<void, Unanswered>;

 private:
  /// @brief One account's used trials in memory.
  struct Held {
    /// @brief The products.
    std::set<ProductId> products{};
    /// @brief The revision at the last change.
    std::uint64_t revision{};
    /// @brief Whether the record has been read from disk.
    bool loaded{};
  };

  /// @brief The account's used trials, reading its record on first use.
  /// Caller holds mutex_.
  ///
  /// @param account The account.
  /// @return The account's entry, valid while mutex_ is held; or
  ///     Unanswered::Reason::storage when the record could not be read, in
  ///     which case the entry stays unloaded.
  [[nodiscard]] auto held(AccountId account) -> Expected<Held*, Unanswered>;
  /// @brief Writes an account's record, or erases it when nothing is left.
  ///
  /// @param account The account.
  /// @param held The account's used trials.
  /// @return Nothing on success; Unanswered::Reason::storage on failure.
  [[nodiscard]] auto save(AccountId account, const Held& held)
      -> Expected<void, Unanswered>;

  /// @brief Where records are kept; not owned.
  cache::Cache& cache_;
  /// @brief Guards accounts_ and changes_.
  platform::Mutex mutex_;
  /// @brief Used trials by account id.
  std::map<std::uint64_t, Held> accounts_;
  /// @brief The global change counter revisions are drawn from; only
  /// increases.
  std::uint64_t changes_{};
};

}  // namespace libchiaking::catalog

/// @brief How a used trial record body is spelled in JSON.
template <>
struct glz::meta<libchiaking::catalog::cache::UsedTrialRecord::Body> {
  /// @brief The type described.
  using T = libchiaking::catalog::cache::UsedTrialRecord::Body;
  /// @brief The JSON keys products.
  static constexpr auto value = glz::object("products", &T::products);
};
