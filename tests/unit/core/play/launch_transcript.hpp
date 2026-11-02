#pragma once

#include "libchiaking/platform/http/provider.hpp"
#include "libchiaking/platform/net/provider.hpp"
#include "libchiaking/platform/net/types.hpp"
#include "libchiaking/platform/websocket/provider.hpp"
#include "src/core/net/nat/holepunch/holepunch_probe.hpp"
#include "src/core/net/protocol/takion/control_chunk_codec.hpp"
#include "src/core/net/protocol/takion/detail/envelope_codec.hpp"
#include "src/core/net/protocol/takion/detail/packet_codec.hpp"
#include "src/core/net/protocol/takion/profile.hpp"
#include "src/core/net/transport/rudp/rudp_frame_codec.hpp"
#include "src/core/utils/text/ascii.hpp"
#include "tests/support/live_providers.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace play_transcript {
namespace platform = libchiaking::platform;
namespace holepunch = libchiaking::net::nat::holepunch;
namespace rudp = libchiaking::net::transport::rudp;
namespace takion = libchiaking::net::protocol::takion;

class Digest {
 public:
  explicit Digest(std::span<const std::byte> bytes) noexcept {
    for (const auto byte : bytes) {
      value_ ^= std::to_integer<std::uint64_t>(byte);
      value_ *= prime;
    }
  }

  auto hex() const -> std::string {
    std::array<char, 17> text{};
    std::snprintf(text.data(), text.size(), "%016llx",
                  static_cast<unsigned long long>(value_));
    return std::string{text.data(), 16};
  }

 private:
  static constexpr std::uint64_t offset_basis = 0xcbf29ce484222325ULL;
  static constexpr std::uint64_t prime = 0x100000001b3ULL;

  std::uint64_t value_{offset_basis};
};

class Transcript {
 public:
  void conceal(std::string_view value, std::string_view label) {
    const platform::Lock lock{mutex_};
    concealed_.emplace_back(value, label);
    std::ranges::stable_sort(concealed_,
                             [](const auto& left, const auto& right) {
                               return left.first.size() > right.first.size();
                             });
  }

  void http(const platform::HttpRequest& request) {
    const platform::Lock lock{mutex_};
    lines_.push_back("http #" + std::to_string(++requests_) + " " +
                     platform::http_method_name(request.method) + " " +
                     masked(request.url.view()));
    for (const auto& header : request.headers)
      lines_.push_back(header_line(header));
    lines_.push_back(body_line(request.body.view()));
  }

  void websocket(const platform::WebSocketRequest& request) {
    const platform::Lock lock{mutex_};
    lines_.push_back("websocket #" + std::to_string(++connections_) + " " +
                     masked(request.url.view()));
    for (const auto& header : request.headers)
      lines_.push_back(header_line(header));
    lines_.push_back("  subprotocol: " + masked(request.subprotocol));
  }

  void socket_opened(unsigned socket, const platform::IpAddress& family) {
    const platform::Lock lock{mutex_};
    lines_.push_back("udp socket " + std::to_string(socket) + " open " +
                     (family.is_ipv6() ? "ipv6" : "ipv4"));
  }

  void socket_closed(unsigned socket) {
    const platform::Lock lock{mutex_};
    lines_.push_back("udp socket " + std::to_string(socket) + " close");
  }

  void datagram(unsigned socket, std::span<const std::byte> bytes,
                const platform::SocketEndpoint* destination) {
    const platform::Lock lock{mutex_};
    const auto target = destination
                            ? platform::to_string(destination->address) + ":" +
                                  std::to_string(destination->port)
                            : std::string{"connected peer"};
    lines_.push_back("udp socket " + std::to_string(socket) + " send to " +
                     target + " " + datagram_kind(bytes) +
                     " bytes=" + std::to_string(bytes.size()) +
                     " fnv1a64=" + Digest{bytes}.hex());
  }

