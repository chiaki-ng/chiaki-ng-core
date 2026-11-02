#include "tests/support/enum_printing.hpp"
#include "tests/support/process_threading.hpp"
#include "tests/support/stop_states.hpp"

#include <libchiaking/platform/gateway/provider.hpp>

#include <gtest/gtest.h>
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>

namespace gateway_test {
namespace {

namespace pf = libchiaking::platform;

class FakeUpnp {
 public:
  static auto instance() -> FakeUpnp& {
    static FakeUpnp upnp;
    return upnp;
  }

  void reset() {
    {
      pf::Lock lock{mutex_};
      block_discovery_ = false;
      block_mapping_ = false;
      entered_ = false;
      freed_ = 0;
    }
    changed_.notify_all();
    deleted_ = 0;
    external_result_ = 0;
  }

  void hold_discovery(bool held) {
    {
      pf::Lock lock{mutex_};
      block_discovery_ = held;
      if (held) entered_ = false;
    }
    changed_.notify_all();
  }

  void hold_mapping(bool held) {
    {
      pf::Lock lock{mutex_};
      block_mapping_ = held;
      if (held) entered_ = false;
    }
    changed_.notify_all();
  }

  void answer_external_address_with(int result) { external_result_ = result; }

  auto wait_entered() -> testing::AssertionResult {
    pf::Lock lock{mutex_};
    if (changed_.wait_for(lock, std::chrono::seconds{1},
                          [this] { return entered_; }))
      return testing::AssertionSuccess();
    return testing::AssertionFailure()
           << "no gateway call was entered within a second";
  }

  auto wait_freed(unsigned count) -> testing::AssertionResult {
    pf::Lock lock{mutex_};
    if (changed_.wait_for(lock, std::chrono::seconds{1},
                          [this, count] { return freed_ >= count; }))
      return testing::AssertionSuccess();
    return testing::AssertionFailure()
           << freed_ << " gateway states were freed within a second, not "
           << count;
  }

  auto freed() -> unsigned {
    pf::Lock lock{mutex_};
    return freed_;
  }

  auto deleted() const -> unsigned { return deleted_.load(); }

  auto discover(int delay, const char* iface, const char* ssdp, int port,
                int ipv6, unsigned char ttl, int* error) -> UPNPDev* {
    EXPECT_EQ(delay, 2000);
    EXPECT_EQ(iface, nullptr);
    EXPECT_EQ(ssdp, nullptr);
    EXPECT_EQ(port, 0);
    EXPECT_EQ(ipv6, 0);
    EXPECT_EQ(static_cast<int>(ttl), 2);
    pf::Lock lock{mutex_};
    entered_ = true;
    changed_.notify_all();
    changed_.wait(lock, [this] { return !block_discovery_; });
    *error = UPNPDISCOVER_SUCCESS;
    return static_cast<UPNPDev*>(std::calloc(1, sizeof(UPNPDev)));
  }

  static void free_devices(UPNPDev* list) { std::free(list); }

  static auto valid_igd(UPNPUrls* urls, IGDdatas* data, char* lan) -> int {
    urls->controlURL = nullptr;
    std::strcpy(data->first.servicetype, "test-service");
    std::strcpy(lan, "192.0.2.10");
    return 1;
  }

  void free_urls() {
    {
      pf::Lock lock{mutex_};
      ++freed_;
    }
    changed_.notify_all();
  }

  auto add_port_mapping(const char* service, const char* external,
                        const char* internal, const char* client,
                        const char* description, const char* protocol,
                        const char* remote, const char* duration) -> int {
    EXPECT_STREQ(service, "test-service");
    EXPECT_STREQ(external, internal);
    EXPECT_STREQ(client, "192.0.2.10");
    EXPECT_STREQ(description, "Chiaki Streaming");
    EXPECT_STREQ(protocol, "UDP");
    EXPECT_EQ(remote, nullptr);
    EXPECT_STREQ(duration, "0");
    pf::Lock lock{mutex_};
    entered_ = true;
    changed_.notify_all();
    changed_.wait(lock, [this] { return !block_mapping_; });
    return 0;
  }

  auto external_address(char* address) const -> int {
    std::strcpy(address, "203.0.113.8");
    return external_result_.load();
  }

  auto delete_port_mapping(const char* port, const char* protocol,
                           const char* remote) -> int {
    EXPECT_GT(std::atoi(port), 0);
    EXPECT_STREQ(protocol, "UDP");
    EXPECT_EQ(remote, nullptr);
    ++deleted_;
    return 0;
  }

