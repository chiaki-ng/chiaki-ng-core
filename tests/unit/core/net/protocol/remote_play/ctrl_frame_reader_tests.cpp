#include "tests/unit/core/net/protocol/remote_play/ctrl_loopback_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace remote_play_test {
namespace {

namespace platform = libchiaking::platform;
namespace remote_play = libchiaking::net::protocol::remote_play;
using remote_play::CtrlReaderError;
using remote_play::RegistrationTarget;

static_assert(!std::is_copy_constructible_v<remote_play::CtrlPlainFrame>);

class ReaderCrypto final : public platform::CryptoProvider {
 public:
  using Result = libchiaking::Expected<void, platform::CryptoError>;

  auto sha256(std::span<const std::byte>, std::span<std::byte>)
      -> Result override {
    return libchiaking::unexpected(platform::CryptoError::backend_failure);
  }

  auto hmac_sha256(std::span<const std::byte>, std::span<const std::byte> input,
                   std::span<std::byte> digest) -> Result override {
    EXPECT_EQ(input.size(), 24U);
    if (fail_hmac)
      return libchiaking::unexpected(platform::CryptoError::backend_failure);
    if (input.size() > 23)
      counters.push_back(std::to_integer<unsigned>(input[23]));
    std::ranges::fill(digest, std::byte{0});
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
    return libchiaking::unexpected(platform::CryptoError::backend_failure);
  }

  std::vector<unsigned> counters;
  bool fail_hmac{false};
};

class SendScript final {
 public:
  std::vector<std::optional<std::size_t>> limits;
  bool fail_once{false};
  std::vector<std::byte> output;
  std::size_t closes{};
};

class SendScriptSocket final : public platform::TcpSocket {
 public:
  using NetError = platform::NetError;

  SendScriptSocket(platform::NetworkProvider& owner, SendScript& script)
      : owner_(owner), script_(script) {}

  const platform::NetworkProvider& owner() const noexcept override {
    return owner_;
  }

  auto bind(const platform::SocketEndpoint&)
      -> libchiaking::Expected<void, NetError> override {
    return {};
  }

  auto connect(const platform::SocketEndpoint&)
      -> libchiaking::Expected<void, NetError> override {
    return {};
  }

  auto complete_connect() -> libchiaking::Expected<void, NetError> override {
    return {};
  }

  auto send(std::span<const std::byte> data)
      -> libchiaking::Expected<std::size_t, NetError> override {
    if (std::exchange(script_.fail_once, false))
      return libchiaking::unexpected(NetError::connection_reset);
    std::optional<std::size_t> limit = data.size();
    if (!script_.limits.empty()) {
      limit = script_.limits.front();
      script_.limits.erase(script_.limits.begin());
    }
    if (!limit) return libchiaking::unexpected(NetError::would_block);
    const auto count = std::min(data.size(), *limit);
    script_.output.insert(script_.output.end(), data.begin(),
                          data.begin() + static_cast<std::ptrdiff_t>(count));
    return count;
  }

  auto receive(std::span<std::byte>)
      -> libchiaking::Expected<std::size_t, NetError> override {
    return libchiaking::unexpected(NetError::would_block);
  }

  auto local_endpoint()
      -> libchiaking::Expected<platform::SocketEndpoint, NetError> override {
    return platform::SocketEndpoint{};
  }

  auto peer_endpoint()
      -> libchiaking::Expected<platform::SocketEndpoint, NetError> override {
    return platform::SocketEndpoint{};
  }

  auto close() -> libchiaking::Expected<void, NetError> override {
    ++script_.closes;
    return {};
  }

 private:
  platform::NetworkProvider& owner_;
  SendScript& script_;
};

class CtrlFrameReader : public CtrlLoopbackRig {
 protected:
  class Pair final {
   public:
    std::unique_ptr<Listener> listener;
    std::unique_ptr<Socket> writer;
    std::unique_ptr<Socket> reader;
  };

