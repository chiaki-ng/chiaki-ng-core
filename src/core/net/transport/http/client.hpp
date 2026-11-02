/// @file
/// @brief Declares Client, the one way the library sends HTTP requests, and the
/// paging and waiting it offers on top of single requests.
#pragma once

#include "src/core/net/transport/http/endpoint.hpp"
#include "src/core/net/transport/http/failure.hpp"
#include "src/core/net/transport/http/request.hpp"
#include "src/core/net/transport/http/request_log.hpp"
#include "src/core/net/transport/http/response.hpp"

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/platform/diagnostics/provider.hpp>
#include <libchiaking/platform/http/provider.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace libchiaking::net::http {

/// @brief Pauses between retries and pages, interruptibly.
///
/// Supplied by the caller so tests can skip the pauses. Called on the thread
/// sending the request.
class Waiter {
 public:
  /// @brief Releases the waiter.
  virtual ~Waiter() = default;
  /// @brief Waits for a time or until stopped.
  ///
  /// Blocks for up to `milliseconds`.
  ///
  /// @param milliseconds How long to wait.
  /// @param stop Ends the wait early.
  /// @return False when the wait was cut short by `stop`.
  [[nodiscard]] virtual auto wait_milliseconds(int milliseconds,
                                               const platform::StopToken& stop)
      -> bool = 0;
};

/// @brief Makes a fresh Waiter each time it is called, so that a caller
/// shared between threads can give each wait a waiter of its own.
using WaiterFactory = std::function<std::unique_ptr<Waiter>()>;

/// @brief How a paged walk asks for pages.
struct Paging {
  /// @brief Items asked for per page; a page with fewer ends the walk.
  int page_size{};
  /// @brief Most pages requested; reaching it marks the walk truncated.
  int page_limit{};
  /// @brief Pause before each page after the first.
  std::chrono::milliseconds delay{};
};

/// @brief How a successful paged walk went.
struct PageSummary {
  /// @brief Pages requested.
  int requests{};
  /// @brief The last page's status.
  std::uint16_t last_status{};
  /// @brief Whether the page limit was reached before a short page.
  bool truncated{};
};

/// @brief Why a paged walk stopped early, and how far it got.
///
/// @tparam E The error the page consumer reports.
template <class E>
struct PageError {
  /// @brief The request's failure, or the consumer's error.
  std::variant<Failure, E> cause;
  /// @brief Pages requested, including the failed one.
  int requests{};
  /// @brief The last page's status; 0 when the last request got none.
  std::uint16_t last_status{};
};

/// @brief Finds the error type of a page consumer's result.
///
/// @tparam T The consumer's result type; only `Expected<int, E>` is
///     defined.
template <class T>
struct page_error_of;

/// @brief The error type of a consumer returning `Expected<int, E>`.
///
/// @tparam E The consumer's error.
template <class E>
struct page_error_of<Expected<int, E>> {
  /// @brief The consumer's error type.
  using type = E;
};

/// @brief Sends requests through the platform's HTTP provider with retries,
/// cancellation, size limits and scrubbed logging.
///
/// Each send() makes up to the endpoint's retries plus one attempts, retrying
/// the failures its Retry allows after its delay, and logs through RequestLog.
/// The stop token cancels a send between and during attempts; a response that
/// arrives after it fired is dropped unless the endpoint keeps it. A client is
/// a cheap value: copies share the provider, clock, token, log and waiter,
/// which must outlive them. send() blocks the calling thread; one client may be used from
/// several threads if the provider allows concurrent requests.
class Client {
 public:
  /// @brief Makes a client.
  ///
  /// @param provider Performs the requests; must outlive the client.
  /// @param clock The monotonic clock request logs measure elapsed time on;
  ///     must outlive the client.
  /// @param stop Cancels every request through this client.
  /// @param log Receives request logs; may be null.
  /// @param waiter Pauses between retries and pages; null retries at once.
  Client(platform::HttpProvider& provider, const platform::ClockProvider& clock,
         platform::StopToken stop, platform::DiagnosticsProvider* log = nullptr,
         Waiter* waiter = nullptr) noexcept;

  /// @brief A request started by start() whose response has not been
  /// collected.
  class Pending;

