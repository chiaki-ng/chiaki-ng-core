#pragma once

#include "libchiaking/platform/default_factory.hpp"
#include "src/core/net/transport/datagram/udp_transport.hpp"
#include "tests/support/enum_printing.hpp"

#include <libchiaking/platform/platform.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace datagram_test {

class UdpLoopbackRig : public ::testing::Test {
 protected:
  using Transport = libchiaking::net::transport::datagram::ProviderUdpTransport;
  using Bytes = std::vector<std::byte>;

  static auto loopback_config()
      -> libchiaking::net::transport::datagram::UdpTransportConfig {
    libchiaking::net::transport::datagram::UdpTransportConfig config{};
    config.local = libchiaking::platform::SocketEndpoint{
        libchiaking::platform::IpAddress::loopback_ipv4(), 0};
    return config;
  }

  static auto ready(Transport& transport,
                    const libchiaking::platform::StopToken& stop) -> bool {
    const auto readable =
        transport.wait_readable(std::chrono::milliseconds{500}, stop);
    return readable && readable.value();
  }

  static auto holds(
      const libchiaking::net::transport::datagram::Datagram& datagram,
      const Bytes& expected) -> testing::AssertionResult {
    const std::span<const std::byte> received{datagram.data(), datagram.size()};
    if (received.size() != expected.size())
      return testing::AssertionFailure()
             << "received " << received.size() << " bytes rather than "
             << expected.size();
    if (!std::equal(received.begin(), received.end(), expected.begin()))
      return testing::AssertionFailure() << "received different bytes";
    return testing::AssertionSuccess();
  }

  auto delivers(Transport& sender, Transport& receiver, const Bytes& payload)
      -> testing::AssertionResult {
    const auto sent = sender.send(payload);
    if (!sent) return testing::AssertionFailure() << "the send failed";
    if (sent.value() != payload.size())
      return testing::AssertionFailure()
             << "sent " << sent.value() << " of " << payload.size() << " bytes";
    if (!ready(receiver, stop))
      return testing::AssertionFailure() << "nothing became readable";
    const auto received = receiver.receive();
    if (!received) return testing::AssertionFailure() << "the receive failed";
    if (!received.value().has_value())
      return testing::AssertionFailure() << "the receive found nothing";
    return holds(received.value().value(), payload);
  }

  void open_pair() {
    auto made_first = Transport::create(factory.network(), loopback_config());
    auto made_second = Transport::create(factory.network(), loopback_config());
    ASSERT_TRUE(made_first);
    ASSERT_TRUE(made_second);
    first = std::move(made_first.value());
    second = std::move(made_second.value());
    const auto first_endpoint = first->local_endpoint();
    const auto second_endpoint = second->local_endpoint();
    ASSERT_TRUE(first_endpoint);
    ASSERT_TRUE(second_endpoint);
    first_at = first_endpoint.value();
    EXPECT_NE(first_endpoint.value().port, 0);
    EXPECT_NE(second_endpoint.value().port, 0);
    ASSERT_TRUE(first->set_peer(second_endpoint.value()));
    ASSERT_TRUE(second->set_peer(first_endpoint.value()));
  }

  libchiaking::platform::DefaultPlatformFactory factory;
  libchiaking::platform::StopToken stop;
  std::unique_ptr<Transport> first;
  std::unique_ptr<Transport> second;
  libchiaking::platform::SocketEndpoint first_at{};
};

}  // namespace datagram_test
