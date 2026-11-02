/// @file
/// @brief Declares legacy_cloud_stream::Games, which decides the
/// LegacyCloudStream rows of MyGames from the Kamaji shelves and the account's
/// PS4 and PS3 library.
///
/// LegacyCloudStream (the old service) is out of scope of the PS5 decisions and
/// keeps its loose matching: owned copies join shelf rows by product, alias,
/// title key, concept or bundle sibling. A copy no shelf carries is listed only
/// when a PS5 store-app list entry streams that same product; a copy that only
/// relates to another entry, or whose entry does not stream, is never listed.
#pragma once

#include "src/core/catalog/model/list/listing_index.hpp"
#include "src/core/catalog/model/my_games/remembered_refusals.hpp"
#include "src/core/catalog/model/my_games/service_listing.hpp"

#include <libchiaking/detail/catalog/explanation.hpp>
#include <libchiaking/detail/catalog/library.hpp>
#include <libchiaking/detail/catalog/my_game.hpp>
#include <libchiaking/detail/catalog/my_games.hpp>
#include <libchiaking/detail/catalog/stream_refusal.hpp>
#include <libchiaking/detail/catalog/streamable_list.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace libchiaking::catalog::legacy_cloud_stream {

/// @brief The LegacyCloudStream games one MyGames build lists, decided once at
/// construction.
///
/// Construction shelves every LegacyCloudStream title of the list, merges the
/// owned PS4 and PS3 copies into them (or lists them off the shelf), hides
/// editions already licensed and everything when the tier is below Premium,
/// and attaches remembered refusals. It borrows nothing after construction.
/// Pure computation; build and read on one thread.
class Games {
 public:
  /// @brief Decides the LegacyCloudStream rows.
  ///
  /// @param library The account's library; only its old-service copies
  ///     are read.
  /// @param list The streamable list; its LegacyCloudStream titles are the
  /// shelf, its
  ///     PS5 entries lend presentation to unshelved copies.
  /// @param inputs What the games are being built from.
  /// @param refusals The remembered refusals to attach.
  Games(const Library& library, const StreamableList& list,
        const MyGames::Inputs& inputs, const RememberedRefusals& refusals);

  /// @brief The rows as listed games, with the products left out.
  ///
  /// @return Every row that MyGame accepts, in shelf order followed by the
  ///     unshelved copies, and the explanations of copies that were not
  ///     listed.
  auto listed() const -> ServiceGames;

 private:
  /// @brief One entry of the streamable list.
  using Entry = StreamableList::Entry;
  /// @brief One explanation step.
  using Step = Explanation::Step;
  /// @brief The rule a step reports on.
  using Rule = Explanation::Rule;
  /// @brief How a step went.
  using Outcome = Explanation::Outcome;

  /// @brief Where a row stands after the rules ran.
  enum class State : std::uint8_t {
    /// @brief On the shelf and not owned; offered with the plan.
    offered,
    /// @brief On the shelf and owned.
    licensed,
    /// @brief Owned and on no shelf, listed through the store entry that
    /// streams the same product.
    licensed_off_shelf,
    /// @brief Offered, but another licensed edition of the same game
    /// covers it; hidden as already owned.
    covered_by_a_licensed_edition,
    /// @brief The user's tier is below Premium; hidden.
    below_the_streaming_tier,
    /// @brief The shelf marks the title as not streaming; hidden as not
    /// in the service.
    outside_every_plan,
  };

  /// @brief One LegacyCloudStream row and what the rules decided for it.
  ///
  /// Starts offered; license, cover, fall_below_the_streaming_tier and
  /// remember move it on.
  class Row {
   public:
    /// @brief A row for a shelf title.
    ///
    /// @param entry The shelf entry.
    /// @return An offered row launched by the entry's product, or
    ///     outside_every_plan when the entry is marked unstreamable.
    static auto on_shelf(const Entry& entry) -> Row;
    /// @brief A row for an owned copy no shelf carries.
    ///
    /// @param title The title built from the copy and the lent entry.
    /// @param launched_as The lent entry's product, which the row
    ///     launches by.
    /// @param owned The copy.
    /// @return A licensed_off_shelf row, not included with PS Plus, with
    ///     passed library_match and own_listing steps.
    static auto off_shelf(Title title, const ProductId& launched_as,
                          const OwnedTitle& owned) -> Row;

