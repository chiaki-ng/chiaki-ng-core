#include "src/core/catalog/adapters/fetch_board.hpp"

#include "src/core/catalog/model/fetch_lifecycle.hpp"

#include <algorithm>
#include <exception>
#include <utility>

namespace libchiaking::catalog {

struct FetchBoard::Machine {
  fetch_lifecycle::LifecycleContext context{};
  fetch_lifecycle::Lifecycle lifecycle{context};
};

FetchBoard::Watch::Registration::Registration(FetchBoard& board)
    : board_(board), id_(board.next_id()) {}

FetchBoard::Watch::Registration::~Registration() { board_.unwatch(id_); }

FetchBoard::Watch::Watch(FetchBoard& board, const FetchObserver& observer)
    : registration_(board) {
  board.enrol(registration_.id(), observer);
}

FetchBoard::Turn::Turn(FetchBoard& board, platform::Lock& lock, Reentry reentry)
    : board_(board),
      lock_(lock),
      outermost_(board.teller_ != platform::ThreadIdentity::current()) {
  if (!outermost_ && reentry == Reentry::refused) std::terminate();
  if (!outermost_) return;
  board_.turn_ended_.wait(lock_, [this] { return !board_.teller_; });
  board_.teller_ = platform::ThreadIdentity::current();
}

FetchBoard::Turn::~Turn() {
  if (!outermost_) return;
  if (!lock_.owns_lock()) lock_.lock();
  board_.teller_ = platform::ThreadIdentity{};
  board_.turn_ended_.notify_all();
}

void FetchBoard::Turn::tell_queued() {
  if (!outermost_) return;
  auto& queued = board_.queued_;
  while (!queued.empty()) {
    auto next = std::move(queued.front());
    queued.pop_front();
    lock_.unlock();
    {
      const platform::Lock latest{board_.latest_};
      board_.state_ = next.state;
    }
    if (next.observer != nullptr) (*next.observer)(next.state);
    lock_.lock();
  }
}

FetchBoard::FetchBoard(platform::ThreadingProvider& threading,
                       const platform::ClockProvider& clock)
    : delivery_(threading),
      turn_ended_(threading, clock),
      latest_(threading),
      machine_(std::make_unique<Machine>()) {}

FetchBoard::~FetchBoard() = default;

auto FetchBoard::state() const -> FetchState {
  const platform::Lock lock{latest_};
  return state_;
}

auto FetchBoard::watch(const FetchObserver& observer) -> Watch {
  return Watch{*this, observer};
}

auto FetchBoard::next_id() -> std::uint64_t {
  const platform::Lock lock{delivery_};
  return next_watch_++;
}

void FetchBoard::enrol(std::uint64_t id, const FetchObserver& observer) {
  platform::Lock lock{delivery_};
  Turn turn{*this, lock, Reentry::refused};
  auto& watcher = watchers_.emplace_back(Watcher{id, &observer});
  if (const auto now = machine_->lifecycle.snapshot(); now.is_running())
    tell(watcher, now);
  turn.tell_queued();
}

auto FetchBoard::begin(const Watch& leader) -> std::optional<Begun> {
  platform::Lock lock{delivery_};
  Turn turn{*this, lock, Reentry::refused};
  const auto attempt = machine_->context.attempt + 1;
  if (!machine_->lifecycle.process(fetch_lifecycle::Start{attempt}))
    return std::nullopt;
  leader_ = leader.registration_.id();
  for (auto& watcher : watchers_)
    if (watcher.id == leader.registration_.id()) watcher.following = true;
  const Begun begun{attempt, epoch_, std::exchange(start_over_pending_, false)};
  deliver();
  turn.tell_queued();
  return begun;
}

void FetchBoard::follow_again(const Watch& watch) {
  platform::Lock lock{delivery_};
  Turn turn{*this, lock, Reentry::refused};
  for (auto& watcher : watchers_) {
    if (watcher.id != watch.registration_.id()) continue;
    watcher.following = true;
    if (const auto now = machine_->lifecycle.snapshot(); now.is_running())
      tell(watcher, now);
  }
  turn.tell_queued();
}

void FetchBoard::catch_up(const Watch& watch, std::uint64_t attempt) {
  platform::Lock lock{delivery_};
  Turn turn{*this, lock, Reentry::refused};
  const auto now = machine_->lifecycle.snapshot();
  if (now.attempt() != attempt || !ends_its_attempt(now)) return;
  for (auto& watcher : watchers_)
    if (watcher.id == watch.registration_.id() && watcher.told != attempt)
      tell(watcher, now);
  turn.tell_queued();
}

auto FetchBoard::epoch() const -> std::uint64_t {
  const platform::Lock lock{delivery_};
  return epoch_;
}

void FetchBoard::began(std::uint64_t attempt, FetchStep step,
                       std::optional<FetchActivity> activity) {
  feed(fetch_lifecycle::StepBegan{attempt, step, activity});
}

void FetchBoard::doing(std::uint64_t attempt, FetchActivity activity) {
  feed(fetch_lifecycle::ActivityChanged{attempt, activity});
}

void FetchBoard::pages(std::uint64_t attempt, std::uint32_t read,
                       std::optional<std::uint32_t> expected) {
  feed(fetch_lifecycle::PagesRead{attempt, read, expected});
}

void FetchBoard::asking(std::uint64_t attempt, std::string_view host) {
  feed(fetch_lifecycle::TimeServerAsked{attempt, host});
}

void FetchBoard::ended(std::uint64_t attempt, StepOutcome outcome) {
  feed(fetch_lifecycle::StepEnded{attempt, outcome});
}

void FetchBoard::failed(std::uint64_t attempt, const Unanswered& why) {
  if (why.is_cancelled())
    feed(fetch_lifecycle::Cancelled{attempt});
  else
    feed(fetch_lifecycle::Failed{attempt, why});
}

void FetchBoard::finished(std::uint64_t attempt) {
  feed(fetch_lifecycle::Finished{attempt});
}

void FetchBoard::start_over() {
  platform::Lock lock{delivery_};
  Turn turn{*this, lock, Reentry::allowed};
  ++epoch_;
  if (machine_->context.active_attempt_id) {
    start_over_pending_ = true;
    return;
  }
  if (machine_->lifecycle.step() == FetchStep::idle) return;
  if (!machine_->lifecycle.process(fetch_lifecycle::Reset{})) return;
  deliver();
  turn.tell_queued();
}

template <class Event>
void FetchBoard::feed(const Event& event) {
  platform::Lock lock{delivery_};
  Turn turn{*this, lock, Reentry::allowed};
  if (!machine_->lifecycle.process(event)) return;
  deliver();
  turn.tell_queued();
}

void FetchBoard::deliver() {
  auto& queued = machine_->context.effects;
  while (!queued.empty()) {
    auto published = std::move(queued.front().state);
    queued.pop_front();
    queued_.push_back(Telling{0, nullptr, published});
    const bool cancelled = published.step() == FetchStep::cancelled;
    for (auto& watcher : watchers_) {
      if (!watcher.following) continue;
      if (cancelled && leader_ != watcher.id) continue;
      tell(watcher, published);
      if (ends_its_attempt(published)) watcher.following = false;
    }
    if (!published.is_running()) leader_.reset();
  }
}

void FetchBoard::tell(Watcher& watcher, const FetchState& state) {
  watcher.told = state.attempt();
  if (*watcher.observer)
    queued_.push_back(Telling{watcher.id, watcher.observer, state});
}

auto FetchBoard::ends_its_attempt(const FetchState& state) noexcept -> bool {
  return state.step() == FetchStep::ready ||
         state.step() == FetchStep::unanswered ||
         state.step() == FetchStep::cancelled;
}

void FetchBoard::unwatch(std::uint64_t id) {
  platform::Lock lock{delivery_};
  const Turn turn{*this, lock, Reentry::allowed};
  std::erase_if(watchers_,
                [id](const Watcher& watcher) { return watcher.id == id; });
  std::erase_if(queued_,
                [id](const Telling& queued) { return queued.watch == id; });
}

}  // namespace libchiaking::catalog
