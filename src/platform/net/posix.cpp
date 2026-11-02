#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include <cerrno>

#include "network_stack.hpp"
#include "posix_dont_fragment.hpp"
#include "socket_stack.hpp"
#include "src/core/utils/memory/c_handle.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace libchiaking::platform::detail {

class PosixSockets {
  using Raw = SocketHandle::Raw;

  struct NativeEndpoint {
    sockaddr_storage storage{};
    socklen_t length{0};

    auto address() const noexcept -> const sockaddr* {
      return reinterpret_cast<const sockaddr*>(&storage);
    }
  };

 public:
  struct DatagramState {
    bool traffic_class{};
  };

  using PollEntry = pollfd;
  static constexpr short int poll_readable = POLLIN;
  static constexpr short int poll_writable = POLLOUT;
  static constexpr short int poll_error = POLLERR;
  static constexpr short int poll_hangup = POLLHUP;
  static constexpr short int poll_invalid = POLLNVAL;

  [[nodiscard]] static auto open(const IpAddress& family_of,
                                 SocketKind kind) noexcept
      -> Expected<SocketHandle, NetError> {
    const bool stream = kind == SocketKind::stream;
    SocketHandle handle(
        ::socket(address_family_of(family_of),
                 (stream ? SOCK_STREAM : SOCK_DGRAM) | socket_cloexec,
                 stream ? IPPROTO_TCP : IPPROTO_UDP));
    if (!handle.is_open()) return unexpected(last_net_error());
    set_cloexec(fd(handle.get()));
    set_nonblocking(fd(handle.get()));
    if (stream) suppress_sigpipe(fd(handle.get()));
    return handle;
  }

  static void close(Raw raw) noexcept { (void)::close(fd(raw)); }

  [[nodiscard]] static auto bind(Raw raw,
                                 const SocketEndpoint& endpoint) noexcept
      -> Expected<void, NetError> {
    const auto native = endpoint_to_native(endpoint);
    return succeeded(::bind(fd(raw), native.address(), native.length));
  }

  [[nodiscard]] static auto connect(Raw raw,
                                    const SocketEndpoint& endpoint) noexcept
      -> Expected<void, NetError> {
    const auto native = endpoint_to_native(endpoint);
    return succeeded(::connect(fd(raw), native.address(), native.length));
  }

  [[nodiscard]] static auto connect_datagram(
      Raw raw, DatagramState&, const SocketEndpoint& endpoint) noexcept
      -> Expected<void, NetError> {
    return connect(raw, endpoint);
  }

  [[nodiscard]] static auto send_to(Raw raw,
                                    std::span<const std::byte> datagram,
                                    const SocketEndpoint& destination) noexcept
      -> Expected<std::size_t, NetError> {
    const auto native = endpoint_to_native(destination);
    return transferred(::sendto(fd(raw), datagram.data(), datagram.size(), 0,
                                native.address(), native.length));
  }

  [[nodiscard]] static auto send(Raw raw, std::span<const std::byte> data,
                                 SocketKind kind) noexcept
      -> Expected<std::size_t, NetError> {
    const int flags = kind == SocketKind::stream ? stream_send_flags : 0;
    return transferred(::send(fd(raw), data.data(), data.size(), flags));
  }

  [[nodiscard]] static auto receive(Raw raw,
                                    std::span<std::byte> buffer) noexcept
      -> Expected<std::size_t, NetError> {
    return transferred(::recv(fd(raw), buffer.data(), buffer.size(), 0));
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
    return set_option(raw, level, option, enabled ? 1 : 0)
        .transform([&state, enabled] { state.traffic_class = enabled; });
  }

