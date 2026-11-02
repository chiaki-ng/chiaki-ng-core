/// @file
/// @brief Declares TcpExchange, the non-blocking request/reply exchange with
/// a console's Remote Play service over TCP, and its policies for the session
/// request.
///
/// A home session sends two HTTP requests to port 9295: `sess/init` (closes
/// after the reply) and `sess/ctrl` (keeps the socket). Both follow the same
/// steps of open, connect, send, receive, each bounded by rp_phase_timeout,
/// so one template runs both and a Reply policy says how they differ.
#pragma once

#include <boost/sml.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <variant>

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/platform/net/socket.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "src/core/utils/timing/deadline.hpp"

#include "limits.hpp"
#include "pairing/rp_version.hpp"
#include "session_codec.hpp"

namespace libchiaking::net::protocol::remote_play {

/// @brief Why a console request exchange failed.
enum class ConsoleExchangeError : std::uint8_t {
  /// @brief A zero port, an empty request or an unusable reply policy.
  invalid_argument,
  /// @brief The call does not fit the exchange's state, such as begin() twice.
  invalid_state,
  /// @brief The socket failed to open, connect, send, receive or close.
  network_failure,
  /// @brief A phase did not finish within rp_phase_timeout.
  timeout,
  /// @brief The console closed the connection before the reply was complete.
  truncated,
  /// @brief The reply is not one the policy can read.
  invalid_response,
  /// @brief The reply head exceeds rp_max_header_bytes.
  too_large,
  /// @brief The console answered with a refusal (control request not 200).
  refused,
  /// @brief The console actively refused the TCP connection: nothing listens
  /// on the port, usually because the console is asleep.
  connection_refused,
  /// @brief The platform has no socket to spare.
  resource_exhausted,
};

/// @brief How far a failed exchange got, for diagnostics.
struct ConsoleExchangeProgress {
  /// @brief The failure.
  ConsoleExchangeError error{};
  /// @brief How many request bytes had been sent.
  std::size_t request_bytes_sent{};
  /// @brief How many reply bytes had been received.
  std::size_t response_bytes_received{};
  /// @brief Whether the request had been resent after a reply timeout.
  bool retried_header_timeout{false};
};

/// @brief Owns a TCP socket and closes it on request, treating an already
/// closed socket as success.
///
/// Move-only through its unique_ptr. Not thread-safe.
class HeldTcpSocket {
 public:
  /// @brief Holds no socket.
  HeldTcpSocket() noexcept = default;
  /// @brief Takes ownership of a socket.
  ///
  /// @param socket The socket, or null.
  explicit HeldTcpSocket(std::unique_ptr<platform::TcpSocket> socket) noexcept
      : socket_(std::move(socket)) {}

  /// @brief Closes and releases the socket.
  ///
  /// @return Nothing when there was no socket or it closed (or was already
  ///     closed); the platform's error otherwise. The socket is released either
  ///     way.
  [[nodiscard]] auto close() -> Expected<void, platform::NetError> {
    if (!socket_) return {};
    auto socket = std::move(socket_);
    const auto closed = socket->close();
    if (!closed && closed.error() != platform::NetError::closed)
      return unexpected(closed.error());
    return {};
  }

  /// @brief Gives up ownership without closing.
  ///
  /// @return The socket, or null.
  auto release() noexcept -> std::unique_ptr<platform::TcpSocket> {
    return std::move(socket_);
  }

  /// @brief The socket, still owned here.
  ///
  /// @return The socket, or null.
  auto get() const noexcept -> platform::TcpSocket* { return socket_.get(); }

  /// @brief Calls through to the socket.
  ///
  /// @return The socket; must not be null.
  auto operator->() const noexcept -> platform::TcpSocket* {
    return socket_.get();
  }

  /// @brief Whether a socket is held.
  ///
  /// @return True when held.
  explicit operator bool() const noexcept { return socket_ != nullptr; }

