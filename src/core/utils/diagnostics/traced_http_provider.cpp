#include "src/core/utils/diagnostics/traced_http_provider.hpp"

#include "src/core/net/transport/http/status.hpp"
#include "src/core/utils/diagnostics/api_name.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string_view>
#include <utility>

namespace libchiaking::diagnostics {

void TracedHttpProvider::write(platform::DiagnosticsProvider& log,
                               platform::LogSeverity severity,
                               const Message& message, int size) noexcept {
  if (size > 0)
    log.write({severity,
               std::string_view{message.data(),
                                std::min(static_cast<std::size_t>(size),
                                         TracedHttpProvider::message_limit)}});
}

TracedHttpProvider::TracedHttpProvider(platform::HttpProvider& provider,
                                       platform::DiagnosticsProvider& log,
                                       const platform::ClockProvider& clock,
                                       ApiCapture* capture,
                                       std::uint64_t attempt) noexcept
    : provider_(provider),
      log_(log),
      clock_(clock),
      capture_(capture),
      attempt_(attempt) {}

class TracedHttpProvider::Traced final
    : public platform::PendingHttpResponse::Source {
 public:
  Traced(TracedHttpProvider& tracer, Started from,
         platform::PendingHttpResponse inner) noexcept
      : tracer_(tracer), from_(from), inner_(std::move(inner)) {}

  Traced(const Traced&) = delete;
  Traced& operator=(const Traced&) = delete;

  ~Traced() override {
    if (!waited_)
      tracer_.finished(from_, unexpected(platform::HttpError::cancelled));
  }

  auto wait()
      -> Expected<platform::HttpResponse, platform::HttpError> override {
    waited_ = true;
    auto result = std::move(inner_).wait();
    tracer_.finished(from_, result);
    return result;
  }

 private:
  TracedHttpProvider& tracer_;
  Started from_;
  platform::PendingHttpResponse inner_;
  bool waited_ = false;
};

auto TracedHttpProvider::perform(const platform::HttpRequest& request,
                                 const platform::StopToken& stop)
    -> Expected<platform::HttpResponse, platform::HttpError> {
  const auto from = started(request);
  auto result = provider_.perform(request, stop);
  finished(from, result);
  return result;
}

auto TracedHttpProvider::start(const platform::HttpRequest& request,
                               const platform::StopToken& stop)
    -> platform::PendingHttpResponse {
  const auto from = started(request);
  return platform::PendingHttpResponse{
      std::make_unique<Traced>(*this, from, provider_.start(request, stop))};
}

auto TracedHttpProvider::started(const platform::HttpRequest& request) noexcept
    -> Started {
  const Started from{.begin = clock_.now_microseconds(),
                     .method = platform::http_method_name(request.method),
                     .api = ApiName{request}.c_str(),
                     .call = capture_ ? capture_->begin(request, attempt_) : 0};
  Message message;
  const int start_size = std::snprintf(
      message.data(), message.size(),
      "API %s started. http method=%s outcome=begin timeout_ms=%lld", from.api,
      from.method, static_cast<long long>(request.total_timeout.count()));
  write(log_, platform::LogSeverity::info, message, start_size);
  return from;
}

void TracedHttpProvider::finished(
    const Started& from,
    const Expected<platform::HttpResponse, platform::HttpError>&
        result) noexcept {
  const auto duration = std::chrono::microseconds{
      static_cast<std::int64_t>(clock_.now_microseconds() - from.begin)};
  if (capture_) capture_->finish(attempt_, from.call, result, duration);
  const char* outcome =
      result                                             ? "response"
      : result.error() == platform::HttpError::timed_out ? "timed_out"
      : result.error() == platform::HttpError::cancelled ? "cancelled"
                                                         : "failed";
  Message message;
  const int size = std::snprintf(
      message.data(), message.size(),
      "API %s result: %s. http method=%s outcome=%s status=%u transport=%u "
      "received_bytes=%llu duration_us=%lld",
      from.api, outcome, from.method, outcome,
      result ? result.value().status_code : 0,
      result ? 0 : static_cast<unsigned>(result.error()),
      result ? static_cast<unsigned long long>(result.value().body.size()) : 0,
      static_cast<long long>(duration.count()));
  const bool failed =
      !result ||
      net::HttpStatus{result.value().status_code}.is_client_error() ||
      net::HttpStatus{result.value().status_code}.is_server_error();
  write(log_,
        failed ? platform::LogSeverity::warning : platform::LogSeverity::info,
        message, size);
}

}  // namespace libchiaking::diagnostics
