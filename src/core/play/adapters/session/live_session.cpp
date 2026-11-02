#include "src/core/play/adapters/session/live_session.hpp"

#include <utility>

namespace libchiaking::play {

LiveSession::LiveSession(platform::ThreadingProvider& threading,
                         const platform::ClockProvider& clock)
    : threading_(threading),
      clock_(clock),
      starting_(threading),
      mutex_(threading) {}

auto LiveSession::one_start_at_a_time() -> platform::Lock {
  return platform::Lock{starting_};
}

auto LiveSession::busy() -> bool {
  Held ended;
  bool live = false;
  {
    const platform::Lock lock(mutex_);
    if (held_.session) {
      if (held_.session->state() != SessionState::ended ||
          held_.core->on_core_thread())
        live = true;
      else
        ended = std::exchange(held_, {});
    }
  }
  ended.session.reset();
  return live;
}

void LiveSession::hold(std::shared_ptr<Session> session, Session::Core& core) {
  const platform::Lock lock(mutex_);
  held_ = {std::move(session), &core};
}

auto LiveSession::current() const -> std::shared_ptr<Session> {
  const platform::Lock lock(mutex_);
  if (!held_.session || held_.session->state() == SessionState::ended)
    return nullptr;
  return held_.session;
}

auto LiveSession::playing() -> bool {
  const platform::Lock starting(starting_);
  return current() != nullptr;
}

void LiveSession::leave_as(AccountId account) {
  std::shared_ptr<Session> leaving;
  {
    const platform::Lock lock(mutex_);
    if (held_.session && held_.session->player().account_id() == account)
      leaving = held_.session;
  }
  if (leaving) static_cast<void>(leaving->leave());
}

void LiveSession::journal_in(
    std::shared_ptr<diagnostics::SessionJournal> journal) {
  const platform::Lock lock(mutex_);
  journal_ = std::move(journal);
}

auto LiveSession::journal() const
    -> std::shared_ptr<diagnostics::SessionJournal> {
  const platform::Lock lock(mutex_);
  return journal_;
}

auto LiveSession::journal_for_session()
    -> std::shared_ptr<diagnostics::SessionJournal> {
  if (auto shared = journal()) return shared;
  return std::make_shared<diagnostics::SessionJournal>(
      threading_, clock_,
      attempts_.fetch_add(1, std::memory_order_relaxed) + 1);
}

}  // namespace libchiaking::play
