#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <iphlpapi.h>
#include <mstcpip.h>
#include <mswsock.h>
#include <netioapi.h>
#include <windows.h>
#include <winsock2.h>
#include <ws2ipdef.h>
#include <ws2tcpip.h>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#ifndef SIO_UDP_NETRESET
#define SIO_UDP_NETRESET _WSAIOW(IOC_VENDOR, 15)
#endif

#include "network_stack.hpp"
#include "socket_stack.hpp"
#include "src/core/utils/memory/c_handle.hpp"
#include "win32_error.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace libchiaking::platform::detail {

class Win32Sockets {
  using Raw = SocketHandle::Raw;

  struct NativeEndpoint {
    sockaddr_storage storage{};
    int length{0};

    auto address() const noexcept -> const sockaddr* {
      return reinterpret_cast<const sockaddr*>(&storage);
    }
  };

 public:
  struct DatagramState {
    bool connected{false};
    bool traffic_class{false};
    LPFN_WSARECVMSG receive_message{nullptr};
  };

  using PollEntry = WSAPOLLFD;
  static constexpr short poll_readable = POLLRDNORM;
  static constexpr short poll_writable = POLLWRNORM;
  static constexpr short poll_error = POLLERR;
  static constexpr short poll_hangup = POLLHUP;
  static constexpr short poll_invalid = POLLNVAL;

