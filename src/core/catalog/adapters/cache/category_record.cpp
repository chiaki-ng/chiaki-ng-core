#include "src/core/catalog/adapters/cache/category_record.hpp"

#include "src/core/catalog/adapters/cache/cache.hpp"

#include <ranges>
#include <utility>
#include <vector>

namespace libchiaking::catalog::cache {

template <class Category>
CategoryRecord<Category>::CategoryRecord(StoreLocale locale,
                                         std::optional<Category> category,
                                         std::optional<Time> failed_at) noexcept
    : locale_(std::move(locale)),
      category_(std::move(category)),
      failed_at_(failed_at) {}

template <class Category>
auto CategoryRecord<Category>::nothing_held(StoreLocale locale)
    -> CategoryRecord {
  return CategoryRecord{std::move(locale), std::nullopt, std::nullopt};
}

template <class Category>
auto CategoryRecord<Category>::for_age(int age) const -> CategoryRecord {
  auto asked = *this;
  asked.age_ = age;
  return asked;
}

template <class Category>
auto CategoryRecord<Category>::name_of(const StoreLocale& locale)
    -> std::optional<platform::RecordName> {
  return Cache::shared_record(kind, locale.lowercase());
}

template <class Category>
auto CategoryRecord<Category>::is_fresh_at(Time now) const noexcept -> bool {
  return category_ && is_fresh(category_->fetched_at(), lifetime, now);
}

template <class Category>
auto CategoryRecord<Category>::was_fetched_recently_at(Time now) const noexcept
    -> bool {
  return category_ && is_fresh(category_->fetched_at(), refetch_cooldown, now);
}

template <class Category>
auto CategoryRecord<Category>::retry_after(Time now) const noexcept
    -> std::optional<std::chrono::milliseconds> {
  if (!failed_at_ || now < *failed_at_ || now - *failed_at_ >= failure_back_off)
    return std::nullopt;
  return std::chrono::milliseconds{failure_back_off} - (now - *failed_at_);
}

template <class Category>
auto CategoryRecord<Category>::next_step(Time now, Demand demand) const noexcept
    -> Step {
  if (demand == Demand::start_over && was_fetched_recently_at(now))
    return Step::serve_recently_fetched;
  if (demand == Demand::when_stale && is_fresh_at(now)) return Step::serve;
  if (retry_after(now)) return Step::wait_after_failure;
  return Step::fetch;
}

template <class Category>
auto CategoryRecord<Category>::accepts(const Category& answer) const -> bool {
  return answer.is_plausible() && answer.locale() == locale_;
}

template <class Category>
auto CategoryRecord<Category>::with_answer(const Category& answer,
                                           Time now) const -> CategoryRecord {
  if (!accepts(answer)) return with_failure(now);
  CategoryRecord answered{locale_, answer, std::nullopt};
  answered.age_ = age_;
  return answered;
}

template <class Category>
auto CategoryRecord<Category>::with_failure(Time now) const -> CategoryRecord {
  CategoryRecord failed{locale_, category_, now};
  failed.age_ = age_;
  return failed;
}

template <class Category>
auto CategoryRecord<Category>::body() const -> Body {
  Body body{.age = age_};
  if (category_) {
    const auto& scanned = category_->scan();
    body.fetched_at_ms = milliseconds_of(category_->fetched_at());
    body.scan =
        Scan{.server_total = static_cast<std::int64_t>(scanned.server_total),
             .products_read = static_cast<std::int64_t>(scanned.products_read),
             .complete = scanned.complete};
    body.products =
        category_->products() |
        std::views::transform([](const cloud_stream::PremiumProduct& kept) {
          return product(kept);
        }) |
        std::ranges::to<std::vector>();
  }
  if (failed_at_) body.failed_at_ms = milliseconds_of(*failed_at_);
  return body;
}

template <class Category>
auto CategoryRecord<Category>::product(const cloud_stream::PremiumProduct& kept)
    -> Product {
  const auto& facts = kept.facts();
  return Product{
      .id = facts.id.text(),
      .name = facts.name,
      .master_image = facts.master_image_url,
      .concept_id = facts.concept_id ? std::optional{facts.concept_id->text()}
                                     : std::nullopt,
      .skus = facts.skus |
              std::views::transform([](const cloud_stream::PremiumSku& held) {
                return sku(held);
              }) |
              std::ranges::to<std::vector>()};
}

template <class Category>
auto CategoryRecord<Category>::sku(const cloud_stream::PremiumSku& kept)
    -> Sku {
  return Sku{
      .id = kept.id() ? std::optional{kept.id()->text()} : std::nullopt,
      .branded = kept.is_premium_stream_branded(),
      .entitlements = kept.entitlements() |
                      std::views::transform(
                          [](const cloud_stream::PremiumEntitlement& held) {
                            return entitlement(held);
                          }) |
                      std::ranges::to<std::vector>()};
}

template <class Category>
auto CategoryRecord<Category>::entitlement(
    const cloud_stream::PremiumEntitlement& kept) -> Entitlement {
  const auto& facts = kept.facts();
  return Entitlement{
      .id = facts.id ? std::optional{facts.id->text()} : std::nullopt,
      .unreadable_id = facts.unreadable_id,
      .package_type = facts.package_type,
      .feature = facts.feature,
      .streaming_supported = facts.streaming_supported};
}

template <class Category>
auto CategoryRecord<Category>::from_body(const Body& body,
                                         const StoreLocale& locale)
    -> std::optional<CategoryRecord> {
  auto read = unaged_from(body, locale);
  if (read && body.age) {
    if (*body.age < 0 || *body.age > oldest_age) return std::nullopt;
    read->age_ = static_cast<int>(*body.age);
  }
  return read;
}

template <class Category>
auto CategoryRecord<Category>::unaged_from(const Body& body,
                                           const StoreLocale& locale)
    -> std::optional<CategoryRecord> {
  const auto failed_at = time_of(body.failed_at_ms);
  if (!body.products) {
    if (!failed_at) return std::nullopt;
    return CategoryRecord{locale, std::nullopt, failed_at};
  }
  const auto scanned = body.scan ? scan(*body.scan) : std::nullopt;
  auto products = every_read<cloud_stream::PremiumProduct>(
      *body.products, [](const Product& wire) { return product(wire); });
  if (!body.fetched_at_ms || !scanned || !products) return std::nullopt;
  Category category{locale, std::move(*products), *scanned,
                    time_of(*body.fetched_at_ms)};
  if (!category.is_plausible()) return std::nullopt;
  return CategoryRecord{locale, std::move(category), failed_at};
}

template <class Category>
auto CategoryRecord<Category>::scan(const Scan& wire)
    -> std::optional<cloud_stream::CategoryScan> {
  if (wire.server_total < 0 || wire.products_read < 0) return std::nullopt;
  return cloud_stream::CategoryScan{
      static_cast<std::size_t>(wire.server_total),
      static_cast<std::size_t>(wire.products_read), wire.complete};
}

template <class Category>
auto CategoryRecord<Category>::product(const Product& wire)
    -> std::optional<cloud_stream::PremiumProduct> {
  auto id = ProductId::parse(wire.id);
  const auto grouped = stated<ConceptId>(
      wire.concept_id,
      [](const std::string& text) { return ConceptId::parse(text); });
  auto skus = every_read<cloud_stream::PremiumSku>(
      wire.skus, [](const Sku& held) { return sku(held); });
  if (!id || !grouped || !skus) return std::nullopt;
  return cloud_stream::PremiumProduct{
      cloud_stream::PremiumProduct::Facts{.id = std::move(*id),
                                          .name = wire.name,
                                          .master_image_url = wire.master_image,
                                          .concept_id = *grouped,
                                          .skus = std::move(*skus)}};
}

template <class Category>
auto CategoryRecord<Category>::sku(const Sku& wire)
    -> std::optional<cloud_stream::PremiumSku> {
  auto id = stated<SkuId>(
      wire.id, [](const std::string& text) { return SkuId::parse(text); });
  auto entitlements = every_read<cloud_stream::PremiumEntitlement>(
      wire.entitlements,
      [](const Entitlement& held) { return entitlement(held); });
  if (!id || !entitlements) return std::nullopt;
  return cloud_stream::PremiumSku{std::move(*id), wire.branded,
                                  std::move(*entitlements)};
}

template <class Category>
auto CategoryRecord<Category>::entitlement(const Entitlement& wire)
    -> std::optional<cloud_stream::PremiumEntitlement> {
  auto id = stated<OwnershipId>(wire.id, [](const std::string& text) {
    return OwnershipId::parse(text);
  });
  if (!id) return std::nullopt;
  return cloud_stream::PremiumEntitlement{
      cloud_stream::PremiumEntitlement::Facts{
          .id = std::move(*id),
          .unreadable_id = wire.unreadable_id,
          .package_type = wire.package_type,
          .feature = wire.feature,
          .streaming_supported = wire.streaming_supported}};
}

template class CategoryRecord<cloud_stream::PremiumCategory>;
template class CategoryRecord<cloud_stream::ForSaleCategory>;

}  // namespace libchiaking::catalog::cache