  /// @brief Starts a request's first attempt on the provider and returns
  /// without waiting for it.
  ///
  /// Several started requests run at once when the provider allows it.
  /// Collecting one with Pending::wait() finishes it as send() would,
  /// making any retries on the collecting thread. Dropping it uncollected
  /// abandons the attempt and logs the request as abandoned.
  ///
  /// @param request The request; consumed.
  /// @return The pending request; it holds a copy of this client.
  [[nodiscard]] auto start(Request request) -> Pending;

  /// @brief Sends a request, retrying as its endpoint allows.
  ///
  /// The body size limit is the smaller of the endpoint's and this client's.
  /// Blocks for each attempt, up to the endpoint's total timeout, and each
  /// retry delay.
  ///
  /// @param request The request; consumed.
  /// @return The response, whatever its status.
  /// @retval Failure::Kind::cancelled The stop token fired before, during or
  ///     after an attempt, or during a retry pause.
  /// @retval Failure::Kind::transport The last attempt got no response.
  /// @retval Failure::Kind::too_large The body exceeded the size limit.
  /// @retval Failure::Kind::invalid_request The platform refused the URL.
  [[nodiscard]] auto send(Request request) -> Expected<Response, Failure>;
  /// @brief A copy whose requests accept at most this many response bytes.
  ///
  /// @param maximum_response_bytes The limit, in bytes; a lower existing
  ///     limit
  ///     is kept.
  /// @return The copy.
  [[nodiscard]] auto limited(std::size_t maximum_response_bytes) const noexcept
      -> Client;
  /// @brief Pauses through the waiter, unless stopped.
  ///
  /// Blocks for up to `delay`; returns at once without a waiter or for a
  /// delay
  /// of 0 or less.
  ///
  /// @param delay How long to pause.
  /// @return False when the stop token fired before or during the pause.
  [[nodiscard]] auto wait(std::chrono::milliseconds delay) -> bool;

  /// @brief Walks a paged collection, one request per page, until a page
  /// comes
  /// back short or the page limit is reached.
  ///
  /// Blocks for every page and the pauses between them.
  ///
  /// @tparam Build Callable taking the item offset and returning the Request
  ///     for that page.
  /// @tparam Consume Callable taking the Response and returning
  ///     `Expected<int, E>`, the number of items it read.
  /// @param paging Page size, page limit and pause.
  /// @param build Builds each page's request from the running offset, 0
  ///     first.
  /// @param consume Reads each page; its error ends the walk.
  /// @return How the walk went; truncated when the limit was reached.
  /// @retval PageError A request failed or the consumer refused a page; the
  ///     cause is the Failure or the consumer's error.
  template <class Build, class Consume>
    requires std::convertible_to<std::invoke_result_t<Build&, int>, Request>
  [[nodiscard]] auto pages(const Paging& paging, Build&& build,
                           Consume&& consume)
      -> Expected<PageSummary,
                  PageError<typename page_error_of<
                      std::invoke_result_t<Consume&, const Response&>>::type>>;

  /// @brief Whether the client's stop token has fired.
  ///
  /// @return True once stopped.
  auto stop_requested() const noexcept -> bool {
    return stop_.stop_requested();
  }
  /// @brief The stop token that cancels this client's requests.
  ///
  /// @return The token.
  auto stop_token() const noexcept -> const platform::StopToken& {
    return stop_;
  }
  /// @brief Where this client logs.
  ///
  /// @return The sink; null for none.
  auto diagnostics() const noexcept -> platform::DiagnosticsProvider* {
    return log_;
  }
  /// @brief Writes a line to the client's log, if it has one.
  ///
  /// @param severity The line's severity.
  /// @param message The line.
  void log(platform::LogSeverity severity,
           std::string_view message) const noexcept;

 private:
  /// @brief Decides whether an attempt's result is worth retrying.
  ///
  /// @param retry The endpoint's policy.
  /// @param result The attempt's result.
  /// @return Why to retry, such as `HTTP 503` or `transport timed_out`;
  ///     nothing when the policy does not retry this result.
  static auto retry_reason(
      const Retry& retry,
      const Expected<platform::HttpResponse, platform::HttpError>& result)
      -> std::optional<std::string>;

