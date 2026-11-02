/// @file
/// @brief Declares Library, the games one account owns.
#pragma once

#include "libchiaking/detail/catalog/owned_title.hpp"
#include "libchiaking/detail/catalog/product_id.hpp"
#include "libchiaking/detail/catalog/title_number.hpp"
#include "libchiaking/detail/catalog/wall_time.hpp"
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/users/account_id.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace libchiaking::catalog {

/// @brief Whether the account's library could be read.
enum class LibraryState : std::uint8_t {
  /// @brief The library was read.
  fetched,
  /// @brief The library could not be read, so ownership of unmatched titles
  /// is unknown.
  unavailable,
  /// @brief Only part of the library was read, because PS's revision kept
  /// changing during the scan or the page cap was reached; the records read
  /// are kept, but a title not among them is not known to be unowned. Set
  /// only for PS5 ownership.
  partial,
};

/// @brief The games one account owns, as read from PS's entitlement records
/// at one moment.
///
/// A library is immutable; the with members return changed copies.
/// Pointers and views it hands out are valid while it lives.
///
/// @see Catalog::library_of
/// @see OwnedTitle
class Library {
 public:
  /// @brief Wall-clock time in milliseconds.
  using Time = catalog::Time;

  /// @brief Why a library could not be built.
  enum class Invalid : std::uint8_t {
    /// @brief Two titles share one OwnershipId.
    duplicate_ownership,
  };

  /// @brief A store product together with the entitlements that were bought
  /// under it.
  struct Bundle {
    /// @brief The product that was bought.
    ProductId product;
    /// @brief The entitlements held under that product, written as product
    /// ids.
    std::vector<ProductId> siblings{};

    /// @brief Compares the product and its siblings.
    friend auto operator==(const Bundle&, const Bundle&) -> bool = default;
  };

  /// @brief A concept PS reported for an owned copy after the library was
  /// fetched.
  struct LearnedConcept {
    /// @brief The entitlement the concept belongs to, when PS named it.
    std::optional<OwnershipId> id{};
    /// @brief The product the concept belongs to; used when the entitlement
    /// does not match.
    std::optional<ProductId> product{};
    /// @brief The concept the copy belongs to.
    ConceptId grouped_under;

    /// @brief Compares every field.
    friend auto operator==(const LearnedConcept&, const LearnedConcept&)
        -> bool = default;
  };

  /// @brief The concept learned for an owned copy that did not carry one.
  struct CopyConcept {
    /// @brief The owned copy.
    OwnershipId id;
    /// @brief The concept it belongs to.
    ConceptId grouped_under;

    /// @brief Compares both fields.
    friend auto operator==(const CopyConcept&, const CopyConcept&)
        -> bool = default;
  };

  /// @brief Builds a library from owned titles.
  ///
  /// @param account The account that owns the titles.
  /// @param titles The owned games, in the order they should be kept.
  /// @param bundles Which entitlements were bought under which product.
  /// @param fetched_at When the records were read.
  /// @param ps5_ownership How completely the account's PS5 entitlements were
  ///     read; LibraryState::fetched unless the PS5 part was missed or cut
  ///     short. See Library::ps5_ownership.
  /// @return The library.
  /// @retval Library::Invalid::duplicate_ownership Two titles share one
  ///     OwnershipId.
  [[nodiscard]] static auto of(
      AccountId account, std::vector<OwnedTitle> titles,
      std::vector<Bundle> bundles, Time fetched_at,
      LibraryState ps5_ownership = LibraryState::fetched)
      -> Expected<Library, Invalid>;

  /// @brief The account that owns the titles.
  ///
  /// @return The account id the library was built for.
  auto account() const noexcept -> AccountId { return account_; }
  /// @brief When the records were read, or when the library was last amended
  /// by Library::with.
  ///
  /// @return The time the records were read, or the time of the latest
  ///     Library::with.
  auto fetched_at() const noexcept -> Time { return fetched_at_; }
  /// @brief Every owned game.
  ///
  /// @return The owned games, valid while this library lives; possibly empty.
  auto titles() const noexcept -> std::span<const OwnedTitle> {
    return titles_;
  }
  /// @brief Which entitlements were bought under which product.
  ///
  /// @return The bundles, valid while this library lives; possibly empty.
  auto bundles() const noexcept -> std::span<const Bundle> { return bundles_; }
  /// @brief Number of owned games.
  ///
  /// @return The count of owned games, zero for an empty library.
  auto size() const noexcept -> std::size_t { return titles_.size(); }

  /// @brief Finds an owned game by its entitlement.
  ///
  /// @param id The entitlement to look for.
  /// @return The title, or null when the account does not hold it.
  auto find(const OwnershipId& id) const noexcept -> const OwnedTitle*;
  /// @brief Finds the first owned game held under a product.
  ///
  /// @param product The product to look for.
  /// @return The title whose product id or entitlement id equals the product,
  ///     or null.
  auto held_under(const ProductId& product) const noexcept -> const OwnedTitle*;
  /// @brief Whether any owned game has the given title number.
  ///
  /// @param number The title number to look for.
  /// @return True when some owned game has that title number; false otherwise.
  auto holds_title(const TitleNumber& number) const -> bool;
  /// @brief The entitlements bought under a product.
  ///
  /// @param product The product that was bought.
  /// @return The entitlements, written as product ids; empty when the product
  ///     has no bundle.
  auto siblings_of(const ProductId& product) const noexcept
      -> std::span<const ProductId>;
  /// @brief The concepts learned for owned copies that did not carry one.
  ///
  /// @return The learned concepts, valid while this library lives; possibly
  /// empty.
  auto copy_concepts() const noexcept -> std::span<const CopyConcept> {
    return copy_concepts_;
  }
  /// @brief Looks up the learned concept of the copy held under a product.
  ///
  /// @param product The product the copy is held under.
  /// @return The concept, or nothing when none was learned for that copy.
  auto concept_of_copy(const ProductId& product) const
      -> std::optional<ConceptId>;
  /// @brief Returns a copy that knows the concepts PS reported for copies
  /// lacking one.
  ///
  /// Any concepts learned earlier are replaced. A row is matched to a title
  /// by entitlement first and by product second; titles that already carry a
  /// concept are skipped.
  ///
  /// @param learned The concepts PS reported.
  /// @return The amended library.
  auto with_learned_concepts(std::span<const LearnedConcept> learned) const
      -> Library;
  /// @brief Returns a copy that holds one more title, or a newer version of
  /// one already held.
  ///
  /// @param title The title to add, or to put in place of the one with the
  ///     same OwnershipId.
  /// @param checked_at When the title was checked; becomes
  ///     Library::fetched_at.
  /// @return The amended library.
  auto with(OwnedTitle title, Time checked_at) const -> Library;
  /// @brief How completely the account's PS5 entitlements were read.
  ///
  /// PS5 ownership is read from a separate source from the rest of the
  /// library, so it can be missing while the rest was read.
  ///
  /// @return LibraryState::fetched when every PS5 entitlement was read,
  ///     LibraryState::partial when the scan was cut short, and
  ///     LibraryState::unavailable when it could not be read, which makes
  ///     PS5 ownership unknown.
  auto ps5_ownership() const noexcept -> LibraryState { return ps5_ownership_; }
  /// @brief Returns a copy whose PS5 titles are the ones just read.
  ///
  /// The PS5 cloud titles already held are dropped and replaced by `read`,
  /// except when the PS5 part could not be read and nothing was read, in
  /// which case they are kept. A title in `read` whose OwnershipId is already
  /// held is not added twice. Titles on other services, bundles, learned
  /// concepts and Library::fetched_at are kept.
  ///
  /// @param read The owned PS5 games from the latest scan.
  /// @param ownership How completely that scan read the PS5 entitlements;
  ///     becomes Library::ps5_ownership.
  /// @return The amended library.
  auto with_ps5_part(std::vector<OwnedTitle> read, LibraryState ownership) const
      -> Library;

  /// @brief Compares account, titles, bundles, learned concepts, fetch time
  /// and PS5 ownership state.
  friend auto operator==(const Library&, const Library&) -> bool = default;

 private:
  Library(AccountId account, std::vector<OwnedTitle> titles,
          std::vector<Bundle> bundles, Time fetched_at) noexcept;

  AccountId account_;
  std::vector<OwnedTitle> titles_;
  std::vector<Bundle> bundles_;
  std::vector<CopyConcept> copy_concepts_{};
  Time fetched_at_;
  LibraryState ps5_ownership_{LibraryState::fetched};
};

}  // namespace libchiaking::catalog
