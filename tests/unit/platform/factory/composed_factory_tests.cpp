#include "tests/support/enum_printing.hpp"

#include <libchiaking/platform/default_factory.hpp>
#include <libchiaking/platform/net/default.hpp>
#include <libchiaking/platform/platform.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace {

namespace platform = libchiaking::platform;
using libchiaking::Expected;
using platform::DefaultPlatformFactory;
using platform::PlatformCapability;
using platform::PlatformFactory;
using platform::PlatformProviders;

class ComposedFactory : public ::testing::Test {
 protected:
  class FakeThreadingProvider final : public platform::ThreadingProvider {
   public:
    auto start_thread(platform::native::ThreadEntry)
        -> Expected<std::unique_ptr<platform::native::Thread>,
                    platform::PlatformError> override {
      return std::unique_ptr<platform::native::Thread>{};
    }
    auto create_mutex(platform::MutexKind)
        -> Expected<std::unique_ptr<platform::native::Mutex>,
                    platform::PlatformError> override {
      return std::unique_ptr<platform::native::Mutex>{};
    }
    auto create_condition_variable()
        -> Expected<std::unique_ptr<platform::native::ConditionVariable>,
                    platform::PlatformError> override {
      return std::unique_ptr<platform::native::ConditionVariable>{};
    }
    auto create_stop_source()
        -> Expected<std::unique_ptr<platform::native::StopSource>,
                    platform::PlatformError> override {
      return std::unique_ptr<platform::native::StopSource>{};
    }
  };

  class FakeClockProvider final : public platform::ClockProvider {
   public:
    auto now_microseconds() const noexcept -> std::uint64_t override {
      return 0;
    }
  };

  class FakeRandomProvider final : public platform::SecureRandomProvider {
   public:
    auto fill_bytes(std::span<std::byte>)
        -> Expected<void, platform::PlatformError> override {
      return {};
    }
  };

  class FakeNetworkProvider final : public platform::NetworkProvider {
   public:
    auto sockets() noexcept -> platform::SocketProvider& override {
      return sockets_;
    }
    auto pollers() noexcept -> platform::PollerProvider& override {
      return pollers_;
    }
    auto dns() noexcept -> platform::DnsProvider& override { return dns_; }
    auto interfaces() noexcept -> platform::InterfacesProvider& override {
      return interfaces_;
    }
    auto capabilities() const noexcept
        -> platform::NetworkCapabilities override {
      return capabilities_;
    }

    platform::NetworkCapabilities capabilities_;

   private:
    platform::DefaultThreadingProvider threading_;
    platform::SteadyClock clock_;
    platform::DefaultSocketProvider sockets_{*this};
    platform::DefaultPollerProvider pollers_{*this, clock_};
    platform::DefaultDnsProvider dns_{threading_, clock_};
    platform::DefaultInterfacesProvider interfaces_;
  };

  class FakeCryptoProvider final : public platform::CryptoProvider {
   public:
    auto sha256(std::span<const std::byte>, std::span<std::byte>)
        -> Expected<void, platform::CryptoError> override {
      return {};
    }
    auto hmac_sha256(std::span<const std::byte>, std::span<const std::byte>,
                     std::span<std::byte>)
        -> Expected<void, platform::CryptoError> override {
      return {};
    }
    auto aes128_encrypt_block(std::span<const std::byte>,
                              std::span<const std::byte>, std::span<std::byte>)
        -> Expected<void, platform::CryptoError> override {
      return {};
    }
    auto aes128_gmac(std::span<const std::byte>, std::span<const std::byte>,
                     std::span<const std::byte>, std::span<std::byte>)
        -> Expected<void, platform::CryptoError> override {
      return {};
    }
  };

  class FakeHttpProvider final : public platform::HttpProvider {
   public:
    auto perform(const platform::HttpRequest&, const platform::StopToken&)
        -> Expected<platform::HttpResponse, platform::HttpError> override {
      return platform::HttpResponse{};
    }
  };

  class FakeWebSocketProvider final : public platform::WebSocketProvider {
   public:
    auto connect(const platform::WebSocketRequest&, const platform::StopToken&)
        -> Expected<std::unique_ptr<platform::WebSocketConnection>,
                    platform::WebSocketError> override {
      return std::unique_ptr<platform::WebSocketConnection>{};
    }
  };

  class FakeDiagnosticsProvider final : public platform::DiagnosticsProvider {
   public:
    void write(platform::LogRecord) noexcept override {}
  };

  static constexpr auto unavailable = PlatformCapability::http |
                                      PlatformCapability::websocket |
                                      PlatformCapability::diagnostics;

