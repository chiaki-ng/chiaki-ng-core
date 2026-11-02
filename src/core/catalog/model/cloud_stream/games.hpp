/// @file
/// @brief Declares cloud_stream::Games, which decides the PS5 cloud streaming
/// rows of MyGames from the Premium category, the PS5 entitlement scan and
/// the public Store streaming category.
///
/// A tile exists only for a Premium offer, an owned entitlement, or (while
/// ownership is known) a product for sale that nothing else claims. Owned
/// copies join Premium rows by entitlement id, then by product id. Bundles list
/// their children and never take an owned copy themselves: bundle ownership
/// only says whether the tile is owned, and each child launches by its own
/// entitlement. A Play First Trial whose play time is used up, and a trial
/// whose launch PS refused as ended (a remembered
/// StreamRefusal::Kind::trial_ended), are listed as ended trials that refuse to
/// launch. A trial is dropped when a full game of the same product or concept
/// is held, keeping the best entitlement per title, and when a Premium row
/// holds its entitlement, product or concept, since such a game launches
/// through the catalog.
#pragma once

#include "src/core/catalog/model/cloud_stream/for_sale_category.hpp"
#include "src/core/catalog/model/cloud_stream/premium_category.hpp"
#include "src/core/catalog/model/library/owned_ps5_games.hpp"
#include "src/core/catalog/model/my_games/remembered_refusals.hpp"
#include "src/core/catalog/model/my_games/service_listing.hpp"
#include "src/core/users/cloud_streaming.hpp"

#include <libchiaking/detail/catalog/explanation.hpp>
#include <libchiaking/detail/catalog/library.hpp>
#include <libchiaking/detail/catalog/my_game.hpp>
#include <libchiaking/detail/catalog/my_games.hpp>
#include <libchiaking/detail/catalog/stream_refusal.hpp>
#include <libchiaking/detail/catalog/title.hpp>

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

namespace libchiaking::catalog::cloud_stream {

/// @brief The PS5 cloud streaming games one MyGames build lists, decided
/// once at construction.
///
/// Construction, in order: offers a row per included Premium product (a
/// product with no included offer is declined), places each owned
/// entitlement into its Premium row or a row of its own (non-games are
/// excluded with their reason), settles bundle ownership, offers "for sale"
/// rows while the PS5 library is known, gives each row its children's and
/// owned copy's products as aliases, and attaches remembered refusals.
/// Every pointer given to the constructor is read only during construction.
/// Pure computation; build and read on one thread.
class Games {
 public:
  /// @brief Decides the PS5 rows.
  ///
  /// @param owned The PS5 entitlement scan; nullptr when not held.
  /// @param category The Premium category; nullptr when not held.
  /// @param for_sale The public Store streaming category; nullptr when not
  ///     held. Ignored unless the PS5 library is LibraryState::fetched.
  /// @param inputs What the games are being built from.
  /// @param streaming The user's cloud streaming benefits; when they carry
  ///     a remedy (subscribe or upgrade) every row is hidden as not
  ///     included by the tier. std::nullopt when unknown.
  /// @param refusals The remembered refusals to attach.
  /// @param store The store region whose product pages owned rows link.
  /// @param used_trials The store games whose Play First Trial the
  ///     signed-in store no longer offers the account; their "for sale"
  ///     rows offer no Play First Trial.
  Games(const OwnedPs5Games* owned, const PremiumCategory* category,
        const ForSaleCategory* for_sale, const MyGames::Inputs& inputs,
        std::optional<users::CloudStreaming> streaming,
        const RememberedRefusals& refusals, StoreLocale store,
        std::span<const ProductId> used_trials = {});

  /// @brief The rows as listed games, with the products left out.
  ///
  /// @return Every row whose facts MyGame accepts, in the order Premium,
  ///     owned, for sale; the explanations of excluded entitlements; and
  ///     the declined Premium products that no row lists.
  auto listed() const -> ServiceGames;