 private:
  /// @brief The socket.
  std::unique_ptr<platform::TcpSocket> socket_;
};

}  // namespace libchiaking::net::protocol::remote_play

namespace libchiaking::net::protocol::remote_play::tcp_exchange_lifecycle {
namespace sml = boost::sml;

/// @brief The phases of a console request exchange.
enum class State : std::uint8_t {
  /// @brief Not started, or a refused begin() was withdrawn.
  idle,
  /// @brief A socket is being created and connect issued.
  opening,
  /// @brief Waiting for a non-blocking connect to complete.
  connecting,
  /// @brief Writing the request.
  sending,
  /// @brief Reading the reply head.
  receiving,
  /// @brief The reply is parsed and waiting to be taken.
  answered,
  /// @brief Taken, failed or cancelled; the exchange cannot be reused.
  finished,
};

/// @brief State tag: not started.
struct Idle {};
/// @brief State tag: opening the socket.
struct Opening {};
/// @brief State tag: connecting.
struct Connecting {};
/// @brief State tag: sending.
struct Sending {};
/// @brief State tag: receiving.
struct Receiving {};
/// @brief State tag: answered.
struct Answered {};
/// @brief State tag: finished.
struct Finished {};

/// @brief Event: begin() was called.
struct Open {};
/// @brief Event: begin() refused its arguments before opening anything.
struct Withdraw {};
/// @brief Event: the socket was created and connect issued.
struct Opened {
  /// @brief When, in monotonic microseconds; arms the phase deadline.
  std::uint64_t now_us{};
  /// @brief Whether connect completed at once.
  bool connected{};
};
/// @brief Event: the socket could not be created or connect failed at once.
struct OpenRefused {
  /// @brief Whether the failure counts as a connect failure (false when
  /// sockets were exhausted).
  bool while_connecting{};
};
/// @brief Event: a non-blocking connect completed.
struct Connected {
  /// @brief When, in monotonic microseconds; re-arms the deadline.
  std::uint64_t now_us{};
};
/// @brief Event: sending found the socket not connected; connecting resumes
/// with the original connect deadline.
struct ConnectionDropped {};
/// @brief Event: the whole request was sent.
struct RequestDelivered {
  /// @brief When, in monotonic microseconds; re-arms the deadline.
  std::uint64_t now_us{};
};
/// @brief Event: the reply head was parsed.
struct ResponseParsed {};
/// @brief Event: the phase deadline passed.
struct DeadlinePassed {};
/// @brief Event: the exchange failed.
struct Failed {};
/// @brief Event: the reply was taken.
struct HandedOver {};
/// @brief Event: the owner cancelled.
struct Cancel {};

/// @brief Effect: resend the request on a new connection.
struct Reopen {};
/// @brief Effect: give up with a timeout.
struct GiveUp {};

/// @brief What the exchange must do after a deadline.
using Effect = std::variant<Reopen, GiveUp>;

/// @brief The lifecycle's data.
struct Context {
  /// @brief Whether one resend after a reply timeout is allowed (the Reply
  /// policy's retries_header_timeout).
  bool header_retry_allowed{};
  /// @brief When the current phase times out.
  timing::Deadline deadline;
  /// @brief The deadline of the connect phase, restored when sending finds the
  /// socket not yet connected.
  timing::Deadline connect_deadline;
  /// @brief Whether the one resend has been used.
  bool header_retried{};
  /// @brief Whether the failure happened while connecting, which the caller
  /// reads as the console being unreachable.
  bool failed_while_connecting{};
  /// @brief The effect of the last event, if any.
  std::optional<Effect> effect;
  /// @brief Events the machine refused.
  std::size_t unexpected_events{};