  [[nodiscard]] static auto receive_from(Raw raw, const DatagramState& state,
                                         std::span<std::byte> buffer) noexcept
      -> Expected<UdpReceiveResult, NetError> {
    if (state.traffic_class) return receive_with_traffic_class(raw, buffer);
    NativeEndpoint native;
    auto received = ssize_t{-1};
    do {
      native.length = static_cast<socklen_t>(sizeof(sockaddr_storage));
      received = ::recvfrom(fd(raw), buffer.data(), buffer.size(), 0,
                            reinterpret_cast<sockaddr*>(&native.storage),
                            &native.length);
    } while (received < 0 && errno == EINTR);
    if (received < 0) return unexpected(last_net_error());
    UdpReceiveResult result;
    result.size = static_cast<std::size_t>(received);
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
#ifdef SO_REUSEPORT
    return set_option(raw, SOL_SOCKET, SO_REUSEPORT, enabled ? 1 : 0);
#else
    return set_reuse_address(raw, enabled);
#endif
  }

  [[nodiscard]] static auto set_dont_fragment(Raw raw, bool enabled) noexcept
      -> Expected<void, NetError> {
    const int level = family_level(raw);
    if (level < 0) return unexpected(NetError::not_supported);
    return apply_dont_fragment(
        level, enabled,
        [raw](int option_level, int name, const void* value, socklen_t size) {
          return ::setsockopt(fd(raw), option_level, name, value, size);
        },
        [] { return last_net_error(); });
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
    if (family_level(raw) != IPPROTO_IP)
      return unexpected(NetError::not_supported);
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
          errno = error_code;
          return unexpected(last_net_error());
        });
  }

  [[nodiscard]] static auto listen(Raw raw, int backlog) noexcept
      -> Expected<void, NetError> {
    return succeeded(::listen(fd(raw), backlog));
  }

  [[nodiscard]] static auto accept(Raw raw) noexcept
      -> Expected<SocketHandle, NetError> {
    sockaddr_storage storage{};
    auto length = static_cast<socklen_t>(sizeof(storage));
    SocketHandle accepted(accept_cloexec(
        fd(raw), reinterpret_cast<sockaddr*>(&storage), &length));
    if (!accepted.is_open()) return unexpected(last_net_error());
    set_nonblocking(fd(accepted.get()));
    suppress_sigpipe(fd(accepted.get()));
    return accepted;
  }

  [[nodiscard]] static auto open_wake_sockets() noexcept
      -> Expected<WakeSockets, NetError> {
    std::array<int, 2> pair{};
    if (::socketpair(AF_UNIX, SOCK_STREAM | socket_cloexec, 0, pair.data()) !=
        0)
      return unexpected(NetError::no_buffers);
    WakeSockets sockets{.receiver = SocketHandle{pair[1]},
                        .sender = SocketHandle{pair[0]}};
    for (const int handle : pair) {
      set_cloexec(handle);
      set_nonblocking(handle);
    }
    suppress_sigpipe(pair[0]);
    return sockets;
  }

  [[nodiscard]] static bool send_wake(Raw raw) noexcept {
    const char token = 1;
    if (::send(fd(raw), &token, 1, stream_send_flags) >= 0) return true;
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS;
  }

  static void drain_wake(Raw raw) noexcept {
    std::array<char, wake_drain_buffer_size> buffer{};
    while (::recv(fd(raw), buffer.data(), buffer.size(), 0) > 0) {
    }
  }

  static auto poll_entry(Raw raw, short int events) noexcept -> PollEntry {
    return pollfd{fd(raw), events, 0};
  }

  [[nodiscard]] static int poll(std::vector<PollEntry>& entries,
                                int timeout_ms) noexcept {
    for (auto& entry : entries) entry.revents = 0;
    const int result =
        ::poll(entries.data(), static_cast<nfds_t>(entries.size()), timeout_ms);
    if (result < 0 && errno == EINTR) {
      for (auto& entry : entries) entry.revents = 0;
      return 0;
    }
    return result;
  }

  [[nodiscard]] static auto resolve(std::string_view host,
                                    std::string_view service)
      -> Expected<std::vector<IpAddress>, NetError> {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
#ifdef AI_ADDRCONFIG
    hints.ai_flags = AI_ADDRCONFIG;
#endif
    const std::string host_text(host);
    const std::string service_text(service);
    addrinfo* raw = nullptr;
    const int status = ::getaddrinfo(
        host_text.c_str(),
        service_text.empty() ? nullptr : service_text.c_str(), &hints, &raw);
    if (status == EAI_AGAIN) return unexpected(NetError::timed_out);
    if (status == EAI_MEMORY) return unexpected(NetError::no_buffers);
    if (status != 0) return unexpected(NetError::host_unreachable);
    const memory::CHandle<addrinfo, ::freeaddrinfo> head{raw};
    std::vector<IpAddress> addresses;
    for (const addrinfo* entry = head.get(); entry != nullptr;
         entry = entry->ai_next) {
      if (const auto endpoint = native_to_endpoint(
              *reinterpret_cast<const sockaddr_storage*>(entry->ai_addr)))
        addresses.push_back(endpoint.value().address);
    }
    if (addresses.empty()) return unexpected(NetError::host_unreachable);
    return addresses;
  }

  [[nodiscard]] static auto enumerate()
      -> Expected<std::vector<InterfaceInfo>, NetError> {
    ifaddrs* raw = nullptr;
    if (::getifaddrs(&raw) != 0) return unexpected(last_net_error());
    const memory::CHandle<ifaddrs, ::freeifaddrs> head{raw};
    std::vector<InterfaceInfo> interfaces;
    for (const ifaddrs* entry = head.get(); entry != nullptr;
         entry = entry->ifa_next) {
      const auto address = interface_address(entry->ifa_addr);
      if (!address) continue;
      const auto endpoint = native_to_endpoint(*address);
      if (!endpoint) continue;
      InterfaceInfo info;
      info.name = entry->ifa_name != nullptr ? entry->ifa_name : "";
      info.index = ::if_nametoindex(info.name.c_str());
      info.address = endpoint.value().address;
      info.loopback = (entry->ifa_flags & IFF_LOOPBACK) != 0;
      info.operational =
          (entry->ifa_flags & (IFF_UP | IFF_RUNNING)) == (IFF_UP | IFF_RUNNING);
      info.administrative_up = (entry->ifa_flags & IFF_UP) != 0;
      if (info.address.is_ipv4())
        if (const auto netmask = interface_address(entry->ifa_netmask);
            netmask && netmask->ss_family == AF_INET) {
          sockaddr_in mask{};
          std::memcpy(&mask, &*netmask, sizeof(mask));
          info.ipv4_prefix_length = static_cast<std::uint8_t>(
              std::countl_one(std::uint32_t{ntohl(mask.sin_addr.s_addr)}));
        }
      if ((entry->ifa_flags & IFF_BROADCAST) != 0)
        if (const auto broadcast_address =
                interface_address(entry->ifa_dstaddr))
          if (const auto broadcast = native_to_endpoint(*broadcast_address)) {
            info.broadcast = broadcast.value().address;
            info.has_broadcast = true;
          }
      interfaces.push_back(std::move(info));
    }
    return interfaces;
  }

  static auto capabilities() noexcept -> NetworkCapabilities {
    static const NetworkCapabilities probed = [] {
      NetworkCapabilities capabilities;
      capabilities.socket_dont_fragment_ipv4 =
          probe_dont_fragment(AF_INET, IPPROTO_IP);
      capabilities.socket_dont_fragment_ipv6 =
          probe_dont_fragment(AF_INET6, IPPROTO_IPV6);
      return capabilities;
    }();
    return probed;
  }

 private:
