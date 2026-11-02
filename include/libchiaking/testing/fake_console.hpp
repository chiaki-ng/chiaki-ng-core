/// @file
/// @brief Declares FakeConsole, a console that answers discovery and wake-up on
/// an InMemoryNetwork.
#pragma once

#include "libchiaking/platform/net/provider.hpp"
#include "libchiaking/testing/in_memory_network.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace libchiaking::testing {

/// @brief Stands in for a PS4 or PS5 on the local network during discovery.
///
/// It owns one UDP socket on an InMemoryNetwork and speaks the text protocol a
/// console uses to announce itself: it answers search requests with its
/// identity and power state, and records wake-up requests. It does not pair or
/// stream.
///
/// The console has no thread. A test runs the code that searches or wakes, then
/// calls serve() to let the console read and answer what arrived, then lets the
/// code under test read the replies. Afterwards searches() and wakeups() say
/// what the console received. rest(), power_on(), wake_for() and stay_silent()
/// change how it answers. receive(), reply(), send_to() and the announce
/// functions let a test drive the exchange one datagram at a time instead.
///
/// It differs from a real console in these ways. Everything it sends goes to a
/// port on its own address, never to the address a request came from, so it is
/// meant for a searcher on the same loopback address. It announces only when
/// told to. It leaves rest mode the moment an accepted wake-up is served.
///
/// @note Set-up and sending check their results with `assert`; in a build
///     without assertions a failure passes unnoticed. The object does no
///     locking.
class FakeConsole {
 public:
  /// @brief What the console says about itself in an announcement.
  struct Identity {
    /// @brief The `host-id` value: the console's hardware id as hexadecimal
    /// text.
    std::string hardware_id{"AABBCCDDEEFF"};
    /// @brief The `host-type` value. `PS5` selects PS5 behaviour; anything
    /// else is treated as a PS4.
    std::string type{"PS5"};
    /// @brief The `host-name` value. When empty the line is left out.
    std::string name{"Fake console"};
    /// @brief The `system-version` value, as the console's eight digit
    /// text.
    std::string system_version{"08050001"};
    /// @brief The `host-request-port` value: where the console says it
    /// accepts session requests. Nothing listens there.
    std::uint16_t request_port{997};
  };

  /// @brief The UDP port a PS4 listens on for searches.
  static constexpr std::uint16_t ps4_search_port = 987;
  /// @brief The UDP port a PS5 listens on for searches.
  static constexpr std::uint16_t ps5_search_port = 9302;
  /// @brief The lowest port announce() sends to.
  static constexpr std::uint16_t first_searcher_port = 9303;
  /// @brief The highest port announce() sends to.
  static constexpr std::uint16_t last_searcher_port = 9319;
  /// @brief The most bytes of a received datagram the console keeps; the rest
  /// is cut off.
  static constexpr std::size_t longest_datagram = 512;

  /// @brief Puts a console with the default identity at a chosen endpoint.
  ///
  /// @param network The network to live on. It must outlive the console.
  /// @param at Where to bind. The default is the IPv4 loopback address on a
  ///     port the network picks; read it back with port().
  /// @pre The endpoint can be bound on `network`.
  explicit FakeConsole(InMemoryNetwork& network,
                       platform::SocketEndpoint at = {
                           platform::IpAddress::loopback_ipv4(), 0}) {
    auto created = network.sockets().create_udp_socket(at.address);
    assert(created);
    socket_ = std::move(created.value());
    const auto bound = socket_->bind(at);
    assert(bound);
    const auto local = socket_->local_endpoint();
    assert(local);
    endpoint_ = local.value();
  }

  /// @brief Puts a console with a given identity on its usual search port.
  ///
  /// It binds to the IPv4 loopback address on ps5_search_port when the
  /// identity's type is `PS5` and on ps4_search_port otherwise.
  ///
  /// @param network The network to live on. It must outlive the console.
  /// @param identity What the console announces.
  /// @pre No other socket on `network` holds that port.
  FakeConsole(InMemoryNetwork& network, Identity identity)
      : FakeConsole(network, {platform::IpAddress::loopback_ipv4(),
                              identity.type == "PS5" ? ps5_search_port
                                                     : ps4_search_port}) {
    identity_ = std::move(identity);
  }

  /// @brief Returns the UDP port the console is bound to.
  ///
  /// @return The bound port, which the network chose when the console was
  ///     put on port zero.
  auto port() const noexcept -> std::uint16_t { return endpoint_.port; }
  /// @brief Returns the address the console is bound to.
  ///
  /// @return The bound address.
  auto address() const noexcept -> platform::IpAddress {
    return endpoint_.address;
  }
  /// @brief Returns the identity the console announces.
  ///
  /// @return The identity, valid as long as the console.
  auto identity() const noexcept -> const Identity& { return identity_; }
  /// @brief Reports whether the console is in rest mode.
  ///
  /// It starts powered on. rest() and power_on() change it, and so does
  /// serving a wake-up that carries the credential set with wake_for().
  ///
  /// @return True while the console is on standby.
  auto in_rest_mode() const noexcept -> bool { return in_rest_mode_; }
  /// @brief Reports how many search requests serve() has read, answered or
  /// not.
  ///
  /// @return The count since construction.
  auto searches() const noexcept -> std::size_t { return searches_; }
  /// @brief Returns every wake-up request serve() has read, as received text.
  ///
  /// Requests are kept in arrival order whether or not their credential was
  /// accepted.
  ///
  /// @return The requests, valid until the next serve() or until the
  ///     console is destroyed.
  auto wakeups() const noexcept -> const std::vector<std::string>& {
    return wakeups_;
  }

