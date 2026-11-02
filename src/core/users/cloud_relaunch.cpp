#include "src/core/users/cloud_relaunch.hpp"

#include "src/core/users/adapters/ps_user_core.hpp"

namespace libchiaking::users {

CloudRelaunch::CloudRelaunch(platform::ThreadingProvider& threading,
                             const platform::ClockProvider& clock)
    : clock_(clock), mutex_(threading) {}

auto CloudRelaunch::of(PsUser& user) noexcept -> CloudRelaunch& {
  return PsUser::Core::of(user).cloud_relaunch();
}

void CloudRelaunch::ended() {
  platform::Lock lock{mutex_};
  ended_us_ = clock_.now_microseconds();
}

auto CloudRelaunch::remaining() const -> std::chrono::milliseconds {
  using std::chrono::microseconds;
  using std::chrono::milliseconds;
  platform::Lock lock{mutex_};
  if (!ended_us_) return milliseconds{0};
  const auto now = clock_.now_microseconds();
  const auto since = microseconds{
      now > *ended_us_ ? static_cast<std::int64_t>(now - *ended_us_) : 0};
  if (since >= settle_time) return milliseconds{0};
  return std::chrono::ceil<milliseconds>(settle_time - since);
}

}  // namespace libchiaking::users
