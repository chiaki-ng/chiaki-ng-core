#include "libchiaking/detail/catalog/storefront_answer.hpp"

#include <algorithm>
#include <utility>

namespace libchiaking::catalog {

StorefrontAnswer::StorefrontAnswer(AccountId account, ProductId asked,
                                   Time checked_at) noexcept
    : account_(account), asked_(std::move(asked)), checked_at_(checked_at) {}

auto StorefrontAnswer::of(AccountId account, ProductId asked,
                          std::span<const Product> answered, Time checked_at)
    -> StorefrontAnswer {
  StorefrontAnswer answer{account, std::move(asked), checked_at};
  const auto edition =
      std::ranges::find_if(answered, [&answer](const Product& product) {
        return answer.asked_.is_answered_by(product.id);
      });
  if (edition == answered.end()) return answer;
  answer.answered_ = edition->id;
  answer.owned_ = edition->owned;
  answer.library_offer_ = edition->library_offer;
  answer.offers_trial_ = edition->offers_trial;
  if (!edition->streaming_supported) {
    answer.streaming_ = Streaming::no_notice;
  } else if (*edition->streaming_supported) {
    answer.streaming_ = Streaming::streams;
  } else {
    answer.streaming_ = Streaming::not_streamable;
    answer.notice_ = edition->notice;
  }
  return answer;
}

auto StorefrontAnswer::offers_library_add() const noexcept -> bool {
  return library_offer_ && !owned_ &&
         library_offer_->entitled_through_subscription == false;
}

auto StorefrontAnswer::is_fresh_at(Time now) const noexcept -> bool {
  return is_fresh(checked_at_, lifetime, now);
}

}  // namespace libchiaking::catalog
