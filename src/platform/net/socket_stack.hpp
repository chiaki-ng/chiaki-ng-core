/// @file
/// @brief Declares SocketStack, the stock socket, listener and poller
/// objects, written once over a per-system `Native` layer of BSD sockets or
/// Winsock calls.
#pragma once

#include <libchiaking/platform/net/poller.hpp>
#include <libchiaking/platform/net/provider.hpp>
#include <libchiaking/platform/net/socket.hpp>

#include "src/core/utils/timing/saturating.hpp"
#include "src/platform/net/network_stack.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace libchiaking::platform::detail {

/// @brief Whether a socket carries datagrams or a byte stream.
enum class SocketKind : std::uint8_t {
  /// @brief UDP.
  datagram,
  /// @brief TCP.
  stream,
};

/// @brief Sole ownership of one system socket, closed when the handle is
/// destroyed or reset.
///
/// Holds a POSIX descriptor or a Winsock `SOCKET` widened to an integer.
/// Move-only. Not synchronised; one thread at a time.
class SocketHandle {
 public:
  /// @brief The socket as an integer wide enough for either system.
  using Raw = std::intptr_t;
  /// @brief The value of a handle that holds no socket.
  static constexpr Raw invalid = -1;

  /// @brief Makes an empty handle.
  SocketHandle() noexcept = default;
  /// @brief Takes ownership of a socket.
  ///
  /// @param raw The socket, or invalid.
  explicit SocketHandle(Raw raw) noexcept : raw_(raw) {}

  SocketHandle(const SocketHandle&) = delete;
  SocketHandle& operator=(const SocketHandle&) = delete;

  /// @brief Takes over another handle's socket, leaving it empty.
  ///
  /// @param other The handle to take from.
  SocketHandle(SocketHandle&& other) noexcept
      : raw_(std::exchange(other.raw_, invalid)) {}

  /// @brief Closes this handle's socket and takes over another's.
  ///
  /// @param other The handle to take from.
  /// @return This handle.
  SocketHandle& operator=(SocketHandle&& other) noexcept {
    if (this != &other) reset(std::exchange(other.raw_, invalid));
    return *this;
  }

  /// @brief Closes the socket, if any.
  ~SocketHandle() { reset(); }

  /// @brief Returns the socket without giving up ownership.
  ///
  /// @return The socket, or invalid.
  auto get() const noexcept -> Raw { return raw_; }

  /// @brief Tells whether the handle holds a socket.
  ///
  /// @return False when empty.
  auto is_open() const noexcept -> bool { return raw_ != invalid; }

  /// @brief Closes the held socket, if any, and takes ownership of
  /// another.
  ///
  /// @param raw The new socket, or invalid to leave the handle empty.
  void reset(Raw raw = invalid) noexcept {
    if (is_open()) close(std::exchange(raw_, invalid));
    raw_ = raw;
  }

 private:
  /// @brief Closes a socket with the system's call; defined by each
  /// system's stack.
  ///
  /// @param raw The socket.
  static void close(Raw raw) noexcept;

  /// @brief The socket, or invalid.
  Raw raw_{invalid};
};

/// @brief The two ends a poller is woken through: a byte written to `sender`
/// makes `receiver` readable.
///
/// A Unix socket pair on POSIX and a connected pair of loopback UDP sockets
/// on Windows, both non-blocking.
struct WakeSockets {
  /// @brief The end the poller watches.
  SocketHandle receiver;
  /// @brief The end wake() writes to.
  SocketHandle sender;
};

/// @brief The UDP sockets, TCP sockets, listeners and pollers of the stock
/// network provider, over one system's socket calls.
///
/// Every socket refuses work with NetError::closed once closed. A poller
/// waits only on sockets of this stack that report the same provider as
/// owner. Each object follows the threading rules of the public interface it
/// implements; in particular a poller's wake() may be called from any
/// thread while another waits.
///
/// @tparam Native The system layer: a class of static functions that open,
///     bind, connect, send, receive, set options on and close raw sockets,
///     make and signal the wake socket pair, and poll; with `PollEntry`,
///     `DatagramState` and the `poll_readable`, `poll_writable`,
///     `poll_error`, `poll_hangup` and `poll_invalid` flags. posix.cpp and
///     win32.cpp each define one.
template <class Native>
class SocketStack {
  /// @brief A raw system socket.
  using Raw = SocketHandle::Raw;
  /// @brief What every socket object shares: its owner, its handle, and the
  /// refusal to work once closed.
  class SocketBase {
   public:
    /// @brief Wraps an open socket.
    ///
    /// @param owner The provider the socket reports as its owner.
    /// @param handle The socket.
    SocketBase(NetworkProvider& owner, SocketHandle handle) noexcept
        : owner_(&owner), handle_(std::move(handle)) {}

