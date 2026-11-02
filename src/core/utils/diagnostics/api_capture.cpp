#include "libchiaking/detail/diagnostics/api_capture.hpp"

#include "src/core/utils/diagnostics/api_name.hpp"
#include "src/core/utils/diagnostics/sensitive_text.hpp"
#include "src/core/utils/json/json.hpp"
#include "src/core/utils/json/ps_reply.hpp"
#include "src/core/utils/json/wire_dialect.hpp"
#include "src/core/utils/text/ascii.hpp"
#include "src/core/utils/text/base64.hpp"
#include "src/core/utils/text/utf8.hpp"

#include <algorithm>
#include <chrono>
#include <new>
#include <optional>
#include <string>
#include <utility>

namespace libchiaking::diagnostics {

auto ApiCapture::redact_body(std::string_view body) -> Text {
  const auto first = std::ranges::find_if_not(body, text::json_space);
  if (first != body.end() && (*first == '{' || *first == '['))
    return ApiCapture::redact_json(body);
  if (!std::ranges::all_of(body, text::ascii_printable)) return Text{body};
  if (body.find('=') == std::string_view::npos) return Text{body};
  return ApiCapture::redact_params(body);
}

struct ApiCapture::Header {
  std::string_view name;
  Text value;
};

struct ApiCapture::Payload {
  bool redacted{};
  std::vector<Header> headers;
  std::size_t headers_omitted{};
  std::size_t body_bytes{};
  std::optional<std::string_view> body_omitted;
  std::optional<Text> body_base64;
};

struct ApiCapture::RequestRecord {
  int schema{1};
  std::string_view source{"core"};
  std::uint64_t attempt{};
  std::uint64_t call{};
  std::string_view phase{"request"};
  Text api;
  std::string_view method;
  Text url;
  std::int64_t timeout_ms{};
  bool redacted{};
  std::vector<Header> headers;
  std::size_t headers_omitted{};
  std::size_t body_bytes{};
  std::optional<std::string_view> body_omitted;
  std::optional<Text> body_base64;
};

struct ApiCapture::ResponseRecord {
  int schema{1};
  std::string_view source{"core"};
  std::uint64_t attempt{};
  std::uint64_t call{};
  std::string_view phase{"response"};
  std::int64_t duration_us{};
  std::optional<std::uint16_t> status;
  std::optional<Text> reason;
  std::optional<Text> url;
  std::optional<bool> redacted;
  std::optional<std::vector<Header>> headers;
  std::optional<std::size_t> headers_omitted;
  std::optional<std::size_t> body_bytes;
  std::optional<std::string_view> body_omitted;
  std::optional<Text> body_base64;
  std::optional<unsigned> transport_error;
};

struct ApiCapture::LossRecord {
  int schema{1};
  std::string_view source{"core"};
  std::string_view phase{"capture_loss"};
  std::uint64_t dropped{};
};

auto ApiCapture::shown(std::string_view value) -> Text {
  if (value.size() > ApiCapture::field_limit)
    return Text{"[omitted: field exceeds 128 KiB capture limit]"};
  Text out;
  out.reserve(value.size());
  while (!value.empty()) {
    const auto checked =
        simdutf::validate_utf8_with_errors(value.data(), value.size());
    if (checked.error == simdutf::error_code::SUCCESS) {
      out.append(value);
      break;
    }
    out.append(value.substr(0, checked.count));
    text::utf8::append(out, static_cast<unsigned char>(value[checked.count]));
    value.remove_prefix(checked.count + 1);
  }
  return out;
}

auto ApiCapture::payload(const std::vector<platform::HttpHeader>& headers,
                         const SecretBytes& body, bool raw) -> Payload {
  Payload out{.redacted = !raw, .body_bytes = body.size()};
  std::size_t header_bytes{};
  for (const auto& header : headers) {
    if (out.headers.size() == ApiCapture::header_count_limit ||
        header.name.size() > ApiCapture::header_bytes_limit - header_bytes)
      break;
    header_bytes += header.name.size();
    if (header.value.size() > ApiCapture::header_bytes_limit - header_bytes)
      break;
    header_bytes += header.value.size();
    auto value =
        raw ? shown(header.value.view())
        : SensitiveText::header(header.name)
            ? Text{ApiCapture::redacted}
            : shown(ApiCapture::redact_url(header.value.view()).view());
    out.headers.push_back({header.name, std::move(value)});
  }
  out.headers_omitted = headers.size() - out.headers.size();
  if (body.size() > ApiCapture::body_limit) {
    out.body_omitted = "exceeds 16 MiB capture limit";
    return out;
  }
  const Text redacted_payload = raw ? Text{} : redact_body(body.view());
  const auto content = raw ? body.view() : redacted_payload.view();
  Text encoded;
  text::base64::append(encoded,
                       std::as_bytes(std::span{content.data(), content.size()}),
                       text::base64::Alphabet::standard);
  out.body_base64 = std::move(encoded);
  return out;
}

template <class Record>
auto ApiCapture::written(const Record& record) -> Text {
  auto text = json::ps::write(record);
  if (!text) throw std::bad_alloc{};
  return std::move(text.value());
}

auto ApiCapture::loss(std::uint64_t lost) -> Text {
  return written(LossRecord{.dropped = lost});
}

class ApiCapture::Turn {
 public:
  explicit Turn(std::atomic_flag& flag) noexcept
      : flag_(flag), held_(!flag.test_and_set(std::memory_order_acquire)) {}

