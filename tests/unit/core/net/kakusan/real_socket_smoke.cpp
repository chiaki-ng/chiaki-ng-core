#include "src/core/net/kakusan/protocol_error.hpp"
#include "libchiaking/platform/net/default.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/net/kakusan/result.hpp"
#include "src/core/net/kakusan/udp_transport_factory.hpp"
#include "src/core/utils/timing/deadline.hpp"
#include "tests/support/crypto/software_crypto.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/kakusan/secure_datagrams.hpp"
#include "tests/support/random.hpp"

#if defined(WIRE_TEST_DTLS_OPENSSL)
#include "libchiaking/platform/dtls/openssl/provider.hpp"
#else
#include "libchiaking/testing/fake_secure_datagram.hpp"
#endif

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

namespace net_kakusan_test {
namespace {

namespace kakusan = libchiaking::net::kakusan;
namespace platform = libchiaking::platform;

class RealSockets : public ::testing::Test {
 protected:
#if defined(WIRE_TEST_DTLS_OPENSSL)
  using Datagrams = test_support::kakusan::SecureDatagrams<
      platform::openssl::OpenSslSecureDatagramProvider>;
#else
  using Datagrams = test_support::kakusan::SecureDatagrams<
      libchiaking::testing::FakeSecureDatagramProvider>;
#endif
  using Bytes = std::vector<std::byte>;

  static constexpr std::uint64_t ms = 1000;
  static constexpr std::uint64_t second = 1'000'000;

  class Side {
   public:
    std::unique_ptr<kakusan::SpokeTransport> transport;
    std::optional<kakusan::TransportDescription> local;
    std::vector<Bytes> records;
    std::optional<kakusan::Error> failure;
    bool established{};
  };

  static auto pattern(std::size_t size, std::uint8_t seed) -> Bytes {
    Bytes bytes(size);
    for (std::size_t index = 0; index < size; ++index)
      bytes[index] =
          static_cast<std::byte>((index * 29U + seed * 7U + 1U) & 0xffU);
    return bytes;
  }

  auto usable_real_interface() -> bool {
    const auto listed = network.interfaces().enumerate();
    if (!listed) return false;
    return std::ranges::any_of(listed.value(), [](const auto& info) {
      if (!info.operational || info.loopback || !info.address.is_ipv4() ||
          info.address.is_unspecified() || info.address.is_loopback())
        return false;
      const auto bytes = info.address.ipv4_bytes();
      return !(bytes[0] == 169 && bytes[1] == 254) && bytes[0] < 224;
    });
  }

  auto clock() const -> std::uint64_t {
    return steady.now_microseconds() - origin + 1;
  }

  void pump(Side& side) {
    std::vector<kakusan::TransportEvent> events;
    EXPECT_TRUE(side.transport->pump(clock(), events));
    for (auto& event : events) {
      if (const auto* gathered =
              std::get_if<kakusan::transport_event::Gathered>(&event))
        side.local = gathered->local.clone();
      else if (std::holds_alternative<kakusan::transport_event::Established>(
                   event))
        side.established = true;
      else if (auto* record =
                   std::get_if<kakusan::transport_event::Record>(&event))
        side.records.emplace_back(record->bytes.begin(), record->bytes.end());
      else if (const auto* failed =
                   std::get_if<kakusan::transport_event::Failed>(&event))
        side.failure = failed->error;
    }
  }

  test_support::SoftwareCrypto crypto;
  platform::DefaultThreadingProvider threading;
  const platform::SteadyClock steady;
  platform::DefaultNetworkProvider network{threading, steady};
  std::uint64_t origin{steady.now_microseconds()};
};

TEST_F(RealSockets, CarryARecordBetweenTwoLocalTransports) {
  if (!usable_real_interface())
    GTEST_SKIP() << "no operational non-loopback IPv4 interface qualifies as "
                    "a candidate";
  test_support::SeededRandom entropy{steady.now_microseconds()};
  kakusan::UdpTransportFactory factory{
      kakusan::UdpTransportDependencies{network, crypto, entropy,
                                        Datagrams::factory()},
      kakusan::UdpTransportConfig{}};
  std::array<Side, 2> sides{};
  auto host = factory.create(kakusan::TransportRole::host);
  auto guest = factory.create(kakusan::TransportRole::guest);
  ASSERT_TRUE(host);
  ASSERT_TRUE(guest);
  sides[0].transport = std::move(host).value();
  sides[1].transport = std::move(guest).value();
  for (auto& side : sides)
    EXPECT_TRUE(side.transport->gather(clock(), std::nullopt));
  const auto message = pattern(256, 77);
  bool connected = false;
  bool sent = false;
  while (clock() < 4 * second && sides[1].records.empty()) {
    for (auto& side : sides) pump(side);
    if (!sides[0].local && sides[0].failure == kakusan::Error::no_path)
      GTEST_SKIP() << "no local candidate could be gathered on a real "
                      "interface";
    ASSERT_FALSE(sides[0].failure);
    ASSERT_FALSE(sides[1].failure);
    if (!connected && sides[0].local && sides[1].local) {
      EXPECT_TRUE(sides[1].transport->connect(*sides[0].local, clock()));
      EXPECT_TRUE(sides[0].transport->connect(*sides[1].local, clock()));
      connected = true;
    }
    if (!sent && sides[0].established && sides[1].established) {
      EXPECT_TRUE(sides[0].transport->send_record(message));
      sent = true;
    }
    const auto deadline = libchiaking::timing::Deadline::earliest(
        sides[0].transport->next_deadline_us(),
        sides[1].transport->next_deadline_us());
    const auto now = clock();
    std::uint64_t timeout = 20 * ms;
    if (deadline)
      timeout = std::min(timeout, *deadline > now ? *deadline - now : 0);
    static_cast<void>(factory.wait(timeout));
  }
  EXPECT_TRUE(sides[0].established);
  EXPECT_TRUE(sides[1].established);
  EXPECT_EQ(sides[1].records, std::vector<Bytes>{message});
  for (auto& side : sides) side.transport->close();
  EXPECT_LT(clock(), 5 * second);
}

}  // namespace
}  // namespace net_kakusan_test
