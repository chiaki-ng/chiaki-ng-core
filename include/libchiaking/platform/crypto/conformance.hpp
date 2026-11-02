/// @file
/// @brief Header-only known-answer and validation checks for a host's
/// CryptoProvider.
#pragma once

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/platform/crypto/error.hpp>
#include <libchiaking/platform/crypto/provider.hpp>
#include <libchiaking/platform/factory.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace libchiaking::platform::crypto_conformance {

/// @brief Identifies one cryptography conformance check; the value is its bit
/// in Report::failed_mask.
enum class Check : std::uint32_t {
  /// @brief SHA-256 of "abc" and of the empty message match the published
  /// digests.
  sha256_known_answer = 1u << 0,
  /// @brief HMAC-SHA-256 matches a published test vector.
  hmac_sha256_known_answer = 1u << 1,
  /// @brief A single AES-128 block encryption matches the published test
  /// vector.
  aes128_block_known_answer = 1u << 2,
  /// @brief AES-128-GMAC matches known tags, including a 16-byte nonce and a
  /// tag truncated to four bytes.
  aes128_gmac_known_answer = 1u << 3,
  /// @brief Every mandatory primitive fails when a key, nonce, block, digest
  /// or tag span has the wrong size.
  validation_rejects_bad_arguments = 1u << 4,
  /// @brief The factory reports the current ABI version, and sets
  /// PlatformCapability::crypto exactly when it supplies a provider: without
  /// the flag, its provider refuses SHA-256 with
  /// CryptoError::unsupported_capability.
  capability_consistency = 1u << 5,
  /// @brief HKDF-SHA-256 matches RFC 5869 test cases 1 and 3, with and
  /// without a salt and context.
  hkdf_known_answer = 1u << 6,
};

/// @brief Outcome of run().
struct Report {
  /// @brief True when every check passed.
  bool ok{true};
  /// @brief Bitwise OR of the Check values that failed.
  std::uint32_t failed_mask{0};
};

