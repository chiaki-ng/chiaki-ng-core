/// @file
/// @brief Declares Link, a Kakusan spoke's transport combined with its reliable
/// control channel and record routing.
#pragma once

#include "src/core/net/kakusan/control_channel.hpp"
#include "src/core/net/kakusan/control_message.hpp"
#include "src/core/net/kakusan/controller_frame.hpp"
#include "src/core/net/kakusan/link_event.hpp"
#include "src/core/net/kakusan/link_lifecycle.hpp"
#include "src/core/net/kakusan/pake.hpp"
#include "src/core/net/kakusan/peer_link.hpp"
#include "src/core/net/kakusan/result.hpp"
#include "src/core/net/kakusan/transport_frame.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace libchiaking::net::kakusan {

/// @brief One spoke's link: its transport plus the reliable control channel,
/// demultiplexing records by prefix and enforcing which side may send what.
///
/// Hosts receive input and media ACKs and send rumble and media; guests the
/// reverse. A malformed record deafens the link (Malformed) so only a terminal
/// control can still go out. Owns its transport and closes it on destruction.
/// Not copyable or movable; one thread drives it.
class Link {
 public:
  /// @brief Takes a transport.
  ///
  /// @param transport The transport; not null.
  /// @param role The side this link serves.
  Link(std::unique_ptr<SpokeTransport> transport, TransportRole role) noexcept;
  /// @brief Closes the link.
  ~Link();
  /// @brief Not copyable.
  Link(const Link&) = delete;
  /// @brief Not copyable.
  ///
  /// @return Never returns.
  Link& operator=(const Link&) = delete;
  /// @brief Not movable.
  Link(Link&&) = delete;
  /// @brief Not movable.
  ///
  /// @return Never returns.
  Link& operator=(Link&&) = delete;

  /// @brief Starts gathering.
  ///
  /// @param now_us The current monotonic time.
  /// @param credentials Derived ICE credentials, or nothing for fresh ones.
  /// @return Nothing on success.
  /// @retval Error::internal The link is closed.
  /// @retval Error The transport refused.
  [[nodiscard]] auto gather(std::uint64_t now_us,
                            std::optional<IceCredentials> credentials)
      -> Result<void>;
  /// @brief Connects to the peer; the transcript binds the exporters.
  ///
  /// @param remote The peer's description.
  /// @param transcript The pairing transcript.
  /// @param now_us The current monotonic time.
  /// @return Nothing on success.
  /// @retval Error::internal Closed or already connecting.
  /// @retval Error The transport refused.
  [[nodiscard]] auto connect(const TransportDescription& remote,
                             const Transcript& transcript, std::uint64_t now_us)
      -> Result<void>;
  /// @brief Runs the transport and control channel and reports events.
  ///
  /// @param now_us The current monotonic time.
  /// @return The events, oldest first, valid until the next pump; none once
  ///     stopped or closed.
  auto pump(std::uint64_t now_us) -> std::span<LinkEvent>;
  /// @brief When pump() must next run.
  ///
  /// @return The earlier of the transport's and channel's deadlines; nothing
  ///     when stopped.
  auto next_deadline_us() const noexcept -> std::optional<std::uint64_t>;

  /// @brief Queues a control message.
  ///
  /// A terminal control replaces everything not yet sent and closes the
  /// queue;
  /// it is Acked once the channel is idle.
  ///
  /// @param message The message.
  /// @param token Returned in Acked when the peer acknowledges it.
  /// @param terminal Whether this is the last control.
  /// @param now_us The current monotonic time.
  /// @return Nothing on success.
  /// @retval Error::no_path No channel, or the link stopped.
  /// @retval Error The queue refused the message.
  [[nodiscard]] auto send_control(const control::Message& message,
                                  AckToken token, bool terminal,
                                  std::uint64_t now_us) -> Result<void>;
  /// @brief Sends a controller state (guest only).
  ///
  /// @param frame The record.
  /// @return Nothing on success.
  /// @retval Error::no_path Not a guest, no channel, or stopped.
  /// @retval Error::protocol The record is invalid.
  [[nodiscard]] auto send_input(const InputFrame& frame) -> Result<void>;
  /// @brief Sends rumble (host only).
  ///
  /// @param frame The record.
  /// @return Nothing on success.
  /// @retval Error::no_path Not a host, no channel, or stopped.
  /// @retval Error::protocol The record is invalid.
  [[nodiscard]] auto send_feedback(const FeedbackFrame& frame) -> Result<void>;
  /// @brief Sends a media acknowledgement (guest only).
  ///
  /// @param record The RCA1 record.
  /// @return Nothing on success.
  /// @retval Error::no_path Not a guest, no channel, or stopped.
  [[nodiscard]] auto send_media_ack(std::span<const std::byte> record)
      -> Result<void>;
  /// @brief Sends an RCPM datagram (host only).
  ///
  /// @param datagram The datagram.
  /// @return Nothing on success.
  /// @retval Error::no_path Not a host, no channel, or stopped.
  [[nodiscard]] auto send_media(std::span<const std::byte> datagram)
      -> Result<void>;
  /// @brief Exports the spoke's media MAC key (`EXPORTER-RCP-1-media`,
  /// context
  /// the transcript).
  ///
  /// @return The key.
  /// @retval Error::internal Not established or no transcript.
  /// @retval Error The export failed.
  [[nodiscard]] auto media_key() -> Result<MediaKey>;

  /// @brief Closes the channel and transport. Idempotent.
  void close() noexcept;
  /// @brief Whether the control channel is open.
  ///
  /// @return True after Established.
  auto established() const noexcept -> bool { return channel_.has_value(); }
  /// @brief Whether close() ran.
  ///
  /// @return True once closed.
  auto closed() const noexcept -> bool { return lifecycle_.closed(); }

 private:
  /// @brief Routes one received record by prefix.
  ///
  /// @param record The record.
  /// @param now_us The current monotonic time.
  void on_record(std::span<const std::byte> record, std::uint64_t now_us);
  /// @brief Exports the pairing secret and opens the control channel.
  ///
  /// @param now_us The current monotonic time.
  void on_established(std::uint64_t now_us);
  /// @brief Reports acknowledgements and sends the channel's due records.
  ///
  /// @param now_us The current monotonic time.
  void flush(std::uint64_t now_us);

  /// @brief The owned transport.
  std::unique_ptr<SpokeTransport> transport_;
  /// @brief The side served.
  TransportRole role_;
  /// @brief The pairing transcript, from connect().
  std::optional<Transcript> transcript_;
  /// @brief The control channel, once established.
  std::optional<ControlChannel> channel_;
  /// @brief Queued messages awaiting acknowledgement, with their tokens.
  std::vector<std::pair<ControlTicket, AckToken>> tickets_;
  /// @brief The terminal control's token, reported when the channel drains.
  std::optional<AckToken> terminal_token_;
  /// @brief Scratch space for transport events.
  std::vector<TransportEvent> scratch_;
  /// @brief Scratch space for records to send.
  std::vector<std::vector<std::byte>> records_;
  /// @brief Whether the link runs and listens, and the events it queued.
  LinkLifecycle lifecycle_;
};

}  // namespace libchiaking::net::kakusan
