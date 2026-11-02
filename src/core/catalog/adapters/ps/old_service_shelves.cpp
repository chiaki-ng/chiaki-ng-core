#include "src/core/catalog/adapters/ps/old_service_shelves.hpp"

#include "src/core/catalog/adapters/ps/store_listing.hpp"
#include "src/core/catalog/model/retry_policy.hpp"
#include "src/core/net/transport/http/status.hpp"
#include "src/core/users/ports/catalog_calls.hpp"
#include "src/core/utils/text/ascii.hpp"

#include <algorithm>
#include <iterator>
#include <set>
#include <utility>

namespace libchiaking::catalog::ps {

OldServiceShelves::OldServiceShelves(
    users::PsUser& user, net::http::Client& http,
    const platform::ClockProvider& wall,
    std::size_t maximum_response_bytes) noexcept
    : user_(user),
      http_(http),
      stamp_(wall),
      maximum_response_bytes_(maximum_response_bytes) {}

auto OldServiceShelves::walk(PageCount pages)
    -> Expected<RawListing, Unanswered> {
  using platform::LogSeverity;
  auto opened = users::CatalogCalls{user_}.old_service_store(
      http_, maximum_response_bytes_);
  if (!opened) return unexpected(Unanswered{opened.error()});
  auto& store = opened.value();
  http_.log(LogSeverity::info,
            "[LEGACY_CLOUD_STREAM] Session created, fetching stores");

  const auto base_url = store.stores();
  if (!base_url) return unexpected(without_stores(base_url.error()));
  http_.log(LogSeverity::info,
            "[LEGACY_CLOUD_STREAM] Stores OK, base_url=" + base_url.value());

  pages.expect(1);
  const auto shelves = shelves_at(store, base_url.value());
  if (!shelves) return unexpected(shelves.error());
  pages.read();
  auto source = region_of(base_url.value());
  if (!source) return unexpected(Unanswered{Unanswered::Reason::malformed});

  std::vector<ListedEntry> entries;
  std::set<std::string, std::less<>> seen;
  for (std::size_t index = 0; index < shelves.value().size(); ++index) {
    const auto& shelf = shelves.value()[index];
    if (index != 0 && !http_.wait(shelf_delay))
      return unexpected(Unanswered::cancelled());
    constexpr auto retry = RetryPolicy::shelf();
    auto page = titles_on(store, shelf, pages);
    for (int asked = 1; !page && retry.asks_again(asked); ++asked) {
      if (!http_.wait(retry.pause()))
        return unexpected(Unanswered::cancelled());
      page = titles_on(store, shelf, pages);
    }
    if (!page) {
      source->complete = false;
      http_.log(LogSeverity::warning, "[LEGACY_CLOUD_STREAM] category " +
                                          std::to_string(index) +
                                          " failed; catalog incomplete");
      continue;
    }
    for (const auto& link : page->links) {
      const auto row = StoreListing::row_of(link);
      const auto id = row.id.view();
      if (id.empty() || seen.contains(id)) continue;
      seen.emplace(id);
      if (auto entry = StoreListing::listed_on(link, Shelf::old_service))
        entries.push_back(std::move(*entry));
    }
  }
  if (http_.stop_requested()) return unexpected(Unanswered::cancelled());

  http_.log(LogSeverity::info,
            "[LEGACY_CLOUD_STREAM] APOLLOROOT native: " +
                std::to_string(entries.size()) + " games from " +
                std::to_string(shelves.value().size()) + " lists");
  source->fetched_at = stamp_.now();
  RawListing listing{std::move(*source)};
  for (auto& entry : entries) listing.add(std::move(entry));
  return listing;
}

auto OldServiceShelves::access_shown_by(
    const Expected<RawListing, Unanswered>& walked) const -> CloudAccess {
  if (walked) {
    const auto& source = walked.value().source();
    return CloudAccess::available(
        user_.account_id(),
        {walked.value().entries().size(), source.complete, source.origin},
        stamp_.now());
  }
  return *CloudAccess::without_listing(
      user_.account_id(), walked.error().cloud_availability(), stamp_.now());
}

auto OldServiceShelves::store_region_of(std::string_view base_url)
    -> users::Region {
  base_url = text::c_string(base_url);
  constexpr std::string_view marker = "/container/";
  const auto start = base_url.find(marker);
  if (start == std::string_view::npos) return {};
  const auto component = [&](std::size_t from) -> std::string_view {
    const auto slash = base_url.find('/', from);
    if (slash == std::string_view::npos || slash == from ||
        slash - from > region_text_limit)
      return {};
    return base_url.substr(from, slash - from);
  };
  users::Region region;
  region.country = component(start + marker.size());
  if (!region.country.empty())
    region.language =
        component(start + marker.size() + region.country.size() + 1);
  return region;
}

auto OldServiceShelves::shelves_at(psn::rest::KamajiClient& store,
                                   std::string_view base_url)
    -> Expected<std::vector<std::string>, Unanswered> {
  const auto root = store.container(base_url, root_page_size);
  if (!root) return unexpected(unanswered(root.error()));
  std::vector<std::string> shelves;
  for (const auto& link : root.value().links) {
    if (shelves.size() >= shelf_limit) break;
    const auto row = StoreListing::row_of(link);
    const auto url = row.url.view();
    if (!url.empty()) shelves.emplace_back(url);
  }
  if (shelves.empty())
    return unexpected(Unanswered{Unanswered::Reason::malformed});
  return shelves;
}

auto OldServiceShelves::titles_on(psn::rest::KamajiClient& store,
                                  std::string_view shelf, PageCount& pages)
    -> std::optional<psn::rest::ContainerPage> {
  auto first = store.category(shelf, 0, shelf_page_size);
  if (!first) return std::nullopt;
  auto titles = std::move(first.value());
  const int total = titles.total_results.value_or(0);
  pages.expect(static_cast<std::uint32_t>(
      std::max(1, (std::min(total, shelf_start_limit) + shelf_page_size - 1) /
                      shelf_page_size)));
  pages.read();
  for (int start = shelf_page_size; start < std::min(total, shelf_start_limit);
       start += shelf_page_size) {
    auto page = store.category(shelf, start, shelf_page_size);
    if (!page) return std::nullopt;
    pages.read();
    if (page.value().links.empty()) break;
    std::ranges::move(page.value().links, std::back_inserter(titles.links));
  }
  return titles;
}

auto OldServiceShelves::region_of(std::string_view base_url) const
    -> std::optional<RawListing::Source> {
  const auto shop = StoreLocale::for_account(store_region_of(base_url));
  const auto known = user_.region();
  auto region = known ? StoreLocale::for_account(*known) : std::nullopt;
  if (!region) region = shop;
  if (!region) return std::nullopt;
  return RawListing::Source{
      *region, shop.value_or(*region), ListOrigin::account_service, true, {}};
}

auto OldServiceShelves::unanswered(const psn::rest::StoreError& error) noexcept
    -> Unanswered {
  using Reason = Unanswered::Reason;
  switch (error.reason()) {
    case psn::rest::StoreError::Reason::failed:
      if (error.failure() && error.failure()->is_cancelled())
        return Unanswered::cancelled();
      break;
    case psn::rest::StoreError::Reason::refused:
    case psn::rest::StoreError::Reason::ineligible:
      if (net::HttpStatus{error.http_status()}.is_client_error())
        return Unanswered{Reason::rejected, error.http_status()};
      break;
    case psn::rest::StoreError::Reason::malformed:
      return Unanswered{Reason::malformed, error.http_status()};
  }
  return Unanswered{Reason::unavailable, error.http_status()};
}

auto OldServiceShelves::without_stores(
    const psn::rest::StoreError& error) noexcept -> Unanswered {
  if (error.reason() == psn::rest::StoreError::Reason::refused &&
      net::HttpStatus{error.http_status()}.is_client_error())
    return Unanswered{Unanswered::Reason::region_unsupported,
                      error.http_status()};
  return unanswered(error);
}

}  // namespace libchiaking::catalog::ps
