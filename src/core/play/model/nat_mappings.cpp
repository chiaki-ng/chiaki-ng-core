#include "src/core/play/model/nat_mappings.hpp"

#include <algorithm>

namespace libchiaking::play {

auto NatMappings::type() const -> Expected<NatType, NatTypeFailure> {
  if (std::ranges::contains(local, first.address)) return NatType::type1;
  if (!second)
    return unexpected(NatTypeFailure{NatTypeFailure::Reason::incomplete});
  return *second == first ? NatType::type2 : NatType::type3;
}

}  // namespace libchiaking::play
