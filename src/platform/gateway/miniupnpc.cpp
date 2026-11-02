#include "../net/network_stack.hpp"
#include "../runtime/pending_call.hpp"

#include <libchiaking/platform/gateway/provider.hpp>

#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace libchiaking::platform {
class MiniProvider final : public GatewayProvider {
  static constexpr std::chrono::seconds map_timeout = default_gateway_timeout;
  static constexpr std::chrono::seconds close_timeout{3};
  static constexpr int discover_delay_ms = 2000;
  static constexpr int discover_ttl = 2;

  class GatewayWorker {
   public:
    GatewayWorker(ThreadingProvider& threading, const ClockProvider& clock)
        : threading_(threading),
          clock_(clock),
          mutex_(threading),
          ready_(threading, clock) {}

    ~GatewayWorker() {
      {
        Lock lock{mutex_};
        stopping_ = true;
      }
      ready_.notify_all();
      static_cast<void>(thread_.join());
    }

    GatewayWorker(const GatewayWorker&) = delete;
    auto operator=(const GatewayWorker&) -> GatewayWorker& = delete;

    [[nodiscard]] auto start() -> bool {
      auto started = Thread::start(threading_, "gateway",
                                   ThreadRole::unspecified, [this] { run(); });
      if (!started) return false;
      thread_ = std::move(started).value();
      return true;
    }

    auto threading() noexcept -> ThreadingProvider& { return threading_; }
    auto clock() const noexcept -> const ClockProvider& { return clock_; }

    [[nodiscard]] bool post(std::function<void()> job) {
      {
        Lock lock{mutex_};
        if (stopping_) return false;
        jobs_.push_back(std::move(job));
      }
      ready_.notify_one();
      return true;
    }

   private:
    void run() noexcept {
      for (;;) {
        std::function<void()> job;
        {
          Lock lock{mutex_};
          ready_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
          if (jobs_.empty()) return;
          job = std::move(jobs_.front());
          jobs_.pop_front();
        }
        try {
          job();
        } catch (...) {
        }
      }
    }

    ThreadingProvider& threading_;
    const ClockProvider& clock_;
    Mutex mutex_;
    ConditionVariable ready_;
    std::deque<std::function<void()>> jobs_;
    bool stopping_{};
    Thread thread_;
  };

  template <class T, class Work>
  static auto bounded(GatewayWorker& worker, Work work, const StopToken& stop,
                      std::chrono::milliseconds timeout)
      -> Expected<T, GatewayError> {
    using Call = runtime::PendingCall<Expected<T, GatewayError>>;
    if (stop.stop_requested()) return unexpected(GatewayError::cancelled);
    try {
      auto call = Call::posted(
          worker.threading(), worker.clock(),
          [work = std::move(work)]() mutable -> Expected<T, GatewayError> {
            try {
              return work();
            } catch (...) {
              return unexpected(GatewayError::network);
            }
          },
          [&worker](std::function<void()> job) {
            return worker.post(std::move(job));
          });
      if (!call) return unexpected(GatewayError::unavailable);
      auto finished = call->await(stop, timeout);
      if (finished) return std::move(finished).value();
      switch (finished.error()) {
        case runtime::CallUnfinished::stopped:
          return unexpected(GatewayError::cancelled);
        case runtime::CallUnfinished::expired:
          return unexpected(GatewayError::timeout);
        case runtime::CallUnfinished::broken:
          return unexpected(GatewayError::network);
      }
      return unexpected(GatewayError::network);
    } catch (...) {
      return unexpected(GatewayError::network);
    }
  }

  struct GatewayState {
    UPNPUrls urls{};
    IGDdatas data{};
    std::array<char, 64> lan{};
    IpAddress local{};
    Mutex mutex;
    std::vector<std::uint16_t> ports;
    std::atomic<bool> closed{};

    void record(std::uint16_t port) {
      Lock lock{mutex};
      ports.push_back(port);
    }

    bool remove_ports() {
      std::vector<std::uint16_t> pending;
      {
        Lock lock{mutex};
        pending.swap(ports);
      }
      bool success = true;
      for (auto port : pending) {
        const auto text = std::to_string(port);
        if (UPNP_DeletePortMapping(urls.controlURL, data.first.servicetype,
                                   text.c_str(), "UDP",
                                   nullptr) != UPNPCOMMAND_SUCCESS)
          success = false;
      }
      return success;
    }

    explicit GatewayState(ThreadingProvider& threading) : mutex(threading) {}
    GatewayState(const GatewayState&) = delete;
    auto operator=(const GatewayState&) -> GatewayState& = delete;
    ~GatewayState() { FreeUPNPUrls(&urls); }
  };

