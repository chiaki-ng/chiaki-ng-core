/// @file
/// @brief Declares ps::StreamingCopies, the check of which PS4 products the
/// store offers a streaming copy for that a plan covers.
///
/// LegacyCloudStream lists PS4 titles whose streaming copy may no longer be on
/// any plan. The check asks the store's products endpoint in batches and
/// reports the products that have no SKU with a streaming package (ending "GS")
/// in a PS_PLUS_STREAM or LEGACY_CLOUD_STREAM collection.
#pragma once

#include "src/core/catalog/adapters/ps/store_listing.hpp"
#include "src/core/catalog/model/page_count.hpp"
#include "src/core/net/transport/http/client.hpp"

#include <libchiaking/detail/catalog/platform.hpp>
#include <libchiaking/detail/catalog/product_id.hpp>
#include <libchiaking/detail/catalog/store_locale.hpp>

#include <cstddef>
#include <set>
#include <span>
#include <string_view>

namespace libchiaking::catalog::ps {

/// @brief Asks the store about the streaming copies of PS4 products.
///
/// Borrows the client, which must outlive it.
class StreamingCopies {
 public:
  /// @brief Products asked per request.
  static constexpr std::size_t batch_size = 50;
  /// @brief The collection branding of PS Plus streaming.
  static constexpr std::string_view stream_branding = "PS_PLUS_STREAM";
  /// @brief The collection branding of the old LegacyCloudStream service.
  static constexpr std::string_view old_stream_branding = "PS_NOW";

  /// @brief What the store said about the products asked.
  struct Answer {
    /// @brief Products with a streaming copy a plan covers.
    std::set<ProductId> covered;
    /// @brief Products the store listed without one.
    std::set<ProductId> without_a_covered_copy;
    /// @brief How many products were asked about.
    std::size_t asked{};

    /// @brief How many products the store answered for.
    ///
    /// @return The size of both sets together; at most asked.
    auto answered() const noexcept -> std::size_t {
      return covered.size() + without_a_covered_copy.size();
    }
  };

  /// @brief Sets up the check.
  ///
  /// @param http Sends the requests. Must outlive this object.
  /// @param maximum_response_bytes The largest reply accepted per batch.
  explicit StreamingCopies(
      net::http::Client& http,
      std::size_t maximum_response_bytes =
          net::http::default_maximum_response_bytes) noexcept;

  /// @brief Checks every product, batch by batch.
  ///
  /// A failed batch is skipped, and the check stops early when the client is
  /// stopped, so a product may be in neither set. Blocks for one request per
  /// batch_size products.
  ///
  /// @param products The PS4 products.
  /// @param locale The store locale.
  /// @param age The account's real age.
  /// @param batches Told the batch total up front and each batch asked.
  /// @return What the store said; never fails.
  auto of(std::span<const ProductId> products, const StoreLocale& locale,
          int age, PageCount batches = {}) -> Answer;

 private:
  /// @brief A products reply.
  using Reply = std::vector<json::ps::Tolerant<StoreProduct>>;

  /// @brief Sorts one batch's products into the answer and logs those dropped.
  ///
  /// @param reply The batch's reply.
  /// @param answer The answer; extended in place.
  void read(const Reply& reply, Answer& answer) const;
  /// @brief Whether a product has a covered streaming copy.
  ///
  /// @param product The product; its SKU list must be present.
  /// @return True when one SKU grants a package ending "GS" and belongs to a
  ///     PS_PLUS_STREAM or LEGACY_CLOUD_STREAM collection.
  static auto has_a_covered_streaming_copy(const StoreProduct& product) -> bool;

  /// @brief Sends requests; not owned.
  net::http::Client& http_;
  /// @brief The largest reply accepted, in bytes.
  std::size_t maximum_response_bytes_;
};

}  // namespace libchiaking::catalog::ps