  ~Turn() {
    if (held_) flag_.clear(std::memory_order_release);
  }

  Turn(const Turn&) = delete;
  auto operator=(const Turn&) -> Turn& = delete;

  explicit operator bool() const noexcept { return held_; }

 private:
  std::atomic_flag& flag_;
  bool held_;
};

template <class Make>
void ApiCapture::capture(Make make) noexcept {
  Text record;
  try {
    record = make();
  } catch (...) {
    ++dropped_;
    return;
  }
  const Turn turn{locked_};
  if (!turn || count_ == capacity || record.size() > byte_limit - bytes_) {
    ++dropped_;
    return;
  }
  bytes_ += record.size();
  records_[(head_ + count_) % capacity] = std::move(record);
  ++count_;
}

void ApiCapture::enable(bool enabled) noexcept { enabled_.store(enabled); }

void ApiCapture::allow_raw(bool allowed) noexcept { raw_.store(allowed); }

auto ApiCapture::begin(const platform::HttpRequest& request,
                       std::uint64_t attempt) noexcept -> std::uint64_t {
  if (!enabled_.load()) return 0;
  const auto id = ++next_;
  const bool raw = raw_.load();
  capture([&] {
    auto shared = payload(request.headers, request.body, raw);
    return written(
        RequestRecord{.attempt = attempt,
                      .call = id,
                      .api = shown(ApiName{request}.view()),
                      .method = platform::http_method_name(request.method),
                      .url = shown(raw ? request.url.view()
                                       : redact_url(request.url.view()).view()),
                      .timeout_ms = request.total_timeout.count(),
                      .redacted = shared.redacted,
                      .headers = std::move(shared.headers),
                      .headers_omitted = shared.headers_omitted,
                      .body_bytes = shared.body_bytes,
                      .body_omitted = shared.body_omitted,
                      .body_base64 = std::move(shared.body_base64)});
  });
  return id;
}

void ApiCapture::finish(
    std::uint64_t attempt, std::uint64_t id,
    const Expected<platform::HttpResponse, platform::HttpError>& response,
    std::chrono::microseconds duration) noexcept {
  if (!id) return;
  const bool raw = raw_.load();
  capture([&] {
    ResponseRecord record{
        .attempt = attempt, .call = id, .duration_us = duration.count()};
    if (!response) {
      record.transport_error = static_cast<unsigned>(response.error());
      return written(record);
    }
    const auto& value = response.value();
    auto shared = payload(value.headers, value.body, raw);
    record.status = value.status_code;
    record.reason = shown(value.reason);
    record.url = shown(raw ? value.final_url.view()
                           : redact_url(value.final_url.view()).view());
    record.redacted = shared.redacted;
    record.headers = std::move(shared.headers);
    record.headers_omitted = shared.headers_omitted;
    record.body_bytes = shared.body_bytes;
    record.body_omitted = shared.body_omitted;
    record.body_base64 = std::move(shared.body_base64);
    return written(record);
  });
}

auto ApiCapture::read(std::span<char> destination) noexcept -> std::size_t {
  const Turn turn{locked_};
  if (!turn) return 0;
  if (count_) {
    auto& record = records_[head_];
    const auto required = record.size();
    if (destination.data() && destination.size() >= required) {
      std::copy_n(record.data(), required, destination.data());
      bytes_ -= required;
      record = Text{};
      head_ = (head_ + 1) % capacity;
      --count_;
    }
    return required;
  }
  const auto lost = dropped_.load();
  if (!lost) return 0;
  try {
    const auto message = loss(lost);
    if (destination.data() && destination.size() >= message.size()) {
      std::ranges::copy(message.view(), destination.data());
      dropped_.fetch_sub(lost);
    }
    return message.size();
  } catch (...) {
    return 0;
  }
}

auto ApiCapture::take() -> std::optional<Text> {
  const Turn turn{locked_};
  if (!turn) return std::nullopt;
  if (count_) {
    auto record = std::exchange(records_[head_], Text{});
    bytes_ -= record.size();
    head_ = (head_ + 1) % capacity;
    --count_;
    return record;
  }
  const auto lost = dropped_.load();
  if (!lost) return std::nullopt;
  auto record = loss(lost);
  dropped_.fetch_sub(lost);
  return record;
}

auto ApiCapture::redact_url(std::string_view url) -> Text {
  const auto start = url.find_first_of("?#");
  if (start == std::string_view::npos) return Text{url};
  Text out{url.substr(0, start)};
  out += redact_params(url.substr(start));
  return out;
}

auto ApiCapture::redact_params(std::string_view text) -> Text {
  Text out;
  std::size_t copied = 0;
  for (const auto& value : SensitiveText::parameter_values(text)) {
    out += text.substr(copied, value.offset - copied);
    out += redacted;
    copied = value.offset + value.length;
  }
  out += text.substr(copied);
  return out;
}

auto ApiCapture::redact_json(std::string_view text) -> Text {
  const auto document = json::admitted(text, text.size());
  if (!document || !json::well_formed(document.value())) return Text{redacted};
  const auto concealed = json::WireDialect::concealed_text(
      document.value(), SensitiveText::json_key, redacted);
  if (!concealed) return Text{redacted};
  Text out;
  text::utf8::append_repaired(out, concealed.value().view());
  return out;
}

}  // namespace libchiaking::diagnostics
