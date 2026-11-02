/// @file
/// @brief Declares ServiceListing, the steps and listing rules that
/// LegacyCloudStream and PS5 cloud streaming share when they turn rows into
/// MyGame entries, and ServiceGames, what one service contributes to MyGames
/// before the two services are merged.
#pragma once

#include <libchiaking/detail/catalog/explanation.hpp>
#include <libchiaking/detail/catalog/my_game.hpp>
#include <libchiaking/detail/catalog/my_games.hpp>
#include <libchiaking/detail/catalog/owned_title.hpp>
#include <libchiaking/detail/catalog/store_locale.hpp>
#include <libchiaking/detail/catalog/store_page.hpp>
#include <libchiaking/detail/catalog/stream_refusal.hpp>
#include <libchiaking/detail/catalog/streamable_list.hpp>
#include <libchiaking/detail/catalog/title.hpp>

#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

namespace libchiaking::catalog {

/// @brief The games one service lists for the user and the products it
/// considered but left out, each with its explanation.
///
/// Produced by legacy_cloud_stream::Games::listed and
/// cloud_stream::Games::listed and consumed by MyGamesAssembly. Plain data with
/// no rules.
struct ServiceGames {
  /// @brief One game the service lists, shown or hidden.
  struct Listed {
    /// @brief The game as the user will see it.
    MyGame game;
    /// @brief Why the game is shown or hidden, step by step.
    Explanation explanation;
  };

  /// @brief The listed games in the order the service produced them.
  std::vector<Listed> listed{};
  /// @brief Explanations for products that were considered and not listed,
  /// such as an owned copy no list carries or a trial superseded by the
  /// full game.
  std::vector<Explanation> unlisted{};
};

/// @brief The listing rules of one service for one MyGames build.
///
/// It knows the inputs the build was made from, which service it lists for
/// and the store region whose pages it links. It writes the explanation
/// steps both services open and close with, and turns a row's facts into a
/// listed game. A value is immutable after construction and used on the
/// thread that assembles MyGames.
class ServiceListing {
 public:
  /// @brief One explanation step.
  using Step = Explanation::Step;
  /// @brief The rule a step reports on.
  using Rule = Explanation::Rule;
  /// @brief How a step went.
  using Outcome = Explanation::Outcome;

  /// @brief Sets up the rules for one service.
  ///
  /// @param inputs What the games are being built from, including the
  ///     user's tier and library states.
  /// @param service The service being listed.
  /// @param store The store region whose product pages are linked when a
  ///     title has no concept page.
  ServiceListing(const MyGames::Inputs& inputs, ServiceGeneration service,
                 StoreLocale store) noexcept
      : inputs_(inputs), service_(service), store_(std::move(store)) {}

  /// @brief What the games are being built from.
  ///
  /// @return The inputs, valid while this object lives.
  auto inputs() const noexcept -> const MyGames::Inputs& { return inputs_; }
  /// @brief Whether this service's ownership was read completely.
  ///
  /// @return True when the PS5 library (for PS5 cloud streaming) or the
  ///     library (for LegacyCloudStream) is LibraryState::fetched; false when
  ///     it is partial or unavailable.
  auto library_is_known() const noexcept -> bool;
  /// @brief Whether the user's known tier is below the streaming tier.
  ///
  /// @return True only when the tier is known and below Premium; false when
  ///     it is Premium or unknown.
  auto below_the_streaming_tier() const noexcept -> bool;
  /// @brief How the user holds a title, for judging remembered refusals.
  ///
  /// @param ownership The owned copy, or std::nullopt when none is held.
  /// @return The holding, marked as known only when library_is_known.
  auto holding_of(const std::optional<OwnedTitle>& ownership) const
      -> StreamRefusal::Holding;

