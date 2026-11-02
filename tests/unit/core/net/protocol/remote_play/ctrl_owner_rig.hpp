#pragma once

#include "libchiaking/platform/net/default.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/net/protocol/remote_play/ctrl_codec.hpp"
#include "src/core/net/protocol/remote_play/ctrl_owner.hpp"
#include "src/core/net/protocol/remote_play/stream/local_ctrl.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/process_threading.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace remote_play_test {

class CtrlOwnerCrypto final : public libchiaking::platform::CryptoProvider {
 public:
  using Result =
      libchiaking::Expected<void, libchiaking::platform::CryptoError>;

  auto sha256(std::span<const std::byte>, std::span<std::byte>)
      -> Result override {
    return failure();
  }

  auto hmac_sha256(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<std::byte> output) -> Result override {
    if (fail_hmac) return failure();
    std::ranges::fill(output, std::byte{0});
    return {};
  }

  auto aes128_encrypt_block(std::span<const std::byte>,
                            std::span<const std::byte>,
                            std::span<std::byte> output) -> Result override {
    std::ranges::fill(output, std::byte{0});
    return {};
  }

  auto aes128_gmac(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<const std::byte>, std::span<std::byte>)
      -> Result override {
    return failure();
  }

  bool fail_hmac{false};

 private:
  static auto failure() -> Result {
    return libchiaking::unexpected(
        libchiaking::platform::CryptoError::backend_failure);
  }
};

class CtrlOwnerScript final {
 public:
  std::vector<std::byte> incoming;
  std::vector<std::optional<std::size_t>> send_limits;
  std::vector<std::byte> sent;
  std::size_t offset{};
  std::size_t closes{};
  bool disconnect{false};
  bool send_fail{false};
  bool close_fail{false};
};

class CtrlOwnerSocket final : public libchiaking::platform::TcpSocket {
 public:
  using NetError = libchiaking::platform::NetError;
  using Endpoint = libchiaking::platform::SocketEndpoint;

  CtrlOwnerSocket(libchiaking::platform::NetworkProvider& owner,
                  CtrlOwnerScript& script)
      : owner_(owner), script_(script) {}

  const libchiaking::platform::NetworkProvider& owner()
      const noexcept override {
    return owner_;
  }

  auto bind(const Endpoint&) -> libchiaking::Expected<void, NetError> override {
    return {};
  }

  auto connect(const Endpoint&)
      -> libchiaking::Expected<void, NetError> override {
    return {};
  }

  auto complete_connect() -> libchiaking::Expected<void, NetError> override {
    return {};
  }

  auto send(std::span<const std::byte> input)
      -> libchiaking::Expected<std::size_t, NetError> override {
    if (script_.send_fail) return libchiaking::unexpected(NetError::closed);
    std::optional<std::size_t> limit = input.size();
    if (!script_.send_limits.empty()) {
      limit = script_.send_limits.front();
      script_.send_limits.erase(script_.send_limits.begin());
    }
    if (!limit) return libchiaking::unexpected(NetError::would_block);
    const auto count = std::min(input.size(), *limit);
    script_.sent.insert(script_.sent.end(), input.begin(),
                        input.begin() + static_cast<std::ptrdiff_t>(count));
    return count;
  }

  auto receive(std::span<std::byte> output)
      -> libchiaking::Expected<std::size_t, NetError> override {
    if (script_.offset == script_.incoming.size())
      return libchiaking::unexpected(
          script_.disconnect ? NetError::closed : NetError::would_block);
    const auto count =
        std::min(output.size(), script_.incoming.size() - script_.offset);
    std::copy_n(
        script_.incoming.begin() + static_cast<std::ptrdiff_t>(script_.offset),
        count, output.begin());
    script_.offset += count;
    return count;
  }

  auto local_endpoint() -> libchiaking::Expected<Endpoint, NetError> override {
    return Endpoint{};
  }

  auto peer_endpoint() -> libchiaking::Expected<Endpoint, NetError> override {
    return Endpoint{};
  }

  auto close() -> libchiaking::Expected<void, NetError> override {
    ++script_.closes;
    if (script_.close_fail)
      return libchiaking::unexpected(NetError::system_failure);
    return {};
  }