  [[nodiscard]] static bool ensure_wsa_started() noexcept {
    static const bool started = [] {
      WSADATA data{};
      return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return started;
  }

  [[nodiscard]] static auto open(const IpAddress& family_of,
                                 SocketKind kind) noexcept
      -> Expected<SocketHandle, NetError> {
    if (!ensure_wsa_started()) return unexpected(NetError::system_failure);
    const bool stream = kind == SocketKind::stream;
    SocketHandle handle(raw_of(open_uninheritable_socket(
        address_family_of(family_of), stream ? SOCK_STREAM : SOCK_DGRAM,
        stream ? IPPROTO_TCP : IPPROTO_UDP)));
    if (!handle.is_open()) return unexpected(last_net_error());
    set_nonblocking(socket_of(handle.get()));
    if (!stream) disable_udp_reset_reports(socket_of(handle.get()));
    return handle;
  }

  static void close(Raw raw) noexcept { (void)::closesocket(socket_of(raw)); }

  [[nodiscard]] static auto bind(Raw raw,
                                 const SocketEndpoint& endpoint) noexcept
      -> Expected<void, NetError> {
    const auto native = endpoint_to_native(endpoint);
    return succeeded(::bind(socket_of(raw), native.address(), native.length));
  }

  [[nodiscard]] static auto connect(Raw raw,
                                    const SocketEndpoint& endpoint) noexcept
      -> Expected<void, NetError> {
    const auto native = endpoint_to_native(endpoint);
    return succeeded(
        ::connect(socket_of(raw), native.address(), native.length));
  }

  [[nodiscard]] static auto connect_datagram(
      Raw raw, DatagramState& state, const SocketEndpoint& endpoint) noexcept
      -> Expected<void, NetError> {
    return connect(raw, endpoint).transform([raw, &state] {
      BOOL enabled = TRUE;
      DWORD returned = 0;
      (void)::WSAIoctl(socket_of(raw), SIO_UDP_CONNRESET, &enabled,
                       sizeof(enabled), nullptr, 0, &returned, nullptr,
                       nullptr);
      state.connected = true;
    });
  }

  [[nodiscard]] static auto send_to(Raw raw,
                                    std::span<const std::byte> datagram,
                                    const SocketEndpoint& destination) noexcept
      -> Expected<std::size_t, NetError> {
    const auto native = endpoint_to_native(destination);
    return transferred(::sendto(
        socket_of(raw), reinterpret_cast<const char*>(datagram.data()),
        static_cast<int>(datagram.size()), 0, native.address(), native.length));
  }

  [[nodiscard]] static auto send(Raw raw, std::span<const std::byte> data,
                                 SocketKind) noexcept
      -> Expected<std::size_t, NetError> {
    return transferred(::send(socket_of(raw),
                              reinterpret_cast<const char*>(data.data()),
                              static_cast<int>(data.size()), 0));
  }

  [[nodiscard]] static auto receive(Raw raw,
                                    std::span<std::byte> buffer) noexcept
      -> Expected<std::size_t, NetError> {
    return transferred(::recv(socket_of(raw),
                              reinterpret_cast<char*>(buffer.data()),
                              static_cast<int>(buffer.size()), 0));
  }

  [[nodiscard]] static auto receive_traffic_class(Raw raw, DatagramState& state,
                                                  bool enabled) noexcept
      -> Expected<void, NetError> {
    const int level = family_level(raw);
    int option = -1;
#ifdef IP_RECVTOS
    if (level == IPPROTO_IP) option = IP_RECVTOS;
#endif
#ifdef IPV6_RECVTCLASS
    if (level == IPPROTO_IPV6) option = IPV6_RECVTCLASS;
#endif
    if (option < 0) return unexpected(NetError::not_supported);
    if (enabled && !state.receive_message) {
      GUID identifier = WSAID_WSARECVMSG;
      LPFN_WSARECVMSG function = nullptr;
      DWORD returned = 0;
      if (::WSAIoctl(socket_of(raw), SIO_GET_EXTENSION_FUNCTION_POINTER,
                     &identifier, sizeof(identifier), &function,
                     sizeof(function), &returned, nullptr, nullptr) != 0 ||
          function == nullptr)
        return unexpected(NetError::not_supported);
      state.receive_message = function;
    }
    return set_option(raw, level, option, enabled ? 1 : 0)
        .transform([&state, enabled] { state.traffic_class = enabled; });
  }

  [[nodiscard]] static auto receive_from(Raw raw, const DatagramState& state,
                                         std::span<std::byte> buffer) noexcept
      -> Expected<UdpReceiveResult, NetError> {
    if (state.traffic_class && state.receive_message)
      return receive_with_traffic_class(raw, state, buffer);
    NativeEndpoint native;
    native.length = static_cast<int>(sizeof(sockaddr_storage));
    const auto received = ::recvfrom(
        socket_of(raw), reinterpret_cast<char*>(buffer.data()),
        static_cast<int>(buffer.size()), 0,
        reinterpret_cast<sockaddr*>(&native.storage), &native.length);
    const auto size =
        received_size(state, received >= 0, received, buffer.size());
    if (!size) return unexpected(size.error());
    UdpReceiveResult result;
    result.size = size.value();
    if (const auto source = native_to_endpoint(native.storage))
      result.source = source.value();
    return result;
  }

  [[nodiscard]] static auto set_broadcast(Raw raw, bool enabled) noexcept
      -> Expected<void, NetError> {
    return set_option(raw, SOL_SOCKET, SO_BROADCAST, enabled ? 1 : 0);
  }

  [[nodiscard]] static auto set_reuse_address(Raw raw, bool enabled) noexcept
      -> Expected<void, NetError> {
    return set_option(raw, SOL_SOCKET, SO_REUSEADDR, enabled ? 1 : 0);
  }

  [[nodiscard]] static auto set_shared_port(Raw raw, bool enabled) noexcept
      -> Expected<void, NetError> {
    return set_reuse_address(raw, enabled);
  }

  [[nodiscard]] static auto set_dont_fragment(Raw raw, bool enabled) noexcept
      -> Expected<void, NetError> {
    const int level = family_level(raw);
    if (level < 0) return unexpected(NetError::not_supported);
#ifdef IP_DONTFRAGMENT
    if (level == IPPROTO_IP)
      return set_option(raw, level, IP_DONTFRAGMENT, enabled ? 1 : 0);
#endif
#ifdef IPV6_DONTFRAG
    if (level == IPPROTO_IPV6)
      return set_option(raw, level, IPV6_DONTFRAG, enabled ? 1 : 0);
#endif
    return unexpected(NetError::not_supported);
  }

  [[nodiscard]] static auto set_receive_buffer_size(Raw raw, int bytes) noexcept
      -> Expected<void, NetError> {
    return set_option(raw, SOL_SOCKET, SO_RCVBUF, bytes);
  }

  [[nodiscard]] static auto receive_buffer_size(Raw raw) noexcept
      -> Expected<std::size_t, NetError> {
    return option_value(raw, SO_RCVBUF)
        .and_then([](int value) -> Expected<std::size_t, NetError> {
          if (value < 0) return unexpected(NetError::system_failure);
          return static_cast<std::size_t>(value);
        });
  }

  [[nodiscard]] static auto set_ipv4_ttl(Raw raw, std::uint8_t ttl) noexcept
      -> Expected<void, NetError> {
    return set_option(raw, IPPROTO_IP, IP_TTL, static_cast<int>(ttl));
  }

  [[nodiscard]] static auto local_endpoint(Raw raw) noexcept
      -> Expected<SocketEndpoint, NetError> {
    return named_endpoint(raw, ::getsockname);
  }

  [[nodiscard]] static auto peer_endpoint(Raw raw) noexcept
      -> Expected<SocketEndpoint, NetError> {
    return named_endpoint(raw, ::getpeername);
  }

  [[nodiscard]] static auto pending_error(Raw raw) noexcept
      -> Expected<void, NetError> {
    return option_value(raw, SO_ERROR)
        .and_then([](int error_code) -> Expected<void, NetError> {
          if (error_code == 0) return {};
          return unexpected(normalize_wsa_error(error_code));
        });
  }

  [[nodiscard]] static auto listen(Raw raw, int backlog) noexcept
      -> Expected<void, NetError> {
    return succeeded(::listen(socket_of(raw), backlog));
  }

  [[nodiscard]] static auto accept(Raw raw) noexcept
      -> Expected<SocketHandle, NetError> {
    sockaddr_storage storage{};
    int length = static_cast<int>(sizeof(storage));
    SocketHandle accepted(raw_of(::accept(
        socket_of(raw), reinterpret_cast<sockaddr*>(&storage), &length)));
    if (!accepted.is_open()) return unexpected(last_net_error());
    (void)::SetHandleInformation(
        reinterpret_cast<HANDLE>(socket_of(accepted.get())),
        HANDLE_FLAG_INHERIT, 0);
    set_nonblocking(socket_of(accepted.get()));
    return accepted;
  }

  [[nodiscard]] static auto open_wake_sockets() noexcept
      -> Expected<WakeSockets, NetError> {
    if (!ensure_wsa_started()) return unexpected(NetError::no_buffers);
    WakeSockets sockets{
        .receiver = SocketHandle{raw_of(
            open_uninheritable_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP))},
        .sender = SocketHandle{raw_of(
            open_uninheritable_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP))},
    };
    if (!sockets.receiver.is_open() || !sockets.sender.is_open())
      return unexpected(NetError::no_buffers);
    const SOCKET receiver = socket_of(sockets.receiver.get());
    const SOCKET sender = socket_of(sockets.sender.get());

