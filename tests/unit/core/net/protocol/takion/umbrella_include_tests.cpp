#include <libchiaking/platform/platform.hpp>
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "src/core/net/protocol/takion/takion.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <span>
#include <utility>

namespace takion_test {
namespace {

namespace takion = libchiaking::net::protocol::takion;
namespace datagram = libchiaking::net::transport::datagram;

class UmbrellaNullTransport final : public datagram::DatagramTransport {
 public:
  auto send(std::span<const std::byte>)
      -> takion::Expected<std::size_t, datagram::TransportError> override {
    return takion::Expected<std::size_t, datagram::TransportError>{
        std::size_t{0}};
  }

  auto receive() -> takion::Expected<std::optional<datagram::Datagram>,
                                     datagram::TransportError> override {
    return takion::Expected<std::optional<datagram::Datagram>,
                            datagram::TransportError>{std::nullopt};
  }

  auto close() -> takion::Expected<void, datagram::TransportError> override {
    return takion::Expected<void, datagram::TransportError>{};
  }
};

class UmbrellaNullSink final : public takion::EventSink {
 public:
  void on_event(takion::Event) override {}
};

static_assert(takion::FeatureSet{}
                  .with(takion::Feature::unresolved_feature_7)
                  .contains(takion::Feature::unresolved_feature_7));

TEST(TakionUmbrellaHeader, DeclaresEnoughToBuildAndDriveAClient) {
  libchiaking::platform::SteadyClock platform_clock;
  libchiaking::platform::DefaultThreadingProvider threading;
  libchiaking::platform::SystemSecureRandom platform_random;
  UmbrellaNullSink sink;
  takion::ClientConfig config;
  config.profile_selection = takion::NegotiatedTakionProfiles{{7, 9}};

  takion::Client client(std::make_unique<UmbrellaNullTransport>(),
                        platform_clock, threading, platform_random, sink,
                        std::move(config));

  takion::TakionCommand command = takion::Stop{};
  (void)client.post(std::move(command));
  (void)client.snapshot();
}

}  // namespace
}  // namespace takion_test