namespace detail {

/// @brief Marks a check as failed in the report.
///
/// @param report Report to update.
/// @param check Check that failed.
/// @return Always false, so a check can return the call directly.
inline bool fail(Report& report, Check check) noexcept {
  report.ok = false;
  report.failed_mask |= static_cast<std::uint32_t>(check);
  return false;
}

/// @brief Tests whether a primitive succeeded and produced the expected bytes.
///
/// @param result What the primitive returned.
/// @param actual Bytes the primitive wrote.
/// @param expected Bytes it should have written.
/// @return True when @p result holds a value and the two spans are equal.
inline bool known_answer(const Expected<void, CryptoError>& result,
                         std::span<const std::byte> actual,
                         std::span<const std::byte> expected) {
  if (!result.has_value()) return false;
  if (actual.size() != expected.size()) return false;
  for (std::size_t index = 0; index < expected.size(); ++index) {
    if (actual[index] != expected[index]) return false;
  }
  return true;
}

/// @brief Decodes hexadecimal text into bytes while compiling.
///
/// @tparam Size Number of bytes the text encodes.
/// @param text Exactly twice @p Size hexadecimal digits, in either case.
/// @return The decoded bytes. Text of the wrong length or with a character
///     that is not a hexadecimal digit stops the build, because the call is
///     always evaluated at compile time.
template <std::size_t Size>
consteval auto from_hex(std::string_view text) -> std::array<std::byte, Size> {
  const auto nibble = [](char digit) -> unsigned {
    if (digit >= '0' && digit <= '9') return static_cast<unsigned>(digit - '0');
    if (digit >= 'a' && digit <= 'f')
      return static_cast<unsigned>(digit - 'a') + 10U;
    if (digit >= 'A' && digit <= 'F')
      return static_cast<unsigned>(digit - 'A') + 10U;
    throw "not a hexadecimal digit";
  };
  if (text.size() != 2 * Size) throw "hexadecimal text has the wrong length";
  std::array<std::byte, Size> bytes{};
  for (std::size_t index = 0; index < Size; ++index)
    bytes[index] = static_cast<std::byte>((nibble(text[2 * index]) << 4U) |
                                          nibble(text[2 * index + 1]));
  return bytes;
}

/// @brief Runs Check::sha256_known_answer.
///
/// @param crypto Provider under test.
/// @param report Receives the failure, if any.
/// @return True when the check passed.
inline bool check_sha256(CryptoProvider& crypto, Report& report) {
  constexpr std::array<std::byte, 32> abc{
      std::byte{0xba}, std::byte{0x78}, std::byte{0x16}, std::byte{0xbf},
      std::byte{0x8f}, std::byte{0x01}, std::byte{0xcf}, std::byte{0xea},
      std::byte{0x41}, std::byte{0x41}, std::byte{0x40}, std::byte{0xde},
      std::byte{0x5d}, std::byte{0xae}, std::byte{0x22}, std::byte{0x23},
      std::byte{0xb0}, std::byte{0x03}, std::byte{0x61}, std::byte{0xa3},
      std::byte{0x96}, std::byte{0x17}, std::byte{0x7a}, std::byte{0x9c},
      std::byte{0xb4}, std::byte{0x10}, std::byte{0xff}, std::byte{0x61},
      std::byte{0xf2}, std::byte{0x00}, std::byte{0x15}, std::byte{0xad}};
  constexpr std::array<std::byte, 32> empty{
      std::byte{0xe3}, std::byte{0xb0}, std::byte{0xc4}, std::byte{0x42},
      std::byte{0x98}, std::byte{0xfc}, std::byte{0x1c}, std::byte{0x14},
      std::byte{0x9a}, std::byte{0xfb}, std::byte{0xf4}, std::byte{0xc8},
      std::byte{0x99}, std::byte{0x6f}, std::byte{0xb9}, std::byte{0x24},
      std::byte{0x27}, std::byte{0xae}, std::byte{0x41}, std::byte{0xe4},
      std::byte{0x64}, std::byte{0x9b}, std::byte{0x93}, std::byte{0x4c},
      std::byte{0xa4}, std::byte{0x95}, std::byte{0x99}, std::byte{0x1b},
      std::byte{0x78}, std::byte{0x52}, std::byte{0xb8}, std::byte{0x55}};

  const std::array<std::byte, 3> input{std::byte{0x61}, std::byte{0x62},
                                       std::byte{0x63}};
  std::array<std::byte, sha256_digest_size> digest{};
  if (!known_answer(crypto.sha256(input, digest), digest, abc))
    return fail(report, Check::sha256_known_answer);

  if (!known_answer(crypto.sha256({}, digest), digest, empty))
    return fail(report, Check::sha256_known_answer);
  return true;
}

/// @brief Runs Check::hmac_sha256_known_answer.
///
/// @param crypto Provider under test.
/// @param report Receives the failure, if any.
/// @return True when the check passed.
inline bool check_hmac_sha256(CryptoProvider& crypto, Report& report) {
  constexpr std::array<std::byte, 32> expected{
      std::byte{0x5b}, std::byte{0xdc}, std::byte{0xc1}, std::byte{0x46},
      std::byte{0xbf}, std::byte{0x60}, std::byte{0x75}, std::byte{0x4e},
      std::byte{0x6a}, std::byte{0x04}, std::byte{0x24}, std::byte{0x26},
      std::byte{0x08}, std::byte{0x95}, std::byte{0x75}, std::byte{0xc7},
      std::byte{0x5a}, std::byte{0x00}, std::byte{0x3f}, std::byte{0x08},
      std::byte{0x9d}, std::byte{0x27}, std::byte{0x39}, std::byte{0x83},
      std::byte{0x9d}, std::byte{0xec}, std::byte{0x58}, std::byte{0xb9},
      std::byte{0x64}, std::byte{0xec}, std::byte{0x38}, std::byte{0x43}};

  const std::array<std::byte, 4> key{std::byte{0x4a}, std::byte{0x65},
                                     std::byte{0x66}, std::byte{0x65}};
  const std::array<std::byte, 28> input{
      std::byte{0x77}, std::byte{0x68}, std::byte{0x61}, std::byte{0x74},
      std::byte{0x20}, std::byte{0x64}, std::byte{0x6f}, std::byte{0x20},
      std::byte{0x79}, std::byte{0x61}, std::byte{0x20}, std::byte{0x77},
      std::byte{0x61}, std::byte{0x6e}, std::byte{0x74}, std::byte{0x20},
      std::byte{0x66}, std::byte{0x6f}, std::byte{0x72}, std::byte{0x20},
      std::byte{0x6e}, std::byte{0x6f}, std::byte{0x74}, std::byte{0x68},
      std::byte{0x69}, std::byte{0x6e}, std::byte{0x67}, std::byte{0x3f}};
  std::array<std::byte, hmac_sha256_digest_size> digest{};
  if (!known_answer(crypto.hmac_sha256(key, input, digest), digest, expected))
    return fail(report, Check::hmac_sha256_known_answer);
  return true;
}

/// @brief Runs Check::aes128_block_known_answer.
///
/// @param crypto Provider under test.
/// @param report Receives the failure, if any.
/// @return True when the check passed.
inline bool check_aes128_block(CryptoProvider& crypto, Report& report) {
  constexpr std::array<std::byte, 16> expected{
      std::byte{0x39}, std::byte{0x25}, std::byte{0x84}, std::byte{0x1d},
      std::byte{0x02}, std::byte{0xdc}, std::byte{0x09}, std::byte{0xfb},
      std::byte{0xdc}, std::byte{0x11}, std::byte{0x85}, std::byte{0x97},
      std::byte{0x19}, std::byte{0x6a}, std::byte{0x0b}, std::byte{0x32}};

  const std::array<std::byte, aes128_key_size> key{
      std::byte{0x2b}, std::byte{0x7e}, std::byte{0x15}, std::byte{0x16},
      std::byte{0x28}, std::byte{0xae}, std::byte{0xd2}, std::byte{0xa6},
      std::byte{0xab}, std::byte{0xf7}, std::byte{0x15}, std::byte{0x88},
      std::byte{0x09}, std::byte{0xcf}, std::byte{0x4f}, std::byte{0x3c}};
  const std::array<std::byte, aes128_block_size> block{
      std::byte{0x32}, std::byte{0x43}, std::byte{0xf6}, std::byte{0xa8},
      std::byte{0x88}, std::byte{0x5a}, std::byte{0x30}, std::byte{0x8d},
      std::byte{0x31}, std::byte{0x31}, std::byte{0x98}, std::byte{0xa2},
      std::byte{0xe0}, std::byte{0x37}, std::byte{0x07}, std::byte{0x34}};
  std::array<std::byte, aes128_block_size> output{};
  if (!known_answer(crypto.aes128_encrypt_block(key, block, output), output,
                    expected))
    return fail(report, Check::aes128_block_known_answer);
  return true;
}

/// @brief Runs Check::aes128_gmac_known_answer.
///
/// @param crypto Provider under test.
/// @param report Receives the failure, if any.
/// @return True when the check passed.
inline bool check_aes128_gmac(CryptoProvider& crypto, Report& report) {
  constexpr std::array<std::byte, 16> first_tag{
      std::byte{0x58}, std::byte{0xe2}, std::byte{0xfc}, std::byte{0xce},
      std::byte{0xfa}, std::byte{0x7e}, std::byte{0x30}, std::byte{0x61},
      std::byte{0x36}, std::byte{0x7f}, std::byte{0x1d}, std::byte{0x57},
      std::byte{0xa4}, std::byte{0xe7}, std::byte{0x45}, std::byte{0x5a}};
  constexpr std::array<std::byte, 16> second_tag{
      std::byte{0x34}, std::byte{0x64}, std::byte{0x34}, std::byte{0xfd},
      std::byte{0x51}, std::byte{0xd5}, std::byte{0xcd}, std::byte{0x0c},
      std::byte{0x58}, std::byte{0x87}, std::byte{0xec}, std::byte{0x63},
      std::byte{0xe3}, std::byte{0x9b}, std::byte{0x90}, std::byte{0x7a}};

  const std::array<std::byte, aes128_key_size> zero_key{};
  const std::array<std::byte, 12> zero_iv{};
  std::array<std::byte, aes128_gcm_tag_size> tag{};
  if (!known_answer(crypto.aes128_gmac(zero_key, zero_iv, {}, tag), tag,
                    first_tag))
    return fail(report, Check::aes128_gmac_known_answer);

  const std::array<std::byte, aes128_key_size> key{
      std::byte{0xfe}, std::byte{0xff}, std::byte{0xe9}, std::byte{0x92},
      std::byte{0x86}, std::byte{0x65}, std::byte{0x73}, std::byte{0x1c},
      std::byte{0x6d}, std::byte{0x6a}, std::byte{0x8f}, std::byte{0x94},
      std::byte{0x67}, std::byte{0x30}, std::byte{0x83}, std::byte{0x08}};
  const std::array<std::byte, 12> iv{
      std::byte{0xca}, std::byte{0xfe}, std::byte{0xba}, std::byte{0xbe},
      std::byte{0xfa}, std::byte{0xce}, std::byte{0xdb}, std::byte{0xad},
      std::byte{0xde}, std::byte{0xca}, std::byte{0xf8}, std::byte{0x88}};
  const std::array<std::byte, 20> aad{
      std::byte{0xfe}, std::byte{0xed}, std::byte{0xfa}, std::byte{0xce},
      std::byte{0xde}, std::byte{0xad}, std::byte{0xbe}, std::byte{0xef},
      std::byte{0xfe}, std::byte{0xed}, std::byte{0xfa}, std::byte{0xce},
      std::byte{0xde}, std::byte{0xad}, std::byte{0xbe}, std::byte{0xef},
      std::byte{0xab}, std::byte{0xad}, std::byte{0xda}, std::byte{0xd2}};
  if (!known_answer(crypto.aes128_gmac(key, iv, aad, tag), tag, second_tag))
    return fail(report, Check::aes128_gmac_known_answer);

  const std::array<std::byte, aes128_key_size> legacy_key{
      std::byte{0x27}, std::byte{0x7a}, std::byte{0xa5}, std::byte{0x1d},
      std::byte{0xac}, std::byte{0xd1}, std::byte{0x5f}, std::byte{0x0e},
      std::byte{0x54}, std::byte{0x12}, std::byte{0xfa}, std::byte{0xce},
      std::byte{0x0d}, std::byte{0xc4}, std::byte{0x63}, std::byte{0x6a}};
  const std::array<std::byte, 16> legacy_iv{
      std::byte{0xef}, std::byte{0x20}, std::byte{0x40}, std::byte{0xc2},
      std::byte{0x15}, std::byte{0x3c}, std::byte{0x02}, std::byte{0x66},
      std::byte{0x32}, std::byte{0x1f}, std::byte{0x42}, std::byte{0xbb},
      std::byte{0xf4}, std::byte{0x50}, std::byte{0x34}, std::byte{0x4d}};
  constexpr std::array<std::byte, 4> legacy_truncated_tag{
      std::byte{0x25}, std::byte{0x5e}, std::byte{0x37}, std::byte{0xd0}};
  constexpr std::array<std::byte, aes128_gcm_tag_size> legacy_full_tag{
      std::byte{0x25}, std::byte{0x5e}, std::byte{0x37}, std::byte{0xd0},
      std::byte{0x0f}, std::byte{0x8c}, std::byte{0xc2}, std::byte{0x5c},
      std::byte{0xd6}, std::byte{0x21}, std::byte{0xe7}, std::byte{0xc5},
      std::byte{0x8f}, std::byte{0x6d}, std::byte{0x50}, std::byte{0x54}};
  std::array<std::byte, 4> truncated_tag{};
  if (!known_answer(
          crypto.aes128_gmac(legacy_key, legacy_iv, {}, truncated_tag),
          truncated_tag, legacy_truncated_tag))
    return fail(report, Check::aes128_gmac_known_answer);

  if (!known_answer(crypto.aes128_gmac(legacy_key, legacy_iv, {}, tag), tag,
                    legacy_full_tag))
    return fail(report, Check::aes128_gmac_known_answer);
  return true;
}

/// @brief Runs Check::hkdf_known_answer.
///
/// Derives 42 bytes with HKDF-SHA-256 twice: RFC 5869 test case 1, with a
/// salt and a context, and test case 3, with neither. A provider that keeps
/// the CryptoProvider::hkdf default passes when its hmac_sha256() is right.
///
/// @param crypto Provider under test.
/// @param report Receives the failure, if any.
/// @return True when the check passed.
inline bool check_hkdf(CryptoProvider& crypto, Report& report) {
  constexpr auto input =
      from_hex<22>("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b");
  constexpr auto salt = from_hex<13>("000102030405060708090a0b0c");
  constexpr auto info = from_hex<10>("f0f1f2f3f4f5f6f7f8f9");
  constexpr auto salted = from_hex<42>(
      "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
      "34007208d5b887185865");
  constexpr auto unsalted = from_hex<42>(
      "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d"
      "9d201395faa4b61a96c8");
  std::array<std::byte, salted.size()> output{};
  if (!known_answer(crypto.hkdf(HkdfDigest::sha256, salt, input, info, output),
                    output, salted))
    return fail(report, Check::hkdf_known_answer);
  if (!known_answer(crypto.hkdf(HkdfDigest::sha256, {}, input, {}, output),
                    output, unsalted))
    return fail(report, Check::hkdf_known_answer);
  return true;
}

/// @brief Runs Check::validation_rejects_bad_arguments.
///
/// @param crypto Provider under test.
/// @param report Receives the failure, if any.
/// @return True when the check passed.
inline bool check_validation(CryptoProvider& crypto, Report& report) {
  const std::array<std::byte, aes128_key_size> key{};
  const std::array<std::byte, aes128_block_size> block{};
  const std::array<std::byte, 12> iv{};
  const std::array<std::byte, aes128_gcm_iv_size_min - 1> short_iv{};
  const std::array<std::byte, aes128_gcm_iv_size_max + 1> long_iv{};
  std::array<std::byte, sha256_digest_size> digest{};
  std::array<std::byte, aes128_block_size> output{};
  std::array<std::byte, aes128_gcm_tag_size> tag{};
  std::array<std::byte, aes128_gcm_tag_size_min - 1> short_tag{};
  std::array<std::byte, aes128_gcm_tag_size + 1> long_tag{};
  std::array<std::byte, hkdf_max_blocks * hmac_sha256_digest_size + 1>
      long_output{};

  bool ok = true;
  if (crypto.sha256(block, std::span<std::byte>(digest).first(31)) ||
      crypto.hmac_sha256(
          key, block,
          std::span<std::byte>(digest).first(hmac_sha256_digest_size - 1)) ||
      crypto.aes128_encrypt_block(
          std::span<const std::byte>(key).first(aes128_key_size - 1), block,
          output) ||
      crypto.aes128_encrypt_block(
          key, std::span<const std::byte>(block).first(aes128_block_size - 1),
          output) ||
      crypto.aes128_encrypt_block(
          key, block,
          std::span<std::byte>(output).first(aes128_block_size - 1)) ||
      crypto.aes128_gmac(key, {}, {}, tag) ||
      crypto.aes128_gmac(
          std::span<const std::byte>(key).first(aes128_key_size - 1), iv, {},
          tag) ||
      crypto.aes128_gmac(key, short_iv, {}, tag) ||
      crypto.aes128_gmac(key, long_iv, {}, tag) ||
      crypto.aes128_gmac(key, iv, {}, short_tag) ||
      crypto.aes128_gmac(key, iv, {}, {}) ||
      crypto.aes128_gmac(key, iv, {}, long_tag) ||
      crypto.hkdf(HkdfDigest::sha256, {}, key, {}, {}) ||
      crypto.hkdf(HkdfDigest::sha256, {}, key, {}, long_output)) {
    ok = false;
  }

  if (!ok) return fail(report, Check::validation_rejects_bad_arguments);
  return true;
}

/// @brief Runs Check::capability_consistency.
///
/// @param factory Factory under test.
/// @param report Receives the failure, if any.
/// @return True when the check passed.
inline bool check_capability_consistency(PlatformFactory& factory,
                                         Report& report) {
  if (factory.info().abi_version != platform_abi_version)
    return fail(report, Check::capability_consistency);
  if (factory.supplies(PlatformCapability::crypto)) return true;
  std::array<std::byte, sha256_digest_size> digest{};
  const auto probed = factory.crypto().sha256({}, digest);
  if (probed || probed.error() != CryptoError::unsupported_capability)
    return fail(report, Check::capability_consistency);
  return true;
}

}  // namespace detail

