#include "src/core/net/protocol/takion/handshake.hpp"

namespace libchiaking::net::protocol::takion {

ConnectionHandshake::ConnectionHandshake(
    HandshakeRetryPolicy retry_policy,
    AdvertisedReceiveWindow advertised_receive_window,
    StreamCount outbound_streams, StreamCount inbound_streams,
    RemoteSequencePolicy remote_sequence_policy)
    : context_{.retry_policy = retry_policy,
               .advertised_receive_window = advertised_receive_window,
               .outbound_streams = outbound_streams,
               .inbound_streams = inbound_streams,
               .remote_sequence_policy = remote_sequence_policy},
      machine_{context_} {}

HandshakeSnapshot ConnectionHandshake::snapshot() const {
  using namespace handshake_detail;
  auto state = HandshakeState::failed;
  if (machine_.is(sml::state<IdleState>))
    state = HandshakeState::idle;
  else if (machine_.is(sml::state<AwaitingInitAcknowledgementState>))
    state = HandshakeState::await_init_acknowledgement;
  else if (machine_.is(sml::state<AwaitingCookieAcknowledgementState>))
    state = HandshakeState::await_cookie_acknowledgement;
  else if (machine_.is(sml::state<ConnectedState>))
    state = HandshakeState::connected;
  return {state, context_.generation, context_.attempt, context_.deadline,
          context_.established};
}

}  // namespace libchiaking::net::protocol::takion