#ifdef SOCK_CLOEXEC
  static constexpr int socket_cloexec = SOCK_CLOEXEC;
#else
  static constexpr int socket_cloexec = 0;
#endif

#ifdef MSG_NOSIGNAL
  static constexpr int stream_send_flags = MSG_NOSIGNAL;
#else
  static constexpr int stream_send_flags = 0;
#endif

  static constexpr std::size_t control_buffer_size = 256;

  static auto fd(Raw raw) noexcept -> int { return static_cast<int>(raw); }

  [[nodiscard]] static NetError last_net_error() noexcept {
    switch (errno) {
      case EAGAIN:
#if EAGAIN != EWOULDBLOCK
      case EWOULDBLOCK:
#endif
      case EINPROGRESS:
      case EALREADY:
        return NetError::would_block;
      case EPIPE:
      case ECONNRESET:
        return NetError::connection_reset;
      case ECONNREFUSED:
        return NetError::connection_refused;
      case ECONNABORTED:
        return NetError::connection_aborted;
      case ENOTCONN:
        return NetError::not_connected;
      case EISCONN:
        return NetError::already_connected;
      case EADDRINUSE:
        return NetError::address_in_use;
      case EADDRNOTAVAIL:
        return NetError::address_not_available;
      case ENETUNREACH:
        return NetError::network_unreachable;
      case EHOSTUNREACH:
        return NetError::host_unreachable;
      case ENETDOWN:
        return NetError::network_down;
      case EACCES:
      case EPERM:
        return NetError::permission_denied;
      case EINVAL:
        return NetError::invalid_argument;
      case EMSGSIZE:
        return NetError::message_too_large;
      case ENOBUFS:
      case ENOMEM:
        return NetError::no_buffers;
      case EMFILE:
      case ENFILE:
        return NetError::resource_exhausted;
      case ETIMEDOUT:
        return NetError::timed_out;
      case EINTR:
        return NetError::interrupted;
      default:
        return NetError::system_failure;
    }
  }

  [[nodiscard]] static auto succeeded(int result) noexcept
      -> Expected<void, NetError> {
    if (result != 0) return unexpected(last_net_error());
    return {};
  }

  [[nodiscard]] static auto transferred(ssize_t result) noexcept
      -> Expected<std::size_t, NetError> {
    if (result < 0) return unexpected(last_net_error());
    return static_cast<std::size_t>(result);
  }

  [[nodiscard]] static auto set_option(Raw raw, int level, int name,
                                       int value) noexcept
      -> Expected<void, NetError> {
    return succeeded(::setsockopt(fd(raw), level, name, &value, sizeof(value)));
  }

  [[nodiscard]] static auto option_value(Raw raw, int name) noexcept
      -> Expected<int, NetError> {
    int value = 0;
    auto length = static_cast<socklen_t>(sizeof(value));
    if (::getsockopt(fd(raw), SOL_SOCKET, name, &value, &length) != 0)
      return unexpected(last_net_error());
    return value;
  }

  template <class Query>
  [[nodiscard]] static auto named_endpoint(Raw raw, Query query) noexcept
      -> Expected<SocketEndpoint, NetError> {
    sockaddr_storage storage{};
    auto length = static_cast<socklen_t>(sizeof(storage));
    if (query(fd(raw), reinterpret_cast<sockaddr*>(&storage), &length) != 0)
      return unexpected(last_net_error());
    return native_to_endpoint(storage);
  }

  static auto endpoint_to_native(const SocketEndpoint& endpoint) noexcept
      -> NativeEndpoint {
    NativeEndpoint native{};
    if (endpoint.address.is_ipv4()) {
      sockaddr_in address{};
      address.sin_family = AF_INET;
      const auto bytes = endpoint.address.ipv4_bytes();
      std::memcpy(&address.sin_addr.s_addr, bytes.data(), bytes.size());
      address.sin_port = htons(endpoint.port);
      std::memcpy(&native.storage, &address, sizeof(address));
      native.length = static_cast<socklen_t>(sizeof(address));
      return native;
    }
    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    const auto bytes = endpoint.address.ipv6_bytes();
    std::memcpy(address.sin6_addr.s6_addr, bytes.data(), bytes.size());
    address.sin6_port = htons(endpoint.port);
    address.sin6_scope_id = endpoint.address.scope_id();
    std::memcpy(&native.storage, &address, sizeof(address));
    native.length = static_cast<socklen_t>(sizeof(address));
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
      std::uint32_t scope_id = address.sin6_scope_id;
      const bool link_scoped = IpAddress::ipv6(bytes).is_link_local();
      if (link_scoped && (bytes[2] != 0 || bytes[3] != 0)) {
        if (scope_id == 0)
          scope_id = (static_cast<std::uint32_t>(bytes[2]) << 8) | bytes[3];
        bytes[2] = 0;
        bytes[3] = 0;
      }
      return SocketEndpoint{IpAddress::ipv6(bytes).with_scope_id(scope_id),
                            ntohs(address.sin6_port)};
    }
    return unexpected(NetError::invalid_argument);
  }

  static int address_family_of(const IpAddress& address) noexcept {
    return address.is_ipv4() ? AF_INET : AF_INET6;
  }

  static int family_level(Raw raw) noexcept {
    sockaddr_storage storage{};
    auto length = static_cast<socklen_t>(sizeof(storage));
    if (::getsockname(fd(raw), reinterpret_cast<sockaddr*>(&storage),
                      &length) != 0)
      return -1;
    if (storage.ss_family == AF_INET) return IPPROTO_IP;
    if (storage.ss_family == AF_INET6) return IPPROTO_IPV6;
    return -1;
  }

  static void set_nonblocking(int handle) noexcept {
    (void)::fcntl(handle, F_SETFL, ::fcntl(handle, F_GETFL, 0) | O_NONBLOCK);
  }

  static void set_cloexec(int handle) noexcept {
    if constexpr (socket_cloexec == 0)
      (void)::fcntl(handle, F_SETFD, ::fcntl(handle, F_GETFD, 0) | FD_CLOEXEC);
  }

  [[nodiscard]] static int accept_cloexec(int listener, sockaddr* address,
                                          socklen_t* length) noexcept {
#if defined(LIBCHIAKING_HAVE_ACCEPT4)
    return ::accept4(listener, address, length, SOCK_CLOEXEC);
#else
    const auto accepted = ::accept(listener, address, length);
    if (accepted >= 0) set_cloexec(accepted);
    return accepted;
#endif
  }

  static void suppress_sigpipe(int handle) noexcept {
#ifdef SO_NOSIGPIPE
    const int value = 1;
    (void)::setsockopt(handle, SOL_SOCKET, SO_NOSIGPIPE, &value, sizeof(value));
#else
    (void)handle;
#endif
  }

  [[nodiscard]] static auto receive_with_traffic_class(
      Raw raw, std::span<std::byte> buffer) noexcept
      -> Expected<UdpReceiveResult, NetError> {
    NativeEndpoint native;
    alignas(cmsghdr) std::array<std::byte, control_buffer_size> control{};
    iovec vector{buffer.data(), buffer.size()};
    msghdr message{};
    message.msg_name = &native.storage;
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    auto received = ssize_t{-1};
    do {
      message.msg_namelen = static_cast<socklen_t>(sizeof(sockaddr_storage));
      message.msg_controllen = control.size();
      received = ::recvmsg(fd(raw), &message, 0);
    } while (received < 0 && errno == EINTR);
    if (received < 0) return unexpected(last_net_error());
    UdpReceiveResult result;
    result.size = static_cast<std::size_t>(received);
    if (const auto source = native_to_endpoint(native.storage))
      result.source = source.value();
    if ((message.msg_flags & MSG_CTRUNC) == 0)
      result.traffic_class = control_traffic_class(message);
    return result;
  }

  [[nodiscard]] static auto control_traffic_class(msghdr& message) noexcept
      -> std::optional<std::uint8_t> {
    std::optional<std::uint8_t> traffic_class;
    for (auto* header = CMSG_FIRSTHDR(&message); header != nullptr;
         header = CMSG_NXTHDR(&message, header)) {
      const bool ipv4 = header->cmsg_level == IPPROTO_IP &&
                        (header->cmsg_type == IP_TOS
#ifdef IP_RECVTOS
                         || header->cmsg_type == IP_RECVTOS
#endif
                        );
      bool ipv6 = false;
#ifdef IPV6_TCLASS
      ipv6 = header->cmsg_level == IPPROTO_IPV6 &&
             header->cmsg_type == IPV6_TCLASS;
#endif
      if (!ipv4 && !ipv6) continue;
      const auto payload = header->cmsg_len - CMSG_LEN(0);
      if (payload == 0) continue;
      if (ipv6 && payload >= sizeof(int)) {
        int value{};
        std::memcpy(&value, CMSG_DATA(header), sizeof(value));
        traffic_class = static_cast<std::uint8_t>(value);
        continue;
      }
      std::uint8_t value{};
      std::memcpy(&value, CMSG_DATA(header), sizeof(value));
      traffic_class = value;
    }
    return traffic_class;
  }

  [[nodiscard]] static bool probe_dont_fragment(int family,
                                                int level) noexcept {
    const SocketHandle probe(
        ::socket(family, SOCK_DGRAM | socket_cloexec, IPPROTO_UDP));
    if (!probe.is_open()) return false;
    set_cloexec(fd(probe.get()));
    return dont_fragment_available(level, [&probe](int option_level, int name,
                                                   const void* option,
                                                   socklen_t size) {
      return ::setsockopt(fd(probe.get()), option_level, name, option, size);
    });
  }

  [[nodiscard]] static auto interface_address(const sockaddr* address) noexcept
      -> std::optional<sockaddr_storage> {
    if (address == nullptr) return std::nullopt;
    sa_family_t family{};
    std::memcpy(&family,
                reinterpret_cast<const unsigned char*>(address) +
                    offsetof(sockaddr, sa_family),
                sizeof family);
    std::size_t length = 0;
    if (family == AF_INET)
      length = sizeof(sockaddr_in);
    else if (family == AF_INET6)
      length = sizeof(sockaddr_in6);
    else
      return std::nullopt;
    sockaddr_storage storage{};
    std::memcpy(&storage, address, length);
    return storage;
  }
};

