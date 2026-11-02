#include "src/core/net/transport/http/request_log.hpp"

#include "src/core/net/transport/http/status.hpp"
#include "src/core/net/transport/http/url.hpp"
#include "src/core/utils/text/ascii.hpp"

#include <libchiaking/detail/secret_string.hpp>

#include <algorithm>
#include <array>
#include <utility>

namespace libchiaking::net::http {

using platform::LogSeverity;

auto Scrubber::secret_header(std::string_view name) noexcept -> bool {
  return std::ranges::any_of(
      secret_header_names,
      [name](std::string_view secret) { return text::ieq(name, secret); });
}

auto Scrubber::replaced(std::string_view text, std::string_view segment)
    -> SecretString {
  SecretString result;
  result.reserve(text.size());
  for (auto position = text.find(segment); position != std::string_view::npos;
       position = text.find(segment)) {
    result.append(text.substr(0, position));
    result.append(scrubbed);
    text.remove_prefix(position + segment.size());
  }
  result.append(text);
  return result;
}

void Scrubber::scrub(SecretString& text, std::string_view segment) {
  if (text.view().find(segment) != std::string_view::npos)
    text = replaced(text.view(), segment);
}

auto RequestLog::line(std::string_view label) -> std::string {
  std::string text{log_prefix};
  text.append(label);
  return text;
}

void RequestLog::write(platform::DiagnosticsProvider* sink,
                       LogSeverity severity,
                       std::string_view message) noexcept {
  if (sink != nullptr) sink->write({severity, message});
}

auto RequestLog::failed_status(HttpStatus status) noexcept -> bool {
  return status.is_client_error() || status.is_server_error();
}

auto Scrubber::log_target(std::string_view url, bool with_query) noexcept
    -> std::string_view {
  if (const auto scheme = url.find("://"); scheme != std::string_view::npos)
    url.remove_prefix(scheme + 3);
  url = url.substr(0, url.find('#'));
  if (!with_query) url = url.substr(0, url.find('?'));
  return url;
}

Scrubber::Scrubber(std::span<const platform::HttpHeader> sent) {
  for (const auto& header : sent) {
    if (!secret_header(header.name)) continue;
    auto value = header.value.view();
    for (auto start = value.find_first_not_of(separators);
         start != std::string_view::npos;
         start = value.find_first_not_of(separators)) {
      value.remove_prefix(start);
      const auto segment = value.substr(0, value.find_first_of(separators));
      if (segment.size() >= secret_segment_minimum)
        segments_.push_back(segment);
      value.remove_prefix(segment.size());
    }
  }
}

auto Scrubber::target(std::string_view url, bool with_query) const
    -> std::string {
  SecretString target{log_target(url, with_query)};
  for (const auto segment : segments_) {
    scrub(target, segment);
    SecretString encoded;
    PercentEncoding::append(encoded, segment);
    scrub(target, encoded.view());
  }
  return std::string{target.view()};
}

auto Scrubber::excerpt(std::string_view body) const -> std::string {
  std::size_t window = log_excerpt_bytes;
  for (const auto segment : segments_) window += segment.size();
  SecretString text{body.substr(0, window)};
  for (const auto segment : segments_) scrub(text, segment);
  std::string excerpt{text.view().substr(0, log_excerpt_bytes)};
  std::ranges::replace_if(
      excerpt, [](char value) { return !text::ascii_printable(value); }, '.');
  return excerpt;
}

RequestLog::RequestLog(platform::DiagnosticsProvider* sink,
                       const platform::ClockProvider& clock,
                       const Endpoint& endpoint,
                       const platform::HttpRequest& request,
                       std::string_view credential)
    : sink_(sink),
      label_(endpoint.label),
      scrubber_(request.headers),
      clock_(&clock),
      started_us_(clock.now_microseconds()) {
  if (sink_ == nullptr) return;
  auto text = line(label_);
  text.push_back(' ');
  text.append(platform::http_method_name(request.method));
  text.push_back(' ');
  text.append(scrubber_.target(request.url.view(), endpoint.log_query));
  if (!credential.empty()) {
    text.append(" auth=");
    text.append(credential);
  }
  write(sink_, LogSeverity::debug, text);
}

RequestLog::~RequestLog() {
  if (sink_ == nullptr) return;
  if (!outcome_.empty()) {
    write(sink_, severity_, outcome_);
    return;
  }
  try {
    write(sink_, LogSeverity::warning, line(label_) + " abandoned");
  } catch (...) {
    write(sink_, LogSeverity::warning, unwritten_line);
  }
}

void RequestLog::cancelled_before_start(platform::DiagnosticsProvider* sink,
                                        std::string_view label) noexcept {
  if (sink == nullptr) return;
  try {
    write(sink, LogSeverity::info, line(label) + " cancelled attempts=0 0ms");
  } catch (...) {
    write(sink, LogSeverity::warning, unwritten_line);
  }
}

void RequestLog::retry(int attempt, std::string_view reason,
                       std::chrono::milliseconds delay) const noexcept {
  if (sink_ == nullptr) return;
  try {
    write(sink_, LogSeverity::info,
          line(label_) + " retry " + std::to_string(attempt) + " after " +
              std::string{reason} + " in " + std::to_string(delay.count()) +
              "ms");
  } catch (...) {
    write(sink_, LogSeverity::warning, unwritten_line);
  }
}

void RequestLog::finish(const Expected<Response, Failure>& outcome) noexcept {
  if (sink_ == nullptr) return;
  try {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::microseconds{static_cast<std::int64_t>(
            clock_->now_microseconds() - started_us_)});
    auto text = line(label_);
    int attempts = 0;
    if (!outcome) {
      const auto& failure = outcome.error();
      attempts = failure.attempts();
      if (failure.is_cancelled()) {
        text.append(" cancelled");
        severity_ = LogSeverity::info;
      } else {
        text.append(" transport_error ");
        text.append(failure.error() ? error_name(*failure.error())
                                    : std::string_view{"failure"});
        severity_ = LogSeverity::warning;
      }
    } else {
      const auto& response = outcome.value();
      attempts = response.attempts();
      const HttpStatus status{response.status()};
      if (status.is_server_error()) {
        text.append(" server_error");
        severity_ = LogSeverity::warning;
      } else if (status.is_client_error()) {
        text.append(" client_error");
        severity_ = LogSeverity::info;
      } else {
        text.append(" ok");
        severity_ = LogSeverity::verbose;
      }
      text.append(" HTTP " + std::to_string(status.code()));
    }
    text.append(" attempts=" + std::to_string(attempts) + " " +
                std::to_string(elapsed.count()) + "ms");
    if (outcome && failed_status(HttpStatus{outcome.value().status()}) &&
        !outcome.value().body().empty())
      text.append(" body=\"" + scrubber_.excerpt(outcome.value().body()) +
                  "\"");
    outcome_ = std::move(text);
  } catch (...) {
    outcome_.clear();
    severity_ = LogSeverity::warning;
    write(sink_, LogSeverity::warning, unwritten_line);
  }
}