  /// @brief Starts a new phase deadline of rp_phase_timeout.
  ///
  /// @param now_us Monotonic microseconds now.
  void arm(std::uint64_t now_us) noexcept {
    deadline =
        timing::Deadline::after(timing::Deadline::instant_of(now_us),
                                timing::Deadline::Duration{rp_phase_timeout});
  }
};

/// @brief The transition table of the exchange.
struct Fsm {
  /// @brief Builds the table.
  ///
  /// @return The boost::sml table.
  auto operator()() const noexcept {
    using namespace sml;
    const auto connected = [](const Opened& e) { return e.connected; };
    const auto header_retry_left = [](const Context& c) {
      return c.header_retry_allowed && !c.header_retried;
    };
    const auto begin_attempt = [](Context& c) {
      c.header_retried = false;
      c.failed_while_connecting = false;
    };
    const auto arm_dial = [](const Opened& e, Context& c) {
      c.arm(e.now_us);
      c.connect_deadline = c.deadline;
    };
    const auto arm_connected = [](const Connected& e, Context& c) {
      c.arm(e.now_us);
    };
    const auto arm_delivered = [](const RequestDelivered& e, Context& c) {
      c.arm(e.now_us);
    };
    const auto restore_connect_deadline = [](Context& c) {
      c.deadline = c.connect_deadline;
    };
    const auto note_refusal = [](const OpenRefused& e, Context& c) {
      c.failed_while_connecting = e.while_connecting;
    };
    const auto note_connect_failure = [](Context& c) {
      c.failed_while_connecting = true;
    };
    const auto give_up = [](Context& c) { c.effect = GiveUp{}; };
    const auto reopen = [](Context& c) {
      c.header_retried = true;
      c.effect = Reopen{};
    };

    return make_transition_table(
        *state<Idle> + sml::event<Open> / begin_attempt = state<Opening>,
        state<Idle> + sml::event<Cancel> = state<Finished>,

        state<Opening> + sml::event<Withdraw> = state<Idle>,
        state<Opening> + sml::event<Opened>[connected] / arm_dial =
            state<Sending>,
        state<Opening> + sml::event<Opened> / arm_dial = state<Connecting>,
        state<Opening> + sml::event<OpenRefused> / note_refusal =
            state<Finished>,
        state<Opening> + sml::event<Failed> = state<Finished>,
        state<Opening> + sml::event<Cancel> = state<Finished>,

        state<Connecting> + sml::event<Connected> / arm_connected =
            state<Sending>,
        state<Connecting> + sml::event<DeadlinePassed> / give_up,
        state<Connecting> + sml::event<Failed> / note_connect_failure =
            state<Finished>,
        state<Connecting> + sml::event<Cancel> = state<Finished>,

        state<Sending> + sml::event<RequestDelivered> / arm_delivered =
            state<Receiving>,
        state<Sending> + sml::event<ConnectionDropped> /
                             restore_connect_deadline = state<Connecting>,
        state<Sending> + sml::event<DeadlinePassed> / give_up,
        state<Sending> + sml::event<Failed> = state<Finished>,
        state<Sending> + sml::event<Cancel> = state<Finished>,

        state<Receiving> + sml::event<ResponseParsed> = state<Answered>,
        state<Receiving> + sml::event<DeadlinePassed>[header_retry_left] /
                               reopen = state<Opening>,
        state<Receiving> + sml::event<DeadlinePassed> / give_up,
        state<Receiving> + sml::event<Failed> = state<Finished>,
        state<Receiving> + sml::event<Cancel> = state<Finished>,

        state<Answered> + sml::event<HandedOver> = state<Finished>,
        state<Answered> + sml::event<Cancel> = state<Finished>,

        state<Finished> + sml::event<Cancel> = state<Finished>);
  }
};

/// @brief The exchange's state machine with its context.
///
/// Not thread-safe; the owning TcpExchange drives it.
class Lifecycle {
 public:
  /// @brief Makes an idle machine.
  ///
  /// @param header_retry_allowed Whether one resend after a reply timeout is
  ///     allowed.
  explicit Lifecycle(bool header_retry_allowed)
      : context_{.header_retry_allowed = header_retry_allowed},
        machine_(context_) {}
  Lifecycle(const Lifecycle&) = delete;
  Lifecycle& operator=(const Lifecycle&) = delete;