 private:
  FakeUpnp() = default;

  pf::SteadyClock steady_;
  pf::Mutex mutex_{test_support::process_threading()};
  pf::ConditionVariable changed_{test_support::process_threading(), steady_};
  bool block_discovery_{};
  bool block_mapping_{};
  bool entered_{};
  unsigned freed_{};
  std::atomic<unsigned> deleted_{};
  std::atomic<int> external_result_{};
};

class MiniupnpcGateway : public ::testing::Test {
 protected:
  void SetUp() override { upnp.reset(); }

  void TearDown() override {
    upnp.hold_discovery(false);
    upnp.hold_mapping(false);
  }

  template <class Work>
  auto started(Work work) -> pf::Thread {
    auto thread =
        pf::Thread::start(threading, "gateway test",
                          pf::ThreadRole::unspecified, std::move(work));
    EXPECT_TRUE(thread);
    return std::move(thread).value();
  }

  auto now() const -> std::chrono::microseconds {
    return std::chrono::microseconds{
        static_cast<std::int64_t>(steady.now_microseconds())};
  }

  auto stopper(const std::shared_ptr<test_support::AtomicStop>& stop)
      -> pf::Thread {
    return started([this, stop] {
      EXPECT_TRUE(upnp.wait_entered());
      stop->stopped = true;
    });
  }

  FakeUpnp& upnp = FakeUpnp::instance();
  pf::DefaultThreadingProvider& threading = test_support::process_threading();
  pf::SteadyClock steady;
  std::unique_ptr<pf::GatewayProvider> provider =
      pf::make_gateway_provider(threading, steady);
};

TEST_F(MiniupnpcGateway, DiscoversTheLocalAddressAndMapsUdpPorts) {
  ASSERT_TRUE(provider);
  {
    auto gateway = provider->discover({});
    ASSERT_TRUE(gateway);
    EXPECT_EQ(gateway.value()->local_address(),
              pf::IpAddress::ipv4({192, 0, 2, 10}));
    auto ctrl = gateway.value()->map_udp(40000, {});
    auto data = gateway.value()->map_udp(40001, {});
    ASSERT_TRUE(ctrl);
    ASSERT_TRUE(data);
    EXPECT_EQ(ctrl.value().port, 40000);
    EXPECT_EQ(data.value().port, 40001);
    EXPECT_EQ(ctrl.value().address, pf::IpAddress::ipv4({203, 0, 113, 8}));
  }
  EXPECT_TRUE(upnp.wait_freed(1));
}

TEST_F(MiniupnpcGateway, RemovesEachMappingOnceWhenClosedAndRefusesNewOnes) {
  {
    auto gateway = provider->discover({});
    ASSERT_TRUE(gateway);
    EXPECT_TRUE(gateway.value()->map_udp(40000, {}));
    EXPECT_TRUE(gateway.value()->map_udp(40001, {}));
    EXPECT_TRUE(gateway.value()->close());
    EXPECT_TRUE(gateway.value()->close());
    EXPECT_EQ(upnp.deleted(), 2U);
    EXPECT_FALSE(gateway.value()->map_udp(40002, {}));
  }
  EXPECT_TRUE(upnp.wait_freed(1));
}

TEST_F(MiniupnpcGateway, ReportsANetworkErrorWhenTheExternalAddressFails) {
  {
    auto gateway = provider->discover({});
    ASSERT_TRUE(gateway);
    upnp.answer_external_address_with(501);
    auto failed = gateway.value()->map_udp(40002, {});
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error(), pf::GatewayError::network);
    EXPECT_TRUE(gateway.value()->close());
    EXPECT_EQ(upnp.deleted(), 1U);
  }
  EXPECT_TRUE(upnp.wait_freed(1));
}

TEST_F(MiniupnpcGateway, StopsWaitingForADiscoveryWhenStopped) {
  auto stop = std::make_shared<test_support::AtomicStop>();
  upnp.hold_discovery(true);
  auto cancel = stopper(stop);
  const auto before = now();
  auto cancelled = provider->discover(pf::StopToken{stop});
  static_cast<void>(cancel.join());
  ASSERT_FALSE(cancelled);
  EXPECT_EQ(cancelled.error(), pf::GatewayError::cancelled);
  EXPECT_LT(now() - before, std::chrono::seconds{1});
  upnp.hold_discovery(false);
  EXPECT_TRUE(upnp.wait_freed(1));
}

TEST_F(MiniupnpcGateway, StopsWaitingForAMappingWhenStopped) {
  {
    auto gateway = provider->discover({});
    ASSERT_TRUE(gateway);
    auto stop = std::make_shared<test_support::AtomicStop>();
    upnp.hold_mapping(true);
    auto cancel_map = stopper(stop);
    auto mapped = gateway.value()->map_udp(40003, pf::StopToken{stop});
    static_cast<void>(cancel_map.join());
    ASSERT_FALSE(mapped);
    EXPECT_EQ(mapped.error(), pf::GatewayError::cancelled);
    upnp.hold_mapping(false);
    EXPECT_TRUE(gateway.value()->close());
    EXPECT_EQ(upnp.deleted(), 1U);
  }
  EXPECT_TRUE(upnp.wait_freed(1));
}

TEST_F(MiniupnpcGateway, TimesOutASlowDiscovery) {
  upnp.hold_discovery(true);
  auto timeout = provider->discover({}, std::chrono::milliseconds{10});
  ASSERT_FALSE(timeout);
  EXPECT_EQ(timeout.error(), pf::GatewayError::timeout);
  upnp.hold_discovery(false);
  EXPECT_TRUE(upnp.wait_freed(1));
}

TEST_F(MiniupnpcGateway, RemovesItsMappingWhenTheGatewayAndProviderGo) {
  {
    auto gateway = provider->discover({});
    ASSERT_TRUE(gateway);
    EXPECT_TRUE(gateway.value()->map_udp(40004, {}));
    const unsigned deleted_before = upnp.deleted();
    gateway.value().reset();
    provider.reset();
    EXPECT_EQ(upnp.deleted(), deleted_before + 1);
  }
  EXPECT_TRUE(upnp.wait_freed(1));
}

TEST_F(MiniupnpcGateway, WaitsForAnAbandonedDiscoveryWhenTheProviderGoes) {
  auto late_stop = std::make_shared<test_support::AtomicStop>();
  upnp.hold_discovery(true);
  auto cancel_late = stopper(late_stop);
  auto abandoned = provider->discover(pf::StopToken{late_stop});
  static_cast<void>(cancel_late.join());
  ASSERT_FALSE(abandoned);
  EXPECT_EQ(abandoned.error(), pf::GatewayError::cancelled);
  auto release = started([this] {
    pf::Sleeper sleeper{threading, steady};
    static_cast<void>(sleeper.sleep_for(std::chrono::milliseconds{50}));
    upnp.hold_discovery(false);
  });
  provider.reset();
  EXPECT_EQ(upnp.freed(), 1U);
  static_cast<void>(release.join());
}

}  // namespace
}  // namespace gateway_test