  auto text() const -> std::string {
    const platform::Lock lock{mutex_};
    std::string joined;
    for (const auto& line : lines_) {
      joined += line;
      joined += '\n';
    }
    return joined;
  }

  auto leaks() const -> bool {
    const auto written = text();
    const platform::Lock lock{mutex_};
    return std::ranges::any_of(concealed_, [&written](const auto& secret) {
      return written.find(secret.first) != std::string::npos;
    });
  }

 private:
  auto masked(std::string_view text) const -> std::string {
    std::string out{text};
    for (const auto& [value, label] : concealed_) {
      if (value.empty()) continue;
      const auto mask =
          "<" + label + " len=" + std::to_string(value.size()) + ">";
      for (auto at = out.find(value); at != std::string::npos;
           at = out.find(value, at + mask.size()))
        out.replace(at, value.size(), mask);
    }
    return out;
  }

  auto header_line(const platform::HttpHeader& header) const -> std::string {
    const auto value = header.value.view();
    if (libchiaking::text::ieq(header.name, "Authorization")) {
      const auto space = value.find(' ');
      if (space == std::string_view::npos)
        return "  " + header.name + ": " + length_only(value.size());
      return "  " + header.name + ": " + std::string{value.substr(0, space)} +
             " " + length_only(value.size() - space - 1);
    }
    if (libchiaking::text::ieq(header.name, "Cookie"))
      return "  " + header.name + ": " + cookie_names(value);
    if (plain_header(header.name))
      return "  " + header.name + ": " + masked(value);
    return "  " + header.name + ": " + length_only(value.size());
  }

  static auto body_line(std::string_view body) -> std::string {
    if (body.empty()) return "  body: none";
    const auto bytes = std::as_bytes(std::span{body.data(), body.size()});
    return "  body: bytes=" + std::to_string(body.size()) +
           " fnv1a64=" + Digest{bytes}.hex();
  }

  static auto cookie_names(std::string_view cookies) -> std::string {
    std::string out;
    while (!cookies.empty()) {
      const auto end = cookies.find(';');
      const auto cookie = libchiaking::text::trim(cookies.substr(0, end));
      const auto equals = cookie.find('=');
      if (!out.empty()) out += "; ";
      if (equals == std::string_view::npos)
        out += length_only(cookie.size());
      else
        out += std::string{cookie.substr(0, equals)} + "=" +
               length_only(cookie.size() - equals - 1);
      if (end == std::string_view::npos) break;
      cookies.remove_prefix(end + 1);
    }
    return out;
  }

  static auto length_only(std::size_t size) -> std::string {
    return "<value len=" + std::to_string(size) + ">";
  }

  static auto plain_header(std::string_view name) -> bool {
    static constexpr std::array<std::string_view, 17> plain{
        "Accept",
        "Content-Type",
        "Origin",
        "Referer",
        "User-Agent",
        "X-Alt-Referer",
        "X-Gaikai-SessionId",
        "X-PSN-APP-TYPE",
        "X-PSN-APP-VER",
        "X-PSN-KEEP-ALIVE-STATUS-TYPE",
        "X-PSN-OS-VER",
        "X-PSN-PROTOCOL-VERSION",
        "X-PSN-RECONNECTION",
        "X-PSN-SESSION-MANAGER-SESSION-IDS",
        "apollographql-client-name",
        "apollographql-client-version",
        "disable_query_whitelist"};
    return std::ranges::any_of(plain, [name](std::string_view allowed) {
      return libchiaking::text::ieq(name, allowed);
    });
  }

  static auto hex(std::uint32_t value, int digits) -> std::string {
    std::array<char, 9> text{};
    std::snprintf(text.data(), text.size(), "%0*x", digits, value);
    return std::string{text.data()};
  }