    /// @brief Closes the socket, if still open.
    virtual ~SocketBase() = default;

    SocketBase(const SocketBase&) = delete;
    SocketBase& operator=(const SocketBase&) = delete;

    /// @brief Returns the provider that made the socket.
    ///
    /// @return The owner.
    auto provider() const noexcept -> NetworkProvider& { return *owner_; }

    /// @brief Returns the raw socket.
    ///
    /// @return The socket, or SocketHandle::invalid once closed.
    auto raw() const noexcept -> Raw { return handle_.get(); }

    /// @brief Tells whether the socket is still open.
    ///
    /// @return False once closed.
    auto is_open() const noexcept -> bool { return handle_.is_open(); }

   protected:
    /// @brief Runs an operation on the raw socket, or refuses it when the
    /// socket is closed.
    ///
    /// @tparam Operation A callable taking the raw socket and returning
    ///     an Expected with NetError.
    /// @param operation The operation.
    /// @return What the operation returned.
    /// @retval NetError::closed The socket was closed.
    template <class Operation>
    auto when_open(Operation&& operation)
        -> std::invoke_result_t<Operation, Raw> {
      if (!is_open()) return unexpected(NetError::closed);
      return std::forward<Operation>(operation)(raw());
    }

    /// @brief Closes the socket.
    ///
    /// @return Nothing on success.
    /// @retval NetError::closed It was already closed.
    auto close_socket() -> Expected<void, NetError> {
      return when_open([this](Raw) -> Expected<void, NetError> {
        handle_.reset();
        return {};
      });
    }

   private:
    /// @brief The provider that made the socket.
    NetworkProvider* owner_;
    /// @brief The socket.
    SocketHandle handle_;
  };

  /// @brief A UDP socket, as platform::UdpSocket describes.
  ///
  /// Constructed with SocketBase's constructor, which it inherits.
  class UdpSocket final : public platform::UdpSocket, public SocketBase {
   public:
    /// @cond
    using SocketBase::SocketBase;
    /// @endcond

    const NetworkProvider& owner() const noexcept override {
      return this->provider();
    }

    [[nodiscard]] auto bind(const SocketEndpoint& endpoint)
        -> Expected<void, NetError> override {
      return this->when_open(
          [&](Raw raw) { return Native::bind(raw, endpoint); });
    }

    [[nodiscard]] auto send_to(std::span<const std::byte> datagram,
                               const SocketEndpoint& destination)
        -> Expected<std::size_t, NetError> override {
      return this->when_open(
          [&](Raw raw) { return Native::send_to(raw, datagram, destination); });
    }

    [[nodiscard]] auto connect(const SocketEndpoint& endpoint)
        -> Expected<void, NetError> override {
      return this->when_open([&](Raw raw) {
        return Native::connect_datagram(raw, state_, endpoint);
      });
    }

    [[nodiscard]] auto send(std::span<const std::byte> datagram)
        -> Expected<std::size_t, NetError> override {
      return this->when_open([&](Raw raw) {
        return Native::send(raw, datagram, SocketKind::datagram);
      });
    }

    [[nodiscard]] auto receive_traffic_class(bool enabled)
        -> Expected<void, NetError> override {
      return this->when_open([&](Raw raw) {
        return Native::receive_traffic_class(raw, state_, enabled);
      });
    }

    [[nodiscard]] auto receive_from(std::span<std::byte> buffer)
        -> Expected<UdpReceiveResult, NetError> override {
      return this->when_open(
          [&](Raw raw) { return Native::receive_from(raw, state_, buffer); });
    }

    [[nodiscard]] auto set_boolean_option(SocketOption option, bool enabled)
        -> Expected<void, NetError> override {
      return this->when_open([&](Raw raw) -> Expected<void, NetError> {
        switch (option) {
          case SocketOption::broadcast:
            return Native::set_broadcast(raw, enabled);
          case SocketOption::reuse_address:
            return Native::set_reuse_address(raw, enabled);
          case SocketOption::dont_fragment:
            return Native::set_dont_fragment(raw, enabled);
        }
        return unexpected(NetError::invalid_argument);
      });
    }