  auto connect(Pair& into) -> testing::AssertionResult {
    platform::SocketEndpoint endpoint;
    if (auto listening = listen(into.listener, endpoint); !listening)
      return listening;
    auto client = network.sockets().create_tcp_socket(
        platform::IpAddress::loopback_ipv4());
    if (!client) return testing::AssertionFailure() << "no client socket";
    into.writer = std::move(client.value());
    const auto connected = into.writer->connect(endpoint);
    if (!connected && connected.error() != NetError::would_block)
      return testing::AssertionFailure() << "connect failed";
    const auto deadline = deadline_after(std::chrono::seconds{2});
    while (!into.reader && still_before(deadline)) {
      auto accepted = into.listener->accept();
      if (accepted)
        into.reader = std::move(accepted.value());
      else if (accepted.error() != NetError::would_block)
        return testing::AssertionFailure() << "accept failed";
      if (!connected) {
        const auto completed = into.writer->complete_connect();
        if (!completed && completed.error() != NetError::would_block)
          return testing::AssertionFailure() << "connect did not complete";
      }
      nap();
    }
    if (!into.reader) return testing::AssertionFailure() << "nothing accepted";
    return testing::AssertionSuccess();
  }

  static auto header(std::uint16_t type, std::size_t size)
      -> std::vector<std::byte> {
    const auto encoded =
        remote_play::CtrlCodec::encode_frame_header(type, size);
    EXPECT_TRUE(encoded);
    if (!encoded) return {};
    return {encoded.value().begin(), encoded.value().end()};
  }

  static auto wait_frame(remote_play::CtrlFrameReader& from)
      -> std::optional<remote_play::CtrlPlainFrame> {
    const auto deadline = deadline_after(std::chrono::seconds{2});
    while (still_before(deadline)) {
      auto frame = from.pump_once();
      EXPECT_TRUE(frame);
      if (!frame) return std::nullopt;
      if (frame.value()) return std::move(*frame.value());
      nap();
    }
    ADD_FAILURE() << "no frame arrived";
    return std::nullopt;
  }

  static void pump_send_until_done(remote_play::CtrlFrameReader& transport) {
    const auto deadline = deadline_after(std::chrono::seconds{2});
    bool done = false;
    while (!done && still_before(deadline)) {
      const auto part = transport.pump_send();
      ASSERT_TRUE(part);
      done = part.value();
      nap();
    }
    ASSERT_TRUE(done);
  }

  static void expect_reader_error(remote_play::CtrlFrameReader& from,
                                  CtrlReaderError expected) {
    const auto deadline = deadline_after(std::chrono::seconds{2});
    bool failed = false;
    while (!failed && still_before(deadline)) {
      const auto result = from.pump_once();
      if (!result) {
        EXPECT_EQ(result.error(), expected);
        failed = true;
      }
      nap();
    }
    EXPECT_TRUE(failed);
  }