    sockaddr_in loopback{};
    loopback.sin_family = AF_INET;
    loopback.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    loopback.sin_port = 0;
    if (::bind(receiver, reinterpret_cast<const sockaddr*>(&loopback),
               sizeof(loopback)) != 0)
      return unexpected(NetError::no_buffers);
    const auto receiver_address = local_address(receiver);
    if (!receiver_address ||
        ::connect(sender, reinterpret_cast<const sockaddr*>(&*receiver_address),
                  sizeof(*receiver_address)) != 0)
      return unexpected(NetError::no_buffers);
    const auto sender_address = local_address(sender);
    if (!sender_address ||
        ::connect(receiver, reinterpret_cast<const sockaddr*>(&*sender_address),
                  sizeof(*sender_address)) != 0)
      return unexpected(NetError::no_buffers);

    disable_udp_reset_reports(sender);
    disable_udp_reset_reports(receiver);
    set_nonblocking(sender);
    set_nonblocking(receiver);
    return sockets;
  }

  [[nodiscard]] static bool send_wake(Raw raw) noexcept {
    const char token = 1;
    if (::send(socket_of(raw), &token, 1, 0) >= 0) return true;
    const int error = ::WSAGetLastError();
    return error == WSAEWOULDBLOCK || error == WSAENOBUFS;
  }

  static void drain_wake(Raw raw) noexcept {
    std::array<char, wake_drain_buffer_size> buffer{};
    while (::recv(socket_of(raw), buffer.data(),
                  static_cast<int>(buffer.size()), 0) >= 0) {
    }
  }

  static auto poll_entry(Raw raw, short events) noexcept -> PollEntry {
    WSAPOLLFD entry{};
    entry.fd = socket_of(raw);
    entry.events = events;
    return entry;
  }

  [[nodiscard]] static int poll(std::vector<PollEntry>& entries,
                                int timeout_ms) noexcept {
    for (auto& entry : entries) entry.revents = 0;
    const int result = ::WSAPoll(
        entries.data(), static_cast<ULONG>(entries.size()), timeout_ms);
    if (result <= 0) {
      for (auto& entry : entries) entry.revents = 0;
    }
    return result;
  }

  [[nodiscard]] static auto resolve(std::string_view host,
                                    std::string_view service)
      -> Expected<std::vector<IpAddress>, NetError> {
    if (!ensure_wsa_started()) return unexpected(NetError::system_failure);
    const auto host_text = wide_from_utf8(host);
    const auto service_text = wide_from_utf8(service);
    if (!host_text || !service_text)
      return unexpected(NetError::invalid_argument);
    ADDRINFOW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
#ifdef AI_ADDRCONFIG
    hints.ai_flags = AI_ADDRCONFIG;
#endif
    ADDRINFOW* raw = nullptr;
    const int status = ::GetAddrInfoW(
        host_text->c_str(),
        service_text->empty() ? nullptr : service_text->c_str(), &hints, &raw);
    if (status != 0) return unexpected(resolve_error(status));
    const memory::CHandle<ADDRINFOW, ::FreeAddrInfoW> head{raw};
    std::vector<IpAddress> addresses;
    for (const ADDRINFOW* entry = head.get(); entry != nullptr;
         entry = entry->ai_next) {
      if (entry->ai_addr == nullptr) continue;
      sockaddr_storage storage{};
      const auto length = std::min(static_cast<std::size_t>(entry->ai_addrlen),
                                   sizeof(storage));
      std::memcpy(&storage, entry->ai_addr, length);
      if (const auto endpoint = native_to_endpoint(storage))
        addresses.push_back(endpoint.value().address);
    }
    if (addresses.empty()) return unexpected(NetError::host_unreachable);
    return addresses;
  }

  [[nodiscard]] static auto enumerate()
      -> Expected<std::vector<InterfaceInfo>, NetError> {
    if (!ensure_wsa_started()) return unexpected(NetError::system_failure);
    ULONG size = adapter_buffer_initial_size;
    std::vector<std::byte> buffer;
    ULONG result = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0;
         attempt < adapter_query_attempts && result == ERROR_BUFFER_OVERFLOW;
         ++attempt) {
      buffer.assign(size, std::byte{0});
      result = ::GetAdaptersAddresses(
          AF_UNSPEC,
          GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_DNS_SERVER |
              GAA_FLAG_SKIP_MULTICAST,
          nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()),
          &size);
    }
    if (result != NO_ERROR) return unexpected(NetError::system_failure);
    std::vector<InterfaceInfo> interfaces;
    for (const auto* adapter =
             reinterpret_cast<const IP_ADAPTER_ADDRESSES*>(buffer.data());
         adapter != nullptr; adapter = adapter->Next) {
      const bool loopback = adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK;
      const bool operational = adapter->OperStatus == IfOperStatusUp;
      MIB_IF_ROW2 interface_row{};
      interface_row.InterfaceLuid = adapter->Luid;
      const std::optional<bool> administrative_up =
          ::GetIfEntry2(&interface_row) == NO_ERROR
              ? std::optional<bool>{interface_row.AdminStatus ==
                                    NET_IF_ADMIN_STATUS_UP}
              : std::nullopt;
      const bool lan_adapter = adapter->IfType == IF_TYPE_ETHERNET_CSMACD ||
                               adapter->IfType == IF_TYPE_IEEE80211;
      for (const auto* unicast = adapter->FirstUnicastAddress;
           unicast != nullptr; unicast = unicast->Next) {
        if (unicast->Address.lpSockaddr == nullptr) continue;
        sockaddr_storage storage{};
        const auto family = unicast->Address.lpSockaddr->sa_family;
        if (family == AF_INET) {
          std::memcpy(&storage, unicast->Address.lpSockaddr,
                      sizeof(sockaddr_in));
        } else if (family == AF_INET6) {
          std::memcpy(&storage, unicast->Address.lpSockaddr,
                      sizeof(sockaddr_in6));
        } else {
          continue;
        }
        const auto endpoint = native_to_endpoint(storage);
        if (!endpoint) continue;
        InterfaceInfo info;
        info.name = utf8_from_wide(adapter->FriendlyName);
        info.index = static_cast<std::uint32_t>(adapter->IfIndex);
        info.address = endpoint.value().address;
        info.loopback = loopback;
        info.operational = operational;
        info.administrative_up = administrative_up;
        info.lan_adapter = lan_adapter;
        info.medium = adapter->IfType == IF_TYPE_IEEE80211
                          ? InterfaceMedium::wireless
                      : adapter->IfType == IF_TYPE_ETHERNET_CSMACD
                          ? InterfaceMedium::ethernet
                          : InterfaceMedium::unclassified;
        if (family == AF_INET) {
          if (unicast->OnLinkPrefixLength <= 32)
            info.ipv4_prefix_length = unicast->OnLinkPrefixLength;
          std::array<std::uint8_t, 4> bytes{};
          const auto broadcast =
              ipv4_broadcast(*reinterpret_cast<const sockaddr_in*>(
                                 unicast->Address.lpSockaddr),
                             unicast->OnLinkPrefixLength);
          std::memcpy(bytes.data(), &broadcast, sizeof(bytes));
          info.broadcast = IpAddress::ipv4(bytes);
          info.has_broadcast = true;
        }
        interfaces.push_back(std::move(info));
      }
    }
    return interfaces;
  }

  static auto capabilities() noexcept -> NetworkCapabilities {
    NetworkCapabilities capabilities;
    capabilities.socket_dont_fragment_ipv4 = true;
    capabilities.socket_dont_fragment_ipv6 = true;
    return capabilities;
  }

 private:
  static constexpr ULONG adapter_buffer_initial_size = 16 * 1024;
  static constexpr int adapter_query_attempts = 4;
  static constexpr std::size_t control_buffer_size = 256;

  static auto socket_of(Raw raw) noexcept -> SOCKET {
    return static_cast<SOCKET>(raw);
  }

  static auto raw_of(SOCKET socket) noexcept -> Raw {
    return static_cast<Raw>(socket);
  }

  [[nodiscard]] static NetError last_net_error() noexcept {
    return normalize_wsa_error(WSAGetLastError());
  }

  [[nodiscard]] static auto succeeded(int result) noexcept
      -> Expected<void, NetError> {
    if (result != 0) return unexpected(last_net_error());
    return {};
  }

  [[nodiscard]] static auto transferred(int result) noexcept
      -> Expected<std::size_t, NetError> {
    if (result < 0) return unexpected(last_net_error());
    return static_cast<std::size_t>(result);
  }

  [[nodiscard]] static auto set_option(Raw raw, int level, int name,
                                       int value) noexcept
      -> Expected<void, NetError> {
    const DWORD native = static_cast<DWORD>(value);
    return succeeded(::setsockopt(socket_of(raw), level, name,
                                  reinterpret_cast<const char*>(&native),
                                  sizeof(native)));
  }

  [[nodiscard]] static auto option_value(Raw raw, int name) noexcept
      -> Expected<int, NetError> {
    int value = 0;
    int length = static_cast<int>(sizeof(value));
    if (::getsockopt(socket_of(raw), SOL_SOCKET, name,
                     reinterpret_cast<char*>(&value), &length) != 0)
      return unexpected(last_net_error());
    return value;
  }

  template <class Query>
  [[nodiscard]] static auto named_endpoint(Raw raw, Query query) noexcept
      -> Expected<SocketEndpoint, NetError> {
    sockaddr_storage storage{};
    int length = static_cast<int>(sizeof(storage));
    if (query(socket_of(raw), reinterpret_cast<sockaddr*>(&storage), &length) !=
        0)
      return unexpected(last_net_error());
    return native_to_endpoint(storage);
  }

  [[nodiscard]] static auto received_size(const DatagramState& state,
                                          bool received, long long count,
                                          std::size_t capacity) noexcept
      -> Expected<std::size_t, NetError> {
    if (received) return static_cast<std::size_t>(count);
    const int error = ::WSAGetLastError();
    if (error == WSAECONNRESET && state.connected)
      return unexpected(NetError::connection_refused);
    if (error == WSAECONNRESET || error == WSAENETRESET)
      return unexpected(NetError::would_block);
    if (error != WSAEMSGSIZE) return unexpected(normalize_wsa_error(error));
    return capacity;
  }

  static auto endpoint_to_native(const SocketEndpoint& endpoint) noexcept
      -> NativeEndpoint {
    NativeEndpoint native{};
    if (endpoint.address.is_ipv4()) {
      sockaddr_in address{};
      address.sin_family = static_cast<ADDRESS_FAMILY>(AF_INET);
      const auto bytes = endpoint.address.ipv4_bytes();
      std::memcpy(&address.sin_addr.s_addr, bytes.data(), bytes.size());
      address.sin_port = htons(endpoint.port);
      std::memcpy(&native.storage, &address, sizeof(address));
      native.length = static_cast<int>(sizeof(address));
      return native;
    }
    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    const auto bytes = endpoint.address.ipv6_bytes();
    std::memcpy(address.sin6_addr.s6_addr, bytes.data(), bytes.size());
    address.sin6_port = htons(endpoint.port);
    address.sin6_scope_id = static_cast<ULONG>(endpoint.address.scope_id());
    std::memcpy(&native.storage, &address, sizeof(address));
    native.length = static_cast<int>(sizeof(address));
    return native;
  }

  [[nodiscard]] static auto native_to_endpoint(
      const sockaddr_storage& storage) noexcept
      -> Expected<SocketEndpoint, NetError> {
    if (storage.ss_family == AF_INET) {
      sockaddr_in address{};
      std::memcpy(&address, &storage, sizeof(address));
      std::array<std::uint8_t, 4> bytes{};
      std::memcpy(bytes.data(), &address.sin_addr.s_addr, bytes.size());
      return SocketEndpoint{IpAddress::ipv4(bytes), ntohs(address.sin_port)};
    }
    if (storage.ss_family == AF_INET6) {
      sockaddr_in6 address{};
      std::memcpy(&address, &storage, sizeof(address));
      std::array<std::uint8_t, 16> bytes{};
      std::memcpy(bytes.data(), address.sin6_addr.s6_addr, bytes.size());
      return SocketEndpoint{
          IpAddress::ipv6(bytes).with_scope_id(
              static_cast<std::uint32_t>(address.sin6_scope_id)),
          ntohs(address.sin6_port)};
    }
    return unexpected(NetError::invalid_argument);
  }

  static int address_family_of(const IpAddress& address) noexcept {
    return address.is_ipv4() ? AF_INET : AF_INET6;
  }

  static int family_level(Raw raw) noexcept {
    WSAPROTOCOL_INFOW info{};
    int length = static_cast<int>(sizeof(info));
    if (::getsockopt(socket_of(raw), SOL_SOCKET, SO_PROTOCOL_INFOW,
                     reinterpret_cast<char*>(&info), &length) != 0)
      return -1;
    if (info.iAddressFamily == AF_INET) return IPPROTO_IP;
    if (info.iAddressFamily == AF_INET6) return IPPROTO_IPV6;
    return -1;
  }

  static void set_nonblocking(SOCKET handle) noexcept {
    u_long mode = 1;
    (void)::ioctlsocket(handle, static_cast<long>(FIONBIO), &mode);
  }

  [[nodiscard]] static SOCKET open_uninheritable_socket(int family, int type,
                                                        int protocol) noexcept {
    return ::WSASocketW(family, type, protocol, nullptr, 0,
                        WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
  }

  static void disable_udp_reset_reports(SOCKET handle) noexcept {
    BOOL disabled = FALSE;
    DWORD returned = 0;
    (void)::WSAIoctl(handle, SIO_UDP_CONNRESET, &disabled, sizeof(disabled),
                     nullptr, 0, &returned, nullptr, nullptr);
    returned = 0;
    (void)::WSAIoctl(handle, SIO_UDP_NETRESET, &disabled, sizeof(disabled),
                     nullptr, 0, &returned, nullptr, nullptr);
  }

  static auto local_address(SOCKET socket) noexcept
      -> std::optional<sockaddr_in> {
    sockaddr_in address{};
    int length = sizeof(address);
    if (::getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) !=
            0 ||
        address.sin_family != AF_INET)
      return std::nullopt;
    return address;
  }

  [[nodiscard]] static auto receive_with_traffic_class(
      Raw raw, const DatagramState& state, std::span<std::byte> buffer) noexcept
      -> Expected<UdpReceiveResult, NetError> {
    NativeEndpoint native;
    alignas(WSACMSGHDR) std::array<char, control_buffer_size> control{};
    WSABUF data{static_cast<ULONG>(buffer.size()),
                reinterpret_cast<char*>(buffer.data())};
    WSAMSG message{};
    message.name = reinterpret_cast<sockaddr*>(&native.storage);
    message.namelen = static_cast<INT>(sizeof(sockaddr_storage));
    message.lpBuffers = &data;
    message.dwBufferCount = 1;
    message.Control.buf = control.data();
    message.Control.len = static_cast<ULONG>(control.size());
    DWORD received = 0;
    const bool delivered =
        state.receive_message(socket_of(raw), &message, &received, nullptr,
                              nullptr) == 0;
    const auto size = received_size(state, delivered, received, buffer.size());
    if (!size) return unexpected(size.error());
    UdpReceiveResult result;
    result.size = size.value();
    if (const auto source = native_to_endpoint(native.storage))
      result.source = source.value();
    if ((message.dwFlags & MSG_CTRUNC) == 0)
      result.traffic_class = control_traffic_class(message);
    return result;
  }

  [[nodiscard]] static auto control_traffic_class(WSAMSG& message) noexcept
      -> std::optional<std::uint8_t> {
    std::optional<std::uint8_t> traffic_class;
    for (auto* header = WSA_CMSG_FIRSTHDR(&message); header != nullptr;
         header = WSA_CMSG_NXTHDR(&message, header)) {
      const bool ipv4 =
          header->cmsg_level == IPPROTO_IP && header->cmsg_type == IP_TOS;
      bool ipv6 = false;
#ifdef IPV6_TCLASS
      ipv6 = header->cmsg_level == IPPROTO_IPV6 &&
             header->cmsg_type == IPV6_TCLASS;
#endif
      if (!ipv4 && !ipv6) continue;
      if (header->cmsg_len <= WSA_CMSG_LEN(0)) continue;
      if (ipv6 && header->cmsg_len >= WSA_CMSG_LEN(sizeof(INT))) {
        INT value{};
        std::memcpy(&value, WSA_CMSG_DATA(header), sizeof(value));
        traffic_class = static_cast<std::uint8_t>(value);
        continue;
      }
      std::uint8_t value{};
      std::memcpy(&value, WSA_CMSG_DATA(header), sizeof(value));
      traffic_class = value;
    }
    return traffic_class;
  }

  static std::string utf8_from_wide(const wchar_t* text) {
    if (text == nullptr) return {};
    const auto length = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0,
                                              nullptr, nullptr);
    if (length <= 1) return {};
    std::string converted(static_cast<std::size_t>(length - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text, -1, converted.data(), length,
                          nullptr, nullptr);
    return converted;
  }

  static auto wide_from_utf8(std::string_view text)
      -> std::optional<std::wstring> {
    if (text.empty()) return std::wstring{};
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
      return std::nullopt;
    const int input_size = static_cast<int>(text.size());
    const int length = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), input_size, nullptr, 0);
    if (length <= 0) return std::nullopt;
    std::wstring converted(static_cast<std::size_t>(length), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                              input_size, converted.data(), length) != length)
      return std::nullopt;
    return converted;
  }

  static NetError resolve_error(int error) noexcept {
    switch (error) {
      case WSATRY_AGAIN:
        return NetError::timed_out;
      case WSA_NOT_ENOUGH_MEMORY:
      case WSAENOBUFS:
        return NetError::no_buffers;
      case WSANOTINITIALISED:
        return NetError::system_failure;
      default:
        return NetError::host_unreachable;
    }
  }

  static std::uint32_t ipv4_broadcast(const sockaddr_in& address,
                                      std::uint8_t prefix) {
    if (prefix > 32) prefix = 32;
    const std::uint32_t mask =
        prefix == 0
            ? 0u
            : (~std::uint32_t{0} << (32u - static_cast<unsigned>(prefix)));
    const auto host = ::ntohl(address.sin_addr.s_addr);
    const auto broadcast = host | ~mask;
    const auto network = ::htonl(broadcast);
    std::uint32_t bytes = 0;
    std::memcpy(&bytes, &network, sizeof(bytes));
    return bytes;
  }
};