  /// @brief The first step of every listed title's explanation.
  ///
  /// @param title The title.
  /// @return One noted Rule::platform step naming the product.
  auto opening(const Title& title) const -> std::vector<Step>;
  /// @brief The explanation of a title hidden because the tier is too low.
  ///
  /// @param title The title.
  /// @param hides The reason the title is hidden.
  /// @param matched The ownership steps already gathered for the row,
  ///     appended after the tier step.
  /// @return The opening step, a failed Rule::tier step and the matched
  ///     steps.
  auto refused_by_tier(const Title& title, std::optional<HideReason> hides,
                       std::span<const Step> matched) const
      -> std::vector<Step>;
  /// @brief Appends the closing steps of a listed title's explanation.
  ///
  /// Adds a passed tier step when the tier is known, then the notes of
  /// note_refusal_and_release.
  ///
  /// @param steps The explanation so far; extended in place.
  /// @param refusal A remembered refusal standing against the title.
  /// @param ownership The owned copy, when held.
  void close(std::vector<Step>& steps,
             const std::optional<StreamRefusal>& refusal,
             const std::optional<OwnedTitle>& ownership) const;
  /// @brief Appends a passed Rule::tier step carrying the user's tier.
  ///
  /// @param steps The explanation so far; extended in place.
  void pass_the_tier(std::vector<Step>& steps) const;
  /// @brief Notes a standing refusal and a release gate, when present.
  ///
  /// @param steps The explanation so far; extended in place.
  /// @param refusal A remembered refusal; adds a noted
  ///     Rule::remembered_refusal step when present.
  /// @param ownership The owned copy; adds a noted Rule::release step when
  ///     its release date is not unrestricted.
  void note_refusal_and_release(
      std::vector<Step>& steps, const std::optional<StreamRefusal>& refusal,
      const std::optional<OwnedTitle>& ownership) const;

  /// @brief Turns a row into a listed game.
  ///
  /// Marks ownership as checked when the library is known or the row is
  /// owned, and gives the title a product page in the store region when it
  /// has no concept page.
  ///
  /// @param games Where the game is appended.
  /// @param facts The row's facts.
  /// @param steps The row's explanation.
  void list(ServiceGames& games, MyGame::Facts facts,
            std::vector<Step> steps) const;

  /// @brief The explanation for a trial dropped because the full game is
  /// also owned.
  ///
  /// @param owned The owned copy.
  /// @param product The product it would be listed under.
  /// @param fully_owned Products the user holds as more than a trial.
  /// @return A failed Rule::trial_superseded explanation when the copy is
  ///     trial access and its product is fully owned; std::nullopt
  ///     otherwise.
  static auto superseded(const OwnedTitle& owned,
                         const std::optional<ProductId>& product,
                         const std::set<ProductId>& fully_owned)
      -> std::optional<Explanation>;
  /// @brief The explanation for a trial dropped because a full game
  /// supersedes it.
  ///
  /// @param owned The trial copy.
  /// @param product The product it would be listed under.
  /// @return A failed Rule::trial_superseded explanation.
  static auto superseded_trial(const OwnedTitle& owned,
                               const ProductId& product) -> Explanation;
  /// @brief The title facts of an owned copy shown through a store entry
  /// that lends it presentation.
  ///
  /// The concept and cover come from the lender when it has them, else
  /// from the copy; the landscape cover and concept page come only from
  /// the lender. The name, platform and service are left for the caller.
  ///
  /// @param lender The store entry lending presentation; may be nullptr.
  /// @param owned The owned copy.
  /// @param product The product the title is listed under.
  /// @return The partly filled facts.
  static auto lent_facts(const Title* lender, const OwnedTitle& owned,
                         ProductId product) -> Title::Facts;

 private:
  /// @brief Gives a title a product page when it lacks a concept page.
  ///
  /// @param title The title.
  /// @return The title with StorePage::of_product in the store region, or
  ///     the title unchanged when it already has a page or the page is
  ///     refused.
  auto with_store_page(Title title) const -> Title;

  /// @brief What the games are being built from.
  MyGames::Inputs inputs_;
  /// @brief The service being listed.
  ServiceGeneration service_;
  /// @brief The store region whose pages are linked.
  StoreLocale store_;
};

}  // namespace libchiaking::catalog