  /// @brief Performs the requests; never null.
  platform::HttpProvider* provider_;
  /// @brief The monotonic clock request logs measure on; never null.
  const platform::ClockProvider* clock_;
  /// @brief Cancels every request.
  platform::StopToken stop_;
  /// @brief Receives logs; may be null.
  platform::DiagnosticsProvider* log_;
  /// @brief Pauses between retries and pages; may be null.
  Waiter* waiter_;
  /// @brief This client's cap on response bodies, in bytes; no cap by
  /// default.
  std::size_t maximum_response_bytes_{std::numeric_limits<std::size_t>::max()};
};

/// @brief A request started by Client::start(), collected once with wait().
///
/// Move-only. Dropping it uncollected abandons the request.
class Client::Pending {
 public:
  /// @brief Takes over another pending request.
  ///
  /// @param other The request; left empty.
  Pending(Pending&& other) noexcept;
  /// @brief Abandons this request, if uncollected, and takes over another.
  ///
  /// @param other The request; left empty.
  /// @return This request.
  auto operator=(Pending&& other) noexcept -> Pending&;
  /// @brief Abandons the request if it was never collected.
  ~Pending();

  /// @brief Waits for the request and finishes it, retrying as its endpoint
  /// allows.
  ///
  /// Blocks until the first attempt completes, then for each retry and its
  /// delay.
  ///
  /// @return The response, whatever its status.
  /// @retval Failure::Kind::cancelled The stop token fired before, during or
  ///     after an attempt, or during a retry pause, or this request was
  ///     already collected or moved from.
  /// @retval Failure::Kind::transport The last attempt got no response.
  /// @retval Failure::Kind::too_large The body exceeded the size limit.
  /// @retval Failure::Kind::invalid_request The platform refused the URL.
  [[nodiscard]] auto wait() && -> Expected<Response, Failure>;

 private:
  /// @brief Starts requests.
  friend class Client;
  /// @brief The request, its log and its first attempt.
  class Exchange;

  /// @brief Wraps a started exchange.
  ///
  /// @param exchange The exchange; never null.
  explicit Pending(std::unique_ptr<Exchange> exchange) noexcept;

  /// @brief The exchange; null once collected or moved from.
  std::unique_ptr<Exchange> exchange_;
};

template <class Build, class Consume>
  requires std::convertible_to<std::invoke_result_t<Build&, int>, Request>
auto Client::pages(const Paging& paging, Build&& build, Consume&& consume)
    -> Expected<PageSummary,
                PageError<typename page_error_of<
                    std::invoke_result_t<Consume&, const Response&>>::type>> {
  using E = typename page_error_of<
      std::invoke_result_t<Consume&, const Response&>>::type;
  using Error = PageError<E>;
  std::string_view label;
  PageSummary summary;
  int offset = 0;
  const auto fail = [&](std::variant<Failure, E> cause,
                        std::string_view outcome) {
    RequestLog::log_paging(log_, label, outcome, summary.requests,
                           summary.last_status);
    return unexpected(
        Error{std::move(cause), summary.requests, summary.last_status});
  };
  for (int page = 0; page < paging.page_limit; ++page) {
    if (page != 0 && !wait(paging.delay))
      return fail(Failure::cancelled(), "cancelled");
    Request request = build(offset);
    label = request.endpoint().label;
    auto sent = send(std::move(request));
    ++summary.requests;
    if (!sent) {
      summary.last_status = 0;
      const auto outcome =
          sent.error().is_cancelled() ? "cancelled" : "unavailable";
      return fail(sent.error(), outcome);
    }
    summary.last_status = sent.value().status();
    auto consumed = consume(std::as_const(sent.value()));
    if (!consumed) {
      const auto outcome =
          RequestLog::status_class(HttpStatus{summary.last_status});
      return fail(std::move(consumed.error()),
                  outcome == "ok" ? "malformed" : outcome);
    }
    const int count = consumed.value();
    RequestLog::log_page(log_, label, page + 1, count);
    if (count < paging.page_size) {
      RequestLog::log_paging(log_, label, "ok", summary.requests,
                             summary.last_status);
      return summary;
    }
    offset += count;
  }
  summary.truncated = true;
  RequestLog::log_paging(log_, label, "truncated", summary.requests,
                         summary.last_status);
  return summary;
}

}  // namespace libchiaking::net::http
