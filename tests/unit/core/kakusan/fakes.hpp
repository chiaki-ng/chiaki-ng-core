#pragma once

#include "libchiaking/detail/kakusan/kakusan.hpp"
#include "libchiaking/platform/net/provider.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "libchiaking/testing/fake_secure_datagram.hpp"

#include "src/core/kakusan/kakusan_core.hpp"
#include "tests/support/crypto/software_crypto.hpp"
#include "tests/support/setup.hpp"
#include "tests/unit/core/kakusan/identities.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace kakusan_test {

class TestPlatform {
 public:
  static auto crypto() -> libchiaking::platform::CryptoProvider& {
    return shared().crypto_;
  }
  static auto secure_datagram()
      -> libchiaking::platform::SecureDatagramProvider& {
    return shared().secure_datagram_;
  }
  static auto threading() -> libchiaking::platform::ThreadingProvider& {
    return shared().threading_;
  }
  static auto steady() -> const libchiaking::platform::ClockProvider& {
    return shared().steady_;
  }
  static auto wall() -> const libchiaking::platform::ClockProvider& {
    return shared().wall_;
  }
  static auto identity(std::uint8_t tag)
      -> libchiaking::kakusan::DeviceIdentity {
    return Identities::tagged(crypto(), tag);
  }

 private:
  static auto shared() -> TestPlatform& {
    static TestPlatform platform;
    return platform;
  }

  test_support::SoftwareCrypto crypto_;
  libchiaking::testing::FakeSecureDatagramProvider secure_datagram_;
  libchiaking::platform::DefaultThreadingProvider threading_;
  libchiaking::platform::SteadyClock steady_;
  libchiaking::platform::SystemClock wall_;
};

class Lan {
 public:
  struct Datagram {
    libchiaking::platform::SocketEndpoint source;
    std::vector<std::byte> bytes;
  };

  [[nodiscard]] auto bind(libchiaking::platform::SocketEndpoint local)
      -> std::optional<libchiaking::platform::SocketEndpoint> {
    const libchiaking::platform::Lock lock{mutex_};
    if (local.port == 0) local.port = next_port_++;
    if (find(local) != ports_.end()) return std::nullopt;
    ports_.push_back(Port{local, {}});
    return local;
  }

  void unbind(const libchiaking::platform::SocketEndpoint& local) {
    const libchiaking::platform::Lock lock{mutex_};
    if (const auto found = find(local); found != ports_.end())
      ports_.erase(found);
  }

  void send(const libchiaking::platform::SocketEndpoint& source,
            const libchiaking::platform::SocketEndpoint& destination,
            std::span<const std::byte> bytes) {
    const libchiaking::platform::Lock lock{mutex_};
    if (const auto found = find(destination); found != ports_.end())
      found->queue.push_back(Datagram{source, {bytes.begin(), bytes.end()}});
  }

  [[nodiscard]] auto receive(const libchiaking::platform::SocketEndpoint& local)
      -> std::optional<Datagram> {
    const libchiaking::platform::Lock lock{mutex_};
    const auto found = find(local);
    if (found == ports_.end() || found->queue.empty()) return std::nullopt;
    auto datagram = std::move(found->queue.front());
    found->queue.pop_front();
    return datagram;
  }

 private:
  struct Port {
    libchiaking::platform::SocketEndpoint endpoint;
    std::deque<Datagram> queue;
  };

  auto find(const libchiaking::platform::SocketEndpoint& endpoint)
      -> std::vector<Port>::iterator {
    return std::ranges::find(ports_, endpoint, &Port::endpoint);
  }

  libchiaking::platform::Mutex mutex_{TestPlatform::threading()};
  std::vector<Port> ports_;
  std::uint16_t next_port_{50000};
};

class Machine final : public libchiaking::platform::NetworkProvider {
 public:
  Machine(Lan& lan, std::uint8_t host_octet)
      : lan_(lan),
        address_(
            libchiaking::platform::IpAddress::ipv4({10, 0, 0, host_octet})) {}

