#pragma once

#include "libchiaking/platform/net/default.hpp"
#include "src/core/net/protocol/takion/versions.hpp"
#include "src/core/net/transport/datagram/udp_transport.hpp"
#include "src/core/play/adapters/links/cloud/server_connect.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/process_threading.hpp"
#include "tests/support/crypto/refusing.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace cloud_test {

namespace platform = libchiaking::platform;
namespace datagram = libchiaking::net::transport::datagram;
namespace takion = libchiaking::net::protocol::takion;
namespace play = libchiaking::play;
using libchiaking::cloud::CloudService;

class FakeCrypto final : public platform::CryptoProvider {
 public:
  auto generate_ecdh(platform::EcdhCurve curve)
      -> libchiaking::Expected<std::unique_ptr<platform::EcdhKeyPair>,
                               platform::CryptoError> override {
    generated_curve = curve;
    if (fail)
      return libchiaking::unexpected(platform::CryptoError::backend_failure);
    return std::unique_ptr<platform::EcdhKeyPair>{new test_support::CountingKeyPair};
  }

  auto sha256(std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void, platform::CryptoError> override {
    return libchiaking::unexpected(platform::CryptoError::backend_failure);
  }

  auto hmac_sha256(std::span<const std::byte> key,
                   std::span<const std::byte> input,
                   std::span<std::byte> output)
      -> libchiaking::Expected<void, platform::CryptoError> override {
    hmac_key.assign(key.begin(), key.end());
    EXPECT_TRUE(input.size() == 65 || input.size() == 133) << input.size();
    EXPECT_EQ(output.size(), std::size_t{32});
    for (auto& byte : output) byte = std::byte{0x5a};
    return {};
  }

  auto aes128_encrypt_block(std::span<const std::byte>,
                            std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void, platform::CryptoError> override {
    return libchiaking::unexpected(platform::CryptoError::backend_failure);
  }

  auto aes128_gmac(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void, platform::CryptoError> override {
    return libchiaking::unexpected(platform::CryptoError::backend_failure);
  }

  platform::EcdhCurve generated_curve{platform::EcdhCurve::secp521r1};
  std::vector<std::byte> hmac_key;
  bool fail{};
};

class TransportState {
 public:
  std::vector<datagram::OwnedDatagram> sent;
  unsigned closes{};
  bool fail_send{};
};

class FakeTransport final : public datagram::DatagramTransport {
 public:
  explicit FakeTransport(std::shared_ptr<TransportState> state)
      : state_(std::move(state)) {}

  auto send(std::span<const std::byte> bytes)
      -> takion::Expected<std::size_t, datagram::TransportError> override {
    if (state_->fail_send)
      return takion::unexpected(datagram::TransportError::io_failure);
    state_->sent.emplace_back(bytes.begin(), bytes.end());
    return bytes.size();
  }

  auto receive() -> takion::Expected<std::optional<datagram::Datagram>,
                                     datagram::TransportError> override {
    return std::optional<datagram::Datagram>{};
  }

  auto close() -> takion::Expected<void, datagram::TransportError> override {
    ++state_->closes;
    return {};
  }

 private:
  std::shared_ptr<TransportState> state_;
};

class ConnectRig {
 public:
  using Reason = play::ConnectError::Reason;
  using Stage = play::ConnectError::Stage;

  static constexpr bool default_versions =
      LIBCHIAKING_TAKION_VERSION_OVERRIDE == 0 &&
      LIBCHIAKING_TAKION_VERSION_LEGACY_CLOUD_STREAM == 12 &&
      LIBCHIAKING_TAKION_VERSION_CLOUD_STREAM == 23;

  static auto allocation() -> libchiaking::psn::gk::Allocation {
    libchiaking::psn::gk::Allocation value;
    value.handshake_key.assign("AAECAwQFBgcICQoLDA0ODw==");
    value.session_id.assign("session-id");
    value.launch_specification.assign("encrypted-launch");
    return value;
  }

  static auto failed_with(const play::ConnectError& error, Stage stage,
                          Reason reason) -> testing::AssertionResult {
    if (error.stage() != stage)
      return testing::AssertionFailure()
             << "failed in stage " << testing::PrintToString(error.stage());
    if (error.reason() != reason)
      return testing::AssertionFailure()
             << "failed for " << testing::PrintToString(error.reason());
    if (!(error == play::ConnectError{reason}))
      return testing::AssertionFailure()
             << "differs from an error built from its reason";
    return testing::AssertionSuccess();
  }

  template <class Value>
  static auto fails_with(
      const libchiaking::Expected<Value, play::ConnectError>& result,
      Stage stage, Reason reason) -> testing::AssertionResult {
    if (result) return testing::AssertionFailure() << "the call succeeded";
    return failed_with(result.error(), stage, reason);
  }

  static auto counts_up(std::span<const std::byte> bytes, std::size_t count)
      -> testing::AssertionResult {
    if (bytes.size() < count)
      return testing::AssertionFailure() << "only " << bytes.size() << " bytes";
    for (std::size_t index = 0; index < count; ++index)
      if (bytes[index] != static_cast<std::byte>(index))
        return testing::AssertionFailure()
               << "byte " << index << " is "
               << std::to_integer<int>(bytes[index]);
    return testing::AssertionSuccess();
  }

  static void pause_briefly() {
    platform::Sleeper{test_support::process_threading(), monotonic()}.sleep_for(
        std::chrono::milliseconds{5});
  }

  static auto monotonic() -> platform::SteadyClock& {
    static platform::SteadyClock clock;
    return clock;
  }

  template <class Transport>
  static auto await_datagram(Transport& transport)
      -> std::optional<datagram::OwnedDatagram> {
    for (int attempt = 0; attempt < 200; ++attempt) {
      auto received = transport.receive();
      if (!received) {
        ADD_FAILURE() << "the receive failed";
        return std::nullopt;
      }
      if (received.value())
        return datagram::OwnedDatagram(received.value()->begin(),
                                       received.value()->end());
      pause_briefly();
    }
    return std::nullopt;
  }
};

class PinnedTakionVersions : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!ConnectRig::default_versions)
      GTEST_SKIP() << "the scripted exchange pins the default Takion versions";
  }
};

}  // namespace cloud_test
