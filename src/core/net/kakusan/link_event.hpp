/// @file
/// @brief Declares what a Kakusan Link reports from pump(), and the tokens
/// and exporter output those reports carry.
#pragma once

#include "src/core/net/kakusan/ack_token.hpp"
#include "src/core/net/kakusan/control_message.hpp"
#include "src/core/net/kakusan/controller_frame.hpp"
#include "src/core/net/kakusan/pake.hpp"
#include "src/core/net/kakusan/peer_link.hpp"
#include "src/core/net/kakusan/result.hpp"

#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

namespace libchiaking::net::kakusan {

/// @brief What a Link reports from pump().
namespace link_event {

/// @brief Gathering finished; this is what to publish.
struct Gathered {
  /// @brief The local description.
  TransportDescription local;
};
/// @brief ICE nominated a pair.
struct Nominated {};
/// @brief DTLS finished; the control channel is open.
struct Established {
  /// @brief The pairing exporter output, for PAKE and IDENTITY.
  Exporter exporter;
};
/// @brief A control message arrived.
struct Control {
  /// @brief The message.
  control::Message message;
};
/// @brief The peer acknowledged a control message.
struct Acked {
  /// @brief The caller's tag.
  AckToken token{};
};
/// @brief A guest's controller state arrived (host only).
struct Input {
  /// @brief The record.
  InputFrame frame;
};
/// @brief The host's rumble arrived (guest only).
struct Feedback {
  /// @brief The record.
  FeedbackFrame frame;
};
/// @brief A guest's media acknowledgement arrived (host only), undecoded.
struct MediaAck {
  /// @brief The RCA1 record.
  std::vector<std::byte> record;
};
/// @brief An RCPM datagram arrived (guest only), unauthenticated.
struct Media {
  /// @brief The datagram.
  std::vector<std::byte> datagram;
};
/// @brief The peer sent something malformed; the link stops listening but can
/// still send one terminal control.
struct Malformed {
  /// @brief Why.
  Error error{Error::protocol};
};
/// @brief The link failed and stops.
struct Failed {
  /// @brief Why.
  Error error{Error::no_path};
};

}  // namespace link_event

/// @brief Any link event.
using LinkEvent =
    std::variant<link_event::Gathered, link_event::Nominated,
                 link_event::Established, link_event::Control,
                 link_event::Acked, link_event::Input, link_event::Feedback,
                 link_event::MediaAck, link_event::Media, link_event::Malformed,
                 link_event::Failed>;

}  // namespace libchiaking::net::kakusan
