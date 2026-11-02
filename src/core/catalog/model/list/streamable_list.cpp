#include "libchiaking/detail/catalog/streamable_list.hpp"

#include <algorithm>
#include <utility>

namespace libchiaking::catalog {

StreamableList::StreamableList(Facts facts)
    : region_(std::move(facts.region)),
      settled_locale_(std::move(facts.settled_locale)),
      origin_(facts.origin),
      complete_(facts.complete),
      fetched_at_(facts.fetched_at),
      titles_(std::move(facts.titles)),
      not_streamable_(std::move(facts.included_but_not_streamable)),
      marked_(facts.marked_unstreamable.begin(),
              facts.marked_unstreamable.end()),
      streaming_copies_(facts.streaming_copies) {
  for (const auto& entry : titles_) {
    if (const auto number = entry.title.title_number()) listed_.insert(*number);
    for (const auto& alias : entry.title.aliases())
      if (const auto number = alias.title_number()) listed_.insert(*number);
  }
}

auto StreamableList::of(Facts facts) -> Expected<StreamableList, Invalid> {
  for (const auto* entries :
       {&facts.titles, &facts.included_but_not_streamable})
    for (auto entry = entries->begin(); entry != entries->end(); ++entry) {
      const auto repeated = std::ranges::any_of(
          entries->begin(), entry, [&entry](const Entry& earlier) {
            return earlier.title.is_known_as(entry->title.product_id());
          });
      if (repeated) return unexpected(Invalid::duplicate_product);
    }
  return StreamableList{std::move(facts)};
}

auto StreamableList::find(const ProductId& product) const noexcept
    -> const Entry* {
  return entry_for(titles_, product);
}

auto StreamableList::find_not_streamable(
    const ProductId& product) const noexcept -> const Entry* {
  return entry_for(not_streamable_, product);
}

auto StreamableList::covers(const ProductId& product) const noexcept -> bool {
  return find(product) != nullptr;
}

auto StreamableList::lists(const TitleNumber& number) const -> bool {
  return listed_.contains(number);
}

auto StreamableList::marks_unstreamable(const TitleNumber& number) const
    -> bool {
  return marked_.contains(number) && !listed_.contains(number);
}

auto StreamableList::count_on(ServiceGeneration service) const noexcept
    -> std::size_t {
  return static_cast<std::size_t>(
      std::ranges::count_if(titles_, [service](const Entry& entry) {
        return entry.title.service() == service;
      }));
}

auto StreamableList::entry_for(std::span<const Entry> entries,
                               const ProductId& product) noexcept
    -> const Entry* {
  const auto found =
      std::ranges::find_if(entries, [&product](const Entry& entry) {
        return entry.title.is_known_as(product);
      });
  return found != entries.end() ? &*found : nullptr;
}

}  // namespace libchiaking::catalog
