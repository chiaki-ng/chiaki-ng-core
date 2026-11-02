#include "src/core/catalog/model/cloud_stream/cloud_game_listing.hpp"

#include <algorithm>
#include <utility>

namespace libchiaking::catalog::cloud_stream {

CloudGameListing::CloudGameListing(Answer answer) noexcept
    : answer_(std::move(answer)) {}

auto CloudGameListing::listed(Product product, std::uint16_t http_status)
    -> CloudGameListing {
  return CloudGameListing{Listed{std::move(product), http_status}};
}

auto CloudGameListing::not_listed(std::uint16_t http_status)
    -> CloudGameListing {
  return CloudGameListing{NotListed{http_status}};
}

auto CloudGameListing::failed(Unanswered cause) -> CloudGameListing {
  return CloudGameListing{Failed{cause}};
}

auto CloudGameListing::not_asked(Unasked why) -> CloudGameListing {
  return CloudGameListing{NotAsked{why}};
}

auto CloudGameListing::is_listed() const noexcept -> bool {
  return std::holds_alternative<Listed>(answer_);
}

auto CloudGameListing::is_not_listed() const noexcept -> bool {
  return std::holds_alternative<NotListed>(answer_);
}

auto CloudGameListing::product() const -> std::optional<Product> {
  if (const auto* listed = std::get_if<Listed>(&answer_))
    return listed->product;
  return std::nullopt;
}

auto CloudGameListing::cause() const -> std::optional<Unanswered> {
  if (const auto* failed = std::get_if<Failed>(&answer_)) return failed->cause;
  return std::nullopt;
}

auto CloudGameListing::unasked() const noexcept -> std::optional<Unasked> {
  if (const auto* skipped = std::get_if<NotAsked>(&answer_))
    return skipped->why;
  return std::nullopt;
}

auto CloudGameListing::http_status() const noexcept
    -> std::optional<std::uint16_t> {
  if (const auto* listed = std::get_if<Listed>(&answer_))
    return listed->http_status;
  if (const auto* absent = std::get_if<NotListed>(&answer_))
    return absent->http_status;
  if (const auto* failed = std::get_if<Failed>(&answer_))
    return failed->cause.http_status();
  return std::nullopt;
}

auto CloudGameListing::unanswered() const -> std::optional<Unanswered> {
  if (const auto* failed = std::get_if<Failed>(&answer_)) return failed->cause;
  if (std::holds_alternative<NotAsked>(answer_))
    return Unanswered{Unanswered::Reason::unavailable};
  return std::nullopt;
}

}  // namespace libchiaking::catalog::cloud_stream
