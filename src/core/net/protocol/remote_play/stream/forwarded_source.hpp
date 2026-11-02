/// @file
/// @brief Declares ForwardedSourceReceiver, which turns Takion media
/// datagrams forwarded from another player's stream back into media.
///
/// In Kakusan the host forwards its console's video and audio datagrams
/// to the guest as they arrived, still encrypted, with the stream's receive
/// key and IV and each datagram's key position. The guest checks each
/// datagram's MAC, decrypts it, and runs the same reorder, assembly and
/// dependency pipeline as the host's own stream.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/platform/crypto/provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/net/protocol/remote_play/stream/stream.hpp"
#include "src/core/net/protocol/takion/profile.hpp"
#include "src/core/net/protocol/takion/source_tap.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace libchiaking::net::protocol::remote_play {

/// @brief What a guest needs to receive a forwarded stream.
///
/// Move-only through the keys.
struct ForwardedSourceSetup {
  /// @brief The host stream's Takion version.
  std::uint32_t takion_version{};
  /// @brief The host stream's receive key and IV.
  takion::SourceReceiveKeys keys;
  /// @brief The picture sizes, each with its codec header.
  std::vector<event::VideoProfile> profiles;
  /// @brief The audio header.
  std::array<std::byte, event::audio_header_size> audio_header{};
  /// @brief Whether the video is H.265.
  bool hevc{};
};

/// @brief Why a forwarded stream could not be received.
enum class ForwardedSourceError : std::uint8_t {
  /// @brief No picture sizes, or a size without a codec header.
  invalid_setup,
  /// @brief The Takion version cannot be forwarded.
  unsupported_version,
  /// @brief The receive cipher could not be made or run.
  crypto_unavailable,
  /// @brief A datagram is not media of the expected kind, is truncated, or
  /// does not match its key position.
  malformed_datagram,
  /// @brief A datagram's MAC is wrong.
  authentication_failed,
  /// @brief The media pipeline failed.
  media_failed,
};

/// @brief Receives one forwarded stream.
///
/// Owned through a unique_ptr. Not thread-safe; one thread pushes, polls and
/// takes.
class ForwardedSourceReceiver {
 public:
  /// @brief Whether a Takion version's media can be forwarded.
  ///
  /// @param version The version.
  /// @return False for unknown versions, version 7 and the PS cloud only
  ///     versions.
  static constexpr bool forwardable_takion_version(
      std::uint32_t version) noexcept {
    const auto profile = takion::resolve_takion_wire_profile(version);
    return profile.has_value() &&
           profile->av_layout() != takion::AvLayout::version_7 &&
           !takion::is_cloud_stream_only_takion_version(version);
  }

  /// @brief Makes a receiver.
  ///
  /// @param crypto The crypto provider; must outlive the receiver.
  /// @param threading Makes the cipher's cache mutex; must outlive the
  ///     receiver.
  /// @param setup What to receive; taken over.
  /// @return The receiver.
  /// @retval ForwardedSourceError::unsupported_version The version cannot be
  ///     forwarded.
  /// @retval ForwardedSourceError::invalid_setup The picture sizes are
  ///     unusable.
  /// @retval ForwardedSourceError::crypto_unavailable The cipher could not be
  ///     made.
  [[nodiscard]] static auto create(platform::CryptoProvider& crypto,
                                   platform::ThreadingProvider& threading,
                                   ForwardedSourceSetup setup)
      -> Expected<std::unique_ptr<ForwardedSourceReceiver>,
                  ForwardedSourceError>;

  /// @brief Releases the pipeline.
  ~ForwardedSourceReceiver();
  ForwardedSourceReceiver(const ForwardedSourceReceiver&) = delete;
  ForwardedSourceReceiver& operator=(const ForwardedSourceReceiver&) = delete;

  /// @brief The stream info the guest presents.
  ///
  /// @return The formats.
  auto stream_info() const -> event::StreamInfoReady;
  /// @brief Takes one forwarded datagram.
  ///
  /// The datagram's 32-bit key position field must match the low half of
  /// `key_position`. Audio goes straight to the pipeline; video through the
  /// reorder queue first.
  ///
  /// @param media Whether it is video or audio.
  /// @param key_position The datagram's 64-bit key position.
  /// @param datagram The datagram as the host received it.
  /// @param now_us Monotonic microseconds now.
  /// @return Nothing on success.
  /// @retval ForwardedSourceError::malformed_datagram The datagram is
  ///     unusable.
  /// @retval ForwardedSourceError::authentication_failed Its MAC is wrong.
  /// @retval ForwardedSourceError::crypto_unavailable The cipher failed.
  /// @retval ForwardedSourceError::media_failed The pipeline failed.
  [[nodiscard]] auto push(takion::SourceMedia media, std::uint64_t key_position,
                          std::span<const std::byte> datagram,
                          std::uint64_t now_us)
      -> Expected<void, ForwardedSourceError>;
  /// @brief Releases video the reorder queue gave up waiting on.
  ///
  /// @param now_us Monotonic microseconds now.
  /// @return Nothing, or ForwardedSourceError::media_failed.
  [[nodiscard]] auto poll(std::uint64_t now_us)
      -> Expected<void, ForwardedSourceError>;
  /// @brief When poll() should next run.
  ///
  /// @return Monotonic microseconds, or nothing when nothing waits.
  auto next_deadline_us() const noexcept -> std::optional<std::uint64_t>;
  /// @brief Takes the next media or FEC failure notice.
  ///
  /// @return The event, or nothing.
  [[nodiscard]] auto take() -> std::optional<event::EncodedMedia>;
  /// @brief Takes whether the pipeline wants a keyframe since the last call.
  ///
  /// @return True once per request.
  [[nodiscard]] auto take_idr_request() noexcept -> bool;

 private:
  /// @brief The pipeline behind the receiver.
  class Impl;
  /// @brief Takes the pipeline.
  ///
  /// @param impl The pipeline.
  explicit ForwardedSourceReceiver(std::unique_ptr<Impl> impl) noexcept;
  /// @brief The pipeline.
  std::unique_ptr<Impl> impl_;
};

}  // namespace libchiaking::net::protocol::remote_play