    [[nodiscard]] auto set_shared_port_binding(bool enabled)
        -> Expected<void, NetError> override {
      return this->when_open(
          [&](Raw raw) { return Native::set_shared_port(raw, enabled); });
    }

    /// @brief Asks the system for a receive buffer of a size, clamped to
    /// max_receive_buffer_bytes.
    ///
    /// @param bytes The size wanted, in bytes; the system may grant
    ///     another size, which receive_buffer_size() reports.
    /// @return Nothing on success.
    /// @retval NetError::closed The socket was closed.
    /// @retval NetError The system refused.
    [[nodiscard]] auto set_receive_buffer_size(std::size_t bytes)
        -> Expected<void, NetError> override {
      return this->when_open([&](Raw raw) {
        return Native::set_receive_buffer_size(
            raw, static_cast<int>(std::min(bytes, max_receive_buffer_bytes)));
      });
    }

    [[nodiscard]] auto receive_buffer_size()
        -> Expected<std::size_t, NetError> override {
      return this->when_open(
          [](Raw raw) { return Native::receive_buffer_size(raw); });
    }

    /// @brief Sets the time to live of outgoing IPv4 datagrams.
    ///
    /// @param ttl The hop limit, 1 to 255.
    /// @return Nothing on success.
    /// @retval NetError::invalid_argument `ttl` is 0.
    /// @retval NetError::closed The socket was closed.
    /// @retval NetError The system refused.
    [[nodiscard]] auto set_ipv4_ttl(std::uint8_t ttl)
        -> Expected<void, NetError> override {
      return this->when_open([ttl](Raw raw) -> Expected<void, NetError> {
        if (ttl == 0) return unexpected(NetError::invalid_argument);
        return Native::set_ipv4_ttl(raw, ttl);
      });
    }

    [[nodiscard]] auto local_endpoint()
        -> Expected<SocketEndpoint, NetError> override {
      return this->when_open(
          [](Raw raw) { return Native::local_endpoint(raw); });
    }

    [[nodiscard]] auto close() -> Expected<void, NetError> override {
      return this->close_socket();
    }

   private:
    /// @brief What the system layer remembers about the socket, such as
    /// whether traffic classes are being received.
    typename Native::DatagramState state_{};
  };

  /// @brief A TCP connection, as platform::TcpSocket describes.
  ///
  /// Constructed with SocketBase's constructor, which it inherits.
  /// A connection reset or aborted by the peer is reported as
  /// NetError::closed, like an orderly close.
  class TcpSocket final : public platform::TcpSocket, public SocketBase {
   public:
    /// @cond
    using SocketBase::SocketBase;
    /// @endcond

    const NetworkProvider& owner() const noexcept override {
      return this->provider();
    }

    [[nodiscard]] auto bind(const SocketEndpoint& endpoint)
        -> Expected<void, NetError> override {
      return this->when_open(
          [&](Raw raw) { return Native::bind(raw, endpoint); });
    }

    /// @brief Starts connecting, without blocking.
    ///
    /// A socket already connected counts as success.
    ///
    /// @param endpoint The peer.
    /// @return Nothing when connected or already connected.
    /// @retval NetError::would_block The connection is under way; wait
    ///     for writability and call complete_connect().
    /// @retval NetError::closed The socket was closed.
    /// @retval NetError Any other failure.
    [[nodiscard]] auto connect(const SocketEndpoint& endpoint)
        -> Expected<void, NetError> override {
      return this->when_open([&](Raw raw) {
        return Native::connect(raw, endpoint).or_else([](NetError error) {
          return error == NetError::already_connected
                     ? Expected<void, NetError>{}
                     : Expected<void, NetError>{unexpected(error)};
        });
      });
    }

    [[nodiscard]] auto complete_connect() -> Expected<void, NetError> override {
      return this->when_open(
          [](Raw raw) { return Native::pending_error(raw); });
    }

    [[nodiscard]] auto send(std::span<const std::byte> data)
        -> Expected<std::size_t, NetError> override {
      return this->when_open([&](Raw raw) {
        return Native::send(raw, data, SocketKind::stream)
            .transform_error(closed_when_dropped);
      });
    }