  static auto datagram_kind(std::span<const std::byte> bytes) -> std::string {
    if (bytes.empty()) return "empty";
    if (bytes.size() == holepunch::CandidateProbe::kEncodedSize &&
        bytes[1] == std::byte{} && bytes[2] == std::byte{} &&
        bytes[3] == std::byte{}) {
      if (bytes[0] == std::byte{0x06}) return "holepunch-request";
      if (bytes[0] == std::byte{0x07}) return "holepunch-response";
    }
    constexpr std::array<std::byte, 4> stun_cookie{
        std::byte{0x21}, std::byte{0x12}, std::byte{0xa4}, std::byte{0x42}};
    if (bytes.size() >= 20 &&
        std::ranges::equal(bytes.subspan(4, 4), stun_cookie))
      return "stun type=" +
             hex((std::to_integer<std::uint32_t>(bytes[0]) << 8U) |
                     std::to_integer<std::uint32_t>(bytes[1]),
                 4);
    const auto frames = rudp::FrameCodec::decode(bytes);
    if (frames && !frames.value().empty() &&
        std::ranges::all_of(frames.value(), [](const auto& frame) {
          return (frame.type & 0x00ffU) == 0x0030U;
        })) {
      std::string kind{"rudp"};
      for (const auto& frame : frames.value())
        kind += " " + hex(frame.type, 4) + "/" +
                std::to_string(frame.payload.size());
      return kind;
    }
    const auto profile = takion::resolve_takion_wire_profile(7);
    const auto envelope =
        takion::EnvelopeCodec::decode(bytes, takion::PlainEnvelope{});
    if (profile && envelope) {
      if (const auto plain = takion::PacketCodec::decode_plain_packet(
              envelope.value(), *profile))
        return takion_kind(plain.value());
    }
    return "unclassified first=" +
           hex(std::to_integer<std::uint32_t>(bytes[0]), 2);
  }

  static auto takion_kind(const takion::PlainPacket& packet) -> std::string {
    const auto family = packet.type.family_code();
    if (family != static_cast<std::uint8_t>(takion::PacketFamily::control))
      return "takion family=" + hex(family, 2);
    const auto chunk = takion::ControlChunkCodec::decode_packet(packet.payload);
    if (!chunk) return "takion control";
    static constexpr std::array<std::string_view, 6> names{
        "init", "init-ack", "cookie", "cookie-ack", "data", "data-ack"};
    std::string kind{"takion control "};
    kind += names[chunk.value().index()];
    if (const auto* data = std::get_if<takion::DataChunk>(&chunk.value());
        data && data->payload.size() >= 6)
      kind += " channel=" +
              std::to_string(
                  (std::to_integer<std::uint32_t>(data->payload[4]) << 8U) |
                  std::to_integer<std::uint32_t>(data->payload[5]));
    return kind;
  }

  mutable platform::Mutex mutex_{test_support::LiveProviders::threading()};
  std::vector<std::string> lines_;
  std::vector<std::pair<std::string, std::string>> concealed_;
  unsigned requests_{};
  unsigned connections_{};
};

class GoldenFile {
 public:
  explicit GoldenFile(std::string_view name)
      : path_(std::string{LIBCHIAKING_PLAY_TRANSCRIPTS} + "/" +
              std::string{name} + ".txt") {}

  auto holds(const Transcript& transcript) const -> bool {
    if (transcript.leaks()) {
      std::fprintf(stderr, "%s: a concealed value reached the transcript\n",
                   path_.c_str());
      return false;
    }
    const auto actual = transcript.text();
    if (regenerating()) {
      std::ofstream file{path_, std::ios::binary | std::ios::trunc};
      file << actual;
      file.close();
      std::fprintf(stderr, "%s: %s\n", path_.c_str(),
                   file ? "regenerated" : "could not be written");
      return static_cast<bool>(file);
    }
    std::ifstream file{path_, std::ios::binary};
    if (!file) {
      std::fprintf(stderr, "%s: golden transcript is missing; %s\n",
                   path_.c_str(), regenerate_hint.data());
      return false;
    }
    const std::string expected{std::istreambuf_iterator<char>{file},
                               std::istreambuf_iterator<char>{}};
    if (expected == actual) return true;
    report(expected, actual);
    return false;
  }

