/// @file
/// @brief Declares RefusalMemory, the refusals PS's streaming service gave
/// each account, remembered in memory and on disk until they stop standing.
///
/// A refusal stands for StreamRefusal::lifetime and only while the user's
/// tier and holding of the title are unchanged; every read clears the ones
/// that have fallen and rewrites the account's record.
#pragma once

#include "src/core/catalog/adapters/cache/cache.hpp"
#include "src/core/catalog/model/unanswered.hpp"

#include <libchiaking/detail/catalog/product_id.hpp>
#include <libchiaking/detail/catalog/stream_refusal.hpp>
#include <libchiaking/detail/expected.hpp>
#include <libchiaking/detail/users/account_id.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace libchiaking::catalog {

/// @brief Every account's remembered refusals, loaded from disk on first
/// use.
///
/// One lock guards all accounts; every member takes it, and the first call
/// for an account reads its record from disk under that lock, so calls may
/// block on disk I/O. Safe to call from any thread. The cache must outlive
/// the memory.
class RefusalMemory {
 public:
  /// @brief The tier and holding a refusal was given under.
  using Circumstances = StreamRefusal::Circumstances;

  /// @brief The refusals that still stand for one account, with the
  /// revision they were read at.
  struct Standing {
    /// @brief The standing refusals, ordered by product.
    std::vector<StreamRefusal> refusals{};
    /// @brief A counter that changes whenever this account's refusals
    /// change; MyGames compares it to know when to rebuild.
    std::uint64_t revision{};

    /// @brief Compares refusals and revision.
    friend auto operator==(const Standing&, const Standing&) -> bool = default;
  };

  /// @brief Starts with nothing loaded.
  ///
  /// @param cache Where each account's record is kept. Must outlive this
  ///     object.
  /// @param threading Makes the lock that guards the loaded records. Must
  ///     outlive this object.
  RefusalMemory(cache::Cache& cache, platform::ThreadingProvider& threading);

  /// @brief Remembers that PS refused to stream a product now,
  /// replacing any earlier refusal of it.
  ///
  /// The refusal is kept in memory even when the disk fails.
  ///
  /// @param account The account refused.
  /// @param product The product refused.
  /// @param kind How PS refused.
  /// @param current The tier and holding at the time.
  /// @return The refusal; Unanswered::Reason::storage when the account's
  ///     record could not be read or written.
  [[nodiscard]] auto remember(AccountId account, const ProductId& product,
                              StreamRefusal::Kind kind,
                              const Circumstances& current)
      -> Expected<StreamRefusal, Unanswered>;
  /// @brief The refusals that still stand under a known tier.
  ///
  /// Each refusal is judged against the given tier and its own recorded
  /// holding; fallen ones are cleared and the record rewritten.
  ///
  /// @param account The account.
  /// @param tier The user's tier now; std::nullopt for no tier.
  /// @return The standing refusals; Unanswered::Reason::storage when the
  ///     record could not be read.
  [[nodiscard]] auto standing(AccountId account,
                              std::optional<users::Tier> tier)
      -> Expected<Standing, Unanswered>;
  /// @brief The refusals that still stand while the tier is unknown.
  ///
  /// Each refusal is judged against its own recorded circumstances, so
  /// only expiry clears it.
  ///
  /// @param account The account.
  /// @return The standing refusals; Unanswered::Reason::storage when the
  ///     record could not be read.
  [[nodiscard]] auto standing_while_membership_is_unknown(AccountId account)
      -> Expected<Standing, Unanswered>;
  /// @brief Clears refusals whose product is now held differently, as after
  /// an ownership recheck.
  ///
  /// @param account The account.
  /// @param held_now How each refused product is held now; called once per
  ///     refusal under the lock, so it must not call back into this object.
  /// @return The account's revision after clearing;
  ///     Unanswered::Reason::storage when the record could not be read.
  [[nodiscard]] auto forget_where_ownership_changed(
      AccountId account,
      const std::function<StreamRefusal::Holding(const ProductId&)>& held_now)
      -> Expected<std::uint64_t, Unanswered>;
  /// @brief The refusal standing against one product now.
  ///
  /// Every refusal is judged against the current tier; the asked product
  /// against the current holding, the others against their recorded one.
  /// Fallen ones are cleared.
  ///
  /// @param account The account.
  /// @param product The product.
  /// @param current The tier and holding now.
  /// @return The refusal, std::nullopt when none stands, or
  ///     Unanswered::Reason::storage when the record could not be read.
  [[nodiscard]] auto refusal_of(AccountId account, const ProductId& product,
                                const Circumstances& current)
      -> Expected<std::optional<StreamRefusal>, Unanswered>;
  /// @brief The account's current revision, without touching the disk.
  ///
  /// @param account The account.
  /// @return The revision, or 0 when the account has not been loaded.
  auto revision(AccountId account) -> std::uint64_t;
  /// @brief Forgets an account's refusals in memory and on disk.
  ///
  /// @param account The account.
  /// @return Nothing on success; Unanswered::Reason::storage when the
  ///     record could not be erased.
  [[nodiscard]] auto forget(AccountId account) -> Expected<void, Unanswered>;
  /// @brief Forgets every account's refusals in memory and on disk.
  ///
  /// Memory is cleared and every account's refusal record erased under the
  /// one lock, so a concurrent remember cannot read an old record back in
  /// between. Blocks on the store.
  ///
  /// @return Nothing on success; Unanswered::Reason::storage when the
  ///     records could not be listed or any could not be erased. Memory is
  ///     cleared either way.
  [[nodiscard]] auto forget_everyone() -> Expected<void, Unanswered>;