    /// @brief Receives what has arrived, without blocking.
    ///
    /// @param buffer Receives the bytes.
    /// @return How many bytes were received; never zero.
    /// @retval NetError::closed The peer closed, reset or aborted the
    ///     connection, or this socket was closed.
    /// @retval NetError::would_block Nothing has arrived.
    /// @retval NetError Any other failure.
    [[nodiscard]] auto receive(std::span<std::byte> buffer)
        -> Expected<std::size_t, NetError> override {
      return this->when_open([&](Raw raw) -> Expected<std::size_t, NetError> {
        const auto received =
            Native::receive(raw, buffer).transform_error(closed_when_dropped);
        if (received && received.value() == 0)
          return unexpected(NetError::closed);
        return received;
      });
    }

    [[nodiscard]] auto local_endpoint()
        -> Expected<SocketEndpoint, NetError> override {
      return this->when_open(
          [](Raw raw) { return Native::local_endpoint(raw); });
    }

    [[nodiscard]] auto peer_endpoint()
        -> Expected<SocketEndpoint, NetError> override {
      return this->when_open(
          [](Raw raw) { return Native::peer_endpoint(raw); });
    }

    [[nodiscard]] auto close() -> Expected<void, NetError> override {
      return this->close_socket();
    }

   private:
    /// @brief Reports a dropped connection as closed.
    ///
    /// @param error The failure.
    /// @return NetError::closed for connection_reset and
    ///     connection_aborted; `error` otherwise.
    static auto closed_when_dropped(NetError error) noexcept -> NetError {
      if (error == NetError::connection_reset ||
          error == NetError::connection_aborted)
        return NetError::closed;
      return error;
    }
  };

  /// @brief A listening TCP socket, as platform::TcpListener describes.
  ///
  /// Constructed with SocketBase's constructor, which it inherits.
  class TcpListener final : public platform::TcpListener, public SocketBase {
   public:
    /// @cond
    using SocketBase::SocketBase;
    /// @endcond

    const NetworkProvider& owner() const noexcept override {
      return this->provider();
    }

    [[nodiscard]] auto bind(const SocketEndpoint& endpoint)
        -> Expected<void, NetError> override {
      return this->when_open(
          [&](Raw raw) { return Native::bind(raw, endpoint); });
    }

    [[nodiscard]] auto listen(int backlog)
        -> Expected<void, NetError> override {
      return this->when_open(
          [backlog](Raw raw) { return Native::listen(raw, backlog); });
    }

    [[nodiscard]] auto accept()
        -> Expected<std::unique_ptr<platform::TcpSocket>, NetError> override {
      return this->when_open([this](Raw raw) {
        return Native::accept(raw).and_then([this](SocketHandle accepted) {
          return wrap<TcpSocket, platform::TcpSocket>(this->provider(),
                                                      std::move(accepted));
        });
      });
    }

    [[nodiscard]] auto local_endpoint()
        -> Expected<SocketEndpoint, NetError> override {
      return this->when_open(
          [](Raw raw) { return Native::local_endpoint(raw); });
    }

    [[nodiscard]] auto close() -> Expected<void, NetError> override {
      return this->close_socket();
    }
  };

  /// @brief The poller's wake-up channel, owning both ends.
  ///
  /// Move-only. Writing to it is safe from any thread.
  class WakePair {
   public:
    /// @brief Makes the channel.
    ///
    /// @return The channel.
    /// @retval NetError The system could not make the socket pair,
    ///     usually no_buffers.
    static auto create() -> Expected<WakePair, NetError> {
      return Native::open_wake_sockets().transform(
          [](WakeSockets sockets) { return WakePair{std::move(sockets)}; });
    }

    /// @brief Returns the end the poller watches.
    ///
    /// @return The raw socket.
    auto receiver() const noexcept -> Raw { return sockets_.receiver.get(); }

    /// @brief Makes the watched end readable by writing one byte.
    ///
    /// Never blocks: when the channel is already full the poller is
    /// already due to wake, which counts as success.
    ///
    /// @return False when the write failed for another reason.
    bool wake() noexcept { return Native::send_wake(sockets_.sender.get()); }

    /// @brief Reads the pending wake-up bytes, so that the next wait blocks
    /// again; a byte written meanwhile may stay and end the next wait early.
    void drain() noexcept { Native::drain_wake(sockets_.receiver.get()); }

