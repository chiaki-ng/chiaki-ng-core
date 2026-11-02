#pragma once

#include <libchiaking/platform/net/default.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include "tests/support/process_threading.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace platform_curl_test {

using namespace libchiaking::platform;

class LoopbackWebSocketServer {
 public:
  enum class Mode {
    fragmented,
    keepalive,
    missing_pong,
    late_pong,
    flood,
    byte_flood
  };

  explicit LoopbackWebSocketServer(Mode mode = Mode::fragmented) : mode_(mode) {
    auto listener =
        network_.sockets().create_tcp_listener(IpAddress::loopback_ipv4());
    if (!listener) return;
    listener_ = std::move(listener).value();
    if (!listener_->bind({IpAddress::loopback_ipv4(), 0}) ||
        !listener_->listen(1))
      return;
    const auto endpoint = listener_->local_endpoint();
    if (!endpoint) return;
    port_ = endpoint.value().port;
    auto started = Thread::start(threading_, "websocket server",
                                 ThreadRole::unspecified, [this] { run(); });
    if (!started) return;
    thread_ = std::move(started).value();
    ready_ = true;
  }

  ~LoopbackWebSocketServer() {
    join();
    if (listener_) (void)listener_->close();
  }

  LoopbackWebSocketServer(const LoopbackWebSocketServer&) = delete;
  LoopbackWebSocketServer& operator=(const LoopbackWebSocketServer&) = delete;

  unsigned pings() const noexcept { return pings_.load(); }
  unsigned closes() const noexcept { return closes_.load(); }
  bool ready() const noexcept { return ready_; }
  bool succeeded() const noexcept { return succeeded_; }
  const std::string& request() const noexcept { return request_; }
  std::string url() const {
    return "ws://127.0.0.1:" + std::to_string(port_) + "/chiaki?x=1";
  }

  void join() {
    if (thread_.joinable()) static_cast<void>(thread_.join());
  }

 private:
  static auto sha1(std::string_view input) -> std::array<std::uint8_t, 20> {
    std::vector<std::uint8_t> data;
    data.reserve(input.size() + 72);
    for (const char value : input)
      data.push_back(
          static_cast<std::uint8_t>(static_cast<unsigned char>(value)));
    const std::uint64_t bit_length =
        static_cast<std::uint64_t>(data.size()) * 8U;
    data.push_back(0x80U);
    while (data.size() % 64U != 56U) data.push_back(0U);
    for (int shift = 56; shift >= 0; shift -= 8)
      data.push_back(static_cast<std::uint8_t>(bit_length >> shift));

    std::array<std::uint32_t, 5> hash{0x67452301U, 0xefcdab89U, 0x98badcfeU,
                                      0x10325476U, 0xc3d2e1f0U};
    std::array<std::uint32_t, 80> words{};
    for (std::size_t block = 0; block < data.size(); block += 64) {
      for (std::size_t index = 0; index < 16; ++index) {
        const std::size_t offset = block + index * 4;
        words[index] = (static_cast<std::uint32_t>(data[offset]) << 24U) |
                       (static_cast<std::uint32_t>(data[offset + 1]) << 16U) |
                       (static_cast<std::uint32_t>(data[offset + 2]) << 8U) |
                       static_cast<std::uint32_t>(data[offset + 3]);
      }
      for (std::size_t index = 16; index < words.size(); ++index) {
        words[index] = std::rotl(words[index - 3] ^ words[index - 8] ^
                                     words[index - 14] ^ words[index - 16],
                                 1);
      }

      std::uint32_t a = hash[0];
      std::uint32_t b = hash[1];
      std::uint32_t c = hash[2];
      std::uint32_t d = hash[3];
      std::uint32_t e = hash[4];
      for (std::size_t index = 0; index < words.size(); ++index) {
        std::uint32_t function = 0;
        std::uint32_t constant = 0;
        if (index < 20) {
          function = (b & c) | (~b & d);
          constant = 0x5a827999U;
        } else if (index < 40) {
          function = b ^ c ^ d;
          constant = 0x6ed9eba1U;
        } else if (index < 60) {
          function = (b & c) | (b & d) | (c & d);
          constant = 0x8f1bbcdcU;
        } else {
          function = b ^ c ^ d;
          constant = 0xca62c1d6U;
        }
        const std::uint32_t next =
            std::rotl(a, 5) + function + e + constant + words[index];
        e = d;
        d = c;
        c = std::rotl(b, 30);
        b = a;
        a = next;
      }
      hash[0] += a;
      hash[1] += b;
      hash[2] += c;
      hash[3] += d;
      hash[4] += e;
    }

    std::array<std::uint8_t, 20> digest{};
    for (std::size_t index = 0; index < hash.size(); ++index) {
      digest[index * 4] = static_cast<std::uint8_t>(hash[index] >> 24U);
      digest[index * 4 + 1] = static_cast<std::uint8_t>(hash[index] >> 16U);
      digest[index * 4 + 2] = static_cast<std::uint8_t>(hash[index] >> 8U);
      digest[index * 4 + 3] = static_cast<std::uint8_t>(hash[index]);
    }
    return digest;
  }

