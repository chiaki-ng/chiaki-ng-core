#include "src/core/catalog/adapters/ps/streaming_category.hpp"

#include "src/core/net/transport/http/status.hpp"

#include <algorithm>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace libchiaking::catalog::ps {

StreamingCategory::StreamingCategory(
    net::http::Client& http, const platform::ClockProvider& wall,
    std::size_t maximum_response_bytes) noexcept
    : http_(http),
      stamp_(wall),
      maximum_response_bytes_(maximum_response_bytes) {}

auto StreamingCategory::included(const StoreLocale& locale, int age,
                                 PageCount pages)
    -> Expected<cloud_stream::PremiumCategory, Unanswered> {
  auto scanned = scan(premium_shelf, locale, age, pages);
  if (!scanned) return unexpected(scanned.error());
  const auto read = scanned.value().scan;
  cloud_stream::PremiumCategory category{
      locale, std::move(scanned.value().products), read, stamp_.now()};
  http_.log(platform::LogSeverity::info,
            "[CLOUD_STREAM] premium streaming category: " +
                std::to_string(read.products_read) + " of " +
                std::to_string(read.server_total) + " products read, " +
                std::to_string(category.products().size()) + " kept, " +
                std::to_string(category.included_count()) + " included, " +
                trials_of(category.products()) + ", " +
                (read.complete ? "complete" : "partial"));
  return category;
}

auto StreamingCategory::for_sale(const StoreLocale& locale, int age,
                                 PageCount pages)
    -> Expected<cloud_stream::ForSaleCategory, Unanswered> {
  auto scanned = scan(store_shelf, locale, age, pages);
  if (!scanned) return unexpected(scanned.error());
  const auto read = scanned.value().scan;
  cloud_stream::ForSaleCategory category{
      locale, std::move(scanned.value().products), read, stamp_.now()};
  http_.log(platform::LogSeverity::info,
            "[CLOUD_STREAM] store streaming category: " +
                std::to_string(read.products_read) + " of " +
                std::to_string(read.server_total) + " products read, " +
                std::to_string(category.products().size()) + " kept, " +
                std::to_string(category.for_sale_count()) + " for sale, " +
                trials_of(category.products()) + ", " +
                (read.complete ? "complete" : "partial"));
  return category;
}

auto StreamingCategory::trials_of(
    const std::vector<cloud_stream::PremiumProduct>& products) -> std::string {
  using Feature = cloud_stream::PremiumEntitlement::Feature;
  const std::set<OwnershipId> none;
  const auto count = [&](Feature feature) {
    return std::ranges::count_if(
        products, [&](const cloud_stream::PremiumProduct& product) {
          return product.offers(feature, none);
        });
  };
  return std::to_string(count(Feature::trial)) + " with a trial, " +
         std::to_string(count(Feature::play_first_trial)) +
         " with a Play First Trial";
}

auto StreamingCategory::scan(const Shelf& shelf, const StoreLocale& locale,
                             int age, PageCount& count)
    -> Expected<Scanned, Unanswered> {
  Scanned scanned;
  auto& read = scanned.scan;
  std::string previous_first;
  psn::rest::StoreClient store{http_.limited(maximum_response_bytes_)};
  std::size_t pages = 0;
  while (pages < page_limit) {
    for (auto& asked : ask(store, shelf, read, pages, locale, age)) {
      auto page = collect(shelf, std::move(asked), locale, age);
      if (!page) {
        if (page.error().is_cancelled() || pages == 0)
          return unexpected(page.error());
        return scanned;
      }
      ++pages;
      read.server_total = page.value().server_total;
      if (pages == 1)
        count.expect(static_cast<std::uint32_t>(std::min<std::size_t>(
            std::max<std::size_t>(
                1, (read.server_total + shelf.page_size - 1) / shelf.page_size),
            page_limit)));
      count.read();
      if (page.value().products == 0) {
        read.complete = true;
        return scanned;
      }
      if (page.value().first_product == previous_first) return scanned;
      previous_first = std::move(page.value().first_product);
      read.products_read += page.value().products;
      const bool short_page = page.value().products != shelf.page_size;
      std::ranges::move(page.value().products_kept,
                        std::back_inserter(scanned.products));
      if (read.products_read >= read.server_total) {
        read.complete = true;
        return scanned;
      }
      if (short_page) break;
    }
  }
  return scanned;
}

auto StreamingCategory::ask(psn::rest::StoreClient& store, const Shelf& shelf,
                            const cloud_stream::CategoryScan& read,
                            std::size_t pages, const StoreLocale& locale,
                            int age) -> std::vector<Asked> {
  std::vector<Asked> asked;
  auto offset = read.products_read;
  do {
    asked.push_back({offset, store.start_streaming_category<Reply>(
                                 {.category = shelf.category,
                                  .country = locale.country(),
                                  .language = locale.language(),
                                  .age = age,
                                  .offset = offset,
                                  .limit = shelf.page_size})});
    offset += shelf.page_size;
  } while (pages + asked.size() < page_limit && offset < read.server_total);
  return asked;
}

auto StreamingCategory::collect(const Shelf& shelf, Asked asked,
                                const StoreLocale& locale, int age)
    -> Expected<Page, Unanswered> {
  if (http_.stop_requested()) return unexpected(Unanswered::cancelled());
  const auto reply = std::move(asked.reply).wait();
  if (http_.stop_requested()) return unexpected(Unanswered::cancelled());
  if (reply) return read(reply.value());
  const auto failed = unanswered(reply.error());
  if (failed.is_cancelled()) return unexpected(failed);
  return page_at(shelf, asked.offset, locale, age,
                 RetryPolicy::category_page().attempts() - 1, failed);
}

