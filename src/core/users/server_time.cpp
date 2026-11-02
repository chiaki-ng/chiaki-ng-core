#include "src/core/users/server_time.hpp"

#include "src/core/users/adapters/ps_user_core.hpp"

namespace libchiaking::users {

ServerTime::ServerTime(const platform::ClockProvider& monotonic) noexcept
    : clock_(monotonic) {}

auto ServerTime::of(PsUser& user) noexcept -> ServerTime& {
  return PsUser::Core::of(user).server_time();
}

auto ServerTime::now() const noexcept -> std::optional<Time> {
  const auto server_ms = clock_.now_ms();
  if (!server_ms) return std::nullopt;
  return Time{std::chrono::milliseconds{*server_ms}};
}

}  // namespace libchiaking::users
