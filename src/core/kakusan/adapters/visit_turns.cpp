#include "libchiaking/testing/kakusan/visit_turns.hpp"

#include <utility>

namespace libchiaking::kakusan::testing {

VisitTurns::VisitTurns(platform::ThreadingProvider& threading,
                       const platform::ClockProvider& clock)
    : threading_(&threading), mutex_(threading), changed_(threading, clock) {}

VisitTurns::~VisitTurns() { static_cast<void>(finish()); }

void VisitTurns::wait(std::chrono::milliseconds) {
  platform::Lock lock{mutex_};
  if (released_) return;
  parked_ = true;
  changed_.notify_all();
  changed_.wait(lock, [this] { return turns_ != 0 || released_; });
  if (turns_ != 0) --turns_;
  parked_ = false;
}

void VisitTurns::wake() noexcept {}

void VisitTurns::run(GuestVisit& visit) {
  platform::Lock lock{mutex_};
  if (thread_.joinable()) return;
  thread_ = platform::Thread::start(
                *threading_, "kakusan-visit-turns", platform::ThreadRole::session,
                [this, &visit] {
                  auto outcome = visit.run();
                  {
                    const platform::Lock done{mutex_};
                    outcome_.emplace(std::move(outcome));
                    over_ = true;
                  }
                  changed_.notify_all();
                })
                .value();
  changed_.wait(lock, [this] { return parked_ || over_ || released_; });
}

auto VisitTurns::turn() -> bool {
  platform::Lock lock{mutex_};
  if (!thread_.joinable() || released_) return false;
  changed_.wait(lock, [this] { return parked_ || over_; });
  if (over_) return false;
  parked_ = false;
  ++turns_;
  changed_.notify_all();
  changed_.wait(lock, [this] { return parked_ || over_; });
  return !over_;
}

auto VisitTurns::ended() const -> bool {
  const platform::Lock lock{mutex_};
  return over_;
}

auto VisitTurns::finish() -> std::optional<VisitOutcome> {
  platform::Thread running;
  {
    const platform::Lock lock{mutex_};
    released_ = true;
    running = std::move(thread_);
  }
  changed_.notify_all();
  if (running.joinable()) static_cast<void>(running.join());
  const platform::Lock lock{mutex_};
  return outcome_;
}

}  // namespace libchiaking::kakusan::testing