using Win32Stack = SocketStack<Win32Sockets>;

void SocketHandle::close(Raw raw) noexcept { Win32Sockets::close(raw); }

bool NetworkStack::ensure() noexcept {
  return Win32Sockets::ensure_wsa_started();
}

std::uint32_t NetworkStack::interface_index_from_name(
    const std::string& name) noexcept {
  return static_cast<std::uint32_t>(::if_nametoindex(name.c_str()));
}

std::string NetworkStack::scope_zone_name(std::uint32_t scope_id, bool) {
  return std::to_string(scope_id);
}

auto NetworkStack::create_udp_socket(NetworkProvider& network,
                                     const IpAddress& family_of)
    -> Expected<std::unique_ptr<UdpSocket>, NetError> {
  return Win32Stack::create_udp_socket(network, family_of);
}

auto NetworkStack::create_tcp_socket(NetworkProvider& network,
                                     const IpAddress& family_of)
    -> Expected<std::unique_ptr<TcpSocket>, NetError> {
  return Win32Stack::create_tcp_socket(network, family_of);
}

auto NetworkStack::create_tcp_listener(NetworkProvider& network,
                                       const IpAddress& family_of)
    -> Expected<std::unique_ptr<TcpListener>, NetError> {
  return Win32Stack::create_tcp_listener(network, family_of);
}

auto NetworkStack::create_poller(NetworkProvider& network,
                                 const ClockProvider& clock)
    -> Expected<std::unique_ptr<Poller>, NetError> {
  return Win32Stack::create_poller(network, clock);
}

auto NetworkStack::resolve(std::string_view host, std::string_view service)
    -> Expected<std::vector<IpAddress>, NetError> {
  return Win32Sockets::resolve(host, service);
}

auto NetworkStack::enumerate()
    -> Expected<std::vector<InterfaceInfo>, NetError> {
  return Win32Sockets::enumerate();
}

auto NetworkStack::capabilities() noexcept -> NetworkCapabilities {
  return Win32Sockets::capabilities();
}

}  // namespace libchiaking::platform::detail