  /// @brief Feeds an event, clearing the previous effect.
  ///
  /// @tparam Event One of the event types above.
  /// @param event The event.
  /// @return True when the machine took it; false (and counted) otherwise.
  template <class Event>
  [[nodiscard]] bool process(const Event& event) {
    context_.effect.reset();
    if (machine_.process_event(event)) return true;
    ++context_.unexpected_events;
    return false;
  }

  /// @brief Takes the effect of the last event.
  ///
  /// @return The effect, or nothing.
  auto take_effect() noexcept -> std::optional<Effect> {
    return std::exchange(context_.effect, std::nullopt);
  }

  /// @brief Whether the current phase deadline has passed.
  ///
  /// @param now_us Monotonic microseconds now.
  /// @return True when overdue.
  auto overdue(std::uint64_t now_us) const noexcept -> bool {
    return context_.deadline.expired(timing::Deadline::instant_of(now_us));
  }
  /// @brief Whether the one resend has been used.
  ///
  /// @return True once resent.
  auto header_retried() const noexcept -> bool {
    return context_.header_retried;
  }
  /// @brief Whether the exchange failed while connecting.
  ///
  /// @return True for a connect failure.
  auto failed_while_connecting() const noexcept -> bool {
    return context_.failed_while_connecting;
  }

  /// @brief The current phase.
  ///
  /// @return The state.
  State state() const noexcept {
    if (machine_.is(sml::state<Opening>)) return State::opening;
    if (machine_.is(sml::state<Connecting>)) return State::connecting;
    if (machine_.is(sml::state<Sending>)) return State::sending;
    if (machine_.is(sml::state<Receiving>)) return State::receiving;
    if (machine_.is(sml::state<Answered>)) return State::answered;
    if (machine_.is(sml::state<Finished>)) return State::finished;
    return State::idle;
  }

 private:
  /// @brief The context.
  Context context_;
  /// @brief The machine.
  sml::sm<Fsm> machine_;
};

}  // namespace libchiaking::net::protocol::remote_play::tcp_exchange_lifecycle

namespace libchiaking::net::protocol::remote_play {

/// @brief Sends one request to a console over TCP and reads its reply head,
/// without blocking.
///
/// The owner calls begin(), then polls pollable() and calls pump_once()
/// until it returns true, then take(). Each phase (connect, send, receive)
/// has rp_phase_timeout from its start. Failures close the socket and
/// leave last_failure_progress() set. Destruction cancels. Not thread-safe:
/// one thread owns it.
///
/// @tparam Reply A policy with `Request`, `Response` and `Taken` types,
///     `keeps_socket` and `retries_header_timeout` flags, `valid()`,
///     `receive_window(remaining)`, `parse(received)`, `on_sent(request)` and
///     `take(socket, response)`; see SessionReply and CtrlStartupReply.
template <class Reply>
class TcpExchange {
 public:
  /// @brief The request type the policy sends.
  using Request = typename Reply::Request;
  /// @brief The reply type the policy parses.
  using Response = typename Reply::Response;
  /// @brief What take() hands over.
  using Taken = typename Reply::Taken;

  /// @brief Makes an idle exchange.
  ///
  /// @param sockets The socket provider; must outlive the exchange.
  /// @param clock The monotonic clock; must outlive the exchange.
  TcpExchange(platform::SocketProvider& sockets,
              const platform::ClockProvider& clock) noexcept
      : sockets_(sockets), clock_(clock) {}
  /// @brief Cancels the exchange, closing any socket.
  ~TcpExchange() { static_cast<void>(cancel()); }

  TcpExchange(const TcpExchange&) = delete;
  TcpExchange& operator=(const TcpExchange&) = delete;

