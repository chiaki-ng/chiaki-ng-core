/// @file
/// @brief Declares ListingIndex, the lookup that lends a PS5 store-app list
/// entry to an owned LegacyCloudStream copy that the Kamaji shelves do not
/// carry.
///
/// The imagic store-app lists are kept only as LegacyCloudStream's lender for
/// owned PS4 copies that are missing from the shelves. ListingIndex answers
/// "which list entry describes this copy" by product, then title key, then
/// concept, in the order legacy_cloud_stream::Games trusts them.
#pragma once

#include <libchiaking/detail/catalog/library.hpp>
#include <libchiaking/detail/catalog/owned_title.hpp>
#include <libchiaking/detail/catalog/product_id.hpp>
#include <libchiaking/detail/catalog/streamable_list.hpp>
#include <libchiaking/detail/catalog/title.hpp>

#include <map>
#include <optional>
#include <span>

namespace libchiaking::catalog {

/// @brief Indexes the PS5 entries of a StreamableList by product, title key
/// and concept.
///
/// Two sections are kept: the entries the list streams and the entries a
/// plan includes but the store does not list for streaming. Only entries of
/// ServiceGeneration::ps5_cloud are indexed. The index points into the list
/// it was built from, which must outlive it and must not change. Lookups are
/// const and may run on several threads at once.
class ListingIndex {
 public:
  /// @brief One entry of the indexed list.
  using Entry = StreamableList::Entry;

  /// @brief The entry lent to a copy and which section it came from.
  struct Lent {
    /// @brief The entry, pointing into the indexed list; nullptr when no
    /// entry matched.
    const Entry* entry{};
    /// @brief True when the entry is listed for streaming, false when it
    /// came from the included-but-not-streamable section or nothing
    /// matched.
    bool streams{};
  };

  /// @brief The lookups over one section of the list.
  ///
  /// When several entries share a key the first one in list order wins.
  /// Aliases are indexed after every product id, so an alias never hides
  /// another entry's own product.
  class Section {
   public:
    /// @brief Indexes the PS5 entries of one section.
    ///
    /// @param entries The section's entries. Must outlive the section.
    explicit Section(std::span<const Entry> entries);

    /// @brief Finds the entry for a product or one of its aliases.
    ///
    /// @param product The product, compared exactly; std::nullopt finds
    ///     nothing.
    /// @return The entry, or nullptr when none matches.
    auto by_product(const std::optional<ProductId>& product) const
        -> const Entry*;
    /// @brief Finds the entry whose product id carries a title key.
    ///
    /// @param key The title key; std::nullopt finds nothing.
    /// @return The entry, or nullptr when none matches.
    auto by_key(const std::optional<TitleKey>& key) const -> const Entry*;
    /// @brief Finds the entry grouped under a concept.
    ///
    /// @param grouped The concept; std::nullopt finds nothing.
    /// @return The entry, or nullptr when none matches.
    auto by_concept(const std::optional<ConceptId>& grouped) const
        -> const Entry*;

   private:
    /// @brief Looks a key up in one of the maps.
    ///
    /// @tparam Key ProductId, TitleKey or ConceptId; must be ordered.
    /// @param entries The map to search.
    /// @param key The key to find.
    /// @return The entry, or nullptr when the key is absent.
    template <class Key>
    static auto found_in(const std::map<Key, const Entry*>& entries,
                         const Key& key) -> const Entry*;

    /// @brief Entries by product id and by every alias.
    std::map<ProductId, const Entry*> products_;
    /// @brief Entries by the title key inside their product id.
    std::map<TitleKey, const Entry*> keys_;
    /// @brief Entries by concept, for those that carry one.
    std::map<ConceptId, const Entry*> concepts_;
  };

  /// @brief Indexes both sections of a list.
  ///
  /// @param list The list. Must outlive the index and stay unchanged.
  explicit ListingIndex(const StreamableList& list);

  /// @brief The section of entries listed for streaming.
  ///
  /// @return The section, valid while the index lives.
  auto streams() const noexcept -> const Section& { return streams_; }
  /// @brief The section of entries a plan includes but the store does not
  /// list for streaming.
  ///
  /// @return The section, valid while the index lives.
  auto aside() const noexcept -> const Section& { return aside_; }

  /// @brief Finds the entry that describes an owned copy.
  ///
  /// Tries, in order: the streaming section by the copy's product, by its
  /// entitlement id read as a product, by the product's title key and by
  /// the entitlement's title key; for a full game only, the aside section
  /// by those two title keys; the streaming section by concept; for a full
  /// game, the aside section by concept; and for a full game whose
  /// entitlement id is its product, the aside section by that product.
  ///
  /// @param owned The owned copy.
  /// @return The first match, or an empty Lent when nothing matches.
  auto for_copy(const OwnedTitle& owned) const -> Lent;
  /// @brief Finds the entry for a product bought in the same bundle as a
  /// copy.
  ///
  /// Tries the streaming section by product, the aside section by product,
  /// then both sections by title key.
  ///
  /// @param sibling The sibling product.
  /// @return The first match, or an empty Lent when nothing matches.
  auto for_sibling(const ProductId& sibling) const -> Lent;
  /// @brief Finds the entry for a copy, falling back to its bundle
  /// siblings.
  ///
  /// @param owned The owned copy.
  /// @param library The library the copy belongs to, for its bundles.
  /// @return for_copy's match, else the first sibling of the copy's product
  ///     that for_sibling matches, else an empty Lent (also when the copy
  ///     has no product).
  auto for_copy_or_sibling(const OwnedTitle& owned,
                           const Library& library) const -> Lent;

 private:
  /// @brief The index over StreamableList::titles.
  Section streams_;
  /// @brief The index over StreamableList::included_but_not_streamable.
  Section aside_;
};

}  // namespace libchiaking::catalog
