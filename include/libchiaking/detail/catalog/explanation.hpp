/// @file
/// @brief Declares Explanation, the record of which rules listed, hid or
/// dropped a product.
#pragma once

#include "libchiaking/detail/catalog/my_game.hpp"
#include "libchiaking/detail/catalog/owned_title.hpp"
#include "libchiaking/detail/catalog/product_id.hpp"
#include "libchiaking/detail/catalog/title_number.hpp"
#include "libchiaking/detail/users/membership.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace libchiaking::catalog {

/// @brief The rules applied to one product while the user's games were built,
/// in the order they were applied.
///
/// An explanation exists for every listed game and for owned copies that were
/// dropped. It is meant for a diagnostics view that answers why a game is
/// shown, hidden or missing.
///
/// @see MyGames::explain
/// @see Catalog::explain
class Explanation {
 public:
  /// @brief A rule that was applied to the product.
  enum class Rule : std::uint8_t {
    /// @brief Whether PS's streamable list carries the title. Fails for a
    /// product that is not among the games, and for an owned LegacyCloudStream
    /// copy
    /// the account's service no longer offers.
    listed_by_service,
    /// @brief Notes the product whose platform was read; the first step of
    /// every listed game.
    platform,
    /// @brief An owned copy on the older service whose title is listed for
    /// PS5 cloud streaming instead. Fails for the dropped copy and is noted
    /// on the listing that carries it.
    service,
    /// @brief Whether a PlayStation Plus plan includes the title. Fails,
    /// hiding the game, when no plan covers a title that can only be played
    /// through a plan.
    included_with_plus,
    /// @brief Whether the user's tier reaches streaming_tier. Fails, hiding
    /// the game, when it does not.
    tier,
    /// @brief Whether an owned record is a game rather than an add-on,
    /// trial or app.
    counts_as_game,
    /// @brief Whether an owned copy was matched to a title PS lists.
    /// Fails for an owned copy that matched nothing.
    library_match,
    /// @brief The owned copy was attached to a title that was already
    /// listed.
    merged_into_listing,
    /// @brief The owned copy was given a listing of its own because no
    /// listed title carried it.
    own_listing,
    /// @brief Notes a further owned copy of a title that is already held.
    owned_duplicate,
    /// @brief Fails for a trial copy when the full game of the same product
    /// is owned; on PS5 a full game of the same concept also counts, as
    /// does a PS Plus Premium row holding the trial's entitlement,
    /// product or concept, so the game launches through the catalog.
    trial_superseded,
    /// @brief A disc upgrade entitlement was resolved to the game it
    /// upgrades; Step::product names that game.
    disc_upgrade,
    /// @brief Whether the user owns the PS5 title. Unchecked when the
    /// library could not be read.
    ownership,
    /// @brief Fails, hiding the game, for an owned PS5 listing that would
    /// not launch a PS5 title.
    ps5_native,
    /// @brief Fails, hiding the game, when another shown game already
    /// launches the same identifier; Step::product names it.
    duplicate_launch,
    /// @brief For a game the user does not own: whether another edition of
    /// it is owned. Fails, hiding the game, when one is; Step::product
    /// names the owned edition.
    already_owned,
    /// @brief What PS says about streaming the copy. Fails, hiding the
    /// game, when the owned copy does not stream; noted with
    /// Step::title_number when the list marks the title unstreamable.
    list_streamability,
    /// @brief What the storefront said about the title.
    storefront,
    /// @brief Notes that a recent refusal by PS's streaming service still
    /// stands.
    remembered_refusal,
    /// @brief Notes that the owned copy has a release date or is
    /// unannounced.
    release,
    /// @brief Notes that two listings of one product were shown as one;
    /// Step::product names the listing that gave way.
    same_product,
    /// @brief Notes which route gives a PS5 tile its right to stream;
    /// Step::path names it.
    access_route,
    /// @brief Whether some SKU of a Premium category product carries the
    /// PS5 Premium streaming branding (`PS_PLUS_STREAM_PS5`); a product
    /// without it is not included.
    premium_branding,
    /// @brief Whether the first branded SKU has an entitlement that is a
    /// full game that streams; when it passes, Step::ownership names that
    /// entitlement. Other SKUs are not tried.
    premium_entitlement,
    /// @brief An owned PS5 copy was joined to a Premium tile because its
    /// entitlement id is one the tile offers; Step::product names the tile
    /// and Step::ownership the copy.
    joined_by_entitlement,
    /// @brief An owned PS5 copy was joined to a Premium tile because its
    /// product id is the tile's, after no entitlement matched;
    /// Step::product names the tile and Step::ownership the copy.
    joined_by_product,
    /// @brief Whether a Premium bundle counts as owned: passed when some
    /// SKU has every game entitlement it names owned and active, or names
    /// no entitlement id at all; otherwise noted.
    /// It only decides whether the tile shows as owned; no copy is held
    /// through the bundle, so Step::ownership is empty.
    bundle_ownership,
  };