  /// @brief Opens a socket and starts connecting.
  ///
  /// @param endpoint The console's address and port.
  /// @param request The request to send; taken over.
  /// @param reply The reply policy.
  /// @return Nothing on success.
  /// @retval ConsoleExchangeError::invalid_state The exchange was already used.
  /// @retval ConsoleExchangeError::invalid_argument A zero port, empty request
  ///     or invalid policy.
  /// @retval ConsoleExchangeError::resource_exhausted No socket could be
  ///     made.
  /// @retval ConsoleExchangeError::connection_refused The console refused.
  /// @retval ConsoleExchangeError::network_failure The socket failed.
  [[nodiscard]] auto begin(platform::SocketEndpoint endpoint, Request&& request,
                           Reply reply = {})
      -> Expected<void, ConsoleExchangeError> {
    if (!lifecycle_.process(tcp_exchange_lifecycle::Open{}))
      return unexpected(ConsoleExchangeError::invalid_state);
    progress_.reset();
    if (endpoint.port == 0 || request.view().empty() || !reply.valid()) {
      static_cast<void>(lifecycle_.process(tcp_exchange_lifecycle::Withdraw{}));
      return unexpected(ConsoleExchangeError::invalid_argument);
    }
    endpoint_ = endpoint;
    request_ = std::move(request);
    reply_ = std::move(reply);
    auto created = sockets_.create_tcp_socket(endpoint.address);
    if (!created) {
      const bool exhausted =
          created.error() == platform::NetError::resource_exhausted;
      refuse_open(!exhausted);
      return unexpected(exhausted ? ConsoleExchangeError::resource_exhausted
                                  : ConsoleExchangeError::network_failure);
    }
    socket_ = HeldTcpSocket{std::move(created.value())};
    const auto connected = socket_->connect(endpoint);
    if (!connected && connected.error() != platform::NetError::would_block) {
      refuse_open(true);
      return unexpected(refusal_or_failure(connected.error()));
    }
    opened(connected.has_value());
    return {};
  }

  /// @brief Advances the exchange as far as it can without waiting, up to
  /// max_exchange_steps_per_pump steps.
  ///
  /// @return True when the reply is ready to take, false when the socket must
  ///     be polled again.
  /// @retval ConsoleExchangeError Any failure; the exchange is finished.
  [[nodiscard]] auto pump_once() -> Expected<bool, ConsoleExchangeError> {
    for (std::size_t steps = 0; steps < max_exchange_steps_per_pump; ++steps) {
      const auto state = lifecycle_.state();
      const auto sent = sent_;
      const auto received = received_;
      const auto stepped = step();
      if (!stepped || stepped.value()) return stepped;
      if (state == lifecycle_.state() && sent == sent_ && received == received_)
        return false;
    }
    return false;
  }

  /// @brief Hands over the reply (and, when the policy keeps it, the socket).
  ///
  /// @return What the policy makes of them.
  /// @retval ConsoleExchangeError::invalid_state No reply is ready.
  [[nodiscard]] auto take() -> Expected<Taken, ConsoleExchangeError> {
    if (!response_ || (Reply::keeps_socket && !socket_) ||
        !lifecycle_.process(tcp_exchange_lifecycle::HandedOver{}))
      return unexpected(ConsoleExchangeError::invalid_state);
    auto taken = reply_.take(socket_.release(), std::move(*response_));
    response_.reset();
    progress_.reset();
    return taken;
  }

  /// @brief Abandons the exchange and closes its socket.
  ///
  /// @return Nothing, or ConsoleExchangeError::network_failure when the close
  ///     failed.
  [[nodiscard]] auto cancel() -> Expected<void, ConsoleExchangeError> {
    static_cast<void>(lifecycle_.process(tcp_exchange_lifecycle::Cancel{}));
    return release();
  }

  /// @brief What the owner polls: readable or writable as the phase needs.
  ///
  /// @return The socket, or null when none is open.
  auto pollable() const noexcept -> const platform::Pollable* {
    return socket_.get();
  }

  /// @brief Whether the exchange failed while connecting.
  ///
  /// @return True for a connect failure.
  auto failed_while_connecting() const noexcept -> bool {
    return lifecycle_.failed_while_connecting();
  }