   private:
    /// @brief Takes ownership of a socket pair.
    ///
    /// @param sockets The pair.
    explicit WakePair(WakeSockets sockets) noexcept
        : sockets_(std::move(sockets)) {}

    /// @brief The two ends.
    WakeSockets sockets_;
  };

  /// @brief Waits for this stack's sockets to become readable or writable,
  /// as platform::Poller describes.
  ///
  /// Waits in slices of at most wait_quantum, timed on the monotonic clock it
  /// is given, checking the stop token between them. An error or hang-up on a
  /// socket is reported as both readable and writable, so that the next read or
  /// write returns the error. On a stack whose socket calls are costly, wakes
  /// are coalesced: only the first wake() after a wait has seen one writes to
  /// the wake channel. One thread may wait at a time; wake() may be called
  /// from any.
  class Poller final : public platform::Poller {
   public:
    /// @brief Makes a poller.
    ///
    /// @param owner The provider whose sockets it may wait on.
    /// @param wake Its wake-up channel.
    /// @param clock The monotonic clock its waits are timed on.
    Poller(NetworkProvider& owner, WakePair wake,
           const ClockProvider& clock) noexcept
        : owner_(&owner),
          wake_(std::move(wake)),
          clock_(&clock),
          coalesce_(Native::capabilities().costly_socket_calls) {}

    const NetworkProvider& owner() const noexcept override { return *owner_; }

    /// @brief Ends the current or next wait; when coalescing, writes to the
    /// wake channel only when no wake is already pending.
    ///
    /// @return Nothing on success.
    /// @retval NetError::system_failure The write failed.
    [[nodiscard]] auto wake() -> Expected<void, NetError> override {
      if (!coalesce_) {
        if (!wake_.wake()) return unexpected(NetError::system_failure);
        return {};
      }
      if (wake_pending_.exchange(true, std::memory_order_acq_rel)) return {};
      if (wake_.wake()) return {};
      wake_pending_.store(false, std::memory_order_release);
      return unexpected(NetError::system_failure);
    }

    /// @brief Waits until a socket is ready, the poller is woken, the
    /// timeout passes or a stop is requested.
    ///
    /// Polls at least once, so a zero timeout checks readiness without
    /// waiting. Blocks for up to `timeout`, noticing a stop within
    /// wait_quantum.
    ///
    /// @param requests The sockets and the readiness wanted of each; at
    ///     most max_requests.
    /// @param timeout The longest to wait; saturates rather than
    ///     overflowing.
    /// @param stop_token Ends the wait early.
    /// @return The ready sockets by tag and whether the poller was woken;
    ///     empty when the timeout passed; with `stop_requested` set when
    ///     the stop token fired.
    /// @retval NetError::invalid_argument More than max_requests
    ///     requests.
    /// @retval NetError::wrong_provider A request is null or belongs to
    ///     another provider.
    /// @retval NetError::system_failure A socket is closed or not of
    ///     this stack, or the system's poll failed.
    [[nodiscard]] auto wait(std::span<const PollRequest> requests,
                            std::chrono::nanoseconds timeout,
                            const platform::StopToken& stop_token)
        -> Expected<PollWaitResult, NetError> override {
      if (requests.size() > max_requests)
        return unexpected(NetError::invalid_argument);
      if (!std::ranges::all_of(requests, [this](const PollRequest& request) {
            return request.pollable != nullptr &&
                   &request.pollable->owner() == owner_;
          }))
        return unexpected(NetError::wrong_provider);
      PollWaitResult result;
      const auto deadline = timing::saturating_add(
          reading(), std::chrono::ceil<std::chrono::microseconds>(timeout));
      if (!build_poll_set(requests))
        return unexpected(NetError::system_failure);
      bool polled = false;
      while (true) {
        if (stop_token.stop_requested()) return stopped(std::move(result));
        const auto now = reading();
        if (polled && now >= deadline) return result;
        polled = true;
        const bool pending =
            coalesce_ && wake_pending_.load(std::memory_order_acquire);
        const auto quantum =
            pending ? std::chrono::nanoseconds{0}
                    : std::min<std::chrono::nanoseconds>(
                          timing::elapsed(now, deadline), wait_quantum);
        if (Native::poll(entries_, timeout_to_milliseconds(quantum)) < 0)
          return unexpected(NetError::system_failure);
        const bool woke = pending || entries_.front().revents != 0;
        if (woke && coalesce_)
          wake_pending_.store(false, std::memory_order_release);
        if (entries_.front().revents != 0) wake_.drain();
        if (stop_token.stop_requested()) return stopped(std::move(result));
        result.woke = result.woke || woke;
        for (std::size_t index = 0; index < requests.size(); ++index) {
          const auto ready = readiness(entries_[index + 1].revents);
          if (!ready) return unexpected(ready.error());
          if (ready.value() != PollEvent::none)
            result.ready.push_back(
                PollReadiness{requests[index].tag, ready.value()});
        }
        if (woke || !result.ready.empty()) return result;
      }
    }