 private:
  static constexpr std::string_view variable = "LIBCHIAKING_REGENERATE_GOLDEN";
  static constexpr std::string_view regenerate_hint =
      "if the wire change is intended, rerun the test with "
      "LIBCHIAKING_REGENERATE_GOLDEN=1 and commit the file";

  static auto regenerating() -> bool {
    const char* value = std::getenv(variable.data());
    return value != nullptr && std::string_view{value} == "1";
  }

  static auto lines(std::string_view text) -> std::vector<std::string_view> {
    std::vector<std::string_view> out;
    while (!text.empty()) {
      const auto end = text.find('\n');
      out.push_back(text.substr(0, end));
      if (end == std::string_view::npos) break;
      text.remove_prefix(end + 1);
    }
    return out;
  }

  void report(std::string_view expected, std::string_view actual) const {
    const auto wanted = lines(expected);
    const auto got = lines(actual);
    const auto shared = std::min(wanted.size(), got.size());
    std::size_t at = 0;
    while (at < shared && wanted[at] == got[at]) ++at;
    std::string_view entry{"<start of transcript>"};
    for (std::size_t index = 0; index <= at && index < got.size(); ++index)
      if (!got[index].starts_with(' ')) entry = got[index];
    const std::string_view missing{"<end of transcript>"};
    const auto expected_line = at < wanted.size() ? wanted[at] : missing;
    const auto actual_line = at < got.size() ? got[at] : missing;
    std::fprintf(
        stderr,
        "%s: transcript differs at line %zu (golden has %zu lines, this run "
        "produced %zu)\n"
        "  entry:    %.*s\n  expected: %.*s\n  actual:   %.*s\n  %s\n",
        path_.c_str(), at + 1, wanted.size(), got.size(),
        static_cast<int>(entry.size()), entry.data(),
        static_cast<int>(expected_line.size()), expected_line.data(),
        static_cast<int>(actual_line.size()), actual_line.data(),
        regenerate_hint.data());
  }

  std::string path_;
};

class TranscribingHttp final : public platform::HttpProvider {
 public:
  TranscribingHttp(platform::HttpProvider& inner,
                   Transcript& transcript) noexcept
      : inner_(inner), transcript_(transcript) {}

  [[nodiscard]] auto perform(const platform::HttpRequest& request,
                             const platform::StopToken& stop)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    transcript_.http(request);
    return inner_.perform(request, stop);
  }

 private:
  platform::HttpProvider& inner_;
  Transcript& transcript_;
};

class TranscribingWebSocket final : public platform::WebSocketProvider {
 public:
  TranscribingWebSocket(platform::WebSocketProvider& inner,
                        Transcript& transcript) noexcept
      : inner_(inner), transcript_(transcript) {}

  [[nodiscard]] auto connect(const platform::WebSocketRequest& request,
                             const platform::StopToken& stop)
      -> libchiaking::Expected<std::unique_ptr<platform::WebSocketConnection>,
                               platform::WebSocketError> override {
    transcript_.websocket(request);
    return inner_.connect(request, stop);
  }

 private:
  platform::WebSocketProvider& inner_;
  Transcript& transcript_;
};

class TranscribingNetwork final : public platform::NetworkProvider {
 public:
  TranscribingNetwork(platform::NetworkProvider& inner,
                      Transcript& transcript) noexcept
      : inner_(inner),
        transcript_(transcript),
        sockets_(*this),
        pollers_(*this) {}

