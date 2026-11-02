#pragma once

#include "src/core/net/protocol/remote_play/session_id.hpp"
#include "src/core/net/protocol/remote_play/stream/local_prepare.hpp"
#include "src/core/net/protocol/remote_play/stream/local_start.hpp"
#include "tests/unit/core/net/protocol/senkusha/client_adapter_rig.hpp"
#include "tests/support/crypto/refusing.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace senkusha_test {

class LocalPipelineKeyPair final : public libchiaking::platform::EcdhKeyPair {
 public:
  explicit LocalPipelineKeyPair(std::size_t size) : size_(size) {}

  auto public_coordinates(std::span<std::byte> output) const
      -> libchiaking::Expected<void,
                               libchiaking::platform::CryptoError> override {
    EXPECT_EQ(output.size(), size_);
    for (std::size_t i = 0; i < output.size(); ++i)
      output[i] = static_cast<std::byte>(i);
    return {};
  }

  auto derive_secret(std::span<const std::byte>, std::span<std::byte>) const
      -> libchiaking::Expected<void,
                               libchiaking::platform::CryptoError> override {
    return libchiaking::unexpected(
        libchiaking::platform::CryptoError::backend_failure);
  }

 private:
  std::size_t size_;
};

class LocalPipelineCrypto final : public libchiaking::platform::CryptoProvider {
 public:
  using CryptoError = libchiaking::platform::CryptoError;

  auto generate_ecdh(libchiaking::platform::EcdhCurve curve)
      -> libchiaking::Expected<
          std::unique_ptr<libchiaking::platform::EcdhKeyPair>,
          CryptoError> override {
    ++generate_calls;
    if (fail_generate)
      return libchiaking::unexpected(CryptoError::backend_failure);
    return std::unique_ptr<libchiaking::platform::EcdhKeyPair>{
        std::make_unique<LocalPipelineKeyPair>(
            libchiaking::platform::ecdh_public_coordinates_size(curve))};
  }

  auto sha256(std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void, CryptoError> override {
    return libchiaking::unexpected(CryptoError::backend_failure);
  }

  auto hmac_sha256(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<std::byte> digest)
      -> libchiaking::Expected<void, CryptoError> override {
    std::ranges::fill(digest, std::byte{0});
    return {};
  }

  auto aes128_encrypt_block(std::span<const std::byte>,
                            std::span<const std::byte>,
                            std::span<std::byte> output)
      -> libchiaking::Expected<void, CryptoError> override {
    std::ranges::fill(output, std::byte{0});
    return {};
  }

  auto aes128_gmac(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void, CryptoError> override {
    return libchiaking::unexpected(CryptoError::backend_failure);
  }

  unsigned generate_calls{};
  bool fail_generate{};
};

class LocalPipelineRig : public SenkushaClientRig {
 protected:
  using RegisteredConsole =
      libchiaking::net::protocol::remote_play::RegisteredConsole;
  using RegistrationTarget =
      libchiaking::net::protocol::remote_play::RegistrationTarget;
  using LocalPipelineInputs =
      libchiaking::net::protocol::remote_play::LocalPipelineInputs;
  using LocalPipelineError =
      libchiaking::net::protocol::remote_play::LocalPipelineError;
  using LocalPipelineState =
      libchiaking::net::protocol::remote_play::LocalPipelineState;

  static auto ready_ticket()
      -> libchiaking::net::protocol::remote_play::LocalCtrlReadyTicket {
    std::vector<std::byte> payload{std::byte{0x4a}};
    constexpr std::string_view value = "ABCdef0123456789ABCdef01";
    for (const char c : value) payload.push_back(static_cast<std::byte>(c));
    auto id =
        libchiaking::net::protocol::remote_play::SessionId::parse(payload);
    EXPECT_TRUE(id);
    libchiaking::SecretArray<16> nonce;
    nonce.fill(std::byte{0x42});
    return {std::move(id.value()), std::move(nonce), 7};
  }

  static auto console(RegistrationTarget target) -> RegisteredConsole {
    RegisteredConsole result;
    result.target = target;
    if (target == RegistrationTarget::ps4_9) result.key.fill(std::byte{0x51});
    return result;
  }

  static auto inputs(SenkushaHarness& harness, LocalPipelineCrypto& crypto,
                     test_support::RefusingTakionCrypto& takion_crypto,
                     const RegisteredConsole& registered)
      -> LocalPipelineInputs {
    return {harness.clock,
            threading(),
            harness.random,
            crypto,
            takion_crypto,
            registered,
            libchiaking::net::protocol::remote_play::VideoCodec::h264,
            {1280, 720, 60, 10000}};
  }
};

}  // namespace senkusha_test
