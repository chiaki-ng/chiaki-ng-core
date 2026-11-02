#pragma once

#include "tests/support/live_providers.hpp"
#include "tests/support/thread_set.hpp"

#include <gtest/gtest.h>

#include <libchiaking/play_types.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace play_test {

using test_support::LiveProviders;
using test_support::ThreadSet;

class RecordedAudience final : public libchiaking::play::SharedAudience {
 public:
  static constexpr std::size_t datagram_capacity = 128;
  static constexpr std::size_t datagram_bytes = 64;

  struct Started {
    libchiaking::play::SharedStream stream;
    std::array<std::byte, libchiaking::play::ReceiveKeys::size> key{};
    std::array<std::byte, libchiaking::play::ReceiveKeys::size> iv{};
  };

  struct Datagram {
    libchiaking::play::SharedDatagram facts{};
    const std::byte* source{};
    std::array<std::byte, datagram_bytes> bytes{};
    std::size_t size{};
    ThreadSet::Identity thread{};

    auto view() const noexcept -> libchiaking::play::SharedDatagram {
      auto described = facts;
      described.bytes = std::span<const std::byte>{bytes.data(), size};
      return described;
    }
  };

  void stream_started(const libchiaking::play::SharedStream& stream,
                      libchiaking::play::ReceiveKeys keys) override {
    const libchiaking::platform::Lock lock(mutex_);
    Started started{stream, {}, {}};
    std::ranges::copy(keys.key(), started.key.begin());
    std::ranges::copy(keys.iv(), started.iv.begin());
    started_.push_back(std::move(started));
    heard("stream_started");
  }

  void encrypted_datagram(
      const libchiaking::play::SharedDatagram& datagram) noexcept override {
    const auto index = datagrams_.load(std::memory_order_relaxed);
    if (index >= datagram_capacity) return;
    auto& slot = slots_[index];
    slot.facts = datagram;
    slot.facts.bytes = {};
    slot.source = datagram.bytes.data();
    slot.size = std::min(datagram.bytes.size(), datagram_bytes);
    std::copy_n(datagram.bytes.begin(), slot.size, slot.bytes.begin());
    slot.thread = ThreadSet::Identity::current();
    datagrams_.store(index + 1, std::memory_order_release);
  }

  void feedback(
      const libchiaking::controllers::ControllerFeedback& feedback) override {
    const libchiaking::platform::Lock lock(mutex_);
    feedback_.push_back(feedback);
    heard("feedback");
  }

  void guest_seated(libchiaking::controllers::SeatNumber seat) override {
    const libchiaking::platform::Lock lock(mutex_);
    seated_.push_back(seat);
    heard("guest_seated");
  }

  void guest_seat_refused(
      const libchiaking::play::SeatRefusal& refusal) override {
    const libchiaking::platform::Lock lock(mutex_);
    refused_.push_back(refusal);
    heard("guest_seat_refused");
  }

  void guest_unseated(libchiaking::controllers::SeatNumber seat) override {
    const libchiaking::platform::Lock lock(mutex_);
    unseated_.push_back(seat);
    heard("guest_unseated");
  }

  void became_unshareable(libchiaking::play::NotShareable reason) override {
    const libchiaking::platform::Lock lock(mutex_);
    unshareable_.push_back(reason);
    heard("became_unshareable");
  }

  auto service(std::chrono::milliseconds wait)
      -> std::chrono::milliseconds override {
    std::function<void()> once;
    {
      const libchiaking::platform::Lock lock(mutex_);
      heard("service");
      once = std::exchange(once_, nullptr);
    }
    if (once) once();
    return wait;
  }

  void session_ended() override {
    const libchiaking::platform::Lock lock(mutex_);
    heard("session_ended");
  }

  void in_next_service(std::function<void()> once) {
    const libchiaking::platform::Lock lock(mutex_);
    once_ = std::move(once);
  }

  void until(std::string_view name, std::size_t times = 1) {
    libchiaking::platform::Lock lock(mutex_);
    if (!changed_.wait_for(lock, std::chrono::seconds{20},
                           [this, name, times] {
                             return count_locked(name) >= times;
                           }))
      ADD_FAILURE() << "no " << name << " event within 20 seconds";
  }

  void until_datagrams(std::size_t count) const {
    const auto deadline = LiveProviders::steady_us() + 20'000'000;
    while (datagrams_.load(std::memory_order_acquire) < count) {
      if (LiveProviders::steady_us() >= deadline) {
        ADD_FAILURE() << "fewer than " << count
                      << " datagrams arrived within 20 seconds";
        return;
      }
      LiveProviders::nap(std::chrono::milliseconds{1});
    }
  }

  auto count(std::string_view name) const -> std::size_t {
    const libchiaking::platform::Lock lock(mutex_);
    return count_locked(name);
  }

  auto started() const -> std::vector<Started> {
    const libchiaking::platform::Lock lock(mutex_);
    return started_;
  }

  auto datagram_count() const noexcept -> std::size_t {
    return datagrams_.load(std::memory_order_acquire);
  }
  auto datagram(std::size_t index) const noexcept -> const Datagram& {
    return slots_[index];
  }

  auto feedback() const
      -> std::vector<libchiaking::controllers::ControllerFeedback> {
    const libchiaking::platform::Lock lock(mutex_);
    return feedback_;
  }

  auto seated() const -> std::vector<libchiaking::controllers::SeatNumber> {
    const libchiaking::platform::Lock lock(mutex_);
    return seated_;
  }

  auto refused() const -> std::vector<libchiaking::play::SeatRefusal> {
    const libchiaking::platform::Lock lock(mutex_);
    return refused_;
  }

  auto unseated() const -> std::vector<libchiaking::controllers::SeatNumber> {
    const libchiaking::platform::Lock lock(mutex_);
    return unseated_;
  }

  auto unshareable() const -> std::vector<libchiaking::play::NotShareable> {
    const libchiaking::platform::Lock lock(mutex_);
    return unshareable_;
  }

  auto threads() const -> ThreadSet {
    const libchiaking::platform::Lock lock(mutex_);
    return threads_;
  }

 private:
  void heard(std::string_view name) {
    names_.emplace_back(name);
    threads_.insert(ThreadSet::Identity::current());
    changed_.notify_all();
  }

  auto count_locked(std::string_view name) const -> std::size_t {
    return static_cast<std::size_t>(std::ranges::count(names_, name));
  }

  mutable libchiaking::platform::Mutex mutex_{LiveProviders::threading()};
  libchiaking::platform::ConditionVariable changed_{LiveProviders::threading(), LiveProviders::steady()};
  std::vector<std::string> names_;
  ThreadSet threads_;
  std::vector<Started> started_;
  std::vector<libchiaking::controllers::ControllerFeedback> feedback_;
  std::vector<libchiaking::controllers::SeatNumber> seated_;
  std::vector<libchiaking::play::SeatRefusal> refused_;
  std::vector<libchiaking::controllers::SeatNumber> unseated_;
  std::vector<libchiaking::play::NotShareable> unshareable_;
  std::function<void()> once_;
  std::array<Datagram, datagram_capacity> slots_{};
  std::atomic<std::size_t> datagrams_{};
};

}  // namespace play_test