 private:
  /// @brief One explanation step.
  using Step = Explanation::Step;
  /// @brief The rule a step reports on.
  using Rule = Explanation::Rule;
  /// @brief How a step went.
  using Outcome = Explanation::Outcome;
  /// @brief A Premium row an owned copy joins: its position and the rule
  /// it joined by.
  using Join = std::pair<std::size_t, Rule>;

  /// @brief One PS5 tile under construction. Plain data.
  struct Row {
    /// @brief The tile's title facts; aliases grow in claim_aliases.
    Title::Facts title;
    /// @brief Whether Premium includes it.
    PlusInclusion inclusion;
    /// @brief The included Premium offer; set only for Premium rows.
    std::optional<PremiumProduct::Offer> offer{};
    /// @brief The offer for sale; set only for "for sale" rows.
    std::optional<PremiumProduct::Offer> sale{};
    /// @brief Whether the Premium product is a bundle.
    bool bundle{};
    /// @brief Whether one of the bundle's SKUs is wholly owned; a display
    /// fact that never sets ownership.
    bool bundle_owned{};
    /// @brief The bundle's streamable children; empty for a non-bundle.
    std::vector<BundledGame> children{};
    /// @brief The copy the tile launches through; set when owned, never
    /// for a bundle.
    std::optional<OwnedTitle> ownership{};
    /// @brief The explanation steps gathered so far; the first is always
    /// the Rule::platform note.
    std::vector<Step> steps{};
    /// @brief The remembered refusal standing against the tile.
    std::optional<StreamRefusal> refusal{};
    /// @brief The trial version a "for sale" row's product offers that the
    /// user does not hold; TrialOffer::none for every other row.
    TrialOffer trial{TrialOffer::none};
  };

  /// @brief Products and entitlement ids some tile already stands for.
  struct Claimed {
    /// @brief Claimed product ids, including aliases and child products.
    std::set<ProductId> products;
    /// @brief Claimed entitlement ids, including every owned and active
    /// entitlement.
    std::set<OwnershipId> entitlements;
  };

  /// @brief The products and concepts of the streamable games held as more
  /// than a trial.
  struct FullGames {
    /// @brief Their products.
    std::set<ProductId> products;
    /// @brief Their concepts.
    std::set<ConceptId> concepts;
  };