  Pair pair;
  ReaderCrypto crypto;
  remote_play::CtrlFrameReader reader{crypto};
};

TEST_F(CtrlFrameReader, ReadsFragmentedAndCoalescedFrames) {
  ASSERT_TRUE(connect(pair));
  ASSERT_TRUE(reader.begin(std::move(pair.reader), RegistrationTarget::ps5_1,
                           remote_play::AuthMaterial{}, 7, 5));
  auto wire = header(0xfe, 0);
  const auto id = header(0x33, 25);
  wire.insert(wire.end(), id.begin(), id.end());
  wire.push_back(std::byte{0x4a});
  wire.insert(wire.end(), 24, std::byte{'A'});
  const auto login = header(0x5, 1);
  wire.insert(wire.end(), login.begin(), login.end());
  wire.push_back(std::byte{0});
  ASSERT_NO_FATAL_FAILURE(send_all(*pair.writer, std::span(wire).first(4)));
  const auto partial = reader.pump_once();
  ASSERT_TRUE(partial);
  EXPECT_FALSE(partial.value());
  ASSERT_NO_FATAL_FAILURE(send_all(*pair.writer, std::span(wire).subspan(4)));
  const auto first = wait_frame(reader);
  ASSERT_TRUE(first);
  EXPECT_EQ(first->type(), 0xfe);
  EXPECT_TRUE(first->payload().empty());
  EXPECT_EQ(reader.remote_counter(), 7U);
  const auto second = wait_frame(reader);
  ASSERT_TRUE(second);
  EXPECT_EQ(second->type(), 0x33);
  ASSERT_EQ(second->payload().size(), 25U);
  EXPECT_EQ(second->payload()[0], std::byte{0x4a});
  EXPECT_EQ(second->payload()[24], std::byte{'A'});
  EXPECT_EQ(reader.remote_counter(), 8U);
  const auto third = wait_frame(reader);
  ASSERT_TRUE(third);
  EXPECT_EQ(third->type(), 0x5);
  ASSERT_EQ(third->payload().size(), 1U);
  EXPECT_EQ(third->payload()[0], std::byte{0});
  EXPECT_EQ(reader.remote_counter(), 9U);
  EXPECT_EQ(crypto.counters, (std::vector<unsigned>{7, 8}));
  EXPECT_TRUE(reader.cancel());
  EXPECT_TRUE(reader.cancel());
  EXPECT_FALSE(reader.pump_once());
  EXPECT_TRUE(pair.writer->close());
  EXPECT_TRUE(pair.listener->close());
}

TEST_F(CtrlFrameReader, FailsOnAnOversizedFrame) {
  ASSERT_TRUE(connect(pair));
  ASSERT_TRUE(reader.begin(std::move(pair.reader), RegistrationTarget::ps4_9,
                           remote_play::AuthMaterial{}, 0, 0));
  const std::array<std::byte, 8> oversized{
      std::byte{0}, std::byte{0},    std::byte{1}, std::byte{0xf9},
      std::byte{0}, std::byte{0x33}, std::byte{0}, std::byte{0}};
  ASSERT_NO_FATAL_FAILURE(send_all(*pair.writer, oversized));
  expect_reader_error(reader, CtrlReaderError::too_large);
  EXPECT_TRUE(reader.cancel());
  EXPECT_TRUE(pair.writer->close());
  EXPECT_TRUE(pair.listener->close());
}

TEST_F(CtrlFrameReader, DropsAFrameWhoseCryptoFailsAndReadsTheNext) {
  ASSERT_TRUE(connect(pair));
  crypto.fail_hmac = true;
  ASSERT_TRUE(reader.begin(std::move(pair.reader), RegistrationTarget::ps5_1,
                           remote_play::AuthMaterial{}, 0, 0));
  auto wire = header(0x5, 1);
  wire.push_back(std::byte{1});
  ASSERT_NO_FATAL_FAILURE(send_all(*pair.writer, wire));
  const auto deadline = deadline_after(std::chrono::seconds{2});
  while (reader.dropped_frames() == 0 && still_before(deadline)) {
    const auto result = reader.pump_once();
    ASSERT_TRUE(result);
    ASSERT_FALSE(result.value());
    nap();
  }
  EXPECT_EQ(reader.dropped_frames(), 1U);
  EXPECT_EQ(reader.remote_counter(), 1U);
  crypto.fail_hmac = false;
  ASSERT_NO_FATAL_FAILURE(send_all(*pair.writer, wire));
  const auto frame = wait_frame(reader);
  ASSERT_TRUE(frame);
  EXPECT_EQ(frame->type(), 0x5);
  EXPECT_EQ(frame->payload().size(), 1U);
  EXPECT_EQ(crypto.counters, (std::vector<unsigned>{1}));
  EXPECT_EQ(reader.remote_counter(), 2U);
  EXPECT_TRUE(reader.cancel());
  EXPECT_FALSE(reader.pump_once());
  EXPECT_TRUE(pair.writer->close());
  EXPECT_TRUE(pair.listener->close());
}

TEST_F(CtrlFrameReader, FailsWhenTheRemoteCounterIsExhausted) {
  ASSERT_TRUE(connect(pair));
  ASSERT_TRUE(reader.begin(std::move(pair.reader), RegistrationTarget::ps4_10,
                           remote_play::AuthMaterial{},
                           std::numeric_limits<std::uint64_t>::max(), 0));
  auto wire = header(0x5, 1);
  wire.push_back(std::byte{0});
  ASSERT_NO_FATAL_FAILURE(send_all(*pair.writer, wire));
  expect_reader_error(reader, CtrlReaderError::counter_exhausted);
  EXPECT_TRUE(crypto.counters.empty());
  EXPECT_TRUE(reader.cancel());
  EXPECT_TRUE(pair.writer->close());
  EXPECT_TRUE(pair.listener->close());
}

TEST_F(CtrlFrameReader, SendsQueuedFramesAndSpendsCountersOnlyOnPayloads) {
  ASSERT_TRUE(connect(pair));
  ASSERT_TRUE(reader.begin(std::move(pair.reader), RegistrationTarget::ps5_1,
                           remote_play::AuthMaterial{}, 0, 5));
  EXPECT_TRUE(reader.queue_send(0x1fe, {}));
  EXPECT_TRUE(reader.queue_send(0x33, {}));
  ASSERT_NO_FATAL_FAILURE(pump_send_until_done(reader));
  EXPECT_EQ(reader.local_counter(), 5U);
  auto both = header(0x1fe, 0);
  const auto queued = header(0x33, 0);
  both.insert(both.end(), queued.begin(), queued.end());
  EXPECT_EQ(receive_exact(*pair.writer, 16), both);

  const std::array<std::byte, 3> plain{std::byte{1}, std::byte{2},
                                       std::byte{3}};
  EXPECT_TRUE(reader.queue_send(0x8004, plain));
  EXPECT_EQ(reader.local_counter(), 6U);
  ASSERT_NO_FATAL_FAILURE(pump_send_until_done(reader));
  auto expected = header(0x8004, 3);
  expected.insert(expected.end(), plain.begin(), plain.end());
  EXPECT_EQ(receive_exact(*pair.writer, expected.size()), expected);
  EXPECT_EQ(crypto.counters, (std::vector<unsigned>{5}));
  EXPECT_TRUE(reader.cancel());
  EXPECT_TRUE(pair.writer->close());
  EXPECT_TRUE(pair.listener->close());
}

TEST_F(CtrlFrameReader, SendsAKeyboardFrameLargerThanOneSend) {
  ASSERT_TRUE(connect(pair));
  ASSERT_TRUE(reader.begin(std::move(pair.reader), RegistrationTarget::ps5_1,
                           remote_play::AuthMaterial{}, 0, 5));
  const std::vector<std::byte> keyboard(578, std::byte{0x61});
  EXPECT_TRUE(reader.queue_send(0x23, keyboard));
  ASSERT_NO_FATAL_FAILURE(pump_send_until_done(reader));
  const auto large_header = header(0x23, keyboard.size());
  const auto large = receive_exact(*pair.writer, 8 + keyboard.size());
  ASSERT_EQ(large.size(), 8 + keyboard.size());
  EXPECT_TRUE(
      std::equal(large_header.begin(), large_header.end(), large.begin()));
  EXPECT_TRUE(std::equal(keyboard.begin(), keyboard.end(), large.begin() + 8));
  EXPECT_TRUE(reader.cancel());
  EXPECT_TRUE(pair.writer->close());
  EXPECT_TRUE(pair.listener->close());
}

TEST_F(CtrlFrameReader, ResumesPartialSendsInOrder) {
  SendScript script;
  script.limits = {2, std::nullopt, 3, 3};
  ASSERT_TRUE(reader.begin(std::make_unique<SendScriptSocket>(network, script),
                           RegistrationTarget::ps4_9,
                           remote_play::AuthMaterial{}, 0, 0));
  EXPECT_TRUE(reader.queue_send(0x1fe, {}));
  EXPECT_FALSE(reader.pump_send().value());
  EXPECT_TRUE(reader.queue_send(0x33, {}));
  EXPECT_FALSE(reader.pump_send().value());
  EXPECT_FALSE(reader.pump_send().value());
  while (!reader.pump_send().value()) {
  }
  auto expected = header(0x1fe, 0);
  const auto next = header(0x33, 0);
  expected.insert(expected.end(), next.begin(), next.end());
  EXPECT_EQ(script.output, expected);
  EXPECT_TRUE(reader.cancel());
  EXPECT_TRUE(reader.cancel());
  EXPECT_EQ(script.closes, 1U);
}

TEST_F(CtrlFrameReader, KeepsTheSocketAfterASendFailure) {
  SendScript script;
  script.fail_once = true;
  ASSERT_TRUE(reader.begin(std::make_unique<SendScriptSocket>(network, script),
                           RegistrationTarget::ps4_9,
                           remote_play::AuthMaterial{}, 0, 0));
  EXPECT_TRUE(reader.queue_send(0x1fe, {}));
  const auto failed = reader.pump_send();
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error(), CtrlReaderError::network_failure);
  EXPECT_EQ(script.closes, 0U);
  EXPECT_TRUE(reader.pump_once());
  EXPECT_TRUE(reader.queue_send(0x1fe, {}));
  EXPECT_TRUE(reader.pump_send().value());
  EXPECT_EQ(script.output, header(0x1fe, 0));
  EXPECT_TRUE(reader.cancel());
  EXPECT_EQ(script.closes, 1U);
}

