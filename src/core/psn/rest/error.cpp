#include "src/core/psn/rest/error.hpp"

#include "src/core/utils/text/ascii.hpp"
#include "src/core/utils/text/utf8.hpp"

#include <utility>

namespace libchiaking::psn::rest {

ErrorStatus::ErrorStatus(std::uint16_t code, std::string excerpt) noexcept
    : code_(code), excerpt_(std::move(excerpt)) {}

auto ErrorStatus::excerpt_of(std::string_view body, std::string_view redact)
    -> std::string {
  std::string text{body};
  if (!redact.empty())
    for (auto position = text.find(redact); position != std::string::npos;
         position = text.find(redact, position + redaction.size()))
      text.replace(position, redact.size(), redaction);
  for (auto& ch : text)
    if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7F) ch = ' ';
  return std::string{text::trim(text::utf8::prefix(text, excerpt_bytes))};
}

auto ErrorStatus::of(const net::http::Response& response,
                     std::string_view redact) -> std::optional<ErrorStatus> {
  const auto status = response.status();
  if (net::HttpStatus{status}.is_success()) return std::nullopt;
  std::string excerpt;
  if (net::HttpStatus{status}.is_client_error())
    excerpt = excerpt_of(response.body(), redact);
  return ErrorStatus{status, std::move(excerpt)};
}

}  // namespace libchiaking::psn::rest
