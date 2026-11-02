#include "src/core/catalog/model/my_games/remembered_refusals.hpp"

namespace libchiaking::catalog {

RememberedRefusals::RememberedRefusals(AccountId account,
                                       std::span<const StreamRefusal> refusals,
                                       std::optional<users::Tier> tier,
                                       Time now) noexcept
    : account_(account), refusals_(refusals), tier_(tier), now_(now) {}

auto RememberedRefusals::standing_against(
    const Title& title, const StreamRefusal::Holding& holding) const
    -> std::optional<StreamRefusal> {
  std::optional<StreamRefusal> latest;
  for (const auto& refusal : refusals_) {
    if (refusal.account() != account_ ||
        !title.is_known_as(refusal.product_id()))
      continue;
    const StreamRefusal::Circumstances current{
        tier_ ? tier_ : refusal.circumstances().tier, holding};
    if (!refusal.still_stands(now_, current)) continue;
    if (!latest || latest->refused_at() < refusal.refused_at())
      latest = refusal;
  }
  return latest;
}

}  // namespace libchiaking::catalog
