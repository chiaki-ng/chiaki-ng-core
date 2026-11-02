#include "src/core/psn/auth/authorization.hpp"

#include "src/core/utils/text/ascii.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace libchiaking::psn::auth {

auto Authorization::sendable(std::string_view url) noexcept -> bool {
  return !url.empty() && url.size() < url_limit &&
         std::ranges::all_of(url, text::ascii_visible) && !url.contains('#');
}

auto Authorization::request(const net::http::Endpoint& endpoint,
                            SecretString url, const Npsso& npsso,
                            std::string_view user_agent,
                            HeaderOrder order) const
    -> Expected<net::http::Response, Fault> {
  if (npsso.refusal()) return unexpected(Fault::invalid_npsso);
  if (!sendable(url.view())) return unexpected(Fault::invalid_request);
  net::http::Request request{endpoint, std::move(url)};
  if (order == HeaderOrder::user_agent_first && !user_agent.empty())
    request.header("User-Agent", user_agent);
  request.authorize(npsso);
  if (order == HeaderOrder::cookie_first && !user_agent.empty())
    request.header("User-Agent", user_agent);
  auto sent = client_->send(std::move(request));
  if (!sent) return unexpected(fault_of(sent.error()));
  return std::move(sent.value());
}

auto Authorization::fault_of(const net::http::Failure& failure) noexcept
    -> Fault {
  return failure.is_cancelled() ? Fault::cancelled : Fault::transport;
}

void Authorization::log(platform::LogSeverity severity, std::string_view label,
                        std::string_view message) const {
  client_->log(severity, std::format("{}{} {}", log_prefix, label, message));
}

void Authorization::report(std::string_view label, bool cancelled,
                           std::string_view failure, std::uint16_t http_status,
                           std::string_view detail) const {
  client_->log(
      cancelled ? platform::LogSeverity::info : platform::LogSeverity::warning,
      std::format("{}{} failed: {} HTTP {}{}", log_prefix, label, failure,
                  http_status, detail));
}

}  // namespace libchiaking::psn::auth