    /// @brief Where the row stands.
    ///
    /// @return The state.
    auto state() const noexcept -> State { return state_; }
    /// @brief The row's title.
    ///
    /// @return The title, valid while the row lives.
    auto title() const noexcept -> const Title& { return title_; }
    /// @brief The copy merged into the row.
    ///
    /// @return The copy, or std::nullopt when the row is not owned.
    auto ownership() const noexcept -> const std::optional<OwnedTitle>& {
      return ownership_;
    }
    /// @brief The steps that record how ownership was matched.
    ///
    /// @return The steps; empty for an unowned row.
    auto matched() const noexcept -> const std::vector<Step>& {
      return matched_;
    }
    /// @brief The remembered refusal standing against the row.
    ///
    /// @return The refusal, or std::nullopt when none stands.
    auto refusal() const noexcept -> const std::optional<StreamRefusal>& {
      return refusal_;
    }
    /// @brief Whether the row is shown and, when hidden, why.
    ///
    /// @return Hidden as already owned (naming the covering edition), as
    ///     below the tier, or as not in the service for those states;
    ///     visible otherwise.
    auto visibility() const -> Visibility;
    /// @brief The row as MyGame facts.
    ///
    /// @return The facts; the play path is PlayPath::included for a PS5
    ///     title and PlayPath::old_service otherwise.
    auto facts() const -> MyGame::Facts;

    /// @brief Merges an owned copy into the row.
    ///
    /// The first copy makes the row licensed. A later copy that streams
    /// replaces one that does not, so a launch starts the streaming
    /// entitlement; either way the release moves to the earliest of the
    /// two and the copy not kept adds a noted owned_duplicate step.
    ///
    /// @param owned The copy.
    void license(const OwnedTitle& owned);
    /// @brief Marks an offered row as covered by a licensed edition.
    ///
    /// Does nothing unless the row is offered.
    ///
    /// @param licensed_edition The product of the edition that covers it.
    void cover(ProductId licensed_edition);
    /// @brief Hides the row because the tier is below Premium.
    ///
    /// Overrides any other state and forgets the covering edition.
    void fall_below_the_streaming_tier();
    /// @brief Attaches the refusal standing against the row.
    ///
    /// @param refusal The refusal, or std::nullopt to clear it.
    void remember(std::optional<StreamRefusal> refusal);

   private:
    /// @brief An offered row.
    ///
    /// @param title The title.
    /// @param inclusion What the list says about PS Plus including it.
    /// @param launch The identifier a launch sends.
    Row(Title title, PlusInclusion inclusion, LaunchIdentifier launch);

    /// @brief Where the row stands.
    State state_{State::offered};
    /// @brief The row's title.
    Title title_;
    /// @brief Whether a plan includes it.
    PlusInclusion inclusion_;
    /// @brief The identifier a launch sends.
    LaunchIdentifier launch_;
    /// @brief The merged copy, when owned.
    std::optional<OwnedTitle> ownership_{};
    /// @brief The licensed edition covering the row, while covered.
    std::optional<ProductId> covered_by_{};
    /// @brief How ownership was matched.
    std::vector<Step> matched_{};
    /// @brief The standing refusal, when one stands.
    std::optional<StreamRefusal> refusal_{};
  };

  /// @brief A store entry lent to an unshelved copy.
  using Lent = ListingIndex::Lent;

  /// @brief What the store lists say about products, offered concepts and
  /// lendable entries; defined in games.cpp.
  class StoreMetadata;

  /// @brief The editions the user licenses, by every way another listing of
  /// the same game can be recognised; defined in games.cpp.
  class OwnedEditions;

