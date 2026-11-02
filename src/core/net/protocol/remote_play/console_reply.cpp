#include "src/core/net/protocol/remote_play/console_reply.hpp"

#include "src/core/net/transport/http/status.hpp"
#include "src/core/utils/text/integer.hpp"

#include <picohttpparser/picohttpparser.h>

#include <ranges>
#include <utility>

namespace libchiaking::net::protocol::remote_play {

ConsoleReply::ConsoleReply(std::int64_t code, std::size_t length,
                           std::vector<Header> headers) noexcept
    : code_(code), length_(length), headers_(std::move(headers)) {}

auto ConsoleReply::response(std::string_view text)
    -> Expected<ConsoleReply, Refusal> {
  const auto line_end = text.find('\n');
  if (line_end == std::string_view::npos)
    return unexpected(Refusal::incomplete);
  const auto code = status_code(text.substr(0, line_end));
  if (!code) return unexpected(code.error());
  const auto status_line_length = line_end + 1;
  return fields(text.substr(status_line_length))
      .transform([&](ConsoleReply&& reply) {
        return ConsoleReply{code.value(), status_line_length + reply.length_,
                            std::move(reply.headers_)};
      });
}

auto ConsoleReply::status_code(std::string_view line) noexcept
    -> Expected<std::int64_t, Refusal> {
  if (line.ends_with('\r')) line.remove_suffix(1);
  if (!line.starts_with(net::http_version_prefix))
    return unexpected(Refusal::malformed);
  line.remove_prefix(net::http_version_prefix.size());
  const auto code =
      text::parse_integer<std::int64_t>(line.substr(0, line.find(' ')));
  if (!code || *code == 0) return unexpected(Refusal::malformed);
  return *code;
}

auto ConsoleReply::fields(std::string_view text)
    -> Expected<ConsoleReply, Refusal> {
  std::vector<phr_header> parsed(capacity(text));
  auto count = parsed.size();
  const auto consumed =
      phr_parse_headers(text.data(), text.size(), parsed.data(), &count, 0);
  if (consumed < 0) return unexpected(refusal(consumed));
  return ConsoleReply{0, static_cast<std::size_t>(consumed),
                      named(std::span{parsed}.first(count))};
}

auto ConsoleReply::code() const noexcept -> std::int64_t { return code_; }

auto ConsoleReply::status() const noexcept -> std::uint16_t {
  return code_ > 0 && code_ <= net::HttpStatus::largest_code
             ? static_cast<std::uint16_t>(code_)
             : std::uint16_t{0};
}

auto ConsoleReply::length() const noexcept -> std::size_t { return length_; }

auto ConsoleReply::headers() const noexcept -> std::span<const Header> {
  return headers_;
}

auto ConsoleReply::capacity(std::string_view text) noexcept -> std::size_t {
  return text.size() / 2 + 1;
}

auto ConsoleReply::refusal(int consumed) noexcept -> Refusal {
  return consumed == -2 ? Refusal::incomplete : Refusal::malformed;
}

auto ConsoleReply::named(std::span<const phr_header> parsed)
    -> std::vector<Header> {
  return parsed | std::views::filter([](const phr_header& header) {
           return header.name != nullptr;
         }) |
         std::views::transform([](const phr_header& header) {
           return Header{{header.name, header.name_len},
                         {header.value, header.value_len}};
         }) |
         std::ranges::to<std::vector>();
}

}  // namespace libchiaking::net::protocol::remote_play
