/// @file
/// @brief Declares the RUDP handshake's states, failures and the state machine
/// that drives the two-step init and cookie exchange.
///
/// The handshake sends an init request until the console answers with a
/// cookie, then a cookie request until the console acknowledges it; each step
/// is sent at most rudp_max_sends times. The machine only decides;
/// Handshake
/// encodes and the runner sends and waits.
#pragma once

#include "src/core/net/transport/rudp/limits.hpp"
#include "src/core/net/transport/rudp/rudp_frame_codec.hpp"

#include <boost/sml.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace libchiaking::net::transport::rudp {

/// @brief Where an RUDP handshake stands.
enum class HandshakeState {
  /// @brief Not begun.
  idle,
  /// @brief The init request is out; waiting for the init response.
  awaiting_init,
  /// @brief The cookie request is out; waiting for the cookie response.
  awaiting_cookie,
  /// @brief The console acknowledged the cookie; the counters are known.
  established,
  /// @brief The handshake gave up; see HandshakeError.
  failed,
  /// @brief The caller cancelled the handshake.
  cancelled,
};

/// @brief Why an RUDP handshake failed.
enum class HandshakeError : std::uint8_t {
  /// @brief A request could not be encoded, such as a cookie that is too
  /// large.
  encode_failed,
  /// @brief A step was sent rudp_max_sends times without its answer, or a
  /// non-answer arrived after the last send.
  retries_exhausted,
  /// @brief A reply no longer than one frame header arrived.
  network,
  /// @brief The handshake was driven out of order, such as begun twice.
  invalid_transition,
};

}  // namespace libchiaking::net::transport::rudp

namespace libchiaking::net::transport::rudp::handshake_lifecycle {
namespace sml = boost::sml;

/// @brief State: not begun.
struct Idle {};
/// @brief State: waiting for the init response.
struct AwaitingInit {};
/// @brief State: waiting for the cookie response.
struct AwaitingCookie {};
/// @brief State: the handshake succeeded.
struct Established {};
/// @brief State: the handshake gave up.
struct Failed {};
/// @brief State: the caller cancelled.
struct Cancelled {};

/// @brief Event: start the handshake by sending the init request.
struct Begin {};
/// @brief Event: a datagram arrived; whichever answers it decodes as are
/// set.
struct Reply {
  /// @brief The init response it holds, if any.
  std::optional<InitResponse> init_answer;
  /// @brief The cookie response it holds, if any.
  std::optional<CookieResponse> cookie_answer;
};
/// @brief Event: a datagram no longer than one frame header arrived.
struct TruncatedReply {};
/// @brief Event: the current request's answer did not come in time.
struct ReplyOverdue {};
/// @brief Event: the request the machine asked for was encoded and handed out.
struct RequestSent {};
/// @brief Event: the request the machine asked for could not be encoded.
struct RequestUnencodable {};
/// @brief Event: the caller gives up.
struct Cancel {};

/// @brief Effect: send an init request.
struct SendInit {
  /// @brief The packet counter to send it with.
  std::uint16_t counter{};
  /// @brief This side's header value.
  std::uint32_t header{};
};
/// @brief Effect: send a cookie request.
struct SendCookie {
  /// @brief The packet counter to send it with.
  std::uint16_t counter{};
  /// @brief This side's header value.
  std::uint32_t header{};
  /// @brief The cookie the init response gave.
  std::vector<std::byte> cookie;
};

/// @brief A request the machine wants sent.
using Effect = std::variant<SendInit, SendCookie>;

/// @brief The handshake's data, read and changed by the machine's actions.
struct Context {
  /// @brief The counter the next request goes out with; raised by one for
  /// every request sent.
  std::uint16_t local_counter{};
  /// @brief This side's random header value; fixed.
  std::uint32_t local_header{};
  /// @brief Requests sent in the current step; reset to 0 when the init
  /// response arrives.
  int stage_sends{};
  /// @brief The console's next expected counter, from its latest answer;
  /// nothing before the init response.
  std::optional<std::uint16_t> remote_counter;
  /// @brief The cookie from the init response.
  std::vector<std::byte> cookie;
  /// @brief The request asked for by the latest event, until taken.
  std::optional<Effect> effect;
  /// @brief Why the handshake failed; set on entering Failed.
  std::optional<HandshakeError> failure;
  /// @brief Datagrams ignored because they belonged to another attempt.
  std::size_t stale_events{};
  /// @brief Events the machine had no transition for.
  std::size_t unexpected_events{};
};

/// @brief The Boost.SML transition table of the handshake.
///
/// From Idle, Begin asks for an init request. In each awaiting state,
/// RequestSent counts the send; a Reply holding the expected answer moves on
/// (init answer: adopt the cookie and counter, ask for a cookie request;
/// cookie answer: Established); any other Reply or a ReplyOverdue asks for the
/// same request again while sends remain and fails with retries_exhausted
/// after; TruncatedReply fails with network; RequestUnencodable fails with
/// encode_failed; Cancel moves to Cancelled from Idle or either awaiting
/// state.
struct Fsm {
  /// @brief Builds the transition table.
  ///
  /// @return The Boost.SML table.
  auto operator()() const noexcept {
    using namespace sml;
    const auto answers_init = [](const Reply& reply) {
      return reply.init_answer.has_value();
    };
    const auto answers_cookie = [](const Reply& reply) {
      return reply.cookie_answer.has_value();
    };
    const auto sends_left = [](const Context& c) {
      return c.stage_sends < rudp_max_sends;
    };
    const auto ask_init = [](Context& c) {
      c.effect = SendInit{c.local_counter, c.local_header};
    };
    const auto ask_cookie = [](Context& c) {
      c.effect = SendCookie{c.local_counter, c.local_header, c.cookie};
    };
    const auto adopt_init_answer = [](const Reply& reply, Context& c) {
      c.cookie = reply.init_answer->cookie;
      c.remote_counter = reply.init_answer->remote_counter;
      c.stage_sends = 0;
    };
    const auto adopt_cookie_answer = [](const Reply& reply, Context& c) {
      c.remote_counter = reply.cookie_answer->remote_counter;
    };
    const auto count_send = [](Context& c) {
      ++c.local_counter;
      ++c.stage_sends;
    };
    const auto fail = [](HandshakeError error) {
      return [error](Context& c) { c.failure = error; };
    };

    return make_transition_table(
        *state<Idle> + sml::event<Begin> / ask_init = state<AwaitingInit>,
        state<Idle> + sml::event<Cancel> = state<Cancelled>,

        state<AwaitingInit> + sml::event<RequestSent> / count_send,
        state<AwaitingInit> + sml::event<RequestUnencodable> /
                                  fail(HandshakeError::encode_failed) =
            state<Failed>,
        state<AwaitingInit> +
            sml::event<Reply>[answers_init] / (adopt_init_answer, ask_cookie) =
            state<AwaitingCookie>,
        state<AwaitingInit> + sml::event<Reply>[sends_left] / ask_init,
        state<AwaitingInit> +
            sml::event<Reply> / fail(HandshakeError::retries_exhausted) =
            state<Failed>,
        state<AwaitingInit> + sml::event<ReplyOverdue>[sends_left] / ask_init,
        state<AwaitingInit> +
            sml::event<ReplyOverdue> / fail(HandshakeError::retries_exhausted) =
            state<Failed>,
        state<AwaitingInit> + sml::event<TruncatedReply> /
                                  fail(HandshakeError::network) = state<Failed>,
        state<AwaitingInit> + sml::event<Cancel> = state<Cancelled>,

        state<AwaitingCookie> + sml::event<RequestSent> / count_send,
        state<AwaitingCookie> + sml::event<RequestUnencodable> /
                                    fail(HandshakeError::encode_failed) =
            state<Failed>,
        state<AwaitingCookie> + sml::event<Reply>[answers_cookie] /
                                    adopt_cookie_answer = state<Established>,
        state<AwaitingCookie> + sml::event<Reply>[sends_left] / ask_cookie,
        state<AwaitingCookie> +
            sml::event<Reply> / fail(HandshakeError::retries_exhausted) =
            state<Failed>,
        state<AwaitingCookie> +
            sml::event<ReplyOverdue>[sends_left] / ask_cookie,
        state<AwaitingCookie> +
            sml::event<ReplyOverdue> / fail(HandshakeError::retries_exhausted) =
            state<Failed>,
        state<AwaitingCookie> +
            sml::event<TruncatedReply> / fail(HandshakeError::network) =
            state<Failed>,
        state<AwaitingCookie> + sml::event<Cancel> = state<Cancelled>);
  }
};

/// @brief The handshake state machine and its context, with the effect each
/// event asks for.
///
/// Not copyable: the machine refers to the context by address. Used by one
/// thread at a time.
class Lifecycle {
 public:
  /// @brief Starts in Idle.
  ///
  /// @param local_counter The counter of the first request.
  /// @param local_header This side's random header value.
  Lifecycle(std::uint16_t local_counter, std::uint32_t local_header)
      : context_{.local_counter = local_counter, .local_header = local_header},
        machine_(context_) {}
  /// @brief Not copyable: the machine holds the context's address.
  Lifecycle(const Lifecycle&) = delete;
  /// @brief Not copyable: the machine holds the context's address.
  ///
  /// @return Never returns.
  Lifecycle& operator=(const Lifecycle&) = delete;

