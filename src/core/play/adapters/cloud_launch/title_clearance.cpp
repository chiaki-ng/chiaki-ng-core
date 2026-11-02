#include "src/core/play/adapters/cloud_launch/title_clearance.hpp"

#include "libchiaking/detail/catalog/catalog.hpp"

namespace libchiaking::play {

auto CatalogClearance::held(users::PsUser& user,
                            const catalog::ProductId& product) const
    -> catalog::LaunchVerdict {
  return catalog_.can_launch(user, product);
}

auto CatalogClearance::clear(users::PsUser& user,
                             const catalog::ProductId& product,
                             const platform::StopToken& stop,
                             const catalog::LaunchCheckObserver& observer)
    -> catalog::LaunchVerdict {
  return catalog_.clear_for_launch(user, product, stop, observer);
}

void CatalogClearance::remember_refusal(users::PsUser& user,
                                        const catalog::ProductId& product,
                                        catalog::StreamRefusal::Kind kind) {
  catalog_.remember_refusal(user, product, kind);
}

}  // namespace libchiaking::play
