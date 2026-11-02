/// @file
/// @brief Declares ps::StreamingCategory, the adapter that scans the mobile
/// catalog's Premium streaming category and the public Store streaming
/// category.
///
/// Both are asked anonymously with the account's country, language and real
/// age, and paged by the raw server offset, never by the count of products
/// kept.
#pragma once

#include "src/core/catalog/adapters/ps/store_listing.hpp"
#include "src/core/catalog/adapters/ps/wall_stamp.hpp"
#include "src/core/catalog/model/cloud_stream/for_sale_category.hpp"
#include "src/core/catalog/model/cloud_stream/premium_category.hpp"
#include "src/core/catalog/model/page_count.hpp"
#include "src/core/catalog/model/retry_policy.hpp"
#include "src/core/catalog/model/unanswered.hpp"
#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/rest/store/store_client.hpp"

#include <libchiaking/detail/catalog/platform.hpp>
#include <libchiaking/detail/expected.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::catalog::ps {

/// @brief Scans one of the two PS5 streaming categories.
///
/// Borrows the client and clock, which must outlive it.
class StreamingCategory {
 public:
  /// @brief The Premium streaming category id.
  static constexpr std::string_view premium_category =
      "a23e4f41-6e78-4dab-95e2-af1c10e4ab96";
  /// @brief The public Store streaming category id.
  static constexpr std::string_view for_sale_category =
      "83999501-dbe5-413a-b265-6484e0a7626e";
  /// @brief Products asked per Premium page.
  static constexpr std::size_t premium_page_size = 50;
  /// @brief Products asked per Store page.
  static constexpr std::size_t for_sale_page_size = 100;
  /// @brief The most pages read in one scan.
  static constexpr std::size_t page_limit = 80;
  /// @brief The image type taken as a product's artwork.
  static constexpr std::string_view master_image = "MASTER";

  /// @brief Sets up the scans.
  ///
  /// @param http Sends the requests. Must outlive this object.
  /// @param wall Stamps the categories. Must outlive this object.
  /// @param maximum_response_bytes The largest reply accepted per page.
  StreamingCategory(net::http::Client& http,
                    const platform::ClockProvider& wall,
                    std::size_t maximum_response_bytes =
                        net::http::default_maximum_response_bytes) noexcept;

  /// @brief Scans the Premium streaming category.
  ///
  /// Blocks for every page in turn, up to page_limit; logs the counts.
  ///
  /// @param locale The account's country and language.
  /// @param age The account's real age.
  /// @param pages Told each page read and the page total.
  /// @return The category, complete or partial; the first page's failure.
  [[nodiscard]] auto included(const StoreLocale& locale, int age,
                              PageCount pages = {})
      -> Expected<cloud_stream::PremiumCategory, Unanswered>;
  /// @brief Scans the public Store streaming category.
  ///
  /// Blocks for every page in turn, up to page_limit; logs the counts.
  ///
  /// @param locale The account's country and language.
  /// @param age The account's real age.
  /// @param pages Told each page read and the page total.
  /// @return The category, complete or partial; the first page's failure.
  [[nodiscard]] auto for_sale(const StoreLocale& locale, int age,
                              PageCount pages = {})
      -> Expected<cloud_stream::ForSaleCategory, Unanswered>;
  /// @brief Maps a store failure to a catalog failure.
  ///
  /// @param error The failure.
  /// @return A cancellation; Unanswered::Reason::unavailable for a transport
  ///     failure or 5xx; Unanswered::Reason::malformed for an unreadable reply;
  ///     Unanswered::Reason::rejected for any other status.
  static auto unanswered(const psn::rest::StoreError& error) noexcept
      -> Unanswered;

 private:
  /// @brief A tolerant text field.
  using Text = json::ps::Text;

  /// @brief Which category to scan and how.
  struct Shelf {
    /// @brief The category id.
    std::string_view category;
    /// @brief Products asked per page.
    std::size_t page_size{};
    /// @brief The name used in log lines.
    std::string_view name;
  };

  /// @brief A scan's products and how it went.
  struct Scanned {
    /// @brief The products kept, in server order.
    std::vector<cloud_stream::PremiumProduct> products;
    /// @brief How the scan went.
    cloud_stream::CategoryScan scan;
  };

  /// @brief What one page carried.
  struct Page {
    /// @brief The stated total product count.
    std::size_t server_total{};
    /// @brief How many products the page carried, kept or not.
    std::size_t products{};
    /// @brief The first product's id, to detect a server repeating a page.
    std::string first_product;
    /// @brief The products that read.
    std::vector<cloud_stream::PremiumProduct> products_kept;
  };

  /// @brief One image of a category product. Plain data.
  struct Image {
    /// @brief The image type, such as MASTER.
    Text type;
    /// @brief The image URL.
    Text url;
  };
  /// @brief A category product's media. Plain data.
  struct Media {
    /// @brief The images.
    StoreList<Image> images;
  };
  /// @brief A category product's concept. Plain data.
  struct Grouped {
    /// @brief The concept id, as a string or a number.
    std::optional<glz::raw_json> id;
  };
  /// @brief One category product as PS sends it. Plain data.
  struct Product {
    /// @brief The product id.
    Text id;
    /// @brief The display name.
    Text name;
    /// @brief The media.
    json::ps::Tolerant<Media> media;
    /// @brief The concept.
    json::ps::Tolerant<Grouped> grouped;
    /// @brief The SKUs.
    StoreList<StoreSku> skus;

