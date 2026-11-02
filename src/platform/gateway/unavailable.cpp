#include <libchiaking/platform/gateway/provider.hpp>
namespace libchiaking::platform {
auto make_gateway_provider(ThreadingProvider&, const ClockProvider&)
    -> std::unique_ptr<GatewayProvider> {
  return {};
}
}  // namespace libchiaking::platform
