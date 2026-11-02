#include "src/core/utils/diagnostics/recording_platform.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace libchiaking::diagnostics {

RecordingPlatform::Recorder::Recorder(
    platform::HttpProvider* inner, HttpCallLog& log, HttpArea area,
    const platform::ClockProvider& clock,
    const platform::ClockProvider& wall) noexcept
    : inner_(inner), log_(log), area_(area), clock_(clock), wall_(wall) {}

auto RecordingPlatform::Recorder::wall_now() const noexcept
    -> HttpCall::Clock::time_point {
  return HttpCall::Clock::time_point{
      std::chrono::duration_cast<HttpCall::Clock::duration>(
          std::chrono::microseconds{wall_.now_microseconds()})};
}

auto RecordingPlatform::Recorder::since(std::uint64_t begun) const noexcept
    -> std::chrono::microseconds {
  return std::chrono::microseconds{
      static_cast<std::int64_t>(clock_.now_microseconds() - begun)};
}

auto RecordingPlatform::Recorder::perform(const platform::HttpRequest& request,
                                          const platform::StopToken& stop)
    -> Expected<platform::HttpResponse, platform::HttpError> {
  if (inner_ == nullptr) return unexpected(platform::HttpError::unsupported);
  const auto started = wall_now();
  const auto begun = clock_.now_microseconds();
  auto outcome = inner_->perform(request, stop);
  log_.record(area_, request, outcome, started, since(begun));
  return outcome;
}

class RecordingPlatform::Recorder::Recording final
    : public platform::PendingHttpResponse::Source {
 public:
  Recording(Recorder& recorder, const platform::HttpRequest& request,
            std::optional<platform::PendingHttpResponse> inner)
      : recorder_(recorder),
        request_(request.clone()),
        inner_(std::move(inner)),
        started_(recorder.wall_now()),
        begun_(recorder.clock_.now_microseconds()) {}

  Recording(const Recording&) = delete;
  Recording& operator=(const Recording&) = delete;

  ~Recording() override {
    if (!waited_ && inner_) record(unexpected(platform::HttpError::cancelled));
  }

  auto wait()
      -> Expected<platform::HttpResponse, platform::HttpError> override {
    waited_ = true;
    if (!inner_) return unexpected(platform::HttpError::unsupported);
    auto outcome = std::move(*inner_).wait();
    record(outcome);
    return outcome;
  }

 private:
  void record(const Expected<platform::HttpResponse, platform::HttpError>&
                  outcome) noexcept {
    recorder_.log_.record(recorder_.area_, request_, outcome, started_,
                          recorder_.since(begun_));
  }

  Recorder& recorder_;
  platform::HttpRequest request_;
  std::optional<platform::PendingHttpResponse> inner_;
  HttpCall::Clock::time_point started_;
  std::uint64_t begun_;
  bool waited_ = false;
};

auto RecordingPlatform::Recorder::start(const platform::HttpRequest& request,
                                        const platform::StopToken& stop)
    -> platform::PendingHttpResponse {
  std::optional<platform::PendingHttpResponse> inner;
  if (inner_ != nullptr) inner.emplace(inner_->start(request, stop));
  return platform::PendingHttpResponse{
      std::make_unique<Recording>(*this, request, std::move(inner))};
}

RecordingPlatform::RecordingPlatform(platform::PlatformFactory& platform,
                                     HttpCallLog& log, HttpArea area) noexcept
    : platform_(platform),
      recorder_(platform.supplies(platform::PlatformCapability::http)
                    ? &platform.http()
                    : nullptr,
                log, area, platform.clock(), platform.wall_clock()) {}

auto RecordingPlatform::info() const noexcept -> platform::PlatformInfo {
  return platform_.info();
}
auto RecordingPlatform::threading() noexcept -> platform::ThreadingProvider& {
  return platform_.threading();
}
auto RecordingPlatform::clock() noexcept -> platform::ClockProvider& {
  return platform_.clock();
}

auto RecordingPlatform::wall_clock() noexcept -> platform::ClockProvider& {
  return platform_.wall_clock();
}
auto RecordingPlatform::random() noexcept -> platform::SecureRandomProvider& {
  return platform_.random();
}
auto RecordingPlatform::network() noexcept -> platform::NetworkProvider& {
  return platform_.network();
}
auto RecordingPlatform::crypto() noexcept -> platform::CryptoProvider& {
  return platform_.crypto();
}
auto RecordingPlatform::http() noexcept -> platform::HttpProvider& {
  return recorder_;
}
auto RecordingPlatform::websocket() noexcept -> platform::WebSocketProvider& {
  return platform_.websocket();
}
auto RecordingPlatform::gateway() noexcept -> platform::GatewayProvider& {
  return platform_.gateway();
}
auto RecordingPlatform::secure_datagram() noexcept
    -> platform::SecureDatagramProvider& {
  return platform_.secure_datagram();
}
auto RecordingPlatform::diagnostics() noexcept
    -> platform::DiagnosticsProvider& {
  return platform_.diagnostics();
}

}  // namespace libchiaking::diagnostics