auto StreamingCategory::page_at(const Shelf& shelf, std::size_t offset,
                                const StoreLocale& locale, int age,
                                int attempts, std::optional<Unanswered> last)
    -> Expected<Page, Unanswered> {
  psn::rest::StoreClient store{http_.limited(maximum_response_bytes_)};
  for (int attempt = 0; attempt < attempts; ++attempt) {
    if (http_.stop_requested()) return unexpected(Unanswered::cancelled());
    const auto reply =
        store.streaming_category<Reply>({.category = shelf.category,
                                         .country = locale.country(),
                                         .language = locale.language(),
                                         .age = age,
                                         .offset = offset,
                                         .limit = shelf.page_size});
    if (http_.stop_requested()) return unexpected(Unanswered::cancelled());
    if (reply) return read(reply.value());
    last = unanswered(reply.error());
    if (last->is_cancelled()) return unexpected(*last);
  }
  http_.log(platform::LogSeverity::warning,
            "[CLOUD_STREAM] could not load the " + std::string{shelf.name} +
                " at offset " + std::to_string(offset));
  return unexpected(last.value_or(Unanswered{Unanswered::Reason::unavailable}));
}

auto StreamingCategory::read(const Reply& reply) -> Expected<Page, Unanswered> {
  const auto stated =
      reply.total_num_products
          ? json::ps::Number::from_token(reply.total_num_products->str)
          : std::nullopt;
  const auto total = stated ? stated->integer().value_or(0) : std::int64_t{};
  if (!reply.products || !reply.total_num_products || total < 0)
    return unexpected(
        Unanswered{Unanswered::Reason::malformed, net::HttpStatus::ok.code()});
  const auto& products = *reply.products;
  Page page;
  page.server_total = static_cast<std::size_t>(total);
  page.products = products.size();
  if (!products.empty() && products.front())
    page.first_product = products.front()->id.view();
  for (const auto& listed : products)
    if (listed)
      if (auto product = product_of(*listed))
        page.products_kept.push_back(std::move(*product));
  return page;
}

auto StreamingCategory::product_of(const Product& listed)
    -> std::optional<cloud_stream::PremiumProduct> {
  auto id = ProductId::parse(listed.id.view());
  if (!id || !listed.skus) return std::nullopt;
  cloud_stream::PremiumProduct::Facts facts{
      .id = std::move(*id),
      .name = std::string{listed.name.view()},
      .master_image_url = master_image_of(listed),
      .concept_id = listed.grouped
                        ? StoreListing::concept_of(listed.grouped->id)
                        : std::nullopt};
  for (const auto& sold : *listed.skus)
    facts.skus.push_back(
        sold ? sku_of(*sold)
             : cloud_stream::PremiumSku{std::nullopt, false, {}});
  return cloud_stream::PremiumProduct{std::move(facts)};
}

auto StreamingCategory::sku_of(const StoreSku& sold)
    -> cloud_stream::PremiumSku {
  const bool branded =
      sold.content_collections &&
      std::ranges::any_of(
          *sold.content_collections, [](const auto& collection) {
            return collection &&
                   collection->branding.view() ==
                       cloud_stream::PremiumSku::premium_stream_branding;
          });
  std::vector<cloud_stream::PremiumEntitlement> entitlements;
  if (sold.entitlements)
    for (const auto& granted : *sold.entitlements)
      entitlements.push_back(granted ? entitlement_of(*granted)
                                     : cloud_stream::PremiumEntitlement{{}});
  return cloud_stream::PremiumSku{SkuId::parse(sold.id.view()), branded,
                                  std::move(entitlements)};
}

auto StreamingCategory::entitlement_of(const StoreEntitlement& granted)
    -> cloud_stream::PremiumEntitlement {
  using Entitlement = cloud_stream::PremiumEntitlement;
  const auto stated = granted.id.view();
  auto id = OwnershipId::parse(stated);
  const auto& streams = granted.streaming_supported;
  const bool decided =
      streams && (streams->str == "true" || streams->str == "false");
  return Entitlement{Entitlement::Facts{
      .id = id,
      .unreadable_id = !stated.empty() && !id,
      .package_type = std::string{granted.package_type.view()},
      .feature = Entitlement::feature_named(granted.feature_type.view()),
      .streaming_supported =
          decided ? std::optional{streams->str == "true"} : std::nullopt}};
}

auto StreamingCategory::master_image_of(const Product& listed) -> std::string {
  if (!listed.media || !listed.media->images) return {};
  const auto& images = *listed.media->images;
  const auto master = std::ranges::find_if(images, [](const auto& image) {
    return image && image->type.view() == master_image;
  });
  return master != images.end() ? std::string{(*master)->url.view()}
                                : std::string{};
}

auto StreamingCategory::unanswered(const psn::rest::StoreError& error) noexcept
    -> Unanswered {
  using Reason = Unanswered::Reason;
  if (const auto& failure = error.failure()) {
    if (failure->kind() == net::http::Failure::Kind::cancelled)
      return Unanswered::cancelled();
    return Unanswered{Reason::unavailable};
  }
  if (error.reason() == psn::rest::StoreError::Reason::malformed)
    return Unanswered{Reason::malformed, error.http_status()};
  if (error.http_status() >= 500)
    return Unanswered{Reason::unavailable, error.http_status()};
  return Unanswered{Reason::rejected, error.http_status()};
}

}  // namespace libchiaking::catalog::ps