    /// @brief How a product is read; the concept comes under "concept".
    struct glaze {
      /// @brief The type described.
      using T = Product;
      /// @brief The keys read.
      static constexpr auto value =
          glz::object("id", &T::id, "name", &T::name, "media", &T::media,
                      "concept", &T::grouped, "skus", &T::skus);
    };
  };
  /// @brief One category page as PS sends it. Plain data.
  struct Reply {
    /// @brief Fields are read under their camelCase names.
    using keys = json::ps::CamelCaseKeys;
    /// @brief The page's products.
    json::ps::Tolerant<std::vector<json::ps::Tolerant<Product>>> products;
    /// @brief The stated total product count.
    std::optional<glz::raw_json> total_num_products;
  };

  /// @brief A page started but not yet read.
  struct Asked {
    /// @brief The raw server offset it was asked at.
    std::size_t offset{};
    /// @brief The started request.
    psn::rest::StoreClient::Pending<Reply> reply;
  };

  /// @brief How the Premium category is scanned.
  static constexpr Shelf premium_shelf{premium_category, premium_page_size,
                                       "premium streaming category"};
  /// @brief How the Store category is scanned.
  static constexpr Shelf store_shelf{for_sale_category, for_sale_page_size,
                                     "store streaming category"};

  /// @brief Scans a category page by page from offset 0.
  ///
  /// The first page is asked alone. Its total decides the offsets of the
  /// remaining pages, which are all started at once, up to page_limit, and
  /// read in order. A page shorter than the shelf's page size drops the pages
  /// started after it, which are asked again from the raw count of products
  /// read so far. The scan is complete on an empty page or once the server
  /// total is reached; it stops as partial when a later page fails, a page
  /// repeats the previous first product, or page_limit is reached.
  ///
  /// @param shelf The category.
  /// @param locale The account's country and language.
  /// @param age The account's real age.
  /// @param count Told each page read and, after the first, the page total.
  /// @return The products and scan; the first page's failure, or a cancellation
  ///     on any page.
  [[nodiscard]] auto scan(const Shelf& shelf, const StoreLocale& locale,
                          int age, PageCount& count)
      -> Expected<Scanned, Unanswered>;
  /// @brief Asks one page, as often as RetryPolicy::category_page allows by
  /// default.
  ///
  /// @param shelf The category.
  /// @param offset The raw server offset.
  /// @param locale The account's country and language.
  /// @param age The account's real age.
  /// @return The page; a cancellation; the last failure otherwise.
  /// @param attempts How many times to ask.
  /// @param last The failure of an earlier attempt, if any, returned when
  ///     no attempt is left.
  [[nodiscard]] auto page_at(
      const Shelf& shelf, std::size_t offset, const StoreLocale& locale,
      int age, int attempts = RetryPolicy::category_page().attempts(),
      std::optional<Unanswered> last = std::nullopt)
      -> Expected<Page, Unanswered>;
  /// @brief Starts the pages from the products read so far up to the server
  /// total, without waiting for them.
  ///
  /// Starts one page when the total is already reached, and never more than
  /// page_limit pages in all.
  ///
  /// @param store The store the pages are asked from.
  /// @param shelf The category.
  /// @param read The scan so far.
  /// @param pages The pages read so far.
  /// @param locale The account's country and language.
  /// @param age The account's real age.
  /// @return The started pages, in offset order.
  [[nodiscard]] static auto ask(psn::rest::StoreClient& store,
                                const Shelf& shelf,
                                const cloud_stream::CategoryScan& read,
                                std::size_t pages, const StoreLocale& locale,
                                int age) -> std::vector<Asked>;
  /// @brief Reads a started page, asking it again when it fails as often as
  /// RetryPolicy::category_page allows in all.
  ///
  /// @param shelf The category.
  /// @param asked The started page.
  /// @param locale The account's country and language.
  /// @param age The account's real age.
  /// @return The page; a cancellation; the last failure otherwise.
  [[nodiscard]] auto collect(const Shelf& shelf, Asked asked,
                             const StoreLocale& locale, int age)
      -> Expected<Page, Unanswered>;

  /// @brief Reads one page.
  ///
  /// @param reply The page.
  /// @return The page; Unanswered::Reason::malformed when products or a
  ///     non-negative total are missing.
  static auto read(const Reply& reply) -> Expected<Page, Unanswered>;
  /// @brief Reads one product.
  ///
  /// @param listed The product.
  /// @return The product, or std::nullopt when its id does not parse or it has
  ///     no SKU list.
  static auto product_of(const Product& listed)
      -> std::optional<cloud_stream::PremiumProduct>;
  /// @brief Reads one SKU and whether it is branded PS_PLUS_STREAM_PS5.
  ///
  /// @param sold The SKU.
  /// @return The SKU; null entitlements become empty ones.
  static auto sku_of(const StoreSku& sold) -> cloud_stream::PremiumSku;
  /// @brief Reads one entitlement.
  ///
  /// @param granted The entitlement.
  /// @return The entitlement; an id that does not parse is marked unreadable,
  ///     and streamingSupported is kept only when exactly true or false.
  static auto entitlement_of(const StoreEntitlement& granted)
      -> cloud_stream::PremiumEntitlement;
  /// @brief The product's MASTER image URL.
  ///
  /// @param listed The product.
  /// @return The URL, or empty when there is none.
  static auto master_image_of(const Product& listed) -> std::string;
  /// @brief How many products offer each kind of trial version, for the
  /// scan's log line.
  ///
  /// @param products The products kept.
  /// @return Text naming how many offer a trial and how many a Play First
  ///     Trial, ignoring what the user holds.
  static auto trials_of(
      const std::vector<cloud_stream::PremiumProduct>& products) -> std::string;

  /// @brief Sends requests; not owned.
  net::http::Client& http_;
  /// @brief Stamps the categories.
  WallStamp stamp_;
  /// @brief The largest reply accepted, in bytes.
  std::size_t maximum_response_bytes_;
};

}  // namespace libchiaking::catalog::ps