TEST_F(CtrlFrameReader, AdoptsAStartupConnectionAndContinuesItsCounters) {
  ASSERT_TRUE(connect(pair));
  remote_play::CtrlStartupConnection connection;
  connection.socket = std::move(pair.reader);
  const std::string_view typed =
      "HTTP/1.1 200 OK\r\nRP-Server-Type: AAAAAAAAAAAAAAAAAAAAAA==\r\n"
      "RP-Prohibit: 1\r\n\r\n";
  auto granted = remote_play::CtrlCodec::parse_response(
      {reinterpret_cast<const std::byte*>(typed.data()), typed.size()});
  ASSERT_TRUE(granted && granted.value());
  connection.response = std::move(granted.value().value());
  connection.next_local_crypt_counter = 5;
  const auto adopted =
      reader.adopt(std::move(connection), RegistrationTarget::ps5_1,
                   remote_play::AuthMaterial{});
  ASSERT_TRUE(adopted);
  EXPECT_EQ(adopted.value().server_type, 0);
  EXPECT_TRUE(adopted.value().prohibited);
  EXPECT_EQ(adopted.value().next_remote_counter, 1U);
  EXPECT_EQ(reader.remote_counter(), 1U);
  EXPECT_EQ(reader.local_counter(), 5U);
  EXPECT_EQ(crypto.counters, (std::vector<unsigned>{0}));

  auto wire = header(0x5, 1);
  wire.push_back(std::byte{0});
  ASSERT_NO_FATAL_FAILURE(send_all(*pair.writer, wire));
  const auto frame = wait_frame(reader);
  ASSERT_TRUE(frame);
  EXPECT_EQ(frame->type(), 0x5);
  EXPECT_EQ(frame->payload().size(), 1U);
  EXPECT_EQ(reader.remote_counter(), 2U);
  EXPECT_EQ(crypto.counters, (std::vector<unsigned>{0, 1}));
  const std::array<std::byte, 1> pin{std::byte{7}};
  EXPECT_TRUE(reader.queue_send(0x8004, pin));
  EXPECT_EQ(reader.local_counter(), 6U);
  EXPECT_EQ(crypto.counters, (std::vector<unsigned>{0, 1, 5}));
  EXPECT_TRUE(reader.cancel());
  EXPECT_TRUE(pair.writer->close());
  EXPECT_TRUE(pair.listener->close());
}