  auto sockets() noexcept -> platform::SocketProvider& override {
    return sockets_;
  }
  auto pollers() noexcept -> platform::PollerProvider& override {
    return pollers_;
  }
  auto dns() noexcept -> platform::DnsProvider& override {
    return inner_.dns();
  }
  auto interfaces() noexcept -> platform::InterfacesProvider& override {
    return inner_.interfaces();
  }
  auto capabilities() const noexcept -> platform::NetworkCapabilities override {
    return inner_.capabilities();
  }

 private:
  class Socket final : public platform::UdpSocket {
   public:
    Socket(TranscribingNetwork& network,
           std::unique_ptr<platform::UdpSocket> inner, unsigned number)
        : network_(network), inner_(std::move(inner)), number_(number) {
      network_.attach(this, inner_.get());
    }

    ~Socket() override {
      network_.detach(this);
      note_closed();
    }

    auto owner() const noexcept -> const platform::NetworkProvider& override {
      return network_;
    }

    [[nodiscard]] auto bind(const platform::SocketEndpoint& endpoint)
        -> libchiaking::Expected<void, platform::NetError> override {
      return inner_->bind(endpoint);
    }

    [[nodiscard]] auto send_to(std::span<const std::byte> datagram,
                               const platform::SocketEndpoint& destination)
        -> libchiaking::Expected<std::size_t, platform::NetError> override {
      network_.transcript_.datagram(number_, datagram, &destination);
      return inner_->send_to(datagram, destination);
    }

    [[nodiscard]] auto receive_from(std::span<std::byte> buffer)
        -> libchiaking::Expected<platform::UdpReceiveResult,
                                 platform::NetError> override {
      return inner_->receive_from(buffer);
    }

    [[nodiscard]] auto connect(const platform::SocketEndpoint& endpoint)
        -> libchiaking::Expected<void, platform::NetError> override {
      return inner_->connect(endpoint);
    }

    [[nodiscard]] auto send(std::span<const std::byte> datagram)
        -> libchiaking::Expected<std::size_t, platform::NetError> override {
      network_.transcript_.datagram(number_, datagram, nullptr);
      return inner_->send(datagram);
    }

    [[nodiscard]] auto set_boolean_option(platform::SocketOption option,
                                          bool enabled)
        -> libchiaking::Expected<void, platform::NetError> override {
      return inner_->set_boolean_option(option, enabled);
    }

    [[nodiscard]] auto set_shared_port_binding(bool enabled)
        -> libchiaking::Expected<void, platform::NetError> override {
      return inner_->set_shared_port_binding(enabled);
    }

    [[nodiscard]] auto set_receive_buffer_size(std::size_t bytes)
        -> libchiaking::Expected<void, platform::NetError> override {
      return inner_->set_receive_buffer_size(bytes);
    }

    [[nodiscard]] auto receive_buffer_size()
        -> libchiaking::Expected<std::size_t, platform::NetError> override {
      return inner_->receive_buffer_size();
    }

    [[nodiscard]] auto set_ipv4_ttl(std::uint8_t ttl)
        -> libchiaking::Expected<void, platform::NetError> override {
      return inner_->set_ipv4_ttl(ttl);
    }

    [[nodiscard]] auto receive_traffic_class(bool enabled)
        -> libchiaking::Expected<void, platform::NetError> override {
      return inner_->receive_traffic_class(enabled);
    }

    [[nodiscard]] auto local_endpoint()
        -> libchiaking::Expected<platform::SocketEndpoint,
                                 platform::NetError> override {
      return inner_->local_endpoint();
    }

    [[nodiscard]] auto close()
        -> libchiaking::Expected<void, platform::NetError> override {
      note_closed();
      return inner_->close();
    }

   private:
    void note_closed() {
      if (!std::exchange(closed_, true))
        network_.transcript_.socket_closed(number_);
    }

    TranscribingNetwork& network_;
    std::unique_ptr<platform::UdpSocket> inner_;
    unsigned number_;
    bool closed_{};
  };

  class Sockets final : public platform::SocketProvider {
   public:
    explicit Sockets(TranscribingNetwork& network) noexcept
        : network_(network) {}