 private:
  /// @brief One account's refusals in memory.
  struct Held {
    /// @brief The refusals by product; at most one per product.
    std::map<ProductId, StreamRefusal> refusals{};
    /// @brief The revision at the last change.
    std::uint64_t revision{};
    /// @brief Whether the record has been read from disk.
    bool loaded{};
  };

  /// @brief The account's refusals, reading its record on first use.
  /// Caller holds mutex_.
  ///
  /// Refusals remembered before the read win over stored ones for the same
  /// product, and the merged set is written back.
  ///
  /// @param account The account.
  /// @return The account's entry, valid while mutex_ is held; or
  ///     Unanswered::Reason::storage when the record could not be read, in
  ///     which case the entry stays unloaded.
  [[nodiscard]] auto held(AccountId account) -> Expected<Held*, Unanswered>;
  /// @brief Writes an account's record, or erases it when nothing is left.
  ///
  /// @param account The account.
  /// @param held The account's refusals.
  /// @return Nothing on success; Unanswered::Reason::storage on failure.
  [[nodiscard]] auto save(AccountId account, const Held& held)
      -> Expected<void, Unanswered>;
  /// @brief Removes fallen refusals, bumping the revision and saving when
  /// any were removed; save failures are ignored. Caller holds mutex_.
  ///
  /// @tparam Fallen A predicate taking a StreamRefusal.
  /// @param account The account.
  /// @param held The account's refusals.
  /// @param fallen True for a refusal to remove.
  template <class Fallen>
  void clear(AccountId account, Held& held, const Fallen& fallen);
  /// @brief The account's refusals as a Standing.
  ///
  /// @param held The account's refusals.
  /// @return Every refusal, ordered by product, with the revision.
  static auto all_of(const Held& held) -> Standing;

  /// @brief Where records are kept; not owned.
  cache::Cache& cache_;
  /// @brief Guards accounts_ and changes_.
  platform::Mutex mutex_;
  /// @brief Refusals by account id.
  std::map<std::uint64_t, Held> accounts_;
  /// @brief The global change counter revisions are drawn from; only
  /// increases.
  std::uint64_t changes_{};
};

}  // namespace libchiaking::catalog
