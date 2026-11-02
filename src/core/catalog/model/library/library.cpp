#include "libchiaking/detail/catalog/library.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace libchiaking::catalog {

Library::Library(AccountId account, std::vector<OwnedTitle> titles,
                 std::vector<Bundle> bundles, Time fetched_at) noexcept
    : account_(account),
      titles_(std::move(titles)),
      bundles_(std::move(bundles)),
      fetched_at_(fetched_at) {}

auto Library::of(AccountId account, std::vector<OwnedTitle> titles,
                 std::vector<Bundle> bundles, Time fetched_at,
                 LibraryState ps5_ownership) -> Expected<Library, Invalid> {
  for (auto title = titles.begin(); title != titles.end(); ++title)
    if (std::ranges::find(titles.begin(), title, title->id(),
                          &OwnedTitle::id) != title)
      return unexpected(Invalid::duplicate_ownership);
  Library library{account, std::move(titles), std::move(bundles), fetched_at};
  library.ps5_ownership_ = ps5_ownership;
  return library;
}

auto Library::find(const OwnershipId& id) const noexcept -> const OwnedTitle* {
  const auto found = std::ranges::find(titles_, id, &OwnedTitle::id);
  return found != titles_.end() ? &*found : nullptr;
}

auto Library::held_under(const ProductId& product) const noexcept
    -> const OwnedTitle* {
  const auto found =
      std::ranges::find_if(titles_, [&product](const OwnedTitle& title) {
        return title.is_held_under(product);
      });
  return found != titles_.end() ? &*found : nullptr;
}

auto Library::holds_title(const TitleNumber& number) const -> bool {
  return std::ranges::any_of(titles_, [&number](const OwnedTitle& title) {
    return title.title_number() == number;
  });
}

auto Library::siblings_of(const ProductId& product) const noexcept
    -> std::span<const ProductId> {
  const auto found = std::ranges::find(bundles_, product, &Bundle::product);
  return found != bundles_.end() ? std::span<const ProductId>{found->siblings}
                                 : std::span<const ProductId>{};
}

auto Library::concept_of_copy(const ProductId& product) const
    -> std::optional<ConceptId> {
  for (const auto& title : titles_) {
    if (!title.is_held_under(product)) continue;
    const auto learned =
        std::ranges::find(copy_concepts_, title.id(), &CopyConcept::id);
    if (learned != copy_concepts_.end()) return learned->grouped_under;
  }
  return std::nullopt;
}

auto Library::with_learned_concepts(
    std::span<const LearnedConcept> learned) const -> Library {
  auto taught = *this;
  taught.copy_concepts_.clear();
  for (const auto& title : titles_) {
    if (title.concept_id()) continue;
    auto found = std::ranges::find_if(
        learned,
        [&title](const LearnedConcept& row) { return row.id == title.id(); });
    if (found == learned.end() && title.product_id())
      found =
          std::ranges::find_if(learned, [&title](const LearnedConcept& row) {
            return row.product == title.product_id();
          });
    if (found != learned.end())
      taught.copy_concepts_.push_back({title.id(), found->grouped_under});
  }
  return taught;
}

auto Library::with(OwnedTitle title, Time checked_at) const -> Library {
  auto titles = titles_;
  const auto held = std::ranges::find(titles, title.id(), &OwnedTitle::id);
  if (held != titles.end())
    *held = std::move(title);
  else
    titles.push_back(std::move(title));
  Library changed{account_, std::move(titles), bundles_, checked_at};
  changed.copy_concepts_ = copy_concepts_;
  changed.ps5_ownership_ = ps5_ownership_;
  return changed;
}

auto Library::with_ps5_part(std::vector<OwnedTitle> read,
                            LibraryState ownership) const -> Library {
  auto changed = *this;
  if (ownership != LibraryState::unavailable || !read.empty())
    std::erase_if(changed.titles_, [](const OwnedTitle& title) {
      return title.service() == ServiceGeneration::ps5_cloud;
    });
  for (auto& title : read)
    if (std::ranges::find(changed.titles_, title.id(), &OwnedTitle::id) ==
        changed.titles_.end())
      changed.titles_.push_back(std::move(title));
  changed.ps5_ownership_ = ownership;
  return changed;
}

}  // namespace libchiaking::catalog
