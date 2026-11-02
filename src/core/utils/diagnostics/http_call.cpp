#include "libchiaking/detail/diagnostics/http_call_log.hpp"

#include "src/core/net/transport/http/status.hpp"
#include "src/core/utils/diagnostics/sensitive_text.hpp"
#include "src/core/utils/json/wire_dialect.hpp"
#include "src/core/utils/text/ascii.hpp"
#include "src/core/utils/text/utf8.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace libchiaking::diagnostics {

struct HttpCall::Recorded {
  std::uint64_t sequence;
  HttpArea area;
  const platform::HttpRequest& request;
  const Expected<platform::HttpResponse, platform::HttpError>& outcome;
  Clock::time_point started;
  std::chrono::microseconds duration;
};

HttpCall::HttpCall(const Recorded& recorded)
    : sequence_(recorded.sequence),
      area_(recorded.area),
      credentials_(recorded.request.provenance.credentials),
      credential_label_(recorded.request.provenance.credential_label),
      label_(recorded.request.label),
      attempt_(recorded.request.provenance.attempt),
      method_(recorded.request.method),
      started_(recorded.started),
      duration_(recorded.duration),
      url_(recorded.request.url.clone()),
      request_headers_(platform::clone(recorded.request.headers)),
      request_body_(kept(recorded.request.body)),
      request_size_(recorded.request.body.size()),
      response_size_(0) {
  carried_.reserve(recorded.request.provenance.carried.size());
  std::ranges::transform(recorded.request.provenance.carried,
                         std::back_inserter(carried_), &SecretString::clone);
  if (!recorded.outcome) {
    transport_error_ = recorded.outcome.error();
    return;
  }
  const auto& response = recorded.outcome.value();
  status_ = response.status_code;
  expected_ = std::ranges::contains(
      recorded.request.provenance.expected_statuses, response.status_code);
  reason_ = response.reason;
  response_headers_ = platform::clone(response.headers);
  response_body_ = kept(response.body);
  response_size_ = response.body.size();
}

HttpCall::~HttpCall() = default;

auto HttpCall::recorded(
    std::uint64_t sequence, HttpArea area, const platform::HttpRequest& request,
    const Expected<platform::HttpResponse, platform::HttpError>& outcome,
    Clock::time_point started, std::chrono::microseconds duration)
    -> std::shared_ptr<const HttpCall> {
  return std::shared_ptr<const HttpCall>{new HttpCall{
      Recorded{sequence, area, request, outcome, started, duration}}};
}

auto HttpCall::base_of(std::string_view url) noexcept -> std::string_view {
  constexpr std::string_view separator = "://";
  const auto scheme = url.find(separator);
  if (scheme == std::string_view::npos) return {};
  return url.substr(0, url.find_first_of("/?#", scheme + separator.size()));
}

auto HttpCall::kept(const SecretBytes& body) -> SecretBytes {
  return SecretBytes{
      body.bytes().first(std::min(body.size(), HttpCallLog::body_limit))};
}

auto HttpCall::base() const noexcept -> std::string_view {
  return base_of(url_.view());
}

auto HttpCall::status() const noexcept -> std::optional<std::uint16_t> {
  return status_;
}

auto HttpCall::reason() const noexcept -> std::string_view { return reason_; }

auto HttpCall::transport_error() const noexcept
    -> std::optional<platform::HttpError> {
  return transport_error_;
}

auto HttpCall::failed() const noexcept -> bool {
  if (transport_error_.has_value()) return true;
  if (!status_ || expected_) return false;
  return net::HttpStatus{*status_}.is_client_error() ||
         net::HttpStatus{*status_}.is_server_error();
}

auto HttpCall::stored_bytes() const noexcept -> std::size_t {
  std::size_t bytes = url_.size() + request_body_.size() +
                      response_body_.size() + reason_.size() + label_.size() +
                      credential_label_.size();
  for (const auto* headers : {&request_headers_, &response_headers_})
    for (const auto& header : *headers)
      bytes += header.name.size() + header.value.size();
  for (const auto& secret : carried_) bytes += secret.size();
  return bytes;
}

void HttpCall::add_parameter_spans(std::vector<HttpSecretSpan>& spans,
                                   std::string_view text, std::size_t from) {
  for (const auto& value : SensitiveText::parameter_values(text, from))
    spans.push_back({value.offset, value.length, HttpSecretKind::parameter});
}