   private:
    /// @brief Marks a result as ended by a stop request.
    ///
    /// @param result What was ready so far.
    /// @return The result with `stop_requested` set.
    static auto stopped(PollWaitResult result) -> PollWaitResult {
      result.stop_requested = true;
      return result;
    }

    /// @brief Turns the system's reported poll flags into readiness.
    ///
    /// @param reported The flags.
    /// @return The readiness; error and hang-up count as both readable
    ///     and writable.
    /// @retval NetError::system_failure The system says the socket is
    ///     invalid.
    static auto readiness(short reported) noexcept
        -> Expected<PollEvent, NetError> {
      if ((reported & Native::poll_invalid) != 0)
        return unexpected(NetError::system_failure);
      constexpr auto failed = Native::poll_error | Native::poll_hangup;
      PollEvent ready = PollEvent::none;
      if ((reported & (Native::poll_readable | failed)) != 0)
        ready = ready | PollEvent::readable;
      if ((reported & (Native::poll_writable | failed)) != 0)
        ready = ready | PollEvent::writable;
      return ready;
    }

    /// @brief Turns wanted readiness into the system's poll flags.
    ///
    /// @param interest The readiness wanted.
    /// @return The flags.
    static auto interest_flags(PollEvent interest) noexcept -> short {
      short flags = 0;
      if ((interest & PollEvent::readable) != PollEvent::none)
        flags = static_cast<short>(flags | Native::poll_readable);
      if ((interest & PollEvent::writable) != PollEvent::none)
        flags = static_cast<short>(flags | Native::poll_writable);
      return flags;
    }

    /// @brief Reads the monotonic clock.
    ///
    /// @return The reading, as time since the clock's origin.
    auto reading() const noexcept -> std::chrono::microseconds {
      return std::chrono::microseconds{
          static_cast<std::int64_t>(clock_->now_microseconds())};
    }

    /// @brief Converts a slice of waiting to the system poll's timeout.
    ///
    /// @param timeout The slice.
    /// @return The milliseconds, rounded up and capped at
    ///     max_poll_timeout; 0 for none or negative.
    static auto timeout_to_milliseconds(
        std::chrono::nanoseconds timeout) noexcept -> int {
      if (timeout.count() <= 0) return 0;
      const auto milliseconds =
          std::chrono::ceil<std::chrono::milliseconds>(timeout);
      return static_cast<int>(std::min(milliseconds, max_poll_timeout).count());
    }

    /// @brief Lists the wake socket and then each requested socket for
    /// the system's poll in entries_, keeping its storage from the last wait
    /// and rewriting only the entries whose socket or interest changed.
    ///
    /// @param requests The sockets and the readiness wanted.
    /// @return False when a request is not an open socket of this stack.
    bool build_poll_set(std::span<const PollRequest> requests) {
      const auto wake =
          Native::poll_entry(wake_.receiver(), Native::poll_readable);
      entries_.resize(requests.size() + 1, wake);
      entries_.front() = wake;
      for (std::size_t index = 0; index < requests.size(); ++index) {
        const auto& request = requests[index];
        const auto* socket = dynamic_cast<const SocketBase*>(request.pollable);
        if (socket == nullptr || !socket->is_open()) return false;
        const auto wanted =
            Native::poll_entry(socket->raw(), interest_flags(request.interest));
        auto& held = entries_[index + 1];
        if (held.fd != wanted.fd || held.events != wanted.events) held = wanted;
      }
      return true;
    }