using PosixStack = SocketStack<PosixSockets>;

void SocketHandle::close(Raw raw) noexcept { PosixSockets::close(raw); }

bool NetworkStack::ensure() noexcept { return true; }

std::uint32_t NetworkStack::interface_index_from_name(
    const std::string& name) noexcept {
  return static_cast<std::uint32_t>(::if_nametoindex(name.c_str()));
}

std::string NetworkStack::scope_zone_name(std::uint32_t scope_id,
                                          bool link_local) {
  if (link_local) {
    std::array<char, IF_NAMESIZE + 1> name{};
    if (::if_indextoname(scope_id, name.data()) != nullptr) return name.data();
  }
  return std::to_string(scope_id);
}

auto NetworkStack::create_udp_socket(NetworkProvider& network,
                                     const IpAddress& family_of)
    -> Expected<std::unique_ptr<UdpSocket>, NetError> {
  return PosixStack::create_udp_socket(network, family_of);
}

auto NetworkStack::create_tcp_socket(NetworkProvider& network,
                                     const IpAddress& family_of)
    -> Expected<std::unique_ptr<TcpSocket>, NetError> {
  return PosixStack::create_tcp_socket(network, family_of);
}

auto NetworkStack::create_tcp_listener(NetworkProvider& network,
                                       const IpAddress& family_of)
    -> Expected<std::unique_ptr<TcpListener>, NetError> {
  return PosixStack::create_tcp_listener(network, family_of);
}

auto NetworkStack::create_poller(NetworkProvider& network,
                                 const ClockProvider& clock)
    -> Expected<std::unique_ptr<Poller>, NetError> {
  return PosixStack::create_poller(network, clock);
}

auto NetworkStack::resolve(std::string_view host, std::string_view service)
    -> Expected<std::vector<IpAddress>, NetError> {
  return PosixSockets::resolve(host, service);
}

auto NetworkStack::enumerate()
    -> Expected<std::vector<InterfaceInfo>, NetError> {
  return PosixSockets::enumerate();
}

auto NetworkStack::capabilities() noexcept -> NetworkCapabilities {
  return PosixSockets::capabilities();
}

}  // namespace libchiaking::platform::detail