  static std::string base64(std::span<const std::uint8_t> input) {
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve((input.size() + 2) / 3 * 4);
    for (std::size_t offset = 0; offset < input.size(); offset += 3) {
      const std::size_t remaining = input.size() - offset;
      const std::uint32_t first = input[offset];
      const std::uint32_t second = remaining > 1 ? input[offset + 1] : 0U;
      const std::uint32_t third = remaining > 2 ? input[offset + 2] : 0U;
      const std::uint32_t value = (first << 16U) | (second << 8U) | third;
      output.push_back(alphabet[(value >> 18U) & 0x3fU]);
      output.push_back(alphabet[(value >> 12U) & 0x3fU]);
      output.push_back(remaining > 1 ? alphabet[(value >> 6U) & 0x3fU] : '=');
      output.push_back(remaining > 2 ? alphabet[value & 0x3fU] : '=');
    }
    return output;
  }

  static std::vector<std::byte> frame(std::uint8_t opcode, bool final,
                                      std::string_view payload) {
    std::vector<std::byte> result;
    result.reserve(payload.size() + 2);
    result.push_back(static_cast<std::byte>((final ? 0x80U : 0U) | opcode));
    if (payload.size() < 126) {
      result.push_back(static_cast<std::byte>(payload.size()));
    } else {
      result.push_back(std::byte{126});
      result.push_back(static_cast<std::byte>(payload.size() >> 8U));
      result.push_back(static_cast<std::byte>(payload.size()));
    }
    const auto* begin = reinterpret_cast<const std::byte*>(payload.data());
    result.insert(result.end(), begin, begin + payload.size());
    return result;
  }

  [[nodiscard]] static bool wait_for(Poller& poller, const Pollable& pollable,
                                     PollEvent event) {
    const std::array<PollRequest, 1> requests{{PollRequest{
        .pollable = &pollable,
        .tag = 1,
        .interest = event,
    }}};
    const auto waited = poller.wait(requests, std::chrono::seconds(5), {});
    if (!waited) return false;
    for (const auto& ready : waited.value().ready) {
      if ((ready.ready & event) != PollEvent::none) return true;
    }
    return false;
  }

