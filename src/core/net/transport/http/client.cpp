#include "src/core/net/transport/http/client.hpp"

#include "src/core/net/transport/http/status.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace libchiaking::net::http {

auto Client::retry_reason(
    const Retry& retry,
    const Expected<platform::HttpResponse, platform::HttpError>& result)
    -> std::optional<std::string> {
  if (!result) {
    if (!retry.on_transport_error) return std::nullopt;
    return "transport " + std::string{error_name(result.error())};
  }
  const HttpStatus status{result.value().status_code};
  if ((retry.on_too_many_requests && status == HttpStatus::too_many_requests) ||
      (retry.on_server_error && status.is_server_error()))
    return "HTTP " + std::to_string(status.code());
  return std::nullopt;
}

Client::Client(platform::HttpProvider& provider,
               const platform::ClockProvider& clock, platform::StopToken stop,
               platform::DiagnosticsProvider* log, Waiter* waiter) noexcept
    : provider_(&provider),
      clock_(&clock),
      stop_(std::move(stop)),
      log_(log),
      waiter_(waiter) {}

auto Client::limited(std::size_t maximum_response_bytes) const noexcept
    -> Client {
  auto capped = *this;
  capped.maximum_response_bytes_ =
      std::min(maximum_response_bytes_, maximum_response_bytes);
  return capped;
}

class Client::Pending::Exchange {
 public:
  Exchange(const Client& client, const Endpoint& endpoint) noexcept
      : client_(client), endpoint_(endpoint) {}

  Exchange(const Client& client, const Endpoint& endpoint,
           platform::HttpRequest sent, std::string_view credential)
      : client_(client), endpoint_(endpoint), sent_(std::move(sent)) {
    log_.emplace(client_.log_, *client_.clock_, endpoint_, sent_, credential);
    sent_.provenance.attempt = 1;
    first_.emplace(client_.provider_->start(sent_, client_.stop_));
  }

  Exchange(const Exchange&) = delete;
  Exchange& operator=(const Exchange&) = delete;

  auto collect() -> Expected<Response, Failure> {
    if (!log_) return unexpected(Failure::cancelled());
    const auto retries = std::clamp(endpoint_.retry.retries, 0, max_retries);
    for (int attempt = 1;; ++attempt) {
      sent_.provenance.attempt = static_cast<std::uint16_t>(attempt);
      auto result = attempt == 1
                        ? std::move(*first_).wait()
                        : client_.provider_->perform(sent_, client_.stop_);
      first_.reset();
      if (!result && (result.error() == platform::HttpError::cancelled ||
                      client_.stop_requested()))
        return finish(unexpected(Failure::cancelled(attempt, result.error())));
      if (result && client_.stop_requested() &&
          !endpoint_.keep_reply_after_stop)
        return finish(unexpected(Failure::cancelled(attempt)));
      const auto reason = retry_reason(endpoint_.retry, result);
      if (!reason || attempt > retries) {
        if (!result)
          return finish(
              unexpected(Failure::transport(result.error(), attempt)));
        return finish(Response{std::move(result.value()), attempt,
                               endpoint_.maximum_response_bytes});
      }
      log_->retry(attempt, *reason, endpoint_.retry.delay);
      if (!client_.wait(endpoint_.retry.delay))
        return finish(unexpected(Failure::cancelled(attempt)));
    }
  }

 private:
  auto finish(Expected<Response, Failure> outcome) noexcept
      -> Expected<Response, Failure> {
    log_->finish(outcome);
    return outcome;
  }

  Client client_;
  Endpoint endpoint_;
  platform::HttpRequest sent_;
  std::optional<RequestLog> log_;
  std::optional<platform::PendingHttpResponse> first_;
};

Client::Pending::Pending(std::unique_ptr<Exchange> exchange) noexcept
    : exchange_(std::move(exchange)) {}

Client::Pending::Pending(Pending&& other) noexcept = default;

auto Client::Pending::operator=(Pending&& other) noexcept -> Pending& = default;

Client::Pending::~Pending() = default;

auto Client::Pending::wait() && -> Expected<Response, Failure> {
  if (exchange_ == nullptr) return unexpected(Failure::cancelled());
  const auto exchange = std::move(exchange_);
  return exchange->collect();
}

auto Client::start(Request request) -> Pending {
  request.endpoint_.maximum_response_bytes = std::min(
      request.endpoint_.maximum_response_bytes, maximum_response_bytes_);
  const auto endpoint = request.endpoint();
  if (stop_.stop_requested()) {
    RequestLog::cancelled_before_start(log_, endpoint.label);
    return Pending{std::make_unique<Pending::Exchange>(*this, endpoint)};
  }
  const auto credential = request.credential_label();
  return Pending{std::make_unique<Pending::Exchange>(
      *this, endpoint, std::move(request).release(), credential)};
}

auto Client::send(Request request) -> Expected<Response, Failure> {
  return start(std::move(request)).wait();
}

auto Client::wait(std::chrono::milliseconds delay) -> bool {
  if (stop_.stop_requested()) return false;
  if (waiter_ == nullptr || delay.count() <= 0) return true;
  return waiter_->wait_milliseconds(static_cast<int>(delay.count()), stop_) &&
         !stop_.stop_requested();
}

void Client::log(platform::LogSeverity severity,
                 std::string_view message) const noexcept {
  if (log_ != nullptr) log_->write({severity, message});
}

}  // namespace libchiaking::net::http
