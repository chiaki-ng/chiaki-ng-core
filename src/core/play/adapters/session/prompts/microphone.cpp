#include "src/core/play/adapters/session/prompts/microphone_core.hpp"

#include "src/core/play/adapters/links/link_driver.hpp"
#include "src/core/play/adapters/links/link_evidence.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace libchiaking::play {

static_assert(Microphone::frame_size == LinkDriver::microphone_frame_size);

Microphone::Core::Core(platform::ThreadingProvider& threading,
                       const CommandRoute& route, const RestRequest& rest,
                       std::size_t depth)
    : route_(route),
      rest_(rest),
      depth_(std::max<std::size_t>(depth, 1)),
      mutex_(threading) {}

auto Microphone::Core::open() const noexcept -> bool {
  return !route_.microphone_refusal() && rest_.idle();
}

auto Microphone::Core::connect(std::uint32_t tag)
    -> Expected<ConnectTicket, InputRefusal> {
  std::uint32_t id = 0;
  {
    const platform::Lock lock(mutex_);
    if (const auto refusal = route_.microphone_refusal())
      return unexpected(*refusal);
    if (!rest_.idle()) return unexpected(InputRefusal::not_ready);
    id = ++next_ticket_;
    connect_pending_ = id;
    latest_ = ConnectTicket{id, tag};
    if (issued_.size() == remembered_tickets) issued_.pop_front();
    issued_.push_back(*latest_);
  }
  route_.wake();
  return ConnectTicket{id, tag};
}

auto Microphone::Core::latest() const -> std::optional<ConnectTicket> {
  const platform::Lock lock(mutex_);
  return latest_;
}

auto Microphone::Core::state(ConnectTicket ticket) const -> ConnectState {
  const platform::Lock lock(mutex_);
  if (std::ranges::find(issued_, ticket) == issued_.end())
    return ConnectState::rejected;
  if (connect_done_ == ticket.id() && connect_pending_ != ticket.id())
    return connect_accepted_ ? ConnectState::accepted : ConnectState::rejected;
  return open() ? ConnectState::pending : ConnectState::rejected;
}

auto Microphone::Core::set_muted(bool muted) -> Expected<void, InputRefusal> {
  {
    const platform::Lock lock(mutex_);
    if (const auto refusal = route_.microphone_refusal())
      return unexpected(*refusal);
    if (!rest_.idle()) return unexpected(InputRefusal::not_ready);
    mute_ = muted ? Mute::on : Mute::off;
    sending_ = !muted;
    if (muted) {
      statistics_.frames_dropped += frames_.size();
      frames_.clear();
    }
  }
  route_.wake();
  return {};
}

auto Microphone::Core::push(std::span<const std::byte, frame_size> frame)
    -> Expected<void, InputRefusal> {
  {
    const platform::Lock lock(mutex_);
    if (const auto refusal = route_.microphone_refusal())
      return unexpected(*refusal);
    if (!rest_.idle() || !sending_) return unexpected(InputRefusal::not_ready);
    if (frames_.size() >= depth_) {
      frames_.pop_front();
      ++statistics_.frames_dropped;
    }
    std::ranges::copy(frame, frames_.emplace_back().begin());
    ++statistics_.frames_queued;
  }
  route_.wake();
  return {};
}

auto Microphone::Core::statistics() const -> Statistics {
  const platform::Lock lock(mutex_);
  return statistics_;
}

auto Microphone::Core::pending() const -> bool {
  const platform::Lock lock(mutex_);
  return connect_pending_.has_value() || mute_ != Mute::idle ||
         !frames_.empty();
}

void Microphone::Core::report(
    std::string_view step,
    const Expected<void, net::protocol::remote_play::StreamError>& result) {
  if (result) return;
  std::uint64_t failures = 0;
  {
    const platform::Lock lock(mutex_);
    failures = ++statistics_.send_failures;
    statistics_.last_error = LinkEvidence::cause(result.error());
  }
  if ((failures & (failures - 1)) == 0)
    route_.log(platform::LogSeverity::warning,
               "Microphone " + std::string{step} + " failed: result=" +
                   std::string{CommandRoute::result_name(result.error())} +
                   " failures=" + std::to_string(failures));
}

void Microphone::Core::flush() {
  std::optional<std::uint32_t> connect;
  Mute mute = Mute::idle;
  {
    const platform::Lock lock(mutex_);
    connect = std::exchange(connect_pending_, std::nullopt);
    mute = std::exchange(mute_, Mute::idle);
  }
  auto& driver = route_.driver();
  if (connect) {
    const auto result = driver.connect_microphone();
    {
      const platform::Lock lock(mutex_);
      connect_done_ = connect;
      connect_accepted_ = result.has_value();
    }
    route_.report("Microphone connect request " + std::to_string(*connect),
                  result);
    report("connect", result);
  }
  if (mute != Mute::idle) {
    const auto result = driver.set_microphone_muted(mute == Mute::on);
    route_.report(mute == Mute::on ? "Microphone mute request"
                                   : "Microphone unmute request",
                  result);
    report("mute", result);
  }
  for (std::size_t handled = 0; handled < depth_; ++handled) {
    Frame frame{};
    {
      const platform::Lock lock(mutex_);
      if (frames_.empty() || !sending_) return;
      frame = frames_.front();
      frames_.pop_front();
    }
    const auto result = driver.send_microphone(frame);
    if (result) {
      const platform::Lock lock(mutex_);
      ++statistics_.frames_sent;
    }
    report("frame", result);
  }
}

auto Microphone::connect(std::uint32_t tag)
    -> Expected<ConnectTicket, InputRefusal> {
  return core_->connect(tag);
}

auto Microphone::latest() const -> std::optional<ConnectTicket> {
  return core_->latest();
}

auto Microphone::state(ConnectTicket ticket) const -> ConnectState {
  return core_->state(ticket);
}

auto Microphone::open() const noexcept -> bool { return core_->open(); }

auto Microphone::set_muted(bool muted) -> Expected<void, InputRefusal> {
  return core_->set_muted(muted);
}

auto Microphone::push(std::span<const std::byte, frame_size> frame)
    -> Expected<void, InputRefusal> {
  return core_->push(frame);
}

auto Microphone::statistics() const -> Statistics {
  return core_->statistics();
}

}  // namespace libchiaking::play
