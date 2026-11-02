#include "src/core/kakusan/adapters/capture.hpp"

#include <type_traits>
#include <utility>

namespace libchiaking::kakusan {

void Capture::set_forwarding(bool enabled) noexcept {
  const platform::Lock lock{mutex_};
  forwarding_ = enabled;
  if (!enabled) {
    queue_.clear();
    queued_bytes_ = 0;
    dropped_ = {};
  }
}

auto Capture::clone_keys() const -> std::optional<CapturedKeys> {
  const platform::Lock lock{mutex_};
  if (!keys_) return std::nullopt;
  return keys_->clone();
}

auto Capture::drain() -> CaptureDrain {
  static_assert(std::is_nothrow_move_constructible_v<CapturedDatagram>);
  std::vector<CapturedDatagram> datagrams{};
  {
    const platform::Lock lock{mutex_};
    datagrams.reserve(queue_.size());
    for (auto& datagram : queue_) datagrams.push_back(std::move(datagram));
    queue_.clear();
    queued_bytes_ = 0;
    return CaptureDrain{std::move(datagrams), std::exchange(dropped_, {})};
  }
}

void Capture::reset() noexcept {
  const platform::Lock lock{mutex_};
  keys_.reset();
  queue_.clear();
  queued_bytes_ = 0;
  dropped_ = {};
  forwarding_ = false;
}

void Capture::keys(std::uint32_t protocol_version,
                   play::ReceiveKeys keys) noexcept {
  const platform::Lock lock{mutex_};
  keys_.emplace(protocol_version, std::move(keys), ++generation_);
  queue_.clear();
  queued_bytes_ = 0;
}

void Capture::datagram(const play::SharedDatagram& datagram) noexcept {
  const platform::Lock lock{mutex_};
  if (!forwarding_ || datagram.bytes.empty() ||
      datagram.bytes.size() > capture_byte_limit)
    return;
  while (!queue_.empty() &&
         (queue_.size() >= capture_datagram_limit ||
          queued_bytes_ + datagram.bytes.size() > capture_byte_limit))
    drop_oldest();
  try {
    queue_.push_back(CapturedDatagram{
        datagram,
        std::vector<std::byte>(datagram.bytes.begin(), datagram.bytes.end())});
    queue_.back().facts.bytes = {};
    queued_bytes_ += datagram.bytes.size();
  } catch (...) {
    ++(datagram.media == play::SharedDatagram::Media::video
           ? dropped_.dropped_video
           : dropped_.dropped_audio);
  }
}

void Capture::drop_oldest() noexcept {
  const auto& oldest = queue_.front();
  ++(oldest.facts.media == play::SharedDatagram::Media::video
         ? dropped_.dropped_video
         : dropped_.dropped_audio);
  queued_bytes_ -= oldest.bytes.size();
  queue_.pop_front();
}

}  // namespace libchiaking::kakusan
