#include "src/core/psn/server_clock.hpp"

#include "src/core/net/transport/http/strict_url.hpp"
#include "src/core/utils/text/ascii.hpp"
#include "src/core/utils/text/integer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>

namespace libchiaking::psn {

class ServerClock::Cursor {
 public:
  explicit Cursor(std::string_view text) noexcept : text_(text) {}

  auto number(std::size_t minimum, std::size_t maximum) noexcept
      -> std::optional<int> {
    const auto run = text_.substr(
        position_,
        text_.find_first_not_of("0123456789", position_) - position_);
    const auto digits = run.substr(0, maximum);
    const auto value = text::parse_integer<unsigned>(digits);
    if (digits.size() < minimum || !value) return std::nullopt;
    position_ += digits.size();
    return static_cast<int>(*value);
  }

  auto word() noexcept -> std::string_view {
    const auto start = position_;
    while (position_ < text_.size() && text::ascii_alpha(text_[position_]))
      ++position_;
    return text_.substr(start, position_ - start);
  }

  auto expect(char value) noexcept -> bool {
    if (position_ >= text_.size() || text_[position_] != value) return false;
    ++position_;
    return true;
  }

  void spaces() noexcept {
    while (position_ < text_.size() && text_[position_] == ' ') ++position_;
  }

  auto done() const noexcept -> bool { return position_ == text_.size(); }

 private:
  std::string_view text_;
  std::size_t position_{};
};

auto ServerClock::parse_http_date(std::string_view value)
    -> std::optional<std::int64_t> {
  Cursor cursor{value};
  if (cursor.word().size() != 3 || !cursor.expect(',')) return std::nullopt;
  cursor.spaces();
  const auto day = cursor.number(1, 2);
  cursor.spaces();
  const auto month_text = cursor.word();
  cursor.spaces();
  const auto year = cursor.number(4, 4);
  cursor.spaces();
  const auto hour = cursor.number(2, 2);
  if (!day || !year || !hour || !cursor.expect(':')) return std::nullopt;
  const auto minute = cursor.number(2, 2);
  if (!minute || !cursor.expect(':')) return std::nullopt;
  const auto second = cursor.number(2, 2);
  cursor.spaces();
  const auto zone = cursor.word();
  if (!second || !cursor.done() ||
      (!text::ieq(zone, "GMT") && !text::ieq(zone, "UTC")))
    return std::nullopt;
  const auto named =
      std::ranges::find_if(month_names, [month_text](std::string_view name) {
        return text::ieq(month_text, name);
      });
  const auto month = static_cast<unsigned>(
      std::ranges::distance(month_names.begin(), named) + 1);
  if (named == month_names.end() || *year < 1970 || *day < 1 || *day > 31 ||
      *hour > 23 || *minute > 59 || *second > 60)
    return std::nullopt;
  const std::chrono::year_month_day date{
      std::chrono::year{static_cast<int>(*year)}, std::chrono::month{month},
      std::chrono::day{static_cast<unsigned>(*day)}};
  if (!date.ok()) return std::nullopt;
  const std::int64_t days =
      std::chrono::sys_days{date}.time_since_epoch().count();
  return ((days * 24 + *hour) * 60 + *minute) * 60000LL + *second * 1000LL;
}

auto ServerClock::url_host(std::string_view url) -> std::string_view {
  const auto parsed = net::http::StrictUrl::read(
      url, net::http::StrictUrl::Strictness::request_target);
  if (!parsed || !text::ieq(parsed->scheme(), "https")) return {};
  return parsed->host();
}

auto ServerClock::is_trusted_host(std::string_view host) -> bool {
  return std::ranges::any_of(
      trusted_host_suffixes, [host](std::string_view suffix) {
        if (host.size() < suffix.size() ||
            !text::ieq(host.substr(host.size() - suffix.size()), suffix))
          return false;
        return host.size() == suffix.size() ||
               host[host.size() - suffix.size() - 1] == '.';
      });
}

void ServerClock::record(std::int64_t server_ms) noexcept {
  offset_ms_.store(server_ms - monotonic_ms(), std::memory_order_release);
}

void ServerClock::observe(const platform::HttpRequest& request,
                          const platform::HttpResponse& response) noexcept {
  const auto url = response.final_url.empty() ? request.url.view()
                                              : response.final_url.view();
  if (!is_trusted_host(url_host(url))) return;
  for (const auto& header : response.headers) {
    if (!text::ieq(header.name, "date")) continue;
    if (const auto parsed = parse_http_date(text::trim(header.value.view()))) {
      record(*parsed);
      return;
    }
  }
}

auto ServerClock::now_ms() const noexcept -> std::optional<std::int64_t> {
  const auto offset = offset_ms_.load(std::memory_order_acquire);
  if (offset == unset) return std::nullopt;
  return monotonic_ms() + offset;
}

void ServerClock::reset() noexcept {
  offset_ms_.store(unset, std::memory_order_release);
}

auto ServerClock::monotonic_ms() const noexcept -> std::int64_t {
  return static_cast<std::int64_t>(monotonic_.now_microseconds() / 1000U);
}

auto ServerClockHttp::perform(const platform::HttpRequest& request,
                              const platform::StopToken& stop)
    -> Expected<platform::HttpResponse, platform::HttpError> {
  auto response = inner_.perform(request, stop);
  if (response) clock_.observe(request, response.value());
  return response;
}

}  // namespace libchiaking::psn