TEST_F(CtrlFrameReader, AdoptsAConnectionWithoutServerTypeSpendingNoCounter) {
  ASSERT_TRUE(connect(pair));
  remote_play::CtrlStartupConnection absent;
  absent.socket = std::move(pair.reader);
  const auto absent_result =
      reader.adopt(std::move(absent), RegistrationTarget::ps4_9,
                   remote_play::AuthMaterial{});
  ASSERT_TRUE(absent_result);
  EXPECT_FALSE(absent_result.value().server_type);
  EXPECT_EQ(reader.remote_counter(), 0U);
  EXPECT_TRUE(crypto.counters.empty());
  EXPECT_TRUE(reader.cancel());
  EXPECT_TRUE(pair.writer->close());
  EXPECT_TRUE(pair.listener->close());
}

TEST_F(CtrlFrameReader, RefusesAHandoffWhileActiveAndClosesOnlyTheNewSocket) {
  SendScript active_script;
  SendScript rejected_script;
  ASSERT_TRUE(reader.begin(
      std::make_unique<SendScriptSocket>(network, active_script),
      RegistrationTarget::ps5_1, remote_play::AuthMaterial{}, 0, 0));
  remote_play::CtrlStartupConnection duplicate;
  duplicate.socket =
      std::make_unique<SendScriptSocket>(network, rejected_script);
  const auto duplicate_result =
      reader.adopt(std::move(duplicate), RegistrationTarget::ps5_1,
                   remote_play::AuthMaterial{});
  ASSERT_FALSE(duplicate_result);
  EXPECT_EQ(duplicate_result.error(), CtrlReaderError::invalid_state);
  EXPECT_EQ(rejected_script.closes, 1U);
  EXPECT_EQ(active_script.closes, 0U);
  EXPECT_TRUE(reader.cancel());
  EXPECT_EQ(active_script.closes, 1U);
}

TEST_F(CtrlFrameReader, RefusesAHandoffWithoutASocket) {
  remote_play::CtrlStartupConnection missing;
  const auto missing_result =
      reader.adopt(std::move(missing), RegistrationTarget::ps5_1,
                   remote_play::AuthMaterial{});
  ASSERT_FALSE(missing_result);
  EXPECT_EQ(missing_result.error(), CtrlReaderError::invalid_state);
}

}  // namespace
}  // namespace remote_play_test