void RequestLog::log_page(platform::DiagnosticsProvider* sink,
                          std::string_view label, int page,
                          int items) noexcept {
  if (sink == nullptr) return;
  try {
    write(sink, LogSeverity::verbose,
          line(label) + " page " + std::to_string(page) + " items " +
              std::to_string(items));
  } catch (...) {
    write(sink, LogSeverity::warning, unwritten_line);
  }
}

void RequestLog::log_paging(platform::DiagnosticsProvider* sink,
                            std::string_view label, std::string_view outcome,
                            int requests, std::uint16_t last_status) noexcept {
  if (sink == nullptr) return;
  try {
    write(sink, outcome == "ok" ? LogSeverity::info : LogSeverity::warning,
          line(label) + " paging " + std::string{outcome} + " after " +
              std::to_string(requests) + " pages, last HTTP " +
              std::to_string(last_status));
  } catch (...) {
    write(sink, LogSeverity::warning, unwritten_line);
  }
}

auto RequestLog::status_class(HttpStatus status) noexcept -> std::string_view {
  if (status.is_success()) return "ok";
  if (status.is_auth_refused()) return "auth_refused";
  if (status == HttpStatus::not_found) return "not_found";
  if (status.is_client_error()) return "rejected";
  return "unavailable";
}

}  // namespace libchiaking::net::http