  /// @brief Puts the console in rest mode, so it announces itself as on
  /// standby.
  void rest() noexcept { in_rest_mode_ = true; }
  /// @brief Takes the console out of rest mode, so it announces itself as
  /// ready.
  void power_on() noexcept { in_rest_mode_ = false; }
  /// @brief Chooses the credential that wakes the console.
  ///
  /// Until this is called no wake-up request takes the console out of rest
  /// mode.
  ///
  /// @param credential The number a wake-up request must carry in its
  ///     `user-credential` line, written in decimal.
  void wake_for(std::uint64_t credential) { wake_credential_ = credential; }
  /// @brief Makes the console ignore searches, or answer them again.
  ///
  /// Searches are still counted while silent. Plays a console that is
  /// switched off or unreachable.
  ///
  /// @param silent True to stop answering; false to answer again.
  void stay_silent(bool silent = true) noexcept { silent_ = silent; }

  /// @brief Builds the text the console currently announces.
  ///
  /// The status line says standby when in rest mode and ok otherwise,
  /// followed by the identity fields and the discovery protocol version for
  /// the console's type.
  ///
  /// @return The announcement exactly as it would be sent.
  auto announcement() const -> std::string {
    const bool ps5 = identity_.type == "PS5";
    std::string text =
        in_rest_mode_ ? "HTTP/1.1 620 Server Standby\n" : "HTTP/1.1 200 Ok\n";
    text += "host-id:" + identity_.hardware_id +
            "\nhost-type:" + identity_.type + "\n";
    if (!identity_.name.empty()) text += "host-name:" + identity_.name + "\n";
    text +=
        "host-request-port:" + std::to_string(identity_.request_port) + "\n";
    text += "system-version:" + identity_.system_version + "\n";
    text += std::string{"device-discovery-protocol-version:"} +
            (ps5 ? "00030010" : "00020020") + "\n";
    return text;
  }

  /// @brief Reads one waiting datagram, if there is one.
  ///
  /// The sender is remembered as the target of the next reply().
  ///
  /// @return The datagram as text, or nothing when none is waiting or the
  ///     receive failed.
  auto try_receive() -> std::optional<std::string> {
    std::array<std::byte, longest_datagram> buffer{};
    const auto received = socket_->receive_from(buffer);
    if (!received) return std::nullopt;
    source_ = received.value().source;
    return std::string{reinterpret_cast<const char*>(buffer.data()),
                       received.value().size};
  }

  /// @brief Reads one waiting datagram that the test knows has arrived.
  ///
  /// @return The datagram as text.
  /// @pre A datagram is waiting.
  auto receive() -> std::string {
    auto datagram = try_receive();
    assert(datagram);
    return std::move(*datagram);
  }

  /// @brief Sends text to the port the last received datagram came from.
  ///
  /// @param datagram The text to send.
  /// @pre A datagram has been received. The reply goes to the console's own
  ///     address on the sender's port, not to the sender's address.
  void reply(std::string_view datagram) const {
    send_to(source_.port, datagram);
  }

  /// @brief Sends text to a port on the console's own address.
  ///
  /// @param destination_port The UDP port to send to.
  /// @param datagram The text to send.
  void send_to(std::uint16_t destination_port,
               std::string_view datagram) const {
    const auto bytes =
        std::as_bytes(std::span{datagram.data(), datagram.size()});
    const auto sent =
        socket_->send_to(bytes, {endpoint_.address, destination_port});
    assert(sent && sent.value() == datagram.size());
  }

  /// @brief Sends the current announcement to one port, unasked.
  ///
  /// @param searcher_port The UDP port to send to.
  void announce_to(std::uint16_t searcher_port) const {
    send_to(searcher_port, announcement());
  }

  /// @brief Sends the current announcement to every port from
  /// first_searcher_port to last_searcher_port.
  ///
  /// It announces even when stay_silent() is in force.
  void announce() const {
    for (std::uint32_t searcher = first_searcher_port;
         searcher <= last_searcher_port; ++searcher)
      announce_to(static_cast<std::uint16_t>(searcher));
  }

  /// @brief Reads and handles everything that has arrived so far.
  ///
  /// A search is counted and, unless the console is silent, answered with the
  /// current announcement. A wake-up is recorded, and takes the console out
  /// of rest mode when it carries the credential set with wake_for().
  /// Anything else is discarded. The call returns when nothing more is
  /// waiting; it never blocks.
  void serve() {
    while (auto datagram = try_receive()) {
      if (datagram->starts_with("SRCH ")) {
        ++searches_;
        if (!silent_) reply(announcement());
      } else if (datagram->starts_with("WAKEUP ")) {
        wakeups_.push_back(*datagram);
        if (wake_credential_ &&
            datagram->find(
                "user-credential:" + std::to_string(*wake_credential_) +
                "\n") != std::string::npos)
          in_rest_mode_ = false;
      }
    }
  }

 private:
  std::unique_ptr<platform::UdpSocket> socket_;
  platform::SocketEndpoint endpoint_;
  platform::SocketEndpoint source_;
  Identity identity_;
  std::optional<std::uint64_t> wake_credential_;
  std::vector<std::string> wakeups_;
  std::size_t searches_{};
  bool in_rest_mode_{};
  bool silent_{};
};

}  // namespace libchiaking::testing