 private:
  libchiaking::platform::NetworkProvider& owner_;
  CtrlOwnerScript& script_;
};

class CtrlOwnerRig : public ::testing::Test {
 protected:
  using Effect =
      libchiaking::net::protocol::remote_play::ctrl_lifecycle_detail::Effect;
  using Owner = libchiaking::net::protocol::remote_play::CtrlOwner;
  using Reader = libchiaking::net::protocol::remote_play::CtrlFrameReader;
  using Target = libchiaking::net::protocol::remote_play::RegistrationTarget;
  using LocalSession =
      libchiaking::net::protocol::remote_play::LocalCtrlSessionId;

  static constexpr std::array<std::byte, 4> pin{std::byte{'1'}, std::byte{'2'},
                                                std::byte{'3'}, std::byte{'4'}};
  static constexpr std::array<std::byte, 4> mic_payload{
      std::byte{0}, std::byte{1}, std::byte{1}, std::byte{89}};

  auto reader(CtrlOwnerScript& script, CtrlOwnerCrypto& with,
              Target target = Target::ps5_1, std::uint64_t local_counter = 5)
      -> std::unique_ptr<Reader> {
    auto result = std::make_unique<Reader>(with);
    const auto begun = result->begin(
        std::make_unique<CtrlOwnerSocket>(network, script), target,
        libchiaking::net::protocol::remote_play::AuthMaterial{}, 0,
        local_counter);
    EXPECT_TRUE(begun);
    if (!begun) return nullptr;
    return result;
  }

  auto owner(CtrlOwnerScript& script, CtrlOwnerCrypto& with,
             Target target = Target::ps5_1, std::uint64_t local_counter = 5)
      -> std::unique_ptr<Owner> {
    auto begun = reader(script, with, target, local_counter);
    if (!begun) return nullptr;
    return std::make_unique<Owner>(std::move(begun));
  }

  static auto local_session(std::unique_ptr<Owner> started,
                            std::uint64_t attempt,
                            std::optional<std::byte> nonce = std::nullopt)
      -> LocalSession {
    libchiaking::net::protocol::remote_play::LocalCtrlStartupResult startup;
    startup.handoff.owner = std::move(started);
    if (nonce) startup.nonce.fill(*nonce);
    return LocalSession(std::move(startup), attempt);
  }

  static auto frame(std::uint16_t type, std::span<const std::byte> payload)
      -> std::vector<std::byte> {
    const auto header =
        libchiaking::net::protocol::remote_play::CtrlCodec::encode_frame_header(
            type, payload.size());
    EXPECT_TRUE(header);
    if (!header) return {};
    std::vector<std::byte> result(header.value().begin(), header.value().end());
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
  }

  static auto frames(std::initializer_list<
                     std::pair<std::uint16_t, std::span<const std::byte>>>
                         parts) -> std::vector<std::byte> {
    std::vector<std::byte> result;
    for (const auto& [type, payload] : parts) {
      const auto one = frame(type, payload);
      result.insert(result.end(), one.begin(), one.end());
    }
    return result;
  }

  static auto session_id_frame() -> std::vector<std::byte> {
    std::vector<std::byte> payload{std::byte{0x4a}};
    payload.insert(payload.end(), 24, std::byte{'A'});
    return frame(0x33, payload);
  }

  template <class T>
  static auto is(const std::optional<Effect>& effect)
      -> testing::AssertionResult {
    if (!effect) return testing::AssertionFailure() << "there is no effect";
    if (!std::holds_alternative<T>(*effect))
      return testing::AssertionFailure()
             << "the effect is alternative " << effect->index();
    return testing::AssertionSuccess();
  }

  template <class T>
  static auto is(const Effect* effect) -> testing::AssertionResult {
    if (!effect) return testing::AssertionFailure() << "there is no effect";
    if (!std::holds_alternative<T>(*effect))
      return testing::AssertionFailure()
             << "the effect is alternative " << effect->index();
    return testing::AssertionSuccess();
  }

  libchiaking::platform::DefaultNetworkProvider network{
      test_support::process_threading(), steady};
  CtrlOwnerCrypto crypto;

 private:
  static inline const libchiaking::platform::SteadyClock steady{};
};

}  // namespace remote_play_test
