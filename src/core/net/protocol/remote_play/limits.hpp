/// @file
/// @brief Fixed values of the Remote Play session and control requests:
/// timeouts, buffer sizes, request paths, constant header values and the
/// counters that key each encrypted control header.
///
/// Both requests are plain HTTP/1.1 over TCP port 9295 (or over RUDP when the
/// console is reached through PSN). The session request (`sess/init`) asks for
/// a nonce; the control request (`sess/ctrl`) opens the long-lived control
/// connection. The paths differ per console generation and RP version.
#pragma once

#include "src/core/net/protocol/remote_play/pairing/limits.hpp"
#include "src/core/net/protocol/remote_play/pairing/rp_version.hpp"
#include "src/core/net/protocol/remote_play/registered_console.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace libchiaking::net::protocol::remote_play {

/// @brief How long one phase of a TCP request exchange (connect, send or
/// receive) may take before it fails, on the monotonic clock (5 s).
inline constexpr std::chrono::microseconds rp_phase_timeout{5'000'000};
/// @brief The most state-machine steps one pump of a TcpExchange runs before
/// yielding, so a misbehaving peer cannot spin the caller forever.
inline constexpr std::size_t max_exchange_steps_per_pump = 4096;
/// @brief The largest HTTP response head accepted from the console for a
/// session or control request, in bytes, including the status line and the
/// blank line.
inline constexpr std::size_t rp_max_header_bytes = 512;
/// @brief The capacity of the encoded control request head, in bytes.
inline constexpr std::size_t ctrl_request_capacity = 1024;
/// @brief The capacity of one base64-encoded encrypted control header value,
/// in characters (fits the 32-byte device id after encryption).
inline constexpr std::size_t encrypted_base64_capacity = 48;
/// @brief The size of the decoded, still encrypted `RP-Server-Type` header,
/// in bytes. Its first byte, once decrypted, is the server type.
inline constexpr std::size_t rp_server_type_size = 16;

/// @brief The shortest session-id control message accepted: one marker byte
/// and 24 characters.
inline constexpr std::size_t session_id_min_len = 25;
/// @brief One more than the longest session id accepted, in characters
/// (excluding the marker byte).
inline constexpr std::size_t session_id_max_len = 79;
/// @brief The byte that begins a well-formed session-id control message
/// (`J`). Other first bytes are accepted but flagged.
inline constexpr std::byte session_id_marker{0x4a};
/// @brief How many random bytes go into a synthesised session id when the
/// console sends none that can be used.
inline constexpr std::size_t session_id_random_size = 48;

/// @brief `RP-StreamingType` value asking for H.264.
inline constexpr std::uint32_t rp_streaming_type_h264 = 1;
/// @brief `RP-StreamingType` value asking for H.265 in standard dynamic range.
inline constexpr std::uint32_t rp_streaming_type_h265 = 2;
/// @brief `RP-StreamingType` value asking for H.265 with HDR allowed.
inline constexpr std::uint32_t rp_streaming_type_h265_hdr = 3;

/// @brief The cipher counters under which each encrypted control request
/// header is sealed.
///
/// Every encrypted header of the control request (`RP-Auth`, `RP-Did`,
/// `RP-OSType`, `RP-StartBitrate`, `RP-StreamingType`) is sealed with the
/// session's AuthMaterial at its own fixed counter, in this order. The counter
/// after the last header sent becomes the first counter of the encrypted
/// control channel.
namespace ctrl_crypt_counter {
/// @brief Counter of `RP-Auth`, the registration key.
inline constexpr std::uint64_t auth = 0;
/// @brief Counter of `RP-Did`, the client device id.
inline constexpr std::uint64_t did = 1;
/// @brief Counter of `RP-OSType`, the client OS string.
inline constexpr std::uint64_t os_type = 2;
/// @brief Counter of `RP-StartBitrate`, sent to PS4 RP 10.0 and PS5 only.
inline constexpr std::uint64_t start_bitrate = 3;
/// @brief Counter of `RP-StreamingType`, sent to PS5 only.
inline constexpr std::uint64_t streaming_type = 4;
}  // namespace ctrl_crypt_counter

/// @brief `RP-ControllerType` sent in the control request, "3", the value
/// the Windows client sends.
inline constexpr std::string_view rp_controller_type = "3";
/// @brief `RP-ClientType` sent in the control request: the Windows Remote
/// Play client.
inline constexpr std::string_view rp_client_type = "11";
/// @brief The OS string the client claims, encrypted into `RP-OSType`. The
/// console checks it against the Windows client's.
inline constexpr std::string_view rp_os_type = "Win10.0.0";
/// @brief `RP-PadProcNo` sent to a PS5 in the control request, "3", the
/// value the Windows client sends.
inline constexpr std::string_view rp_pad_processing = "3";

/// @brief An `RP-Version` some PS4 firmware states for RP 9.0 when refusing a
/// session request; read as RegistrationTarget::ps4_9.
inline constexpr std::string_view rp_version_legacy_ps4_9_alias = "5.0";

/// @brief Session request path for PS4 RP 8.0 and 9.0.
inline constexpr std::string_view rp_session_path_ps4_legacy =
    "/sce/rp/session";
/// @brief Session request path for PS4 RP 10.0.
inline constexpr std::string_view rp_session_path_ps4 = "/sie/ps4/rp/sess/init";
/// @brief Session request path for PS5.
inline constexpr std::string_view rp_session_path_ps5 = "/sie/ps5/rp/sess/init";
/// @brief Control request path for PS4 RP 8.0 and 9.0.
inline constexpr std::string_view rp_ctrl_path_ps4_legacy =
    "/sce/rp/session/ctrl";
/// @brief Control request path for PS4 RP 10.0.
inline constexpr std::string_view rp_ctrl_path_ps4 = "/sie/ps4/rp/sess/ctrl";
/// @brief Control request path for PS5.
inline constexpr std::string_view rp_ctrl_path_ps5 = "/sie/ps5/rp/sess/ctrl";

/// @brief The `RP-Version` text a request for this target carries.
///
/// @param target The console variant being addressed.
/// @return "8.0", "9.0", "10.0" or "1.0" (PS5), or nothing for
///     RegistrationTarget::ps4_unknown and values outside the enumeration.
[[nodiscard]] constexpr auto rp_version_string(
    RegistrationTarget target) noexcept -> std::optional<std::string_view> {
  const auto version = RpVersion::of(target);
  if (!version) return std::nullopt;
  return version->text();
}

/// @brief The client OS string as bytes with its terminating NUL, the
/// plaintext encrypted into `RP-OSType`.
///
/// @return The ten bytes "Win10.0.0\0".
constexpr auto rp_os_type_bytes() noexcept
    -> std::array<std::byte, rp_os_type.size() + 1> {
  std::array<std::byte, rp_os_type.size() + 1> bytes{};
  std::ranges::transform(rp_os_type, bytes.begin(), [](char value) {
    return static_cast<std::byte>(value);
  });
  return bytes;
}

}  // namespace libchiaking::net::protocol::remote_play
