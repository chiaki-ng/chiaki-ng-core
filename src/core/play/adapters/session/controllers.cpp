#include "src/core/play/adapters/session/controllers_core.hpp"

#include "src/core/net/protocol/remote_play/stream/controller_delivery.hpp"
#include "src/core/play/adapters/links/link_driver.hpp"

#include <algorithm>
#include <limits>
#include <string>

namespace libchiaking::play {

static_assert(controllers::ControllerState::button_mask ==
              net::protocol::remote_play::controller_button_mask);
static_assert(controllers::ControllerState::button_count ==
              net::protocol::remote_play::controller_button_count);
static_assert(Controllers::default_depth >= Controllers::Core::queue_depth);

auto Controllers::Core::wire(controllers::SeatNumber seat,
                             const controllers::ControllerState& state) noexcept
    -> net::protocol::remote_play::ControllerState {
  net::protocol::remote_play::ControllerState wired;
  wired.controller_id = seat.index();
  wired.buttons = state.buttons;
  wired.l2 = state.l2;
  wired.r2 = state.r2;
  wired.left_x = state.left_x;
  wired.left_y = state.left_y;
  wired.right_x = state.right_x;
  wired.right_y = state.right_y;
  std::ranges::transform(state.touches, wired.touches.begin(),
                         [](const controllers::ControllerTouch& touch) {
                           return net::protocol::remote_play::ControllerTouch{
                               touch.id, touch.x, touch.y};
                         });
  wired.gyro_x = state.gyro_x;
  wired.gyro_y = state.gyro_y;
  wired.gyro_z = state.gyro_z;
  wired.accel_x = state.accel_x;
  wired.accel_y = state.accel_y;
  wired.accel_z = state.accel_z;
  wired.orient_x = state.orient_x;
  wired.orient_y = state.orient_y;
  wired.orient_z = state.orient_z;
  wired.orient_w = state.orient_w;
  wired.trigger_states = state.trigger_states;
  return wired;
}

auto Controllers::Core::typed(
    const net::protocol::remote_play::ControllerState& state) noexcept
    -> controllers::ControllerState {
  controllers::ControllerState plain;
  plain.buttons = state.buttons;
  plain.l2 = state.l2;
  plain.r2 = state.r2;
  plain.left_x = state.left_x;
  plain.left_y = state.left_y;
  plain.right_x = state.right_x;
  plain.right_y = state.right_y;
  std::ranges::transform(
      state.touches, plain.touches.begin(),
      [](const net::protocol::remote_play::ControllerTouch& touch) {
        return controllers::ControllerTouch{touch.id, touch.x, touch.y};
      });
  plain.gyro_x = state.gyro_x;
  plain.gyro_y = state.gyro_y;
  plain.gyro_z = state.gyro_z;
  plain.accel_x = state.accel_x;
  plain.accel_y = state.accel_y;
  plain.accel_z = state.accel_z;
  plain.orient_x = state.orient_x;
  plain.orient_y = state.orient_y;
  plain.orient_z = state.orient_z;
  plain.orient_w = state.orient_w;
  plain.trigger_states = state.trigger_states;
  return plain;
}

static_assert(controllers::SeatNumber::count == 4);

Controllers::Core::SeatQueue::SeatQueue(std::size_t depth)
    : depth_(std::max<std::size_t>(depth, 1)),
      older_(std::max<std::size_t>(depth_, 2) - 1) {}

void Controllers::Core::SeatQueue::push(
    const controllers::ControllerState& state) {
  if (!newest_) {
    newest_ = state;
    return;
  }
  if (same_sample(*newest_, state)) {
    return;
  }
  const bool replace = !older_.empty() && same_edges(*newest_, state);
  if (!replace && size() < depth_) {
    static_cast<void>(older_.try_push(*newest_));
  }
  newest_ = state;
}

auto Controllers::Core::SeatQueue::front()
    -> std::optional<controllers::ControllerState> {
  if (const auto* oldest = older_.front()) {
    return *oldest;
  }
  return newest_;
}

void Controllers::Core::SeatQueue::pop() noexcept {
  if (older_.front() != nullptr) {
    older_.pop();
    return;
  }
  newest_.reset();
}

void Controllers::Core::SeatQueue::clear() noexcept {
  while (older_.front() != nullptr) {
    older_.pop();
  }
  newest_.reset();
}

auto Controllers::Core::SeatQueue::size() const noexcept -> std::size_t {
  return older_.size() + (newest_ ? 1U : 0U);
}

auto Controllers::Core::SeatQueue::empty() const noexcept -> bool {
  return !newest_;
}

Controllers::Core::Core(platform::ThreadingProvider& threading,
                        const CommandRoute& route, std::size_t depth,
                        Waking waking)
    : route_(route),
      depth_(std::max<std::size_t>(depth, 1)),
      waking_(waking),
      mutex_(threading),
      queues_{Queue{.samples = SeatQueue{depth_}},
              Queue{.samples = SeatQueue{depth_}},
              Queue{.samples = SeatQueue{depth_}},
              Queue{.samples = SeatQueue{depth_}}} {}

auto Controllers::Core::same_edges(
    const controllers::ControllerState& a,
    const controllers::ControllerState& b) noexcept -> bool {
  return a.buttons == b.buttons && a.touches[0].id == b.touches[0].id &&
         a.touches[1].id == b.touches[1].id;
}

auto Controllers::Core::same_sample(
    const controllers::ControllerState& a,
    const controllers::ControllerState& b) noexcept -> bool {
  const auto same_touch = [](const controllers::ControllerTouch& left,
                             const controllers::ControllerTouch& right) {
    return left.id == right.id && left.x == right.x && left.y == right.y;
  };
  return a.buttons == b.buttons && a.l2 == b.l2 && a.r2 == b.r2 &&
         a.left_x == b.left_x && a.left_y == b.left_y &&
         a.right_x == b.right_x && a.right_y == b.right_y &&
         std::ranges::equal(a.touches, b.touches, same_touch) &&
         a.gyro_x == b.gyro_x && a.gyro_y == b.gyro_y && a.gyro_z == b.gyro_z &&
         a.accel_x == b.accel_x && a.accel_y == b.accel_y &&
         a.accel_z == b.accel_z && a.orient_x == b.orient_x &&
         a.orient_y == b.orient_y && a.orient_z == b.orient_z &&
         a.orient_w == b.orient_w && a.trigger_states == b.trigger_states;
}

auto Controllers::Core::worth_waking(
    controllers::SeatNumber seat,
    const std::optional<controllers::ControllerState>& woken,
    const controllers::ControllerState& state) noexcept -> bool {
  return !woken || !net::protocol::remote_play::ControllerSample::same_feedback(
                       wire(seat, *woken), wire(seat, state),
                       std::numeric_limits<float>::infinity());
}

auto Controllers::Core::send(controllers::SeatNumber seat,
                             const controllers::ControllerState& state)
    -> Expected<void, InputRefusal> {
  if ((state.buttons & ~controllers::ControllerState::button_mask) != 0)
    return unexpected(InputRefusal::invalid);
  if (const auto refusal = route_.refusal()) return unexpected(*refusal);
  bool wake = false;
  {
    const platform::Lock lock(mutex_);
    auto& queue = queues_[seat.index()];
    if (queue.diverted) return unexpected(InputRefusal::not_owner);
    queue.samples.push(state);
    wake = waking_ == Waking::every_sample ||
           worth_waking(seat, queue.woken, state);
    if (wake) queue.woken = state;
  }
  if (wake) route_.wake();
  return {};
}

auto Controllers::Core::forward(controllers::SeatNumber seat,
                                const controllers::ControllerState& state)
    -> Expected<void, InputRefusal> {
  if (!seat.is_added() ||
      (state.buttons & ~controllers::ControllerState::button_mask) != 0)
    return unexpected(InputRefusal::invalid);
  if (const auto refusal = route_.refusal()) return unexpected(*refusal);
  if (!diverted(seat)) {
    return unexpected(InputRefusal::not_owner);
  }
  const auto result = route_.driver().send_controller_state(wire(seat, state));
  Settled settled;
  {
    const platform::Lock lock(mutex_);
    settled = settle(seat, state, result, std::nullopt);
  }
  follow_up(seat, state, result, settled, true);
  return {};
}

void Controllers::Core::clear(Queue& queue) {
  queue.samples.clear();
  ++queue.generation;
  queue.woken.reset();
  queue.retries = 0;
  queue.reported = false;
  queue.refusal.reset();
}

void Controllers::Core::discard_pending(controllers::SeatNumber seat) {
  const platform::Lock lock(mutex_);
  clear(queues_[seat.index()]);
}

auto Controllers::Core::divert(controllers::SeatNumber seat) -> bool {
  if (!seat.is_added()) return false;
  const platform::Lock lock(mutex_);
  auto& queue = queues_[seat.index()];
  if (queue.diverted) return false;
  queue.diverted = true;
  clear(queue);
  return true;
}

void Controllers::Core::restore(controllers::SeatNumber seat) {
  const platform::Lock lock(mutex_);
  auto& queue = queues_[seat.index()];
  queue.diverted = false;
  clear(queue);
}

void Controllers::Core::restore_all() {
  const platform::Lock lock(mutex_);
  for (auto& queue : queues_) queue.diverted = false;
}

auto Controllers::Core::diverted(controllers::SeatNumber seat) const -> bool {
  const platform::Lock lock(mutex_);
  return queues_[seat.index()].diverted;
}

auto Controllers::Core::pending() const -> std::size_t {
  const platform::Lock lock(mutex_);
  std::size_t total = 0;
  for (const auto& queue : queues_) total += queue.samples.size();
  return total;
}

auto Controllers::Core::pending(controllers::SeatNumber seat) const
    -> std::size_t {
  const platform::Lock lock(mutex_);
  return queues_[seat.index()].samples.size();
}

auto Controllers::Core::settle(
    controllers::SeatNumber seat, const controllers::ControllerState& sample,
    const Expected<void, net::protocol::remote_play::StreamError>& result,
    std::optional<std::uint64_t> generation) -> Settled {
  auto& queue = queues_[seat.index()];
  Settled settled;
  if (generation && *generation == queue.generation && !queue.samples.empty()) {
    if (result || queue.retries >= send_retries) {
      queue.samples.pop();
      queue.retries = 0;
    } else {
      ++queue.retries;
    }
  }
  if (result) {
    if (!queue.reported) {
      queue.reported = true;
      settled.notice = Notice::first_accepted;
    }
    queue.refusal.reset();
    const bool down =
        (sample.buttons &
         static_cast<std::uint32_t>(controllers::ControllerButton::ps)) != 0;
    auto& held = system_button_down_[seat.index()];
    settled.system_button_pressed = down && !held;
    held = down;
  } else {
    if (queue.refusal != result.error()) {
      settled.notice = Notice::newly_refused;
    }
    queue.refusal = result.error();
  }
  if (generation) {
    settled.next = queue.samples.front();
    settled.generation = queue.generation;
  }
  return settled;
}

void Controllers::Core::follow_up(
    controllers::SeatNumber seat, const controllers::ControllerState& sample,
    const Expected<void, net::protocol::remote_play::StreamError>& result,
    const Settled& settled, bool guest) {
  if (settled.system_button_pressed) {
    const auto venue = route_.venue();
    if (venue && venue->on_old_service()) {
      route_.report("System menu toggle",
                    route_.driver().toggle_system_menu(
                        static_cast<std::uint32_t>(seat.index())));
    }
  }
  if (settled.notice == Notice::none) {
    return;
  }
  const std::string name = "pad " + std::to_string(seat.index()) +
                           (guest ? " guest input" : " input");
  if (settled.notice == Notice::first_accepted) {
    route_.log(platform::LogSeverity::info,
               name + " accepted by the session: buttons=" +
                   std::to_string(sample.buttons));
    return;
  }
  route_.log(platform::LogSeverity::warning,
             name + " was refused: result=" +
                 std::string{CommandRoute::result_name(result.error())});
}

auto Controllers::Core::flush(controllers::SeatNumber seat)
    -> Expected<void, net::protocol::remote_play::StreamError> {
  std::optional<controllers::ControllerState> sample;
  std::uint64_t generation{};
  {
    const platform::Lock lock(mutex_);
    auto& queue = queues_[seat.index()];
    sample = queue.samples.front();
    generation = queue.generation;
  }
  for (std::size_t sent = 0; sent < depth_ && sample; ++sent) {
    const auto result =
        route_.driver().send_controller_state(wire(seat, *sample));
    Settled settled;
    {
      const platform::Lock lock(mutex_);
      settled = settle(seat, *sample, result, generation);
    }
    follow_up(seat, *sample, result, settled, false);
    if (!result) {
      return result;
    }
    sample = settled.next;
    generation = settled.generation;
  }
  return {};
}

auto Controllers::Core::flush()
    -> Expected<void, net::protocol::remote_play::StreamError> {
  Expected<void, net::protocol::remote_play::StreamError> first{};
  for (std::uint8_t index = 0; index < controllers::SeatNumber::count;
       ++index) {
    const auto result = flush(controllers::SeatNumber::masked(index));
    if (!result && first) first = result;
  }
  return first;
}

auto Controllers::send(controllers::SeatNumber seat,
                       const controllers::ControllerState& state)
    -> Expected<void, InputRefusal> {
  return core_->send(seat, state);
}

void Controllers::discard_pending(controllers::SeatNumber seat) {
  core_->discard_pending(seat);
}

auto Controllers::pending() const -> std::size_t { return core_->pending(); }

auto Controllers::pending(controllers::SeatNumber seat) const -> std::size_t {
  return core_->pending(seat);
}

}  // namespace libchiaking::play
