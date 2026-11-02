/// @file
/// @brief Declares SourceTap, the hook through which a live Takion session
/// hands its still-sealed media packets and their receive keys to session
/// sharing.
///
/// Sharing a session to another player forwards the console's encrypted video
/// and audio unchanged; the receiving side needs the receive keys to open
/// them. The tap is told the keys once crypto is installed and then every
/// authenticated AV datagram before it is decrypted.
#pragma once

#include <libchiaking/detail/secret_array.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace libchiaking::net::protocol::takion {

/// @brief Size, in bytes, of a receive key and of its IV.
inline constexpr std::size_t source_key_size = 16;

/// @brief The AES-128 key and counter IV that open one direction of a Takion
/// session (GKCrypt), as derived from the ECDH secret and handshake key.
///
/// Move-only; both arrays are wiped on destruction. clone() makes an explicit
/// copy.
class SourceReceiveKeys {
 public:
  /// @brief Takes ownership of a key and IV.
  ///
  /// @param key The AES-128 key.
  /// @param iv The 16-byte IV the CTR counter and GMAC nonce are offset from.
  SourceReceiveKeys(SecretArray<source_key_size> key,
                    SecretArray<source_key_size> iv) noexcept
      : key_{std::move(key)}, iv_{std::move(iv)} {}

  /// @brief Reads the key.
  ///
  /// @return The 16-byte key; valid while this object lives.
  auto key() const noexcept -> std::span<const std::byte, source_key_size> {
    return std::span<const std::byte, source_key_size>{key_.data(),
                                                       key_.size()};
  }
  /// @brief Reads the IV.
  ///
  /// @return The 16-byte IV; valid while this object lives.
  auto iv() const noexcept -> std::span<const std::byte, source_key_size> {
    return std::span<const std::byte, source_key_size>{iv_.data(), iv_.size()};
  }
  /// @brief Copies the keys into a new owner.
  ///
  /// @return An independent copy.
  auto clone() const noexcept -> SourceReceiveKeys {
    return SourceReceiveKeys{key_.clone(), iv_.clone()};
  }

 private:
  /// @brief The AES-128 key.
  SecretArray<source_key_size> key_;
  /// @brief The IV.
  SecretArray<source_key_size> iv_;
};

/// @brief Which medium a sealed datagram carries.
enum class SourceMedia : std::uint8_t {
  /// @brief A video packet (type 0x02).
  video,
  /// @brief An audio or haptics packet (type 0x03).
  audio,
};

/// @brief One authenticated, still-encrypted AV datagram and what its header
/// says.
struct SourceDatagram {
  /// @brief Its medium.
  SourceMedia media{SourceMedia::video};
  /// @brief The full 64-bit key position its payload was encrypted at,
  /// reconstructed from the 32-bit field on the wire.
  std::uint64_t key_position{};
  /// @brief The whole Takion packet, without envelope; valid only during the
  /// call.
  std::span<const std::byte> bytes;
  /// @brief Bytes of encrypted unit data at the end of the packet.
  std::size_t sealed_size{};
  /// @brief The unit's index within its frame.
  std::uint16_t unit_index{};
  /// @brief Units in the frame, data and FEC; for audio only when the unit
  /// shape could be worked out.
  std::uint16_t unit_total{};
  /// @brief FEC units in the frame; for audio, redundant units.
  std::uint16_t unit_redundant{};
  /// @brief Bytes per audio unit, when known; zero for video.
  std::uint16_t unit_size{};
  /// @brief The codec byte.
  std::uint8_t codec{};
  /// @brief The adaptive stream (resolution profile) index of a video packet.
  std::uint8_t stream_index{};
  /// @brief Whether an audio packet carries haptics rather than sound.
  bool auxiliary{};
};

/// @brief Receives a live session's sealed media for forwarding.
///
/// Called on the thread pumping the Takion client, while it holds its lock;
/// implementations must be quick, must not throw, and must not call back into
/// the client.
class SourceTap {
 public:
  /// @brief Releases the tap.
  virtual ~SourceTap() = default;
  /// @brief Takes the keys that open the console's packets, once crypto is
  /// installed.
  ///
  /// @param takion_version The session's Takion version.
  /// @param keys The receive keys; copy them to keep them.
  virtual void on_source_keys(std::uint32_t takion_version,
                              const SourceReceiveKeys& keys) noexcept = 0;
  /// @brief Takes one authenticated AV datagram before it is decrypted.
  ///
  /// @param datagram The datagram and its header fields.
  virtual void on_source_datagram(const SourceDatagram& datagram) noexcept = 0;
};

}  // namespace libchiaking::net::protocol::takion