  /// @brief How far the last failure got.
  ///
  /// @return The progress, or nothing when there was no failure since the last
  ///     begin() or take().
  auto last_failure_progress() const noexcept
      -> std::optional<ConsoleExchangeProgress> {
    return progress_;
  }

 private:
  /// @brief The lifecycle's states.
  using State = tcp_exchange_lifecycle::State;

  /// @brief Maps a connect error.
  ///
  /// @param error The platform error.
  /// @return connection_refused for a refused connect, network_failure
  ///     otherwise.
  static auto refusal_or_failure(platform::NetError error) noexcept
      -> ConsoleExchangeError {
    return error == platform::NetError::connection_refused
               ? ConsoleExchangeError::connection_refused
               : ConsoleExchangeError::network_failure;
  }

  /// @brief Tells the lifecycle a socket was opened.
  ///
  /// @param connected Whether connect completed at once.
  void opened(bool connected) {
    static_cast<void>(lifecycle_.process(
        tcp_exchange_lifecycle::Opened{clock_.now_microseconds(), connected}));
  }

  /// @brief Tells the lifecycle opening failed and releases everything.
  ///
  /// @param while_connecting Whether to count it as a connect failure.
  void refuse_open(bool while_connecting) {
    static_cast<void>(lifecycle_.process(
        tcp_exchange_lifecycle::OpenRefused{while_connecting}));
    static_cast<void>(release());
  }

  /// @brief Runs one step of the current phase, checking its deadline first.
  ///
  /// @return As pump_once().
  [[nodiscard]] auto step() -> Expected<bool, ConsoleExchangeError> {
    switch (lifecycle_.state()) {
      case State::idle:
      case State::opening:
      case State::finished:
        return unexpected(ConsoleExchangeError::invalid_state);
      case State::answered:
        return true;
      case State::connecting:
      case State::sending:
      case State::receiving:
        break;
    }
    if (lifecycle_.overdue(clock_.now_microseconds())) return on_deadline();
    switch (lifecycle_.state()) {
      case State::connecting:
        return complete_connect();
      case State::sending:
        return send();
      default:
        return receive();
    }
  }

  /// @brief Handles a passed deadline: resends once when allowed, else fails.
  ///
  /// @return As pump_once().
  [[nodiscard]] auto on_deadline() -> Expected<bool, ConsoleExchangeError> {
    static_cast<void>(
        lifecycle_.process(tcp_exchange_lifecycle::DeadlinePassed{}));
    const auto effect = lifecycle_.take_effect();
    if (effect &&
        std::holds_alternative<tcp_exchange_lifecycle::Reopen>(*effect))
      return reopen();
    return fail(ConsoleExchangeError::timeout);
  }

  /// @brief Checks whether a non-blocking connect completed.
  ///
  /// @return False to keep waiting; a failure when the connect failed.
  [[nodiscard]] auto complete_connect()
      -> Expected<bool, ConsoleExchangeError> {
    const auto connected = socket_->complete_connect();
    if (!connected) {
      if (connected.error() == platform::NetError::would_block) return false;
      return fail(refusal_or_failure(connected.error()));
    }
    const auto peer = socket_->peer_endpoint();
    if (!peer) {
      if (peer.error() == platform::NetError::would_block ||
          peer.error() == platform::NetError::not_connected)
        return false;
      const auto deferred = socket_->complete_connect();
      return fail(deferred ? ConsoleExchangeError::network_failure
                           : refusal_or_failure(deferred.error()));
    }
    static_cast<void>(lifecycle_.process(
        tcp_exchange_lifecycle::Connected{clock_.now_microseconds()}));
    return false;
  }

