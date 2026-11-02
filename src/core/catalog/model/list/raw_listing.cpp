#include "src/core/catalog/model/list/raw_listing.hpp"

#include "src/core/catalog/model/list/title_name.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <set>

namespace libchiaking::catalog {

class RawListing::Filter {
 public:
  void take(const ListedEntry& entry) {
    if (entry.shelf == Shelf::old_service || entry.shelf == Shelf::public_store)
      take_from_old_service(entry);
    else
      take_from_ps5_store(entry);
  }

  auto list(const Source& source) const
      -> Expected<StreamableList, StreamableList::Invalid> {
    StreamableList::Facts facts{.region = source.region,
                                .settled_locale = source.settled_locale};
    facts.origin = source.origin;
    facts.complete = source.complete;
    facts.fetched_at = source.fetched_at;
    facts.streaming_copies = source.streaming_copies;
    for (const auto& entry : old_service_) {
      const bool dropped = entry.streaming_supported == false;
      if (auto title = title_of(entry, ServiceGeneration::old_service,
                                dropped ? ListStreamability::marked_unstreamable
                                        : ListStreamability::listed,
                                {}))
        facts.titles.push_back(
            {std::move(*title), dropped
                                    ? PlusInclusion::not_included(
                                          PlusInclusion::Source::old_service)
                                    : PlusInclusion::with_premium(
                                          PlusInclusion::Source::old_service)});
    }
    for (const auto& edition : editions_)
      if (auto title = title_of(edition.first, ServiceGeneration::ps5_cloud,
                                ListStreamability::listed, edition.aliases))
        facts.titles.push_back(
            {std::move(*title), inclusion(edition.included)});
    for (const auto& entry : included_only_) {
      const auto streamability = entry.streaming_supported
                                     ? ListStreamability::marked_unstreamable
                                     : ListStreamability::unknown;
      if (auto title =
              title_of(entry, ServiceGeneration::ps5_cloud, streamability, {}))
        facts.included_but_not_streamable.push_back(
            {std::move(*title), inclusion(true)});
    }
    for (const auto& product : flagged_unstreamable_) {
      const auto key = product.title_key();
      const auto number = product.title_number();
      if (!key || !number || streams_.contains(*key) ||
          std::ranges::find(facts.marked_unstreamable, *number) !=
              facts.marked_unstreamable.end())
        continue;
      facts.marked_unstreamable.push_back(*number);
    }
    return StreamableList::of(std::move(facts));
  }

 private:
  struct Edition {
    ListedEntry first;
    bool included{};
    std::vector<ProductId> aliases{};
  };

  static constexpr std::size_t edition_concept_bytes = 79;

  static auto includes_with_plus(Shelf shelf) noexcept -> bool {
    return shelf == Shelf::plus_games || shelf == Shelf::plus_classics ||
           shelf == Shelf::ubisoft_classics ||
           shelf == Shelf::plus_monthly_games;
  }

  static auto inclusion(bool included) noexcept -> PlusInclusion {
    return included
               ? PlusInclusion::with_premium(PlusInclusion::Source::store_lists)
               : PlusInclusion::not_included(
                     PlusInclusion::Source::store_lists);
  }

  static auto platform_of(const ListedEntry& entry) -> Platform {
    const auto named = entry.product.platform();
    if (entry.on_ps5 || named == Platform::ps5) return Platform::ps5;
    if (entry.on_ps4 || named == Platform::ps4) return Platform::ps4;
    return Platform::ps3;
  }

  static auto edition_key(const ListedEntry& entry) -> std::string {
    const auto& grouped =
        entry.concept_id ? entry.concept_id->text() : entry.product.text();
    auto key = grouped.substr(0, edition_concept_bytes);
    const auto named = entry.product.platform();
    key += '|';
    key += named == Platform::ps5 ? "ps5" : named == Platform::ps4 ? "ps4" : "";
    key += '|';
    key += TitleName{entry.name}.squashed();
    return key;
  }

  static auto title_of(const ListedEntry& entry, ServiceGeneration service,
                       ListStreamability streamability,
                       std::vector<ProductId> aliases) -> std::optional<Title> {
    Title::Facts facts{.product = entry.product};
    facts.name = entry.name;
    facts.platform = platform_of(entry);
    facts.service = service;
    facts.streamability = streamability;
    facts.concept_id = entry.concept_id;
    facts.cover_url = entry.cover_url;
    facts.landscape_cover_url = entry.landscape_cover_url;
    facts.concept_page = entry.concept_page;
    facts.aliases = std::move(aliases);
    auto title = Title::from(std::move(facts));
    if (!title) return std::nullopt;
    return std::move(title).value();
  }

  void take_from_old_service(const ListedEntry& entry) {
    if (!known_.insert(entry.product).second) return;
    old_service_.push_back(entry);
  }

  void take_from_ps5_store(const ListedEntry& entry) {
    if (!entry.on_ps5 && !entry.on_ps4) return;
    const auto key = entry.product.title_key();
    const bool streams = entry.streaming_supported.value_or(false);
    const bool included = includes_with_plus(entry.shelf);
    if (entry.shelf == Shelf::all_ps5 && key) {
      on_all_ps5_.insert(*key);
      if (streams)
        streams_.insert(*key);
      else if (entry.streaming_supported)
        flagged_unstreamable_.push_back(entry.product);
    }
    if (!streams) {
      if (included && key && on_all_ps5_.contains(*key)) keep_aside(entry);
      return;
    }
    const auto edition = edition_key(entry);
    if (const auto found = edition_at_.find(edition);
        found != edition_at_.end()) {
      auto& kept = editions_[found->second];
      if (entry.product != kept.first.product &&
          known_.insert(entry.product).second)
        kept.aliases.push_back(entry.product);
      kept.included = kept.included || included;
      return;
    }
    if (const auto listed = edition_of_.find(entry.product);
        listed != edition_of_.end()) {
      editions_[listed->second].included =
          editions_[listed->second].included || included;
      return;
    }
    if (!known_.insert(entry.product).second) return;
    edition_at_.emplace(edition, editions_.size());
    edition_of_.emplace(entry.product, editions_.size());
    editions_.push_back(Edition{entry, included});
  }

  void keep_aside(const ListedEntry& entry) {
    const auto held =
        std::ranges::find(included_only_, entry.product, &ListedEntry::product);
    if (held != included_only_.end())
      *held = entry;
    else
      included_only_.push_back(entry);
  }

  std::vector<ListedEntry> old_service_;
  std::vector<Edition> editions_;
  std::map<std::string, std::size_t> edition_at_;
  std::map<ProductId, std::size_t> edition_of_;
  std::vector<ListedEntry> included_only_;
  std::set<ProductId> known_;
  std::set<TitleKey> on_all_ps5_;
  std::set<TitleKey> streams_;
  std::vector<ProductId> flagged_unstreamable_;
};

auto RawListing::filter_raw_streamable() const
    -> Expected<StreamableList, StreamableList::Invalid> {
  std::vector<const ListedEntry*> by_shelf;
  by_shelf.reserve(entries_.size());
  for (const auto& entry : entries_) by_shelf.push_back(&entry);
  std::ranges::stable_sort(
      by_shelf, {}, [](const ListedEntry* entry) { return entry->shelf; });
  Filter filter;
  for (const auto* entry : by_shelf) filter.take(*entry);
  return filter.list(source_);
}

}  // namespace libchiaking::catalog
