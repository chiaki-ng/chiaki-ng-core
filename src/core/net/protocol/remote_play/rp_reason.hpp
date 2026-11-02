/// @file
/// @brief The `RP-Application-Reason` codes a PlayStation sends when it
/// refuses a Remote Play session or registration request.
///
/// The console states the reason as a hexadecimal header value beside a
/// non-200 status. Only the codes the library acts on are named here; any other
/// value is carried through to the caller as a raw number.
#pragma once

#include <cstdint>

namespace libchiaking {

/// @brief The console refused the registration (wrong PIN, expired PIN or a
/// failed key exchange).
inline constexpr std::uint32_t rp_reason_regist_failed = 0x80108b09U;
/// @brief The PSN account named in the request is not one the console knows.
inline constexpr std::uint32_t rp_reason_invalid_psn_id = 0x80108b02U;
/// @brief The console is already streaming to another Remote Play client.
inline constexpr std::uint32_t rp_reason_in_use = 0x80108b10U;
/// @brief The console's Remote Play service failed internally.
inline constexpr std::uint32_t rp_reason_crash = 0x80108b15U;
/// @brief The console does not speak the `RP-Version` the request named; the
/// reply's own `RP-Version` header says which one it wants.
inline constexpr std::uint32_t rp_reason_rp_version = 0x80108b11U;
/// @brief An unspecified refusal. Older PS4 firmware sends it instead of
/// rp_reason_rp_version when the version is wrong, so a different
/// `RP-Version` in the same reply is still treated as a version mismatch.
inline constexpr std::uint32_t rp_reason_unknown = 0x80108bffU;

}  // namespace libchiaking