  /// @brief How a rule turned out for the product.
  enum class Outcome : std::uint8_t {
    /// @brief The rule held.
    passed,
    /// @brief The rule dropped or hid the product.
    failed,
    /// @brief Recorded for information without deciding anything.
    noted,
    /// @brief The rule could not be evaluated.
    unchecked,
  };

  /// @brief One rule's outcome, with whichever details that rule records.
  struct Step {
    /// @brief The rule that was applied.
    Rule rule{};
    /// @brief How the rule turned out.
    Outcome outcome{};
    /// @brief Why the game is hidden, when this step is the one that hides
    /// it.
    std::optional<HideReason> hides{};
    /// @brief Why an owned record was not counted as a game, when that is
    /// what the step records.
    std::optional<NotAGame> not_a_game{};
    /// @brief The other product the step refers to; its meaning depends on
    /// Step::rule.
    std::optional<ProductId> product{};
    /// @brief The owned copy the step refers to.
    std::optional<OwnershipId> ownership{};
    /// @brief The title number PS's list marks as unstreamable.
    std::optional<TitleNumber> title_number{};
    /// @brief The user's tier when a tier rule was applied.
    std::optional<users::Tier> tier{};
    /// @brief Which of PS's sources decided whether a plan includes the
    /// title.
    std::optional<PlusInclusion::Source> inclusion_source{};
    /// @brief The route that gives the tile its right to stream; set by
    /// Rule::access_route.
    std::optional<PlayPath> path{};

    /// @brief Compares every field.
    friend auto operator==(const Step&, const Step&) -> bool = default;
  };

  /// @brief Builds an explanation from recorded steps.
  ///
  /// @param product The product the steps are about.
  /// @param steps The steps in the order the rules were applied; may be
  ///     empty.
  /// @return The explanation.
  static auto of(ProductId product, std::vector<Step> steps) -> Explanation;
  /// @brief Builds the explanation for a product that is not among the games.
  ///
  /// @param product The product that was asked about.
  /// @return An explanation with a single failed
  ///     Explanation::Rule::listed_by_service step.
  static auto not_listed(ProductId product) -> Explanation;

  /// @brief The product the explanation is about.
  ///
  /// @return The product id, valid while this explanation lives.
  auto product_id() const noexcept -> const ProductId& { return product_; }
  /// @brief The steps in the order the rules were applied; the view is valid
  /// while this explanation lives.
  ///
  /// @return The steps, oldest rule first; possibly empty.
  auto steps() const noexcept -> std::span<const Step> { return steps_; }
  /// @brief Whether no rule failed, which means the product is listed and
  /// shown.
  ///
  /// @note A game that is listed but hidden has a failed step, so this
  ///     returns false for it.
  ///
  /// @return True when every step passed; false when any step failed.
  auto is_listed() const noexcept -> bool;
  /// @brief The first step that failed, which is the rule that dropped or hid
  /// the product.
  ///
  /// @return A pointer into this explanation, or null when no step failed.
  auto first_failure() const noexcept -> const Step*;
  /// @brief Why the game is hidden, taken from the first failed step.
  ///
  /// @return Nothing when no step failed or the failed step dropped the
  ///     product without listing it.
  auto hide_reason() const noexcept -> std::optional<HideReason>;
  /// @brief Whether ownership went unchecked because the library could not be
  /// read.
  ///
  /// @return True when the ownership check was skipped for lack of a readable
  ///     library; false when ownership was checked.
  auto could_not_check_ownership() const noexcept -> bool;

  /// @brief Compares the product and every step.
  friend auto operator==(const Explanation&, const Explanation&)
      -> bool = default;

 private:
  Explanation(ProductId product, std::vector<Step> steps) noexcept;

  ProductId product_;
  std::vector<Step> steps_;
};

}  // namespace libchiaking::catalog