  FakeThreadingProvider fake_threading;
  FakeClockProvider fake_clock;
  FakeRandomProvider fake_random;
  FakeNetworkProvider fake_network;
  FakeCryptoProvider fake_crypto;
  FakeHttpProvider fake_http;
  FakeWebSocketProvider fake_websocket;
  FakeDiagnosticsProvider fake_diagnostics;
};

TEST_F(ComposedFactory, HandsOutEveryProviderItWasGiven) {
  fake_network.capabilities_.socket_dont_fragment_ipv4 = true;
  DefaultPlatformFactory composed(PlatformProviders{
      .threading = &fake_threading,
      .clock = &fake_clock,
      .random = &fake_random,
      .network = &fake_network,
      .crypto = &fake_crypto,
      .http = &fake_http,
      .websocket = &fake_websocket,
      .diagnostics = &fake_diagnostics,
  });
  PlatformFactory& composed_base = composed;
  EXPECT_EQ(&composed_base.threading(), &fake_threading);
  EXPECT_EQ(&composed_base.clock(), &fake_clock);
  EXPECT_EQ(&composed_base.random(), &fake_random);
  EXPECT_EQ(&composed_base.network(), &fake_network);
  EXPECT_EQ(&composed_base.crypto(), &fake_crypto);
  EXPECT_EQ(&composed_base.http(), &fake_http);
  EXPECT_EQ(&composed_base.websocket(), &fake_websocket);
  EXPECT_EQ(&composed_base.diagnostics(), &fake_diagnostics);

  auto composed_info = composed_base.info();
  const auto required =
      PlatformCapability::threading | PlatformCapability::monotonic_clock |
      PlatformCapability::secure_random | PlatformCapability::network |
      PlatformCapability::crypto | PlatformCapability::http |
      PlatformCapability::websocket | PlatformCapability::diagnostics;
  EXPECT_EQ(composed_info.capabilities & required, required);
  EXPECT_EQ(composed_info.capabilities & PlatformCapability::socket_dontfrag_v4,
            PlatformCapability::socket_dontfrag_v4);

  fake_network.capabilities_.socket_dont_fragment_ipv4 = false;
  fake_network.capabilities_.socket_dont_fragment_ipv6 = true;
  composed_info = composed_base.info();
  EXPECT_EQ(composed_info.capabilities & PlatformCapability::socket_dontfrag_v4,
            PlatformCapability::none);
  EXPECT_EQ(composed_info.capabilities & PlatformCapability::socket_dontfrag_v6,
            PlatformCapability::socket_dontfrag_v6);
}

TEST_F(ComposedFactory, FillsMissingCoreProvidersWithItsOwn) {
  DefaultPlatformFactory mixed(PlatformProviders{.clock = &fake_clock});
  EXPECT_EQ(&mixed.clock(), &fake_clock);
  EXPECT_NE(&mixed.threading(), &fake_threading);
  EXPECT_NE(&mixed.random(), &fake_random);
  EXPECT_NE(&mixed.network(), &fake_network);
  const auto mixed_info = mixed.info();
  EXPECT_EQ(mixed_info.capabilities & PlatformCapability::crypto,
            PlatformCapability::none);
  PlatformFactory& mixed_base = mixed;
  EXPECT_FALSE(mixed_base.supplies(PlatformCapability::crypto));
  EXPECT_FALSE(mixed_base.supplies(PlatformCapability::http));
  EXPECT_FALSE(mixed_base.supplies(PlatformCapability::websocket));
  EXPECT_FALSE(mixed_base.supplies(PlatformCapability::diagnostics));
  EXPECT_EQ(mixed_info.capabilities & unavailable, PlatformCapability::none);
}

TEST_F(ComposedFactory, TheCryptoOnlyConstructorAddsJustCrypto) {
  DefaultPlatformFactory legacy(&fake_crypto);
  PlatformFactory& legacy_base = legacy;
  EXPECT_EQ(&legacy_base.crypto(), &fake_crypto);
  EXPECT_FALSE(legacy_base.supplies(PlatformCapability::http));
  EXPECT_FALSE(legacy_base.supplies(PlatformCapability::websocket));
  EXPECT_FALSE(legacy_base.supplies(PlatformCapability::diagnostics));
  const auto legacy_info = legacy_base.info();
  EXPECT_EQ(legacy_info.capabilities & PlatformCapability::crypto,
            PlatformCapability::crypto);
  EXPECT_EQ(legacy_info.capabilities & unavailable, PlatformCapability::none);
}

}  // namespace
