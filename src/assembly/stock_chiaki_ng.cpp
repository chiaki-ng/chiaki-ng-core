#include "libchiaking/chiaki_ng.hpp"

#if LIBCHIAKING_HAS_DEFAULT_PLATFORM
#include "libchiaking/platform/default.hpp"
#endif

#include <memory>
#include <utility>

namespace libchiaking {

auto ChiakiNg::open(Stores stores, Settings settings)
    -> Expected<ChiakiNg, Missing> {
#if LIBCHIAKING_HAS_DEFAULT_PLATFORM
  auto stock = std::make_shared<platform::DefaultPlatform>();
  auto& factory = stock->factory();
  return open_owning(std::move(stock), factory, stores, std::move(settings));
#else
  static_cast<void>(stores);
  static_cast<void>(settings);
  return unexpected(Missing::http);
#endif
}

}  // namespace libchiaking
