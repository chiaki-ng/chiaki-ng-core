/// @file
/// @brief Declares cache::CategoryRecord, the stored form of a PS5 store
/// category (Premium or for sale) for one locale and age, with the rules
/// that decide when it is served, refetched or left alone after a failure.
///
/// Each category is held per locale and age with a week's lifetime, an
/// hour's refetch cooldown and half an hour's back-off after a failure. A
/// failed fetch keeps the last good category and records the failure time.
#pragma once

#include "src/core/catalog/adapters/cache/record_format.hpp"
#include "src/core/catalog/model/cloud_stream/for_sale_category.hpp"
#include "src/core/catalog/model/cloud_stream/premium_category.hpp"

#include <libchiaking/detail/catalog/store_locale.hpp>
#include <libchiaking/platform/storage/record_store.hpp>

#include <chrono>
#include <concepts>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::catalog::cache {

namespace category {

/// @brief The stored form of a cloud_stream::PremiumEntitlement. Plain data.
struct Entitlement {
  /// @brief The entitlement id text; absent when PS sent none or it did
  /// not parse.
  std::optional<std::string> id;
  /// @brief PS sent an id that did not parse.
  bool unreadable_id{};
  /// @brief The packageType.
  std::string package_type;
  /// @brief The featureType.
  cloud_stream::PremiumEntitlement::Feature feature{};
  /// @brief streamingSupported, when it was exactly true or false.
  std::optional<bool> streaming_supported;
};

/// @brief The stored form of a cloud_stream::PremiumSku. Plain data.
struct Sku {
  /// @brief The SKU id text, when known.
  std::optional<std::string> id;
  /// @brief Whether the SKU is branded PS_PLUS_STREAM_PS5.
  bool branded{};
  /// @brief The SKU's entitlements in PS's order.
  std::vector<Entitlement> entitlements;
};

/// @brief The stored form of a cloud_stream::PremiumProduct. Plain data.
struct Product {
  /// @brief The product id text.
  std::string id;
  /// @brief The display name.
  std::string name;
  /// @brief The MASTER image URL; empty when none.
  std::string master_image;
  /// @brief The concept id text, when known.
  std::optional<std::string> concept_id;
  /// @brief The SKUs in PS's order.
  std::vector<Sku> skus;
};

/// @brief The stored form of a cloud_stream::CategoryScan; negative counts make
/// the record unreadable. Plain data.
struct Scan {
  /// @brief The server's stated total.
  std::int64_t server_total{};
  /// @brief Products carried by the pages read.
  std::int64_t products_read{};
  /// @brief Whether the scan reached the end.
  bool complete{};
};

/// @brief The body of a category record. Plain data.
///
/// A record holds a category (fetched_at_ms, scan and products all set), a
/// failure time, or both.
struct Body {
  /// @brief When the held category was fetched; wall-clock milliseconds.
  std::optional<std::int64_t> fetched_at_ms;
  /// @brief How the held category's scan went.
  std::optional<Scan> scan;
  /// @brief The held category's products; absent when only a failure is
  /// recorded.
  std::optional<std::vector<Product>> products;
  /// @brief When the latest fetch failed; wall-clock milliseconds.
  std::optional<std::int64_t> failed_at_ms;
  /// @brief The age the category was asked with, 0 to
  /// CategoryRecord::oldest_age.
  std::optional<std::int64_t> age;
};

/// @brief What the caller wants from a held category.
enum class Demand : std::uint8_t {
  /// @brief Serve the held category while it is within its lifetime.
  when_stale,
  /// @brief The user asked to start over: refetch unless the category was
  /// fetched within the refetch cooldown.
  start_over
};
/// @brief What to do with a held category now.
enum class Step : std::uint8_t {
  /// @brief The held category is fresh; serve it.
  serve,
  /// @brief Starting over, but the category was fetched within the last
  /// hour; serve it instead of refetching.
  serve_recently_fetched,
  /// @brief The latest fetch failed less than the back-off ago; serve
  /// whatever is held (possibly nothing) without asking PS.
  wait_after_failure,
  /// @brief Ask PS.
  fetch
};

}  // namespace category

