#include "src/core/play/usecases/nat_type_check.hpp"

#include "libchiaking/platform/diagnostics/provider.hpp"
#include "src/core/play/adapters/nat/stun_probe.hpp"

#include <string>

namespace libchiaking::play::usecases {

NatTypeCheck::NatTypeCheck(StunProbe& probe,
                           platform::DiagnosticsProvider* log) noexcept
    : probe_(probe), log_(log) {}

auto NatTypeCheck::check(const platform::StopToken& stop)
    -> Expected<NatType, NatTypeFailure> {
  auto mappings = probe_.probe(stop);
  if (!mappings) {
    if (log_ != nullptr &&
        mappings.error().reason == NatTypeFailure::Reason::unanswered)
      log_->write({platform::LogSeverity::warning,
                   "NAT type: the party voice STUN server did not answer"});
    return unexpected(mappings.error());
  }
  auto type = mappings.value().type();
  if (log_ != nullptr) {
    const std::string line =
        type
            ? "NAT type: type " + std::to_string(static_cast<int>(type.value()))
            : std::string{"NAT type: the second server did not answer"};
    log_->write({platform::LogSeverity::info, line});
  }
  return type;
}

}  // namespace libchiaking::play::usecases