  /// @brief Adds a row per Premium product with an included offer.
  ///
  /// A repeated product id is skipped. The row's offer, its children and
  /// its product are indexed so owned copies can join it.
  ///
  /// @param category The Premium category.
  void offer_premium_rows(const PremiumCategory& category);
  /// @brief Adds a "for sale" row per product that has an offer for sale
  /// and that no tile or owned and active entitlement already claims.
  ///
  /// Each row notes the trial version the product offers that the user
  /// does not hold, as MyGame::trial_offer describes, leaving out a Play
  /// First Trial the account has used.
  ///
  /// @param category The public Store streaming category.
  /// @param used_trials The store games whose Play First Trial the account
  ///     has used.
  void offer_rows_for_sale(const ForSaleCategory& category,
                           std::span<const ProductId> used_trials);
  /// @brief Records why a Premium product is not included.
  ///
  /// @param product The product whose included_offer is empty.
  void decline(const PremiumProduct& product);
  /// @brief Places every scanned entitlement.
  ///
  /// A Play First Trial whose play time is used up is placed as an ended
  /// trial, which supersession drops like any other trial; every other
  /// record that is not a game is excluded with its reason.
  ///
  /// @param owned The PS5 entitlement scan.
  void place_owned(const OwnedPs5Games& owned);
  /// @brief The streamable games in a scan that are held as more than a
  /// trial.
  ///
  /// @param owned The PS5 entitlement scan.
  /// @return Their products and concepts.
  static auto full_games_of(const OwnedPs5Games& owned) -> FullGames;
  /// @brief Whether a full game supersedes an owned copy.
  ///
  /// @param owned The copy.
  /// @param full The full games held.
  /// @return True when the copy is trial access and a full game shares its
  ///     product or concept.
  static auto is_superseded(const OwnedTitle& owned, const FullGames& full)
      -> bool;
  /// @brief Whether a Premium row stands for an owned trial's game, so the
  /// game launches through the catalog.
  ///
  /// @param owned The copy.
  /// @return True when the copy is trial access and a Premium row holds
  ///     its entitlement, its product, a bundled child's product or its
  ///     concept.
  auto is_in_premium(const OwnedTitle& owned) const -> bool;
  /// @brief Records an entitlement that is not a streamable game.
  ///
  /// Nothing is recorded when the record has no product and its id is not
  /// a product id.
  ///
  /// @param record The entitlement.
  /// @param why Why it is not a game.
  void exclude(const Ps5Entitlement& record, NotAGame why);
  /// @brief Joins an owned copy to a Premium row.
  ///
  /// The join is noted in the row's steps. A bundle row takes no copy: its
  /// children say which of them are owned. A second copy for an owned row
  /// is noted as a duplicate.
  ///
  /// @param joined The row and the rule it joined by.
  /// @param owned The copy.
  void join(Join joined, const OwnedTitle& owned);
  /// @brief Gives an owned copy that joins no Premium row a row of its own.
  ///
  /// The row is listed under the copy's product. When another owned row
  /// already uses that product, the copy's entitlement id is used as the
  /// product instead, or the copy is noted as a duplicate when that is not
  /// possible.
  ///
  /// @param owned The copy; must have a product.
  void list_owned(const OwnedTitle& owned);
  /// @brief Notes each bundle's ownership in its steps,
  /// without giving the bundle an owned copy.
  void settle_bundles();
  /// @brief Adds child products and owned copies' products as aliases,
  /// skipping products another row already uses.
  void claim_aliases();
  /// @brief Attaches the refusal standing against each row.
  ///
  /// A StreamRefusal::Kind::trial_ended refusal marks a row held as trial
  /// access as an ended trial and is dropped from any other row, since it
  /// spoke only of the trial.
  ///
  /// @param refusals The remembered refusals.
  void remember(const RememberedRefusals& refusals);

  /// @brief The Premium row an owned copy belongs to.
  ///
  /// @param owned The copy.
  /// @return The row by the copy's entitlement id, else by its product id,
  ///     else std::nullopt.
  auto premium_row_for(const OwnedTitle& owned) const -> std::optional<Join>;
  /// @brief Everything the rows built so far stand for.
  ///
  /// @return The claimed products and entitlements.
  auto claimed() const -> Claimed;
  /// @brief Whether cloud streaming access hides every tile.
  ///
  /// @return True when the benefits are known and carry a remedy.
  auto streaming_hides_tiles() const noexcept -> bool;
  /// @brief Whether cloud streaming access is known to be allowed.
  ///
  /// @return True when the benefits are known and carry no remedy.
  auto streaming_is_allowed() const noexcept -> bool;
  /// @brief A row's complete explanation.
  ///
  /// @param row The row.
  /// @return The platform note, a failed tier step when tiles are hidden,
  ///     the row's own steps, the access route, the ownership step, a passed
  ///     tier step when streaming is allowed, and the refusal and release
  ///     notes.
  auto steps_of(const Row& row) const -> std::vector<Step>;
  /// @brief The ownership step of a row.
  ///
  /// @param row The row.
  /// @return Passed with the copy when owned; otherwise noted when the
  ///     PS5 library is known and unchecked when it is not.
  auto ownership_step(const Row& row) const -> Step;
  /// @brief A row as MyGame facts.
  ///
  /// An unowned Premium row carries its add-to-library offer.
  ///
  /// @param row The row.
  /// @return The facts, or std::nullopt when the title facts are refused.
  auto facts_of(const Row& row) const -> std::optional<MyGame::Facts>;

