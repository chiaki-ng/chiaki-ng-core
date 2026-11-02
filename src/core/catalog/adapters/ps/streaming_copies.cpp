#include "src/core/catalog/adapters/ps/streaming_copies.hpp"

#include "src/core/psn/rest/store/store_client.hpp"

#include <algorithm>
#include <string>

namespace libchiaking::catalog::ps {

StreamingCopies::StreamingCopies(net::http::Client& http,
                                 std::size_t maximum_response_bytes) noexcept
    : http_(http), maximum_response_bytes_(maximum_response_bytes) {}

auto StreamingCopies::of(std::span<const ProductId> products,
                         const StoreLocale& locale, int age, PageCount batches)
    -> Answer {
  Answer answer;
  answer.asked = products.size();
  psn::rest::StoreClient store{http_.limited(maximum_response_bytes_)};
  batches.expect(static_cast<std::uint32_t>((products.size() + batch_size - 1) /
                                            batch_size));
  for (std::size_t first = 0; first < products.size(); first += batch_size) {
    if (http_.stop_requested()) break;
    std::string ids;
    for (const auto& product :
         products.subspan(first, std::min(batch_size, products.size() - first)))
      ids += (ids.empty() ? "" : ",") + product.text();
    const auto reply = store.products<Reply>({.product_ids = ids,
                                              .country = locale.country(),
                                              .language = locale.language(),
                                              .age = age});
    if (reply) read(reply.value(), answer);
    batches.read();
  }
  http_.log(platform::LogSeverity::info,
            "[LEGACY_CLOUD_STREAM] streaming copies: " +
                std::to_string(answer.answered()) + " of " +
                std::to_string(answer.asked) + " titles answered, " +
                std::to_string(answer.without_a_covered_copy.size()) +
                " have no copy a plan covers");
  return answer;
}

void StreamingCopies::read(const Reply& reply, Answer& answer) const {
  for (const auto& product : reply) {
    if (!product) continue;
    const auto id = ProductId::parse(product->id.view());
    if (!id || !product->skus) continue;
    if (has_a_covered_streaming_copy(*product)) {
      answer.covered.insert(*id);
      continue;
    }
    answer.without_a_covered_copy.insert(*id);
    http_.log(platform::LogSeverity::info,
              "[LEGACY_CLOUD_STREAM] dropped " + id->text() +
                  ": the store offers no streaming copy that a plan covers");
  }
}

auto StreamingCopies::has_a_covered_streaming_copy(const StoreProduct& product)
    -> bool {
  return std::ranges::any_of(*product.skus, [](const auto& sku) {
    if (!sku || !sku->entitlements || !sku->content_collections) return false;
    const bool streams =
        std::ranges::any_of(*sku->entitlements, [](const auto& granted) {
          return granted && granted->package_type.view().ends_with("GS");
        });
    return streams &&
           std::ranges::any_of(
               *sku->content_collections, [](const auto& collection) {
                 const auto branding = collection ? collection->branding.view()
                                                  : std::string_view{};
                 return branding == stream_branding ||
                        branding == old_stream_branding;
               });
  });
}

}  // namespace libchiaking::catalog::ps
