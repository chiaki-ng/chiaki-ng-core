#include "src/core/consoles/adapters/search/shared_search.hpp"

#include <utility>

namespace libchiaking::consoles {

SharedSearch::Seat::Seat(SharedSearch& shared,
                         std::shared_ptr<NetworkSearch> search) noexcept
    : shared_(shared), search_(std::move(search)) {}

SharedSearch::Seat::~Seat() { shared_.leave(); }

auto SharedSearch::Seat::turn(const Nearby& nearby) -> Turn {
  Turn turn;
  platform::Lock lock{shared_.turn_};
  if (shared_.stepping_) {
    static_cast<void>(
        shared_.stepped_.wait_for(lock, NetworkSearch::poll_step));
  } else {
    shared_.stepping_ = true;
    lock.unlock();
    turn.stepped = shared_.step(*search_, nearby);
    lock.lock();
    shared_.stepping_ = false;
    shared_.stepped_.notify_all();
  }
  if (shared_.view_revision_ != delivered_) {
    delivered_ = shared_.view_revision_;
    turn.fresh = shared_.view_;
  }
  return turn;
}

SharedSearch::SharedSearch(platform::NetworkProvider& network,
                           platform::ThreadingProvider& threading,
                           const platform::ClockProvider& clock,
                           platform::DiagnosticsProvider* log)
    : network_(network),
      threading_(threading),
      clock_(clock),
      log_(log),
      mutex_(threading),
      turn_(threading),
      stepped_(threading, clock) {}

auto SharedSearch::join(const SearchRanges& ranges)
    -> Expected<std::shared_ptr<NetworkSearch>, SearchFailure> {
  platform::Lock lock{mutex_};
  if (search_) {
    if (search_->ranges() != ranges) return unexpected(SearchFailure::busy);
    ++searchers_;
    return search_;
  }
  auto search = std::make_shared<NetworkSearch>(network_, threading_, clock_,
                                                ranges, log_);
  if (!search->start()) return unexpected(SearchFailure::network);
  platform::Lock turn{turn_};
  view_ = NetworkView{};
  view_revision_ = 0;
  seen_revision_ = 0;
  stepping_ = false;
  search_ = search;
  searchers_ = 1;
  return search;
}

void SharedSearch::leave() {
  std::shared_ptr<NetworkSearch> ended;
  {
    platform::Lock lock{mutex_};
    if (--searchers_ == 0) ended = std::move(search_);
  }
  if (!ended) return;
  platform::Lock turn{turn_};
  (void)ended->stop();
}

auto SharedSearch::step(NetworkSearch& search, const Nearby& nearby)
    -> Expected<void, SearchFailure> {
  if (!search.poll()) return unexpected(SearchFailure::network);
  const auto progress = search.progress();
  if (!progress) return unexpected(SearchFailure::network);
  if (view_revision_ == 0 || search.revision() != seen_revision_ ||
      progress->targets != view_.targets ||
      progress->visited != view_.visited ||
      progress->network_available != view_.network_available ||
      progress->permission_denied != view_.permission_denied ||
      progress->broadcast_ranges != view_.broadcast_ranges ||
      progress->unicast_ranges != view_.unicast_ranges)
    look_again(search, nearby);
  if (progress->permission_denied)
    return unexpected(SearchFailure::permission_denied);
  return {};
}

void SharedSearch::look_again(const NetworkSearch& search,
                              const Nearby& nearby) {
  const auto progress = search.progress();
  NetworkView view;
  if (progress) {
    view.targets = progress->targets;
    view.visited = progress->visited;
    view.network_available = progress->network_available;
    view.permission_denied = progress->permission_denied;
    view.broadcast_ranges = progress->broadcast_ranges;
    view.unicast_ranges = progress->unicast_ranges;
  }
  view.nearby = nearby(search);
  seen_revision_ = search.revision();
  platform::Lock turn{turn_};
  view_ = std::move(view);
  ++view_revision_;
}

}  // namespace libchiaking::consoles