auto HttpCall::header_spans(std::string_view name, std::string_view value)
    -> std::vector<HttpSecretSpan> {
  std::vector<HttpSecretSpan> spans;
  if (!SensitiveText::header(name)) {
    add_parameter_spans(spans, value, value.find_first_of("?#"));
    return spans;
  }
  if (text::ieq(name, "authorization") ||
      text::ieq(name, "proxy-authorization")) {
    const auto space = value.find(' ');
    const auto from = space == std::string_view::npos
                          ? 0
                          : value.find_first_not_of(' ', space);
    if (from != std::string_view::npos)
      spans.push_back({from, value.size() - from, HttpSecretKind::credential});
    return spans;
  }
  const bool setting = text::ieq(name, "set-cookie");
  if (!setting && !text::ieq(name, "cookie")) {
    spans.push_back({0, value.size(), HttpSecretKind::session});
    return spans;
  }
  for (std::size_t start = 0; start < value.size();) {
    const auto end = std::min(value.find(';', start), value.size());
    const auto equals = value.find('=', start);
    if (equals != std::string_view::npos && equals < end)
      spans.push_back({equals + 1, end - equals - 1, HttpSecretKind::cookie});
    if (setting) break;
    start = end + 1;
  }
  return spans;
}

void HttpCall::add_known(std::vector<Known>& known, std::string_view text,
                         std::span<const HttpSecretSpan> spans) {
  for (const auto& span : spans)
    if (span.length >= shortest_known)
      known.push_back({text.substr(span.offset, span.length), span.kind});
}

auto HttpCall::known() const -> std::vector<Known> {
  std::vector<Known> known;
  for (const auto* headers : {&request_headers_, &response_headers_})
    for (const auto& header : *headers)
      if (SensitiveText::header(header.name))
        add_known(known, header.value.view(),
                  header_spans(header.name, header.value.view()));
  for (const auto& secret : carried_)
    if (secret.size() >= shortest_known)
      known.push_back({secret.view(), HttpSecretKind::credential});
  std::vector<HttpSecretSpan> parameters;
  const auto url = url_.view();
  add_parameter_spans(parameters, url, url.find_first_of("?#"));
  add_known(known, url, parameters);
  return known;
}

void HttpCall::add_matches(std::vector<HttpSecretSpan>& spans,
                           std::string_view text,
                           std::span<const Known> known) {
  for (const auto& secret : known)
    for (auto found = text.find(secret.value); found != std::string_view::npos;
         found = text.find(secret.value, found + secret.value.size()))
      spans.push_back({found, secret.value.size(), secret.kind});
}

auto HttpCall::form_like(std::string_view text) noexcept -> bool {
  return text.find('=') != std::string_view::npos &&
         std::ranges::all_of(text, text::ascii_printable);
}

auto HttpCall::url() const -> HttpMarkedText {
  const auto text = url_.view();
  std::vector<HttpSecretSpan> spans;
  add_parameter_spans(spans, text, text.find_first_of("?#"));
  add_matches(spans, text, known());
  return HttpMarkedText{url_.clone(), std::move(spans)};
}

auto HttpCall::marked_headers(const std::vector<platform::HttpHeader>& headers)
    const -> std::vector<HttpMarkedHeader> {
  const auto secrets = known();
  std::vector<HttpMarkedHeader> marked;
  marked.reserve(headers.size());
  for (const auto& header : headers) {
    auto spans = header_spans(header.name, header.value.view());
    add_matches(spans, header.value.view(), secrets);
    marked.push_back(
        {header.name, HttpMarkedText{header.value.clone(), std::move(spans)}});
  }
  return marked;
}

auto HttpCall::request_headers() const -> std::vector<HttpMarkedHeader> {
  return marked_headers(request_headers_);
}

auto HttpCall::response_headers() const -> std::vector<HttpMarkedHeader> {
  return marked_headers(response_headers_);
}

auto HttpCall::request_body() const -> HttpShownBody {
  return shown(request_body_, request_size_);
}

auto HttpCall::response_body() const -> HttpShownBody {
  return shown(response_body_, response_size_);
}

auto HttpCall::shown(const SecretBytes& body, std::size_t size) const
    -> HttpShownBody {
  HttpShownBody out{
      .size = size, .kept = body.size(), .truncated = body.size() < size};
  auto text = body.view();
  if (out.truncated)
    text = text.substr(0, simdutf::trim_partial_utf8(text.data(), text.size()));
  if (!text::utf8::valid(text) || text.find('\0') != std::string_view::npos) {
    out.binary = true;
    return out;
  }
  const auto secrets = known();
  std::vector<HttpSecretSpan> spans;
  const auto first = std::ranges::find_if_not(text, text::json_space);
  if (first != text.end() && (*first == '{' || *first == '[')) {
    auto marked = json::WireDialect::marked_text(text, SensitiveText::json_key);
    if (!marked) {
      out.text = HttpMarkedText{SecretString{text},
                                {{0, text.size(), HttpSecretKind::body}}};
      return out;
    }
    for (const auto& [offset, length] : marked.value().concealed)
      spans.push_back({offset, length, HttpSecretKind::field});
    add_matches(spans, marked.value().text.view(), secrets);
    out.text = HttpMarkedText{std::move(marked.value().text), std::move(spans)};
    return out;
  }
  if (form_like(text)) add_parameter_spans(spans, text, 0);
  add_matches(spans, text, secrets);
  out.text = HttpMarkedText{SecretString{text}, std::move(spans)};
  return out;
}

}  // namespace libchiaking::diagnostics
