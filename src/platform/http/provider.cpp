#include <libchiaking/platform/http/provider.hpp>

#include <memory>
#include <utility>

namespace libchiaking::platform {

class HttpProvider::Deferred final : public PendingHttpResponse::Source {
 public:
  Deferred(HttpProvider& provider, HttpRequest request,
           StopToken stop_token) noexcept
      : provider_(provider),
        request_(std::move(request)),
        stop_token_(std::move(stop_token)) {}

  auto wait() -> Expected<HttpResponse, HttpError> override {
    return provider_.perform(request_, stop_token_);
  }

 private:
  HttpProvider& provider_;
  HttpRequest request_;
  StopToken stop_token_;
};

auto HttpProvider::start(const HttpRequest& request,
                         const StopToken& stop_token) -> PendingHttpResponse {
  return PendingHttpResponse{
      std::make_unique<Deferred>(*this, request.clone(), stop_token)};
}

}  // namespace libchiaking::platform
