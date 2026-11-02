#include "src/core/play/adapters/session/prompts/login_pin_core.hpp"

#include "src/core/play/adapters/links/link_driver.hpp"

#include <string>
#include <utility>

namespace libchiaking::play {

LoginPin::Core::Core(platform::ThreadingProvider& threading,
                     const platform::ClockProvider& clock,
                     const CommandRoute& route)
    : route_(route), mutex_(threading), changed_(threading, clock) {}

LoginPin::Core::~Core() {
  platform::Lock lock(mutex_);
  closed_ = true;
  changed_.notify_all();
  changed_.wait(lock, [this] { return awaiting_ == 0; });
}

auto LoginPin::Core::submit(AccessCode code) -> Expected<Ticket, InputRefusal> {
  std::uint64_t id = 0;
  {
    const platform::Lock lock(mutex_);
    if (const auto refusal = route_.refusal()) return unexpected(*refusal);
    code_ = std::move(code);
    state_ = State::queued;
    id = ++generation_;
    ++submissions_;
  }
  route_.wake();
  return Ticket{id};
}

auto LoginPin::Core::progress(Ticket ticket) const -> Progress {
  const platform::Lock lock(mutex_);
  if (ticket.id() != generation_ || state_ == State::failed ||
      state_ == State::idle)
    return Progress::rejected;
  if (state_ == State::accepted) return Progress::accepted;
  return closed_ || !route_.accepting() ? Progress::rejected
                                        : Progress::pending;
}

auto LoginPin::Core::await(std::chrono::milliseconds timeout) -> Verdict {
  platform::Lock lock(mutex_);
  if (submissions_ == 0) return Verdict::accepted;
  ++awaiting_;
  static_cast<void>(changed_.wait_for(lock, timeout, [this] {
    return state_ != State::queued || closed_ || !route_.accepting();
  }));
  if (state_ == State::queued) resolve(false);
  const auto verdict =
      state_ == State::accepted ? Verdict::accepted : Verdict::rejected;
  --submissions_;
  --awaiting_;
  changed_.notify_all();
  return verdict;
}

auto LoginPin::Core::queued() const -> bool {
  const platform::Lock lock(mutex_);
  return state_ == State::queued;
}

void LoginPin::Core::resolve(bool accepted) noexcept {
  code_.reset();
  state_ = accepted ? State::accepted : State::failed;
  changed_.notify_all();
}

auto LoginPin::Core::flush()
    -> Expected<void, net::protocol::remote_play::StreamError> {
  std::optional<AccessCode> code;
  std::uint64_t generation = 0;
  {
    const platform::Lock lock(mutex_);
    if (state_ != State::queued || !code_ || !route_.accepting()) return {};
    code = code_->clone();
    generation = generation_;
  }
  const auto result = route_.driver().submit_pin(code->bytes());
  bool failed = false;
  {
    const platform::Lock lock(mutex_);
    if (state_ == State::queued && generation_ == generation) {
      failed = !result;
      resolve(!failed);
    }
  }
  if (failed)
    route_.log(platform::LogSeverity::warning,
               "login code submission failed: result=" +
                   std::string{CommandRoute::result_name(result.error())});
  return result;
}

void LoginPin::Core::interrupt() noexcept {
  const platform::Lock lock(mutex_);
  changed_.notify_all();
}

auto LoginPin::submit(AccessCode code) -> Expected<Ticket, InputRefusal> {
  return core_->submit(std::move(code));
}

auto LoginPin::progress(Ticket ticket) const -> Progress {
  return core_->progress(ticket);
}

auto LoginPin::await(std::chrono::milliseconds timeout) -> Verdict {
  return core_->await(timeout);
}

}  // namespace libchiaking::play
