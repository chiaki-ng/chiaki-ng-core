#include "src/core/catalog/adapters/ps/public_store_walk.hpp"

#include "src/core/catalog/adapters/ps/store_listing.hpp"
#include "src/core/net/transport/http/status.hpp"
#include "src/core/utils/text/ascii.hpp"

#include <algorithm>
#include <deque>
#include <ranges>
#include <utility>

namespace libchiaking::catalog::ps {

PublicStoreWalk::PublicStoreWalk(net::http::Client& http,
                                 const platform::ClockProvider& wall,
                                 std::size_t maximum_response_bytes) noexcept
    : http_(http),
      stamp_(wall),
      maximum_response_bytes_(maximum_response_bytes) {}

auto PublicStoreWalk::walk(const StoreLocale& region, PageCount pages)
    -> Expected<RawListing, Unanswered> {
  psn::rest::KamajiClient store{http_.limited(maximum_response_bytes_)};
  const Storefront own{std::string{region.country()},
                       std::string{region.language()},
                       ListOrigin::regional_store};
  auto listing = walked_store(store, own, region, pages);
  if (!listing && !http_.stop_requested()) {
    const Storefront shared{
        std::string{StoreLocale::shared_store_country(region.country())},
        std::string{fallback_language}, ListOrigin::public_store};
    if (!text::ieq(shared.country, own.country) ||
        !text::ieq(shared.language, own.language)) {
      http_.log(platform::LogSeverity::info,
                "[UNIFIED] APOLLO store " + own.country +
                    " has no root; walking the " + shared.country + " store");
      listing = walked_store(store, shared, region, pages);
    }
  }
  if (http_.stop_requested()) return unexpected(Unanswered::cancelled());
  if (!listing) return unexpected(Unanswered{Unanswered::Reason::unavailable});
  return std::move(*listing);
}

auto PublicStoreWalk::walked_store(psn::rest::KamajiClient& store,
                                   const Storefront& front,
                                   const StoreLocale& region,
                                   PageCount& pages) const
    -> std::optional<RawListing> {
  Progress progress{.pages = pages};
  const auto root = walked(store, front, root_for(front.country),
                           Nested::collected, progress, std::nullopt);
  if (root == Walked::missing) return std::nullopt;
  bool complete = root == Walked::read;
  const auto children = progress.children;
  std::vector<std::optional<Asked>> firsts;
  firsts.reserve(children.size());
  for (const auto& child : children)
    firsts.push_back(ask(store, front, child, 0, progress));
  for (auto&& [child, first] : std::views::zip(children, firsts))
    if (walked(store, front, child, Nested::refused, progress,
               std::move(first)) != Walked::read)
      complete = false;
  http_.log(platform::LogSeverity::info,
            "[UNIFIED] APOLLO " + front.country +
                " walk: " + std::to_string(children.size()) + " containers, " +
                std::to_string(progress.entries.size()) + " titles" +
                (complete ? "" : " (incomplete)"));
  const auto shop = StoreLocale::parse(front.language + "-" + front.country);
  RawListing listing{
      {region, shop.value_or(region), front.origin, complete, stamp_.now()}};
  for (auto& entry : progress.entries) listing.add(std::move(entry));
  return listing;
}

auto PublicStoreWalk::root_for(std::string_view country) -> std::string_view {
  return StoreLocale::shared_store_country(country) == "US" ? americas_root
                                                            : other_root;
}

void PublicStoreWalk::warn(const std::string& message) const {
  http_.log(platform::LogSeverity::warning,
            "[UNIFIED] APOLLO fallback " + message);
}

auto PublicStoreWalk::walked(psn::rest::KamajiClient& store,
                             const Storefront& front,
                             std::string_view container, Nested nested,
                             Progress& progress,
                             std::optional<Asked> first) const -> Walked {
  const std::string name{container};
  int total = -1;
  bool children_dropped = false;
  std::deque<Asked> queue;
  if (!first) first = ask(store, front, container, 0, progress);
  if (first) queue.push_back(std::move(*first));
  int next = 0;
  for (;;) {
    if (queue.empty()) {
      warn("page budget exhausted at " + name +
           " start=" + std::to_string(next) + "; list incomplete");
      return Walked::incomplete;
    }
    const int start = queue.front().start;
    const auto page = std::move(queue.front().page).wait();
    queue.pop_front();
    if (!page && start == 0 &&
        page.error().reason() == psn::rest::StoreError::Reason::refused &&
        net::HttpStatus{page.error().http_status()} ==
            net::HttpStatus::not_found)
      return Walked::missing;
    if (!page) {
      const bool unparseable =
          page.error().reason() == psn::rest::StoreError::Reason::malformed;
      warn(name + " page at start=" + std::to_string(start) +
           (unparseable ? " unparseable; list incomplete"
                        : " failed; list incomplete"));
      return Walked::incomplete;
    }
    progress.pages.read();
    if (total < 0 && page.value().total_results.value_or(-1) >= 0) {
      total = *page.value().total_results;
      progress.pages.expect(static_cast<std::uint32_t>(
          (std::min(total, start_limit) + page_size - 1) / page_size));
    }
    int link_count = 0;
    for (const auto& row : page.value().links) {
      ++link_count;
      const auto fields = StoreListing::row_of(row);
      const auto id = fields.id.view();
      const auto type = fields.container_type.view();
      if (text::ieq(type, "product")) {
        if (id.empty() || progress.seen.contains(id)) continue;
        progress.seen.emplace(id);
        if (auto entry = StoreListing::listed_on(row, Shelf::public_store))
          progress.entries.push_back(std::move(*entry));
      } else if (text::ieq(type, "container")) {
        if (nested == Nested::refused) {
          if (!children_dropped)
            warn(name + " contains nested container " +
                 (id.empty() ? std::string{"(no id)"} : std::string{id}) +
                 "; not walked, list incomplete");
          children_dropped = true;
          continue;
        }
        if (id.empty()) {
          warn(name + " lists a container without an id; list incomplete");
          children_dropped = true;
          continue;
        }
        if (std::ranges::find(progress.children, id) != progress.children.end())
          continue;
        if (std::ssize(progress.children) >= child_limit) {
          warn("child " + std::string{id} + " dropped; list incomplete");
          children_dropped = true;
          continue;
        }
        progress.children.emplace_back(id);
      }
    }
    next = start + page_size;
    if (total >= 0 && next >= total)
      return children_dropped ? Walked::incomplete : Walked::read;
    if (link_count <= 0) {
      warn(name + " ended at start=" + std::to_string(start) +
           " without a confirmed total; list incomplete");
      return Walked::incomplete;
    }
    if (next >= start_limit) {
      warn(name + " page cap hit at start=" + std::to_string(next) +
           "; list incomplete");
      return Walked::incomplete;
    }
    if (queue.empty()) {
      const int end = total >= 0 ? std::min(total, start_limit) : next + 1;
      for (int more = next; more < end; more += page_size) {
        auto asked = ask(store, front, container, more, progress);
        if (!asked) break;
        queue.push_back(std::move(*asked));
      }
    }
  }
}

auto PublicStoreWalk::ask(psn::rest::KamajiClient& store,
                          const Storefront& front, std::string_view container,
                          int start, Progress& progress)
    -> std::optional<Asked> {
  if (progress.pages_left <= 0) return std::nullopt;
  --progress.pages_left;
  return Asked{start, store.start_container_page(front.country, front.language,
                                                 container, start, page_size)};
}

}  // namespace libchiaking::catalog::ps
