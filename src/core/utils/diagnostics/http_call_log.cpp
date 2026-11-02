#include "libchiaking/detail/diagnostics/http_call_log.hpp"

#include "src/core/net/transport/http/status.hpp"

#include <algorithm>
#include <utility>

namespace libchiaking::diagnostics {

HttpCallLog::HttpCallLog(platform::ThreadingProvider& threading)
    : mutex_(threading) {}

HttpCallLog::~HttpCallLog() = default;

void HttpCallLog::count(
    HttpTally& tally, std::size_t sent,
    const Expected<platform::HttpResponse, platform::HttpError>& outcome,
    bool expected) noexcept {
  ++tally.requests;
  tally.bytes_sent += sent;
  if (!outcome) {
    ++tally.transport_errors;
    return;
  }
  const auto status = outcome.value().status_code;
  if (!expected && (net::HttpStatus{status}.is_client_error() ||
                    net::HttpStatus{status}.is_server_error()))
    ++tally.http_errors;
  tally.bytes_received += outcome.value().body.size();
}

void HttpCallLog::count(
    const platform::HttpRequest& request,
    const Expected<platform::HttpResponse, platform::HttpError>& outcome) {
  const auto base = HttpCall::base_of(request.url.view());
  auto found = counts_.bases.find(base);
  if (found == counts_.bases.end())
    found = counts_.bases.emplace(std::string{base}, HttpTally{}).first;
  const auto sent = request.body.size();
  const auto credentials = request.provenance.credentials;
  const auto expected =
      outcome && std::ranges::contains(request.provenance.expected_statuses,
                                       outcome.value().status_code);
  count(found->second, sent, outcome, expected);
  count(counts_.all, sent, outcome, expected);
  if (credentials.anonymous())
    count(counts_.anonymous, sent, outcome, expected);
  for (std::size_t index = 0; index < platform::every_http_credential.size();
       ++index)
    if (credentials.contains(platform::every_http_credential[index]))
      count(counts_.credentials[index], sent, outcome, expected);
}

void HttpCallLog::record(
    HttpArea area, const platform::HttpRequest& request,
    const Expected<platform::HttpResponse, platform::HttpError>& outcome,
    HttpCall::Clock::time_point started,
    std::chrono::microseconds duration) noexcept {
  const auto sequence = sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
  std::shared_ptr<const HttpCall> call;
  std::vector<std::shared_ptr<const HttpCall>> dropped;
  try {
    call =
        HttpCall::recorded(sequence, area, request, outcome, started, duration);
    dropped.reserve(evictions_reserved);
  } catch (...) {
    call.reset();
  }
  try {
    const platform::Lock lock{mutex_};
    count(request, outcome);
    if (call) {
      const auto bytes = call->stored_bytes();
      const auto newer = std::ranges::find_if(
          calls_,
          [sequence](const auto& kept) { return kept->sequence() < sequence; });
      calls_.insert(newer, std::move(call));
      bytes_ += bytes;
      while (calls_.size() > capacity ||
             (bytes_ > byte_limit && calls_.size() > 1)) {
        bytes_ -= calls_.back()->stored_bytes();
        dropped.push_back(std::move(calls_.back()));
        calls_.pop_back();
      }
    }
    revision_.fetch_add(1, std::memory_order_acq_rel);
  } catch (...) {
    revision_.fetch_add(1, std::memory_order_acq_rel);
  }
}

auto HttpCallLog::snapshot() const -> HttpCallSnapshot {
  HttpCallSnapshot snapshot;
  const platform::Lock lock{mutex_};
  snapshot.revision = revision_.load(std::memory_order_acquire);
  snapshot.calls.assign(calls_.begin(), calls_.end());
  snapshot.totals.all = counts_.all;
  snapshot.totals.anonymous = counts_.anonymous;
  snapshot.totals.credentials.reserve(platform::every_http_credential.size());
  for (std::size_t index = 0; index < platform::every_http_credential.size();
       ++index)
    snapshot.totals.credentials.push_back(
        {platform::every_http_credential[index], counts_.credentials[index]});
  snapshot.totals.bases.reserve(counts_.bases.size());
  for (const auto& [base, tally] : counts_.bases)
    snapshot.totals.bases.push_back({base, tally});
  return snapshot;
}

void HttpCallLog::clear() noexcept {
  std::deque<std::shared_ptr<const HttpCall>> dropped;
  {
    const platform::Lock lock{mutex_};
    dropped.swap(calls_);
    bytes_ = 0;
    revision_.fetch_add(1, std::memory_order_acq_rel);
  }
}

void HttpCallLog::reset_totals() noexcept {
  Counts zero;
  {
    const platform::Lock lock{mutex_};
    std::swap(counts_, zero);
    revision_.fetch_add(1, std::memory_order_acq_rel);
  }
}

}  // namespace libchiaking::diagnostics