  /// @brief Writes more of the request.
  ///
  /// @return False to keep going; a failure when the socket failed.
  [[nodiscard]] auto send() -> Expected<bool, ConsoleExchangeError> {
    const auto bytes = request_.bytes();
    const auto written = socket_->send(bytes.subspan(sent_));
    if (!written) {
      if (written.error() == platform::NetError::would_block) return false;
      if (written.error() == platform::NetError::not_connected) {
        static_cast<void>(
            lifecycle_.process(tcp_exchange_lifecycle::ConnectionDropped{}));
        return false;
      }
      return fail(ConsoleExchangeError::network_failure);
    }
    if (written.value() == 0 || written.value() > bytes.size() - sent_)
      return fail(ConsoleExchangeError::network_failure);
    sent_ += written.value();
    if (sent_ != bytes.size()) return false;
    reply_.on_sent(request_);
    if (!Reply::retries_header_timeout) request_ = Request{};
    static_cast<void>(lifecycle_.process(
        tcp_exchange_lifecycle::RequestDelivered{clock_.now_microseconds()}));
    return false;
  }

  /// @brief Reads more of the reply and tries to parse it.
  ///
  /// @return True when the reply is parsed; false to keep reading.
  [[nodiscard]] auto receive() -> Expected<bool, ConsoleExchangeError> {
    if (received_ == response_bytes_.size())
      return fail(ConsoleExchangeError::too_large);
    const auto window =
        Reply::receive_window(response_bytes_.size() - received_);
    const auto chunk =
        socket_->receive({response_bytes_.data() + received_, window});
    if (!chunk) {
      if (chunk.error() == platform::NetError::would_block) return false;
      return fail(chunk.error() == platform::NetError::closed
                      ? ConsoleExchangeError::truncated
                      : ConsoleExchangeError::network_failure);
    }
    if (chunk.value() == 0) return fail(ConsoleExchangeError::truncated);
    if (chunk.value() > window)
      return fail(ConsoleExchangeError::network_failure);
    received_ += chunk.value();
    auto parsed = reply_.parse({response_bytes_.data(), received_});
    if (!parsed) return fail(parsed.error());
    if (!parsed.value()) return false;
    response_.emplace(std::move(*parsed.value()));
    request_ = Request{};
    response_bytes_.clear();
    if (!Reply::keeps_socket)
      if (auto closed = close_socket(); !closed) return fail(closed.error());
    static_cast<void>(
        lifecycle_.process(tcp_exchange_lifecycle::ResponseParsed{}));
    return true;
  }

  /// @brief Closes the socket and resends the request on a new connection.
  ///
  /// @return False to keep going; a failure when no socket could be made.
  [[nodiscard]] auto reopen() -> Expected<bool, ConsoleExchangeError> {
    if (auto closed = close_socket(); !closed) return fail(closed.error());
    response_bytes_.clear();
    received_ = 0;
    sent_ = 0;
    auto created = sockets_.create_tcp_socket(endpoint_.address);
    if (!created)
      return fail(created.error() == platform::NetError::resource_exhausted
                      ? ConsoleExchangeError::resource_exhausted
                      : ConsoleExchangeError::network_failure);
    socket_ = HeldTcpSocket{std::move(created.value())};
    const auto connected = socket_->connect(endpoint_);
    if (!connected && connected.error() != platform::NetError::would_block)
      return fail(ConsoleExchangeError::network_failure);
    opened(connected.has_value());
    return false;
  }

  /// @brief Closes the socket.
  ///
  /// @return Nothing, or ConsoleExchangeError::network_failure.
  [[nodiscard]] auto close_socket() -> Expected<void, ConsoleExchangeError> {
    if (!socket_.close())
      return unexpected(ConsoleExchangeError::network_failure);
    return {};
  }

  /// @brief Closes the socket and wipes the request and reply.
  ///
  /// @return Nothing, or ConsoleExchangeError::network_failure.
  [[nodiscard]] auto release() -> Expected<void, ConsoleExchangeError> {
    const auto closed = close_socket();
    request_ = Request{};
    response_bytes_.clear();
    response_.reset();
    sent_ = 0;
    received_ = 0;
    return closed;
  }