  /// @brief Adds a row for every LegacyCloudStream title of the list and
  /// indexes it.
  ///
  /// @param list The streamable list.
  void shelve(const StreamableList& list);
  /// @brief Merges the old-service copies into rows or lists them off the
  /// shelf.
  ///
  /// A copy with no shelf row that the store-app lists carry as a PS5
  /// title is left out as on the other service. A copy with neither a
  /// shelf row nor a lendable entry is left out as unmatched. A copy whose
  /// lendable entry is another product, or does not stream, is left out as
  /// not listed by PS. A trial whose full game is owned is left out as
  /// superseded.
  ///
  /// @param library The account's library.
  /// @param list The streamable list.
  void hold(const Library& library, const StreamableList& list);
  /// @brief Hides editions already licensed, then everything when the tier
  /// is below Premium.
  void hide();
  /// @brief Attaches the refusal standing against each row.
  ///
  /// @param refusals The remembered refusals.
  void remember(const RememberedRefusals& refusals);

  /// @brief Indexes a row by product, title key and concept; the first row
  /// keeps a shared key.
  ///
  /// @param at The row's position in rows_.
  void index(std::size_t at);
  /// @brief The shelf rows an owned copy belongs to.
  ///
  /// Tries the copy's product and entitlement id, then their title keys,
  /// then its concept, then the rows of its bundle siblings.
  ///
  /// @param owned The copy.
  /// @param library The library, for bundle siblings.
  /// @return The positions of matching rows: one for a direct match,
  ///     possibly several for siblings, none when nothing matches.
  auto shelved_for(const OwnedTitle& owned, const Library& library) const
      -> std::vector<std::size_t>;
  /// @brief The row listed under a product or its title key.
  ///
  /// @param product The product.
  /// @return The row's position, or std::nullopt when none.
  auto row_listing(const ProductId& product) const
      -> std::optional<std::size_t>;
  /// @brief Lists an owned copy no shelf carries through the entry that
  /// streams the same product.
  ///
  /// The name is the entry's, else the copy's, else the product id. Adds
  /// nothing when the title facts are refused.
  ///
  /// @param owned The copy.
  /// @param lent The lent entry; it must stream the copy's product.
  /// @param product The product the copy is listed under.
  void list_unshelved(const OwnedTitle& owned, const Lent& lent,
                      const ProductId& product);
  /// @brief The explanation steps of a row.
  ///
  /// @param row The row.
  /// @return The tier refusal steps for a row below the tier; otherwise
  ///     the opening, the PS Plus inclusion step, the matched steps, the
  ///     already-owned step for an unowned row, and the closing steps.
  auto steps_of(const Row& row) const -> std::vector<Step>;

  /// @brief The product a copy is listed under.
  ///
  /// @param owned The copy.
  /// @return Its product, else its entitlement id read as a product, else
  ///     std::nullopt.
  static auto product_of(const OwnedTitle& owned) -> std::optional<ProductId>;
  /// @brief The explanation of a copy nothing matched.
  ///
  /// @param product The product the copy would be listed under.
  /// @param owned The copy.
  /// @return A failed Rule::library_match explanation.
  static auto unmatched(const ProductId& product, const OwnedTitle& owned)
      -> Explanation;
  /// @brief Whether a lent entry streams the copy's own product.
  ///
  /// @param lent The lent entry.
  /// @param owned The copy.
  /// @return True when the entry is in the streaming section and is known
  ///     as the copy's product or its entitlement read as a product.
  static auto streams_as_held(const Lent& lent, const OwnedTitle& owned)
      -> bool;
  /// @brief The explanation of a copy whose lent entry is another product
  /// or does not stream.
  ///
  /// @param product The product the copy would be listed under.
  /// @param owned The copy.
  /// @param related The lent entry's product.
  /// @return A passed Rule::library_match step naming the related entry,
  ///     then a failed Rule::listed_by_service step.
  static auto off_the_list(const ProductId& product, const OwnedTitle& owned,
                           const ProductId& related) -> Explanation;

  /// @brief The shared listing rules for LegacyCloudStream.
  ServiceListing listing_;
  /// @brief Every row, shelf rows first.
  std::vector<Row> rows_;
  /// @brief Row position by product.
  std::map<ProductId, std::size_t> by_product_;
  /// @brief Row position by title key.
  std::map<TitleKey, std::size_t> by_key_;
  /// @brief Row position by concept.
  std::map<ConceptId, std::size_t> by_concept_;
  /// @brief Explanations of copies that were not listed.
  std::vector<Explanation> unlisted_;
};

}  // namespace libchiaking::catalog::legacy_cloud_stream