  [[nodiscard]] static bool send_all(Poller& poller, TcpSocket& socket,
                                     std::span<const std::byte> data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
      if (!wait_for(poller, socket, PollEvent::writable)) return false;
      const auto sent = socket.send(data.subspan(offset));
      if (!sent) {
        if (sent.error() == NetError::would_block) continue;
        return false;
      }
      if (sent.value() == 0) return false;
      offset += sent.value();
    }
    return true;
  }

  [[nodiscard]] static bool send_all(Poller& poller, TcpSocket& socket,
                                     std::string_view data) {
    return send_all(
        poller, socket,
        std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(data.data()), data.size()));
  }

  [[nodiscard]] bool receive_handshake(Poller& poller, TcpSocket& socket) {
    std::array<std::byte, 2048> buffer{};
    while (request_.find("\r\n\r\n") == std::string::npos) {
      if (request_.size() > 16 * 1024 ||
          !wait_for(poller, socket, PollEvent::readable))
        return false;
      const auto received = socket.receive(buffer);
      if (!received) {
        if (received.error() == NetError::would_block) continue;
        return false;
      }
      if (received.value() == 0) return false;
      request_.append(reinterpret_cast<const char*>(buffer.data()),
                      received.value());
    }
    return true;
  }

  [[nodiscard]] bool send_handshake(Poller& poller, TcpSocket& socket) {
    constexpr std::string_view key_name = "Sec-WebSocket-Key:";
    const std::size_t key_position = request_.find(key_name);
    if (key_position == std::string::npos) return false;
    std::size_t key_start = key_position + key_name.size();
    while (key_start < request_.size() && request_[key_start] == ' ')
      ++key_start;
    const std::size_t key_end = request_.find("\r\n", key_start);
    if (key_end == std::string::npos) return false;
    const std::string key = request_.substr(key_start, key_end - key_start);
    const auto digest = sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    const std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " +
        base64(digest) + "\r\nSec-WebSocket-Protocol: chiaki\r\n\r\n";
    return send_all(poller, socket, response);
  }

  bool serve_keepalive(Poller& poller, TcpSocket& socket) {
    std::vector<std::byte> buffered;
    std::array<std::byte, 512> input{};
    for (;;) {
      if (!wait_for(poller, socket, PollEvent::readable)) return false;
      auto received = socket.receive(input);
      if (!received) {
        if (received.error() == NetError::would_block) continue;
        if (received.error() == NetError::closed) return buffered.empty();
        return false;
      }
      if (received.value() == 0) return true;
      buffered.insert(buffered.end(), input.begin(),
                      input.begin() + received.value());
      while (buffered.size() >= 6) {
        const auto opcode = std::to_integer<unsigned>(buffered[0]) & 0x0fU;
        const auto length = std::to_integer<unsigned>(buffered[1]) & 0x7fU;
        if (length > 125) return false;
        if (buffered.size() < length + 6) break;
        std::string payload(length, '\0');
        for (unsigned i = 0; i < length; ++i)
          payload[i] = static_cast<char>(buffered[6 + i] ^ buffered[2 + i % 4]);
        buffered.erase(buffered.begin(), buffered.begin() + length + 6);
        if (opcode == 8) {
          ++closes_;
          return true;
        }
        if (opcode != 9) continue;
        ++pings_;
        if (mode_ == Mode::missing_pong) continue;
        if (mode_ == Mode::late_pong) {
          Sleeper sleeper{threading_, clock_};
          static_cast<void>(sleeper.sleep_for(std::chrono::milliseconds(35)));
          if (!send_all(poller, socket, frame(10, true, payload))) return false;
          continue;
        }
        if (pings_ == 1 && !send_all(poller, socket, frame(1, true, "before")))
          return false;
        if (!send_all(poller, socket, frame(10, true, payload))) return false;
        if (pings_ == 2 && !send_all(poller, socket, frame(1, true, "after")))
          return false;
      }
    }
  }

  void run() noexcept {
    auto poller = network_.pollers().create_poller();
    if (!poller || !wait_for(*poller.value(), *listener_, PollEvent::readable))
      return;
    auto accepted = listener_->accept();
    if (!accepted) return;
    auto socket = std::move(accepted).value();
    if (!receive_handshake(*poller.value(), *socket) ||
        !send_handshake(*poller.value(), *socket))
      return;

    if (mode_ == Mode::keepalive || mode_ == Mode::missing_pong ||
        mode_ == Mode::late_pong) {
      succeeded_ = serve_keepalive(*poller.value(), *socket);
      (void)socket->close();
      return;
    }
    if (mode_ == Mode::flood || mode_ == Mode::byte_flood) {
      std::vector<std::byte> messages;
      const int count = mode_ == Mode::flood ? 65 : 33;
      const std::string payload(mode_ == Mode::flood ? 6 : 8192, 'x');
      for (int i = 0; i < count; ++i) {
        auto message = frame(1, true, payload);
        messages.insert(messages.end(), message.begin(), message.end());
      }
      succeeded_ = send_all(*poller.value(), *socket, messages) &&
                   serve_keepalive(*poller.value(), *socket);
      (void)socket->close();
      return;
    }

    const auto first = frame(0x2U, false, "hel");
    const auto ping = frame(0x9U, true, "hb");
    const auto second = frame(0x0U, false, "lo w");
    const auto final = frame(0x0U, true, "orld");
    succeeded_ = send_all(*poller.value(), *socket, first) &&
                 send_all(*poller.value(), *socket, ping) &&
                 send_all(*poller.value(), *socket, second) &&
                 send_all(*poller.value(), *socket, final);
    (void)socket->close();
  }

  Mode mode_;
  std::atomic<unsigned> pings_{};
  std::atomic<unsigned> closes_{};
  DefaultThreadingProvider& threading_ = test_support::process_threading();
  SteadyClock clock_;
  DefaultNetworkProvider network_{threading_, clock_};
  std::unique_ptr<TcpListener> listener_;
  std::string request_;
  std::uint16_t port_{0};
  Thread thread_;
  bool ready_{false};
  bool succeeded_{false};
};

}  // namespace platform_curl_test