/// @brief Runs the cryptography conformance checks against a factory's
/// provider.
///
/// A host that implements its own CryptoProvider calls this from a test. Unlike
/// the runtime checks, every check runs even after one fails, so
/// Report::failed_mask can name several. The mandatory primitives and
/// HKDF-SHA-256, which every provider has through the CryptoProvider::hkdf
/// default, are exercised; the other optional ones are not checked here. A
/// factory that does not report PlatformCapability::crypto passes as long as
/// its provider refuses SHA-256 with CryptoError::unsupported_capability; the
/// other checks do not exercise it.
///
/// The call blocks while the checks run and calls the provider from the
/// calling thread only. Run it while nothing else uses the factory.
///
/// @param factory Factory whose PlatformFactory::crypto is tested.
/// @return The report; Report::ok is true when every check passed, and
///     Report::failed_mask names each Check that did not.
inline Report run(PlatformFactory& factory) {
  Report report;
  detail::check_capability_consistency(factory, report);
  if (factory.supplies(PlatformCapability::crypto)) {
    CryptoProvider& crypto = factory.crypto();
    detail::check_sha256(crypto, report);
    detail::check_hmac_sha256(crypto, report);
    detail::check_aes128_block(crypto, report);
    detail::check_aes128_gmac(crypto, report);
    detail::check_hkdf(crypto, report);
    detail::check_validation(crypto, report);
  }
  return report;
}

}  // namespace libchiaking::platform::crypto_conformance
