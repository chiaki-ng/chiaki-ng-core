#include "src/core/catalog/usecases/show_images.hpp"

#include "src/core/catalog/adapters/ps/store_details.hpp"
#include "src/core/net/transport/http/client.hpp"

#include <memory>
#include <utility>

namespace libchiaking::catalog::usecases {

ShowImages::ShowImages(platform::HttpProvider& http,
                       platform::ThreadingProvider& threading,
                       const platform::ClockProvider& clock,
                       platform::DiagnosticsProvider* log, HeldAnswers& held)
    : http_(http),
      clock_(clock),
      log_(log),
      held_(held),
      downloads_(threading, clock, downloads_at_once, downloads_in_queue) {}

auto ShowImages::image(const ImageRef& image, const Stop& stop)
    -> Expected<std::vector<std::byte>, Unanswered> {
  return HeldAnswers::answer(
             held_.in_flight().images, HeldAnswers::Question::showing(image),
             stop, [&] { return held_.images().find(image); },
             [&]() -> Expected<cache::ImageShelf::Bytes, Unanswered> {
               auto bytes = download(image, stop);
               if (!bytes) return unexpected(bytes.error());
               auto kept = held_.images().keep(image, bytes.value());
               if (kept || kept.error().reason() != Unanswered::Reason::storage)
                 return kept;
               return std::make_shared<const std::vector<std::byte>>(
                   std::move(bytes.value()));
             })
      .transform([](const cache::ImageShelf::Bytes& held) {
        return std::vector<std::byte>{*held};
      });
}

auto ShowImages::download(const ImageRef& image, const Stop& stop)
    -> Expected<std::vector<std::byte>, Unanswered> {
  const auto pass = downloads_.enter(stop);
  if (!pass)
    return unexpected(pass.error() == concurrency::Turnstile::Refusal::stopped
                          ? Unanswered::cancelled()
                          : Unanswered{Unanswered::Reason::unavailable});
  net::http::Client client{http_, clock_, stop, log_};
  return ps::StoreImages{client}.bytes_of(image);
}

}  // namespace libchiaking::catalog::usecases
