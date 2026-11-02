#include "src/core/catalog/model/list/listing_index.hpp"

namespace libchiaking::catalog {

ListingIndex::Section::Section(std::span<const Entry> entries) {
  for (const auto& entry : entries) {
    if (entry.title.service() != ServiceGeneration::ps5_cloud) continue;
    products_.emplace(entry.title.product_id(), &entry);
    if (const auto key = entry.title.product_id().title_key())
      keys_.emplace(*key, &entry);
    if (const auto& grouped = entry.title.concept_id())
      concepts_.emplace(*grouped, &entry);
  }
  for (const auto& entry : entries)
    if (entry.title.service() == ServiceGeneration::ps5_cloud)
      for (const auto& alias : entry.title.aliases())
        products_.emplace(alias, &entry);
}

template <class Key>
auto ListingIndex::Section::found_in(const std::map<Key, const Entry*>& entries,
                                     const Key& key) -> const Entry* {
  const auto found = entries.find(key);
  if (found == entries.end()) return nullptr;
  return found->second;
}

auto ListingIndex::Section::by_product(
    const std::optional<ProductId>& product) const -> const Entry* {
  return product ? found_in(products_, *product) : nullptr;
}

auto ListingIndex::Section::by_key(const std::optional<TitleKey>& key) const
    -> const Entry* {
  return key ? found_in(keys_, *key) : nullptr;
}

auto ListingIndex::Section::by_concept(
    const std::optional<ConceptId>& grouped) const -> const Entry* {
  return grouped ? found_in(concepts_, *grouped) : nullptr;
}

ListingIndex::ListingIndex(const StreamableList& list)
    : streams_(list.titles()), aside_(list.included_but_not_streamable()) {}

auto ListingIndex::for_copy(const OwnedTitle& owned) const -> Lent {
  const auto& product = owned.product_id();
  const auto held_as_product = owned.id().as_product();
  const auto product_key = product ? product->title_key() : std::nullopt;
  const auto id_key = owned.id().title_key();
  const bool full_game = owned.held_as() == HeldAs::full_game;
  if (const auto* entry = streams_.by_product(product)) return {entry, true};
  if (const auto* entry = streams_.by_product(held_as_product))
    return {entry, true};
  if (const auto* entry = streams_.by_key(product_key)) return {entry, true};
  if (const auto* entry = streams_.by_key(id_key)) return {entry, true};
  if (const auto* entry = full_game ? aside_.by_key(product_key) : nullptr)
    return {entry, false};
  if (const auto* entry = full_game ? aside_.by_key(id_key) : nullptr)
    return {entry, false};
  if (const auto* entry = streams_.by_concept(owned.concept_id()))
    return {entry, true};
  if (const auto* entry =
          full_game ? aside_.by_concept(owned.concept_id()) : nullptr)
    return {entry, false};
  if (const auto* entry = full_game && product && held_as_product == product
                              ? aside_.by_product(product)
                              : nullptr)
    return {entry, false};
  return {};
}

auto ListingIndex::for_sibling(const ProductId& sibling) const -> Lent {
  const std::optional<ProductId> product{sibling};
  const auto key = sibling.title_key();
  if (const auto* entry = streams_.by_product(product)) return {entry, true};
  if (const auto* entry = aside_.by_product(product)) return {entry, false};
  if (const auto* entry = streams_.by_key(key)) return {entry, true};
  if (const auto* entry = aside_.by_key(key)) return {entry, false};
  return {};
}

auto ListingIndex::for_copy_or_sibling(const OwnedTitle& owned,
                                       const Library& library) const -> Lent {
  if (const auto lent = for_copy(owned); lent.entry != nullptr) return lent;
  if (!owned.product_id()) return {};
  for (const auto& sibling : library.siblings_of(*owned.product_id()))
    if (const auto lent = for_sibling(sibling); lent.entry != nullptr)
      return lent;
  return {};
}

}  // namespace libchiaking::catalog