/// @brief The held state of one store category for one locale: the last
/// good category, the latest failure, and the age it was asked with.
///
/// Stored as {kind}/{locale} through cache::Cache. A value is immutable; the
/// with members return changed copies. HeldAnswers keeps copies in memory
/// under its own lock.
///
/// @tparam Category cloud_stream::PremiumCategory or
/// cloud_stream::ForSaleCategory;
///     both are instantiated in category_record.cpp.
template <class Category>
class CategoryRecord {
 public:
  /// @brief Records are keyed by the store locale.
  using Key = StoreLocale;
  /// @brief Wall-clock time in milliseconds since the Unix epoch.
  using Time = cache::Time;
  /// @brief The stored form of an entitlement.
  using Entitlement = category::Entitlement;
  /// @brief The stored form of a SKU.
  using Sku = category::Sku;
  /// @brief The stored form of a product.
  using Product = category::Product;
  /// @brief The stored form of a scan.
  using Scan = category::Scan;
  /// @brief The stored body.
  using Body = category::Body;
  /// @brief What the caller wants.
  using Demand = category::Demand;
  /// @brief What to do now.
  using Step = category::Step;

  /// @brief True for the Premium category, false for the for-sale one.
  static constexpr bool includes =
      std::same_as<Category, cloud_stream::PremiumCategory>;
  /// @brief The record kind and first name segment: "included" or
  /// "for_sale".
  static constexpr std::string_view kind =
      includes ? std::string_view{"included"} : std::string_view{"for_sale"};
  /// @brief The schema version: 2 for Premium, 1 for sale.
  static constexpr std::int64_t schema = includes ? 2 : 1;
  /// @brief How long a fetched category is served without refetching.
  static constexpr std::chrono::days lifetime{7};
  /// @brief How recent a fetch makes a start-over serve the held category.
  static constexpr std::chrono::hours refetch_cooldown{1};
  /// @brief How long after a failure PS is not asked again.
  static constexpr std::chrono::minutes failure_back_off{30};

  /// @brief A record holding nothing yet.
  ///
  /// @param locale The locale.
  /// @return No category, no failure, no age.
  static auto nothing_held(StoreLocale locale) -> CategoryRecord;
  /// @brief Reads a stored body back.
  ///
  /// @param body The stored body.
  /// @param locale The locale the record is stored under.
  /// @return The record; std::nullopt when the body holds neither products
  ///     nor a failure, when products are present but the fetch time or a
  ///     valid scan is missing, any product, SKU or entitlement does not
  ///     read, the category is not plausible, or the age is outside 0 to
  ///     oldest_age.
  [[nodiscard]] static auto from_body(const Body& body,
                                      const StoreLocale& locale)
      -> std::optional<CategoryRecord>;
  /// @brief The oldest age a record may carry.
  static constexpr std::int64_t oldest_age = 150;
  /// @brief The record name for a locale.
  ///
  /// @param locale The locale.
  /// @return {kind}/{lower-case locale}, or std::nullopt when that is not a
  ///     valid name.
  [[nodiscard]] static auto name_of(const StoreLocale& locale)
      -> std::optional<platform::RecordName>;

  /// @brief The locale the record is for.
  ///
  /// @return The locale, valid while the record lives.
  auto key() const noexcept -> const StoreLocale& { return locale_; }
  /// @brief The last good category.
  ///
  /// @return The category, or std::nullopt when none has been fetched.
  auto category() const noexcept -> const std::optional<Category>& {
    return category_;
  }
  /// @brief When the latest fetch failed.
  ///
  /// @return The time, or std::nullopt when the latest fetch succeeded or
  ///     none was made.
  auto failed_at() const noexcept -> std::optional<Time> { return failed_at_; }
  /// @brief The age the category was asked with.
  ///
  /// @return The age, or std::nullopt for a record stored without one.
  auto age() const noexcept -> std::optional<int> { return age_; }
  /// @brief A copy marked as asked with an age.
  ///
  /// @param age The real age of the account.
  /// @return The copy.
  auto for_age(int age) const -> CategoryRecord;

