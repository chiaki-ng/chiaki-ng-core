/// @file
/// @brief Declares SharedSetup, the translation between the host's stream as
/// the play context describes it and the `net::kakusan` source setup message a
/// guest receives.
#pragma once

#include "src/core/net/kakusan/protocol_error.hpp"
#include "libchiaking/detail/play/session_share.hpp"
#include "src/core/kakusan/adapters/capture.hpp"
#include "src/core/net/kakusan/control_message.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace libchiaking::kakusan {

/// @brief Builds the source setup message on the host and reads it back on the
/// guest.
///
/// The source setup tells a guest everything it needs to open the forwarded
/// stream: the stream protocol version, codec, frame rate, video profiles,
/// audio header, the stream's receive keys, and a random id for this source.
/// Stateless; both members are static and may be called from any thread.
class SharedSetup {
 public:
  /// @brief A forwarded stream as the guest's play context opens it.
  struct Stream {
    /// @brief What the stream consists of.
    play::SharedStream stream;
    /// @brief The keys that decrypt it.
    play::ReceiveKeys keys;
  };

  /// @brief The random id the host draws for one source, carried in the
  /// setup; a view of exactly its size.
  using SessionId =
      std::span<const std::byte,
                std::tuple_size_v<net::kakusan::control::SourceSessionId>>;

  /// @brief Builds the setup a guest is sent when their seat joins.
  ///
  /// The codec is detected from the first profile's header, and every profile
  /// header must agree with it.
  ///
  /// @param keys The stream's current keys and protocol version.
  /// @param description The stream's video profiles and audio header.
  /// @param video_fps The video frame rate.
  /// @param source_session_id The source's random id.
  /// @return The setup, with copies of the keys in it.
  /// @retval net::kakusan::Error::version The protocol version does not fit a
  ///     byte or is not one the play context can share.
  /// @retval net::kakusan::Error::codec The frame rate is out of range, there
  ///     are no profiles or too many, the picture is HDR H.265, or a
  ///     profile's size or header is not acceptable.
  /// @retval net::kakusan::Error Any failure detecting the codec or checking
  ///     the audio header.
  [[nodiscard]] static auto build(const CapturedKeys& keys,
                                  const media::StreamDescription& description,
                                  std::uint32_t video_fps,
                                  SessionId source_session_id)
      -> net::kakusan::Result<net::kakusan::control::SourceSetup>;

  /// @brief Reads a setup the guest received back into a stream description
  /// and keys.
  ///
  /// @param setup The setup; its key material moves into the result.
  /// @return The stream and its keys.
  /// @retval net::kakusan::Error The setup or one of its profile headers is
  ///     not valid.
  [[nodiscard]] static auto stream(net::kakusan::control::SourceSetup setup)
      -> net::kakusan::Result<Stream>;
};

}  // namespace libchiaking::kakusan
