/// @file
/// @brief Declares AbsentHttpProvider, the stand-in a factory returns when the
/// host supplies no HTTP port.
#pragma once

#include <libchiaking/platform/http/provider.hpp>

namespace libchiaking::platform {

/// @brief HttpProvider that performs no request.
///
/// A factory returns it from PlatformFactory::http when the host supplies no
/// provider, and leaves PlatformCapability::http unset. It has no state and
/// may be called from any thread at once.
class AbsentHttpProvider final : public HttpProvider {
 public:
  /// @brief Creates the stand-in.
  AbsentHttpProvider() = default;

  /// @brief Refuses the request without touching the network.
  ///
  /// @param request Ignored.
  /// @param stop_token Ignored.
  /// @return Always HttpError::unsupported.
  [[nodiscard]] auto perform(const HttpRequest& request,
                             const StopToken& stop_token)
      -> Expected<HttpResponse, HttpError> override {
    (void)request;
    (void)stop_token;
    return unexpected(HttpError::unsupported);
  }
};

}  // namespace libchiaking::platform
