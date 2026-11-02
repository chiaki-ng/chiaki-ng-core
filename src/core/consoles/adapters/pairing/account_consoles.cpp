#include "src/core/consoles/adapters/pairing/account_consoles.hpp"

#include "libchiaking/detail/consoles/device_uid.hpp"
#include "src/core/net/transport/http/client.hpp"
#include "src/core/users/ports/console_calls.hpp"

#include <algorithm>
#include <iterator>
#include <string>
#include <utility>

namespace libchiaking::consoles {

AccountConsoles::AccountConsoles(platform::HttpProvider& http,
                                 const platform::ClockProvider& clock,
                                 platform::DiagnosticsProvider* log,
                                 std::size_t maximum_response_bytes) noexcept
    : http_(http),
      clock_(clock),
      log_(log),
      maximum_response_bytes_(maximum_response_bytes) {}

auto AccountConsoles::of(users::PsUser& user, Generation generation,
                         const platform::StopToken& stop)
    -> Expected<Listed, users::Unanswered> {
  using Reason = users::Unanswered::Reason;
  if (user.is_off_psn())
    return unexpected(users::Unanswered{Reason::signed_out});
  if (generation == Generation::ps4)
    return Listed{{AccountConsole::main_ps4()}, 0};
  net::http::Client http{http_, clock_, stop, log_};
  users::ConsoleCalls calls{user};
  auto listed = paged(http, calls);
  if (!listed) return unexpected(listed.error());
  http.log(
      platform::LogSeverity::info,
      "account consoles: " + std::to_string(listed.value().consoles.size()) +
          " listed, " + std::to_string(listed.value().skipped) + " skipped");
  return listed;
}

auto AccountConsoles::paged(net::http::Client& http, users::ConsoleCalls& calls)
    -> Expected<Listed, users::Unanswered> {
  Listed listed;
  std::size_t offset = 0;
  for (int page = 0; page < page_limit; ++page) {
    auto reply = calls.account_consoles(http, offset, maximum_response_bytes_);
    if (!reply) return unexpected(reply.error());
    const auto received = reply.value().received;
    auto part = translated(std::move(reply.value()));
    listed.skipped += part.skipped;
    std::ranges::move(part.consoles, std::back_inserter(listed.consoles));
    if (received < page_size) return listed;
    offset += received;
  }
  http.log(platform::LogSeverity::warning,
           "account consoles: page bound reached");
  return listed;
}

auto AccountConsoles::translated(psn::rest::ClientsPage page) -> Listed {
  Listed listed;
  listed.skipped = page.unaddressable;
  for (auto& console : page.consoles) {
    if (!console.device_uid) continue;
    const auto device_uid = DeviceUid::from_bytes(*console.device_uid);
    auto translated = device_uid
                          ? AccountConsole::create(
                                *device_uid, std::move(console.name),
                                Generation::ps5, console.remote_play_enabled)
                          : std::nullopt;
    if (!translated) {
      ++listed.skipped;
      continue;
    }
    listed.consoles.push_back(std::move(*translated));
  }
  return listed;
}

}  // namespace libchiaking::consoles