  auto sockets() noexcept -> libchiaking::platform::SocketProvider& override {
    return sockets_;
  }
  auto pollers() noexcept -> libchiaking::platform::PollerProvider& override {
    return pollers_;
  }
  auto dns() noexcept -> libchiaking::platform::DnsProvider& override {
    return dns_;
  }
  auto interfaces() noexcept
      -> libchiaking::platform::InterfacesProvider& override {
    return interfaces_;
  }
  auto capabilities() const noexcept
      -> libchiaking::platform::NetworkCapabilities override {
    return libchiaking::platform::NetworkCapabilities{true, true};
  }

 private:
  class Socket final : public libchiaking::platform::UdpSocket {
   public:
    explicit Socket(Machine& machine) noexcept : machine_(machine) {}
    ~Socket() override { static_cast<void>(close()); }

    auto owner() const noexcept
        -> const libchiaking::platform::NetworkProvider& override {
      return machine_;
    }

    [[nodiscard]] auto bind(
        const libchiaking::platform::SocketEndpoint& endpoint)
        -> libchiaking::Expected<void,
                                 libchiaking::platform::NetError> override {
      if (local_ || endpoint.address != machine_.address_)
        return libchiaking::unexpected(
            libchiaking::platform::NetError::address_not_available);
      local_ = machine_.lan_.bind(endpoint);
      if (!local_)
        return libchiaking::unexpected(
            libchiaking::platform::NetError::address_in_use);
      return {};
    }

    [[nodiscard]] auto send_to(
        std::span<const std::byte> datagram,
        const libchiaking::platform::SocketEndpoint& destination)
        -> libchiaking::Expected<std::size_t,
                                 libchiaking::platform::NetError> override {
      if (!local_)
        return libchiaking::unexpected(
            libchiaking::platform::NetError::invalid_argument);
      machine_.lan_.send(*local_, destination, datagram);
      return datagram.size();
    }

    [[nodiscard]] auto receive_from(std::span<std::byte> buffer)
        -> libchiaking::Expected<libchiaking::platform::UdpReceiveResult,
                                 libchiaking::platform::NetError> override {
      if (!local_)
        return libchiaking::unexpected(
            libchiaking::platform::NetError::would_block);
      const auto datagram = machine_.lan_.receive(*local_);
      if (!datagram)
        return libchiaking::unexpected(
            libchiaking::platform::NetError::would_block);
      const auto size = std::min(buffer.size(), datagram->bytes.size());
      std::copy_n(datagram->bytes.begin(), size, buffer.begin());
      return libchiaking::platform::UdpReceiveResult{size, datagram->source,
                                                     std::nullopt};
    }

    [[nodiscard]] auto set_boolean_option(libchiaking::platform::SocketOption,
                                          bool)
        -> libchiaking::Expected<void,
                                 libchiaking::platform::NetError> override {
      return {};
    }

    [[nodiscard]] auto set_receive_buffer_size(std::size_t)
        -> libchiaking::Expected<void,
                                 libchiaking::platform::NetError> override {
      return {};
    }

    [[nodiscard]] auto local_endpoint()
        -> libchiaking::Expected<libchiaking::platform::SocketEndpoint,
                                 libchiaking::platform::NetError> override {
      if (!local_)
        return libchiaking::unexpected(
            libchiaking::platform::NetError::not_connected);
      return *local_;
    }

    [[nodiscard]] auto close()
        -> libchiaking::Expected<void,
                                 libchiaking::platform::NetError> override {
      if (local_) machine_.lan_.unbind(*local_);
      local_.reset();
      return {};
    }

   private:
    Machine& machine_;
    std::optional<libchiaking::platform::SocketEndpoint> local_;
  };

  class Poller final : public libchiaking::platform::Poller {
   public:
    explicit Poller(Machine& machine) noexcept : machine_(machine) {}

    auto owner() const noexcept
        -> const libchiaking::platform::NetworkProvider& override {
      return machine_;
    }
    [[nodiscard]] auto wake()
        -> libchiaking::Expected<void,
                                 libchiaking::platform::NetError> override {
      return {};
    }
    [[nodiscard]] auto wait(std::span<const libchiaking::platform::PollRequest>,
                            std::chrono::nanoseconds,
                            const libchiaking::platform::StopToken&)
        -> libchiaking::Expected<libchiaking::platform::PollWaitResult,
                                 libchiaking::platform::NetError> override {
      return libchiaking::platform::PollWaitResult{};
    }