  /// @brief Feeds one event to the machine, clearing any untaken effect
  /// first.
  ///
  /// @tparam Event One of the event types above.
  /// @param event The event.
  /// @return Whether the current state had a transition for it; false counts
  ///     it in unexpected_events.
  template <class Event>
  [[nodiscard]] bool process(const Event& event) {
    context_.effect.reset();
    if (machine_.process_event(event)) return true;
    ++context_.unexpected_events;
    return false;
  }

  /// @brief Counts a datagram ignored as belonging to another attempt.
  void count_stale() noexcept { ++context_.stale_events; }

  /// @brief Takes the request the last event asked for.
  ///
  /// @return The request; nothing when none was asked for or it was taken.
  auto take_effect() noexcept -> std::optional<Effect> {
    return std::exchange(context_.effect, std::nullopt);
  }

  /// @brief The handshake's data.
  ///
  /// @return The context; valid while the lifecycle lives.
  auto context() const noexcept -> const Context& { return context_; }

  /// @brief Where the machine stands.
  ///
  /// @return The current state.
  HandshakeState state() const noexcept {
    if (machine_.is(sml::state<Idle>)) return HandshakeState::idle;
    if (machine_.is(sml::state<AwaitingInit>))
      return HandshakeState::awaiting_init;
    if (machine_.is(sml::state<AwaitingCookie>))
      return HandshakeState::awaiting_cookie;
    if (machine_.is(sml::state<Established>))
      return HandshakeState::established;
    if (machine_.is(sml::state<Failed>)) return HandshakeState::failed;
    return HandshakeState::cancelled;
  }

 private:
  /// @brief The handshake's data.
  Context context_;
  /// @brief The machine, bound to `context_`.
  sml::sm<Fsm> machine_;
};

}  // namespace libchiaking::net::transport::rudp::handshake_lifecycle