  /// @brief How a row may be played.
  ///
  /// @param row The row.
  /// @return PlayPath::owned when owned, PlayPath::store_only for a "for
  ///     sale" row, PlayPath::included otherwise.
  static auto path_of(const Row& row) noexcept -> PlayPath;
  /// @brief The identifier a launch of the row sends.
  ///
  /// @param row The row.
  /// @return The owned entitlement, else the Premium offer's entitlement,
  ///     else the sale's entitlement, else the product.
  static auto launch_of(const Row& row) -> LaunchIdentifier;
  /// @brief The title facts of a category product.
  ///
  /// @param product The product.
  /// @param locale The category's locale, for the product's store page.
  /// @return PS5 cloud facts with the product's name (its id when the name
  ///     is blank), MASTER image and concept.
  static auto title_of(const PremiumProduct& product, const StoreLocale& locale)
      -> Title::Facts;
  /// @brief The title facts of an owned copy listed on its own.
  ///
  /// @param owned The copy.
  /// @param listed_as The product the row is listed under.
  /// @return PS5 cloud facts with the copy's name (the product id when the
  ///     name is blank), cover and concept.
  static auto title_of(const OwnedTitle& owned, const ProductId& listed_as)
      -> Title::Facts;
  /// @brief Whether a tile already stands for a product.
  ///
  /// @param product The product.
  /// @param claimed What is claimed.
  /// @return True when its id or any streamable child's entitlement is
  ///     claimed.
  static auto is_claimed(const PremiumProduct& product, const Claimed& claimed)
      -> bool;
  /// @brief Claims a product and its streamable children.
  ///
  /// @param product The product.
  /// @param claimed What is claimed; extended in place.
  static void claim(const PremiumProduct& product, Claimed& claimed);
  /// @brief The add-to-library offer shown for an unclaimed Premium game.
  ///
  /// @param offer The included offer.
  /// @return The offer's SKU id (empty when none), its entitlement id, and
  ///     entitlement through the subscription.
  static auto offer_of(const PremiumProduct::Offer& offer)
      -> StorefrontAnswer::LibraryOffer;
  /// @brief The trial version a product offers that the user does not
  /// hold.
  ///
  /// @param product The product.
  /// @param owned_and_active The ids of the user's active PS5
  ///     entitlements.
  /// @param trial_used Whether the account has used the product's Play
  ///     First Trial.
  /// @return TrialOffer::play_first_trial when it offers such a Play First
  ///     Trial that is not used, else TrialOffer::demo for such an
  ///     ordinary trial, else TrialOffer::none.
  static auto trial_of(const PremiumProduct& product,
                       const std::set<OwnershipId>& owned_and_active,
                       bool trial_used) -> TrialOffer;

  /// @brief The shared listing rules for PS5 cloud streaming.
  ServiceListing listing_;
  /// @brief The user's cloud streaming access, when known.
  std::optional<users::CloudStreaming> streaming_;
  /// @brief The ids of every active PS5 entitlement in the scan.
  std::set<OwnershipId> owned_and_active_;
  /// @brief Every row: Premium, then owned, then for sale.
  std::vector<Row> rows_;
  /// @brief Premium row position by product id.
  std::map<ProductId, std::size_t> premium_products_;
  /// @brief Premium row position by offered or child entitlement id.
  std::map<OwnershipId, std::size_t> premium_entitlements_;
  /// @brief Own-row position by the product it is listed under.
  std::map<ProductId, std::size_t> owned_products_;
  /// @brief Products listed as "for sale".
  std::set<ProductId> for_sale_products_;
  /// @brief Explanations of Premium products that are not included.
  std::vector<Explanation> declined_;
  /// @brief Explanations of entitlements that are not games.
  std::vector<Explanation> unlisted_;
};

}  // namespace libchiaking::catalog::cloud_stream