extern "C" {
UPNPDev* upnpDiscover(int delay, const char* iface, const char* ssdp, int port,
                      int ipv6, unsigned char ttl, int* error) {
  return gateway_test::FakeUpnp::instance().discover(delay, iface, ssdp, port,
                                                     ipv6, ttl, error);
}

void freeUPNPDevlist(UPNPDev* list) {
  gateway_test::FakeUpnp::free_devices(list);
}

#if MINIUPNPC_API_VERSION >= 18
int UPNP_GetValidIGD(UPNPDev*, UPNPUrls* urls, IGDdatas* data, char* lan, int,
                     char*, int)
#else
int UPNP_GetValidIGD(UPNPDev*, UPNPUrls* urls, IGDdatas* data, char* lan, int)
#endif
{
  return gateway_test::FakeUpnp::valid_igd(urls, data, lan);
}

void FreeUPNPUrls(UPNPUrls*) { gateway_test::FakeUpnp::instance().free_urls(); }

int UPNP_AddPortMapping(const char*, const char* service, const char* external,
                        const char* internal, const char* client,
                        const char* description, const char* protocol,
                        const char* remote, const char* duration) {
  return gateway_test::FakeUpnp::instance().add_port_mapping(
      service, external, internal, client, description, protocol, remote,
      duration);
}

int UPNP_GetExternalIPAddress(const char*, const char*, char* address) {
  return gateway_test::FakeUpnp::instance().external_address(address);
}

int UPNP_DeletePortMapping(const char*, const char*, const char* port,
                           const char* protocol, const char* remote) {
  return gateway_test::FakeUpnp::instance().delete_port_mapping(port, protocol,
                                                                remote);
}
}