   private:
    Machine& machine_;
  };

  class Sockets final : public libchiaking::platform::SocketProvider {
   public:
    explicit Sockets(Machine& machine) noexcept : machine_(machine) {}

    [[nodiscard]] auto create_udp_socket(
        const libchiaking::platform::IpAddress&)
        -> libchiaking::Expected<
            std::unique_ptr<libchiaking::platform::UdpSocket>,
            libchiaking::platform::NetError> override {
      std::unique_ptr<libchiaking::platform::UdpSocket> socket =
          std::make_unique<Socket>(machine_);
      return socket;
    }
    [[nodiscard]] auto create_tcp_socket(
        const libchiaking::platform::IpAddress&)
        -> libchiaking::Expected<
            std::unique_ptr<libchiaking::platform::TcpSocket>,
            libchiaking::platform::NetError> override {
      return libchiaking::unexpected(
          libchiaking::platform::NetError::not_supported);
    }
    [[nodiscard]] auto create_tcp_listener(
        const libchiaking::platform::IpAddress&)
        -> libchiaking::Expected<
            std::unique_ptr<libchiaking::platform::TcpListener>,
            libchiaking::platform::NetError> override {
      return libchiaking::unexpected(
          libchiaking::platform::NetError::not_supported);
    }

   private:
    Machine& machine_;
  };

  class Pollers final : public libchiaking::platform::PollerProvider {
   public:
    explicit Pollers(Machine& machine) noexcept : machine_(machine) {}

    [[nodiscard]] auto create_poller()
        -> libchiaking::Expected<std::unique_ptr<libchiaking::platform::Poller>,
                                 libchiaking::platform::NetError> override {
      std::unique_ptr<libchiaking::platform::Poller> poller =
          std::make_unique<Poller>(machine_);
      return poller;
    }

   private:
    Machine& machine_;
  };

  class Dns final : public libchiaking::platform::DnsProvider {
   public:
    using libchiaking::platform::DnsProvider::resolve;

    [[nodiscard]] auto resolve(std::string_view, std::string_view)
        -> libchiaking::Expected<std::vector<libchiaking::platform::IpAddress>,
                                 libchiaking::platform::NetError> override {
      return libchiaking::unexpected(
          libchiaking::platform::NetError::host_unreachable);
    }
  };

  class Interfaces final : public libchiaking::platform::InterfacesProvider {
   public:
    explicit Interfaces(Machine& machine) noexcept : machine_(machine) {}

    [[nodiscard]] auto enumerate() -> libchiaking::Expected<
        std::vector<libchiaking::platform::InterfaceInfo>,
        libchiaking::platform::NetError> override {
      libchiaking::platform::InterfaceInfo info;
      info.name = "en0";
      info.address = machine_.address_;
      info.operational = true;
      info.medium = libchiaking::platform::InterfaceMedium::ethernet;
      return std::vector<libchiaking::platform::InterfaceInfo>{info};
    }

   private:
    Machine& machine_;
  };

  Lan& lan_;
  libchiaking::platform::IpAddress address_;
  Sockets sockets_{*this};
  Pollers pollers_{*this};
  Dns dns_;
  Interfaces interfaces_{*this};
};

struct VirtualClock {
  std::atomic<std::uint64_t> now_us{2'000'000};
  std::uint64_t start_wall{TestPlatform::wall().now_microseconds() / 1'000'000};

  auto source() -> libchiaking::kakusan::Surroundings::ClockSource {
    return [this] {
      const auto now = now_us.load();
      return libchiaking::kakusan::Clock{now, start_wall + now / 1'000'000};
    };
  }
};

class SteppedClock final : public libchiaking::platform::ClockProvider {
 public:
  explicit SteppedClock(VirtualClock& clock) noexcept : clock_(clock) {}

  std::uint64_t now_microseconds() const noexcept override {
    return clock_.now_us.load();
  }