  class MiniGateway final : public GatewaySession {
   public:
    MiniGateway(std::shared_ptr<GatewayWorker> worker,
                std::shared_ptr<GatewayState> state)
        : worker_(std::move(worker)), state_(std::move(state)) {}
    MiniGateway(const MiniGateway&) = delete;
    auto operator=(const MiniGateway&) -> MiniGateway& = delete;
    ~MiniGateway() override {
      if (state_->closed.exchange(true)) return;
      try {
        auto state = state_;
        (void)worker_->post([state] { (void)state->remove_ports(); });
      } catch (...) {
      }
    }
    auto local_address() const -> IpAddress override { return state_->local; }
    auto map_udp(std::uint16_t port, const StopToken& stop)
        -> Expected<SocketEndpoint, GatewayError> override {
      if (!port) return unexpected(GatewayError::invalid_response);
      if (state_->closed.load()) return unexpected(GatewayError::unavailable);
      auto state = state_;
      return bounded<SocketEndpoint>(
          *worker_,
          [state, port]() -> Expected<SocketEndpoint, GatewayError> {
            if (state->closed.load())
              return unexpected(GatewayError::unavailable);
            const auto text = std::to_string(port);
            if (UPNP_AddPortMapping(state->urls.controlURL,
                                    state->data.first.servicetype, text.c_str(),
                                    text.c_str(), state->lan.data(),
                                    "Chiaki Streaming", "UDP", nullptr,
                                    "0") != UPNPCOMMAND_SUCCESS)
              return unexpected(GatewayError::network);
            state->record(port);
            std::array<char, 64> external{};
            if (UPNP_GetExternalIPAddress(
                    state->urls.controlURL, state->data.first.servicetype,
                    external.data()) != UPNPCOMMAND_SUCCESS)
              return unexpected(GatewayError::network);
            auto ip = parse_ip_address(external.data());
            if (!ip || !ip.value().is_ipv4() || ip.value().is_unspecified())
              return unexpected(GatewayError::invalid_response);
            return SocketEndpoint{ip.value(), port};
          },
          stop, map_timeout);
    }
    auto close() -> Expected<void, GatewayError> override {
      state_->closed.store(true);
      auto state = state_;
      return bounded<void>(
          *worker_,
          [state]() -> Expected<void, GatewayError> {
            if (!state->remove_ports())
              return unexpected(GatewayError::network);
            return {};
          },
          {}, close_timeout);
    }

   private:
    std::shared_ptr<GatewayWorker> worker_;
    std::shared_ptr<GatewayState> state_;
  };

 public:
  MiniProvider(ThreadingProvider& threading, const ClockProvider& clock)
      : threading_(threading), clock_(clock), mutex_(threading) {}

  auto discover(const StopToken& stop, std::chrono::milliseconds timeout)
      -> Expected<std::unique_ptr<GatewaySession>, GatewayError> override {
    if (!detail::NetworkStack::ensure())
      return unexpected(GatewayError::unavailable);
    auto worker = acquire_worker();
    if (!worker) return unexpected(GatewayError::unavailable);
    auto discovered = bounded<std::shared_ptr<GatewayState>>(
        *worker,
        [&threading = threading_]()
            -> Expected<std::shared_ptr<GatewayState>, GatewayError> {
          int error{};
          auto* devices = upnpDiscover(discover_delay_ms, nullptr, nullptr, 0,
                                       0, discover_ttl, &error);
          if (!devices) return unexpected(GatewayError::unavailable);
          auto free_devices =
              std::unique_ptr<UPNPDev, decltype(&freeUPNPDevlist)>(
                  devices, freeUPNPDevlist);
          if (error != UPNPDISCOVER_SUCCESS)
            return unexpected(GatewayError::network);
          auto state = std::make_shared<GatewayState>(threading);
#if MINIUPNPC_API_VERSION >= 18
          const auto valid = UPNP_GetValidIGD(
              devices, &state->urls, &state->data, state->lan.data(),
              static_cast<int>(state->lan.size()), nullptr, 0);
#else
          const auto valid = UPNP_GetValidIGD(
              devices, &state->urls, &state->data, state->lan.data(),
              static_cast<int>(state->lan.size()));
#endif
          if (valid != 1) return unexpected(GatewayError::unavailable);
          auto local = parse_ip_address(state->lan.data());
          if (!local || !local.value().is_ipv4() ||
              local.value().is_unspecified())
            return unexpected(GatewayError::invalid_response);
          state->local = local.value();
          return state;
        },
        stop, timeout);
    if (!discovered) return unexpected(discovered.error());
    try {
      return std::unique_ptr<GatewaySession>{std::make_unique<MiniGateway>(
          std::move(worker), std::move(discovered.value()))};
    } catch (...) {
      return unexpected(GatewayError::unavailable);
    }
  }

 private:
  auto acquire_worker() -> std::shared_ptr<GatewayWorker> {
    Lock lock{mutex_};
    if (!worker_) {
      try {
        auto worker = std::make_shared<GatewayWorker>(threading_, clock_);
        if (!worker->start()) return nullptr;
        worker_ = std::move(worker);
      } catch (...) {
        return nullptr;
      }
    }
    return worker_;
  }

  ThreadingProvider& threading_;
  const ClockProvider& clock_;
  Mutex mutex_;
  std::shared_ptr<GatewayWorker> worker_;
};

auto make_gateway_provider(ThreadingProvider& threading,
                           const ClockProvider& clock)
    -> std::unique_ptr<GatewayProvider> {
  return std::make_unique<MiniProvider>(threading, clock);
}
}  // namespace libchiaking::platform
