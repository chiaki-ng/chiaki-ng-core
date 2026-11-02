/// @file
/// @brief Declares SharedFlight, which lets concurrent callers asking the same
/// question share one request instead of each sending their own.
///
/// The users context uses it so that, for example, several screens asking for
/// a user's membership at once cause one call to PSN, all of them receiving
/// its answer.
#pragma once

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace libchiaking::concurrency {

/// @brief Collapses concurrent identical questions into one request whose
/// reply every asker receives.
///
/// Questions are keyed by a string. The first caller to ask a question that
/// is not already in flight becomes its leader and sends it; callers asking
/// the same question while it is in flight wait for the leader's reply and
/// receive a copy of it. Nothing is remembered once a flight lands: the next
/// caller to ask starts a new flight. Different questions never wait on each
/// other.
///
/// A leader's cancelled reply is not shared: waiters whose own stop token has
/// not fired ask again, one of them becoming the next leader. A leader that
/// throws lands with no reply, and its waiters likewise ask again.
///
/// The object is safe to share between threads; one internal mutex guards
/// it, and it is never held while a request is sent. Waiting is not fair:
/// whichever waiter wakes first after a cancelled flight leads the next one.
/// It must outlive every call in progress.
///
/// @tparam Answer The successful answer; must be copyable, since every
///     waiter receives its own copy.
/// @tparam Refusal The failure; must be copyable and provide a static
///     `cancelled()` making the cancellation failure and a member
///     `is_cancelled()` recognising it.
template <class Answer, class Refusal>
class SharedFlight {
 public:
  /// @brief The reply a question receives: the answer or the refusal.
  using Reply = Expected<Answer, Refusal>;

  /// @brief Makes a set of flights with nothing in the air.
  ///
  /// @param threading Provider of the internal mutex and condition variable;
  ///     must outlive the object.
  /// @param clock Monotonic clock the condition variable measures on; must
  ///     outlive the object.
  SharedFlight(platform::ThreadingProvider& threading,
               const platform::ClockProvider& clock)
      : mutex_(threading), landed_(threading, clock) {}

  /// @brief Answers a question, sending it only when no identical question
  /// is already in flight.
  ///
  /// When `question` is in flight, blocks until that flight lands and
  /// returns a copy of its reply, or a cancellation once `stop` fires.
  /// Otherwise runs `send` on the calling thread, without the internal lock
  /// held, and shares its reply with every caller that started waiting on
  /// it meanwhile. Blocks for as long as the leading `send` takes.
  ///
  /// May be called from any thread, and reentrantly for a different
  /// question; `send` asking the same question again waits on itself
  /// until its stop token fires.
  ///
  /// @tparam Send A callable taking no arguments and returning Reply.
  /// @param question The key identifying the question; callers with equal
  ///     keys share one flight.
  /// @param stop Cancels this caller's wait. It does not reach into the
  ///     `send` of another caller; a leader's `send` should observe a stop
  ///     token of its own.
  /// @param send Sends the question when this caller leads; not called
  ///     when an in-flight reply is shared.
  /// @return The reply of `send`, either this caller's or the shared one
  ///     of the flight it waited on.
  /// @retval Refusal::cancelled() `stop` fired before this caller received
  ///     a reply; the flight it waited on, if any, carries on.
  /// @throws Whatever `send` throws, when this caller leads.
  template <class Send>
  [[nodiscard]] auto answer(std::string_view question,
                            const platform::StopToken& stop, const Send& send)
      -> Reply {
    platform::Lock lock{mutex_};
    for (auto found = flights_.find(question); found != flights_.end();
         found = flights_.find(question)) {
      const auto flight = found->second;
      ++flight->waiting;
      if (!landed_.wait(lock, stop, [&] { return flight->landed; }))
        return unexpected(Refusal::cancelled());
      if (flight->reply && (flight->reply->has_value() ||
                            !flight->reply->error().is_cancelled()))
        return *flight->reply;
    }
    if (stop.stop_requested()) return unexpected(Refusal::cancelled());
    Departure departure{*this, question, lock};
    Reply reply = send();
    departure.land(reply);
    return reply;
  }

 private:
  /// @brief One question in flight, shared by its leader and its waiters.
  ///
  /// Guarded by the owning SharedFlight's mutex. Held through a shared
  /// pointer so that waiters can still read it after the leader has erased
  /// it from the map.
  struct Flight {
    /// @brief The leader's reply, kept only when someone waited for it;
    /// empty until landing, and when the leader threw.
    std::optional<Reply> reply;
    /// @brief How many callers have waited on this flight; only whether
    /// it is zero matters, deciding whether the reply is kept.
    std::size_t waiting{};
    /// @brief Whether the leader has finished, with a reply or by
    /// throwing.
    bool landed{};
  };

  /// @brief The flights in progress, keyed by question, with
  /// heterogeneous lookup by string view.
  using Flights = std::map<std::string, std::shared_ptr<Flight>, std::less<>>;

  /// @brief The leader's hold on its flight: registers it on construction,
  /// and lands and removes it on destruction, also when `send` throws.
  ///
  /// Lives on the leader's stack for the duration of one answer() call.
  class Departure {
   public:
    /// @brief Registers a new flight for a question and releases the
    /// lock so that `send` runs unlocked.
    ///
    /// @param flights The owner of the flight map.
    /// @param question The question being sent; must not already be in
    ///     flight.
    /// @param lock The caller's lock on the owner's mutex; held on entry,
    ///     released on return.
    Departure(SharedFlight& flights, std::string_view question,
              platform::Lock& lock)
        : flights_(flights),
          lock_(lock),
          entry_(flights.flights_
                     .emplace(std::string{question}, std::make_shared<Flight>())
                     .first) {
      lock_.unlock();
    }

    Departure(const Departure&) = delete;
    auto operator=(const Departure&) -> Departure& = delete;

    /// @brief Marks the flight landed, removes it from the map and wakes
    /// every waiter, taking the lock first when it is not held.
    ~Departure() {
      if (!lock_.owns_lock()) lock_.lock();
      entry_->second->landed = true;
      flights_.flights_.erase(entry_);
      flights_.landed_.notify_all();
    }

    /// @brief Takes the lock again and records the reply for the waiters,
    /// when there are any.
    ///
    /// Blocks until the owner's mutex is free. The lock stays held until
    /// the Departure is destroyed.
    ///
    /// @param reply The leader's reply.
    void land(const Reply& reply) {
      lock_.lock();
      if (entry_->second->waiting != 0) entry_->second->reply = reply;
    }

   private:
    /// @brief The owner whose flight map this departure is in.
    SharedFlight& flights_;
    /// @brief The leader's lock on the owner's mutex.
    platform::Lock& lock_;
    /// @brief This flight's entry in the owner's map.
    typename Flights::iterator entry_;
  };

  /// @brief Guards flights_ and every Flight in it.
  platform::Mutex mutex_;
  /// @brief Signalled whenever a flight lands.
  platform::ConditionVariable landed_;
  /// @brief The questions currently in flight.
  Flights flights_;
};

}  // namespace libchiaking::concurrency