  /// @brief Whether the held category is within lifetime.
  ///
  /// @param now The wall-clock time now.
  /// @return True when a category is held and was fetched no more than
  ///     seven days ago.
  auto is_fresh_at(Time now) const noexcept -> bool;
  /// @brief Whether the held category is within refetch_cooldown.
  ///
  /// @param now The wall-clock time now.
  /// @return True when a category is held and was fetched no more than an
  ///     hour ago.
  auto was_fetched_recently_at(Time now) const noexcept -> bool;
  /// @brief How long until PS may be asked again after a failure.
  ///
  /// @param now The wall-clock time now.
  /// @return The time left, or std::nullopt when there is no failure, the
  ///     back-off has passed, or the failure is in the future.
  auto retry_after(Time now) const noexcept
      -> std::optional<std::chrono::milliseconds>;
  /// @brief Decides what to do with this record now.
  ///
  /// @param now The wall-clock time now.
  /// @param demand What the caller wants.
  /// @return Step::serve_recently_fetched when starting over within the
  ///     cooldown, Step::serve when fresh and not starting over,
  ///     Step::wait_after_failure inside the back-off, Step::fetch
  ///     otherwise.
  auto next_step(Time now, Demand demand) const noexcept -> Step;
  /// @brief Whether a fetched category may replace the held one.
  ///
  /// @param answer The fetched category.
  /// @return True when it is plausible and for this record's locale.
  auto accepts(const Category& answer) const -> bool;

  /// @brief The record after a fetch.
  ///
  /// @param answer The fetched category.
  /// @param now When it was fetched.
  /// @return The answer with the failure cleared when accepted; otherwise
  ///     the same as with_failure.
  auto with_answer(const Category& answer, Time now) const -> CategoryRecord;
  /// @brief The record after a failed fetch.
  ///
  /// @param now When it failed.
  /// @return The held category kept, with the failure time set.
  auto with_failure(Time now) const -> CategoryRecord;

  /// @brief The record's stored body.
  ///
  /// @return The body.
  auto body() const -> Body;

  /// @brief Compares locale, category, failure and age.
  friend auto operator==(const CategoryRecord&, const CategoryRecord&)
      -> bool = default;

 private:
  /// @brief Reads a body back, ignoring its age.
  ///
  /// @param body The stored body.
  /// @param locale The locale.
  /// @return The record, or std::nullopt as from_body describes.
  [[nodiscard]] static auto unaged_from(const Body& body,
                                        const StoreLocale& locale)
      -> std::optional<CategoryRecord>;
  /// @brief The stored form of a product.
  ///
  /// @param kept The product.
  /// @return Its stored form.
  static auto product(const cloud_stream::PremiumProduct& kept) -> Product;
  /// @brief The stored form of a SKU.
  ///
  /// @param kept The SKU.
  /// @return Its stored form.
  static auto sku(const cloud_stream::PremiumSku& kept) -> Sku;
  /// @brief The stored form of an entitlement.
  ///
  /// @param kept The entitlement.
  /// @return Its stored form.
  static auto entitlement(const cloud_stream::PremiumEntitlement& kept)
      -> Entitlement;
  /// @brief A stored product read back.
  ///
  /// @param wire The stored product.
  /// @return The product, or std::nullopt when its id, concept or a SKU
  ///     does not read.
  [[nodiscard]] static auto product(const Product& wire)
      -> std::optional<cloud_stream::PremiumProduct>;
  /// @brief A stored SKU read back.
  ///
  /// @param wire The stored SKU.
  /// @return The SKU, or std::nullopt when its id or an entitlement does
  ///     not read.
  [[nodiscard]] static auto sku(const Sku& wire)
      -> std::optional<cloud_stream::PremiumSku>;
  /// @brief A stored entitlement read back.
  ///
  /// @param wire The stored entitlement.
  /// @return The entitlement, or std::nullopt when a stored id does not
  ///     parse.
  [[nodiscard]] static auto entitlement(const Entitlement& wire)
      -> std::optional<cloud_stream::PremiumEntitlement>;
  /// @brief A stored scan read back.
  ///
  /// @param wire The stored scan.
  /// @return The scan, or std::nullopt when a count is negative.
  [[nodiscard]] static auto scan(const Scan& wire)
      -> std::optional<cloud_stream::CategoryScan>;

