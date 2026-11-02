#include "libchiaking/detail/diagnostics/http_call_log.hpp"

#include <algorithm>
#include <utility>

namespace libchiaking::diagnostics {

HttpMarkedText::HttpMarkedText(SecretString text,
                               std::vector<HttpSecretSpan> spans)
    : text_(std::move(text)) {
  const auto size = text_.size();
  std::erase_if(spans, [size](const HttpSecretSpan& span) {
    return span.length == 0 || span.offset >= size;
  });
  std::ranges::stable_sort(spans, {}, &HttpSecretSpan::offset);
  for (auto span : spans) {
    span.length = std::min(span.length, size - span.offset);
    if (!spans_.empty() &&
        span.offset <= spans_.back().offset + spans_.back().length) {
      auto& last = spans_.back();
      last.length =
          std::max(last.offset + last.length, span.offset + span.length) -
          last.offset;
      continue;
    }
    spans_.push_back(span);
  }
}

auto HttpMarkedText::clone() const -> HttpMarkedText {
  HttpMarkedText copy;
  copy.text_ = text_.clone();
  copy.spans_ = spans_;
  return copy;
}

}  // namespace libchiaking::diagnostics