  void advance(std::chrono::milliseconds duration) noexcept {
    clock_.now_us += static_cast<std::uint64_t>(duration.count()) * 1'000;
  }

 private:
  VirtualClock& clock_;
};

class MemoryStore final : public libchiaking::kakusan::DeviceStore {
 public:
  MemoryStore() = default;
  explicit MemoryStore(
      const libchiaking::kakusan::DeviceIdentity& identity,
      std::vector<libchiaking::kakusan::SavedDevice> known = {})
      : devices(std::move(known)), identity_(identity.saved()) {}

  [[nodiscard]] auto load_identity() -> libchiaking::Expected<
      std::optional<libchiaking::kakusan::SavedIdentity>, Error> override {
    const libchiaking::platform::Lock lock{mutex_};
    if (unavailable) return libchiaking::unexpected(Error::unavailable);
    if (!identity_) return std::optional<libchiaking::kakusan::SavedIdentity>{};
    return std::optional<libchiaking::kakusan::SavedIdentity>{
        identity_->clone()};
  }

  [[nodiscard]] auto save_identity(
      const libchiaking::kakusan::SavedIdentity& identity)
      -> libchiaking::Expected<void, Error> override {
    const libchiaking::platform::Lock lock{mutex_};
    if (unavailable) return libchiaking::unexpected(Error::unavailable);
    identity_.emplace(identity.clone());
    ++identity_saves;
    return {};
  }

  [[nodiscard]] auto load_devices()
      -> libchiaking::Expected<std::vector<libchiaking::kakusan::SavedDevice>,
                               Error> override {
    const libchiaking::platform::Lock lock{mutex_};
    if (unavailable) return libchiaking::unexpected(Error::unavailable);
    return devices;
  }

  [[nodiscard]] auto save(const libchiaking::kakusan::SavedDevice& device)
      -> libchiaking::Expected<void, Error> override {
    const libchiaking::platform::Lock lock{mutex_};
    if (unavailable || refuses_saves)
      return libchiaking::unexpected(Error::unavailable);
    const auto found = std::ranges::find(
        devices, device.key, &libchiaking::kakusan::SavedDevice::key);
    if (found == devices.end())
      devices.push_back(device);
    else
      *found = device;
    return {};
  }

  [[nodiscard]] auto remove(const libchiaking::kakusan::IdentityKey& key)
      -> libchiaking::Expected<void, Error> override {
    const libchiaking::platform::Lock lock{mutex_};
    if (unavailable || refuses_removing == key)
      return libchiaking::unexpected(Error::unavailable);
    std::erase_if(devices,
                  [&key](const libchiaking::kakusan::SavedDevice& device) {
                    return device.key == key;
                  });
    return {};
  }

  auto saved() const -> std::vector<libchiaking::kakusan::SavedDevice> {
    const libchiaking::platform::Lock lock{mutex_};
    return devices;
  }

  std::vector<libchiaking::kakusan::SavedDevice> devices;
  std::size_t identity_saves{};
  bool unavailable{};
  bool refuses_saves{};
  std::optional<libchiaking::kakusan::IdentityKey> refuses_removing{};

 private:
  mutable libchiaking::platform::Mutex mutex_{TestPlatform::threading()};
  std::optional<libchiaking::kakusan::SavedIdentity> identity_;
};

class Neighbourhood {
 public:
  static auto surroundings(
      Machine& machine, libchiaking::platform::SecureRandomProvider& random,
      libchiaking::kakusan::Surroundings::ClockSource clock, MemoryStore& store,
      libchiaking::platform::DiagnosticsProvider* log = nullptr)
      -> std::shared_ptr<libchiaking::kakusan::Surroundings> {
    auto made = libchiaking::kakusan::Surroundings::of(
        libchiaking::kakusan::Surroundings::Injected{
            TestPlatform::crypto(), random, std::move(clock), store,
            TestPlatform::threading(), log, nullptr, &machine,
            &TestPlatform::secure_datagram()});
    return test_support::Setup::value(std::move(made),
                                      "the surroundings are made");
  }
};

}  // namespace kakusan_test