  /// @brief Records a failure and releases everything.
  ///
  /// @param error The failure.
  /// @return The failure.
  [[nodiscard]] auto fail(ConsoleExchangeError error)
      -> Expected<bool, ConsoleExchangeError> {
    static_cast<void>(lifecycle_.process(tcp_exchange_lifecycle::Failed{}));
    progress_.emplace(ConsoleExchangeProgress{error, sent_, received_,
                                              lifecycle_.header_retried()});
    static_cast<void>(release());
    return unexpected(error);
  }

  /// @brief The socket provider.
  platform::SocketProvider& sockets_;
  /// @brief The monotonic clock.
  const platform::ClockProvider& clock_;
  /// @brief The socket.
  HeldTcpSocket socket_;
  /// @brief Where the request goes.
  platform::SocketEndpoint endpoint_{};
  /// @brief The request; kept after sending only when it may be resent.
  Request request_;
  /// @brief The reply policy.
  Reply reply_{};
  /// @brief The reply bytes received so far.
  SecretArray<rp_max_header_bytes> response_bytes_;
  /// @brief The parsed reply, until taken.
  std::optional<Response> response_;
  /// @brief Request bytes sent.
  std::size_t sent_{};
  /// @brief Reply bytes received.
  std::size_t received_{};
  /// @brief How far the last failure got.
  std::optional<ConsoleExchangeProgress> progress_;
  /// @brief The lifecycle.
  tcp_exchange_lifecycle::Lifecycle lifecycle_{Reply::retries_header_timeout};
};

/// @brief The Reply policy that lets TcpExchange run the session request.
///
/// The socket is closed once the reply head is read, and a reply timeout is
/// not retried.
class SessionReply {
 public:
  /// @brief The request type sent.
  using Request = SessionRequest;
  /// @brief The reply type parsed.
  using Response = SessionResponse;
  /// @brief What take() hands over.
  using Taken = SessionResponse;

  /// @brief The socket is closed after the reply.
  static constexpr bool keeps_socket = false;
  /// @brief A reply timeout fails the exchange.
  static constexpr bool retries_header_timeout = false;

  /// @brief Makes an invalid policy (no target).
  SessionReply() noexcept = default;
  /// @brief Makes the policy for a console variant.
  ///
  /// @param target The variant the request was encoded for.
  explicit SessionReply(RegistrationTarget target) noexcept : target_(target) {}

  /// @brief Whether the target has an RP version.
  ///
  /// @return True when usable.
  auto valid() const noexcept -> bool {
    return RpVersion::of(target_).has_value();
  }

  /// @brief How many bytes to read next.
  ///
  /// @param remaining The room left in the buffer.
  /// @return All of it.
  static constexpr auto receive_window(std::size_t remaining) noexcept
      -> std::size_t {
    return remaining;
  }

  /// @brief Reads the reply received so far.
  ///
  /// @param received The bytes received so far.
  /// @return The reply once complete (a grant, or a rejection or version
  ///     mismatch), or nothing while incomplete.
  /// @retval ConsoleExchangeError::too_large The head is too long.
  /// @retval ConsoleExchangeError::invalid_response Any other unreadable
  ///     reply.
  [[nodiscard]] auto parse(std::span<const std::byte> received) const
      -> Expected<std::optional<SessionResponse>, ConsoleExchangeError>;

  /// @brief Nothing to remember once the request is sent.
  void on_sent(const SessionRequest&) noexcept {}

  /// @brief Hands over the reply; the socket is already closed.
  ///
  /// @param response The reply.
  /// @return The reply.
  static auto take(std::unique_ptr<platform::TcpSocket>,
                   SessionResponse response) -> SessionResponse {
    return response;
  }

 private:
  /// @brief The variant the request was encoded for.
  RegistrationTarget target_{RegistrationTarget::ps4_unknown};
};

/// @brief Sends the session request over TCP and returns the reply.
using SessionTcpExchange = TcpExchange<SessionReply>;

}  // namespace libchiaking::net::protocol::remote_play