  /// @brief Keeps the parts of a record; the age starts unset.
  ///
  /// @param locale The locale.
  /// @param category The last good category, if any.
  /// @param failed_at The latest failure, if any.
  CategoryRecord(StoreLocale locale, std::optional<Category> category,
                 std::optional<Time> failed_at) noexcept;

  /// @brief The locale the record is for.
  StoreLocale locale_;
  /// @brief The last good category.
  std::optional<Category> category_;
  /// @brief When the latest fetch failed.
  std::optional<Time> failed_at_;
  /// @brief The age the category was asked with.
  std::optional<int> age_{};
};

/// @brief The held Premium streaming category.
using PremiumCategoryRecord = CategoryRecord<cloud_stream::PremiumCategory>;
/// @brief The held public Store streaming category.
using ForSaleCategoryRecord = CategoryRecord<cloud_stream::ForSaleCategory>;

extern template class CategoryRecord<cloud_stream::PremiumCategory>;
extern template class CategoryRecord<cloud_stream::ForSaleCategory>;

}  // namespace libchiaking::catalog::cache

/// @brief How a category entitlement feature is spelled in JSON.
template <>
struct glz::meta<
    libchiaking::catalog::cloud_stream::PremiumEntitlement::Feature> {
  using enum libchiaking::catalog::cloud_stream::PremiumEntitlement::Feature;
  /// @brief The stored names, which match the enumerator names.
  static constexpr auto value = glz::enumerate(
      "full_game", full_game, "trial", trial, "play_first_trial",
      play_first_trial, "disc_required", disc_required, "unknown", unknown);
};

/// @brief How a stored entitlement is spelled in JSON.
template <>
struct glz::meta<libchiaking::catalog::cache::category::Entitlement> {
  /// @brief The type described.
  using T = libchiaking::catalog::cache::category::Entitlement;
  /// @brief The keys id, unreadableId, packageType, featureType and
  /// streamingSupported.
  static constexpr auto value =
      glz::object("id", &T::id, "unreadableId", &T::unreadable_id,
                  "packageType", &T::package_type, "featureType", &T::feature,
                  "streamingSupported", &T::streaming_supported);
};

/// @brief How a stored SKU is spelled in JSON.
template <>
struct glz::meta<libchiaking::catalog::cache::category::Sku> {
  /// @brief The type described.
  using T = libchiaking::catalog::cache::category::Sku;
  /// @brief The keys id, branded and entitlements.
  static constexpr auto value = glz::object(
      "id", &T::id, "branded", &T::branded, "entitlements", &T::entitlements);
};

/// @brief How a stored product is spelled in JSON.
template <>
struct glz::meta<libchiaking::catalog::cache::category::Product> {
  /// @brief The type described.
  using T = libchiaking::catalog::cache::category::Product;
  /// @brief The keys id, name, masterImage, concept and skus.
  static constexpr auto value = glz::object(
      "id", &T::id, "name", &T::name, "masterImage", &T::master_image,
      "concept", &T::concept_id, "skus", &T::skus);
};

/// @brief How a stored scan is spelled in JSON.
template <>
struct glz::meta<libchiaking::catalog::cache::category::Scan> {
  /// @brief The type described.
  using T = libchiaking::catalog::cache::category::Scan;
  /// @brief The keys serverTotal, productsRead and complete.
  static constexpr auto value =
      glz::object("serverTotal", &T::server_total, "productsRead",
                  &T::products_read, "complete", &T::complete);
};

/// @brief How a category record body is spelled in JSON.
template <>
struct glz::meta<libchiaking::catalog::cache::category::Body> {
  /// @brief The type described.
  using T = libchiaking::catalog::cache::category::Body;
  /// @brief The keys fetchedAtMs, scan, products, failedAtMs and age.
  static constexpr auto value = glz::object(
      "fetchedAtMs", &T::fetched_at_ms, "scan", &T::scan, "products",
      &T::products, "failedAtMs", &T::failed_at_ms, "age", &T::age);
};