    /// @brief The provider whose sockets may be waited on.
    NetworkProvider* owner_;
    /// @brief The wake-up channel.
    WakePair wake_;
    /// @brief The monotonic clock waits are timed on.
    const ClockProvider* clock_;
    /// @brief The system poll's entries, the wake socket first, kept between
    /// waits.
    std::vector<typename Native::PollEntry> entries_;
    /// @brief Whether wakes are coalesced, because socket calls are costly.
    const bool coalesce_;
    /// @brief Whether a wake() has not yet been seen by a wait.
    std::atomic<bool> wake_pending_{false};
  };

  /// @brief Wraps an open socket in its object.
  ///
  /// @tparam Socket The object type.
  /// @tparam Interface The public interface it is returned as.
  /// @param network The owner.
  /// @param handle The socket; closed if wrapping fails.
  /// @return The object.
  /// @retval NetError::no_buffers Memory ran out.
  template <class Socket, class Interface>
  static auto wrap(NetworkProvider& network, SocketHandle handle)
      -> Expected<std::unique_ptr<Interface>, NetError> {
    try {
      return std::unique_ptr<Interface>{
          std::make_unique<Socket>(network, std::move(handle))};
    } catch (...) {
      return unexpected(NetError::no_buffers);
    }
  }

  /// @brief Opens a socket of a family and kind and wraps it.
  ///
  /// @tparam Socket The object type.
  /// @tparam Interface The public interface it is returned as.
  /// @param network The owner.
  /// @param family_of An address whose family the socket uses.
  /// @param kind Datagram or stream.
  /// @return The object.
  /// @retval NetError The system refused to open the socket, or memory ran
  ///     out.
  template <class Socket, class Interface>
  static auto open(NetworkProvider& network, const IpAddress& family_of,
                   SocketKind kind)
      -> Expected<std::unique_ptr<Interface>, NetError> {
    return Native::open(family_of, kind)
        .and_then([&network](SocketHandle handle) {
          return wrap<Socket, Interface>(network, std::move(handle));
        });
  }

 public:
  /// @brief Opens a UDP socket.
  ///
  /// @param network The owner; must outlive the socket.
  /// @param family_of An address whose family the socket uses.
  /// @return The socket.
  /// @retval NetError Opening failed.
  [[nodiscard]] static auto create_udp_socket(NetworkProvider& network,
                                              const IpAddress& family_of)
      -> Expected<std::unique_ptr<platform::UdpSocket>, NetError> {
    return open<UdpSocket, platform::UdpSocket>(network, family_of,
                                                SocketKind::datagram);
  }

  /// @brief Opens a TCP socket.
  ///
  /// @param network The owner; must outlive the socket.
  /// @param family_of An address whose family the socket uses.
  /// @return The socket.
  /// @retval NetError Opening failed.
  [[nodiscard]] static auto create_tcp_socket(NetworkProvider& network,
                                              const IpAddress& family_of)
      -> Expected<std::unique_ptr<platform::TcpSocket>, NetError> {
    return open<TcpSocket, platform::TcpSocket>(network, family_of,
                                                SocketKind::stream);
  }

  /// @brief Opens a TCP listener.
  ///
  /// @param network The owner; must outlive the listener.
  /// @param family_of An address whose family the listener uses.
  /// @return The listener.
  /// @retval NetError Opening failed.
  [[nodiscard]] static auto create_tcp_listener(NetworkProvider& network,
                                                const IpAddress& family_of)
      -> Expected<std::unique_ptr<platform::TcpListener>, NetError> {
    return open<TcpListener, platform::TcpListener>(network, family_of,
                                                    SocketKind::stream);
  }

  /// @brief Makes a poller with its own wake-up channel.
  ///
  /// @param network The provider whose sockets it may wait on.
  /// @param clock The monotonic clock its waits are timed on.
  /// @return The poller.
  /// @retval NetError::no_buffers The channel could not be made or memory
  ///     ran out.
  /// @retval NetError::system_failure Anything else went wrong.
  [[nodiscard]] static auto create_poller(NetworkProvider& network,
                                          const ClockProvider& clock)
      -> Expected<std::unique_ptr<platform::Poller>, NetError> {
    try {
      return WakePair::create().transform([&network, &clock](WakePair wake) {
        return std::unique_ptr<platform::Poller>{
            std::make_unique<Poller>(network, std::move(wake), clock)};
      });
    } catch (const std::bad_alloc&) {
      return unexpected(NetError::no_buffers);
    } catch (...) {
      return unexpected(NetError::system_failure);
    }
  }
};

}  // namespace libchiaking::platform::detail