    [[nodiscard]] auto create_udp_socket(const platform::IpAddress& family)
        -> libchiaking::Expected<std::unique_ptr<platform::UdpSocket>,
                                 platform::NetError> override {
      auto inner = network_.inner_.sockets().create_udp_socket(family);
      if (!inner) return libchiaking::unexpected(inner.error());
      const auto number = ++created_;
      network_.transcript_.socket_opened(number, family);
      return std::unique_ptr<platform::UdpSocket>{
          std::make_unique<Socket>(network_, std::move(inner.value()), number)};
    }

    [[nodiscard]] auto create_tcp_socket(const platform::IpAddress& family)
        -> libchiaking::Expected<std::unique_ptr<platform::TcpSocket>,
                                 platform::NetError> override {
      return network_.inner_.sockets().create_tcp_socket(family);
    }

    [[nodiscard]] auto create_tcp_listener(const platform::IpAddress& family)
        -> libchiaking::Expected<std::unique_ptr<platform::TcpListener>,
                                 platform::NetError> override {
      return network_.inner_.sockets().create_tcp_listener(family);
    }

   private:
    TranscribingNetwork& network_;
    unsigned created_{};
  };

  class Poller final : public platform::Poller {
   public:
    Poller(TranscribingNetwork& network,
           std::unique_ptr<platform::Poller> inner)
        : network_(network), inner_(std::move(inner)) {}

    auto owner() const noexcept -> const platform::NetworkProvider& override {
      return network_;
    }

    [[nodiscard]] auto wake()
        -> libchiaking::Expected<void, platform::NetError> override {
      return inner_->wake();
    }

    [[nodiscard]] auto wait(std::span<const platform::PollRequest> requests,
                            std::chrono::nanoseconds timeout,
                            const platform::StopToken& stop)
        -> libchiaking::Expected<platform::PollWaitResult,
                                 platform::NetError> override {
      std::vector<platform::PollRequest> translated{requests.begin(),
                                                    requests.end()};
      for (auto& request : translated)
        request.pollable = network_.inner_of(request.pollable);
      return inner_->wait(translated, timeout, stop);
    }

   private:
    TranscribingNetwork& network_;
    std::unique_ptr<platform::Poller> inner_;
  };

  class Pollers final : public platform::PollerProvider {
   public:
    explicit Pollers(TranscribingNetwork& network) noexcept
        : network_(network) {}

    [[nodiscard]] auto create_poller()
        -> libchiaking::Expected<std::unique_ptr<platform::Poller>,
                                 platform::NetError> override {
      auto inner = network_.inner_.pollers().create_poller();
      if (!inner) return libchiaking::unexpected(inner.error());
      return std::unique_ptr<platform::Poller>{
          std::make_unique<Poller>(network_, std::move(inner.value()))};
    }

   private:
    TranscribingNetwork& network_;
  };

  void attach(const platform::Pollable* outer,
              const platform::Pollable* inner) {
    const platform::Lock lock{mutex_};
    live_.emplace_back(outer, inner);
  }

  void detach(const platform::Pollable* outer) {
    const platform::Lock lock{mutex_};
    std::erase_if(live_,
                  [outer](const auto& entry) { return entry.first == outer; });
  }

  auto inner_of(const platform::Pollable* outer) -> const platform::Pollable* {
    const platform::Lock lock{mutex_};
    const auto found =
        std::ranges::find(live_, outer,
                          &std::pair<const platform::Pollable*,
                                     const platform::Pollable*>::first);
    return found == live_.end() ? outer : found->second;
  }

  platform::NetworkProvider& inner_;
  Transcript& transcript_;
  Sockets sockets_;
  Pollers pollers_;
  platform::Mutex mutex_{test_support::LiveProviders::threading()};
  std::vector<std::pair<const platform::Pollable*, const platform::Pollable*>>
      live_;
};

}  // namespace play_transcript
