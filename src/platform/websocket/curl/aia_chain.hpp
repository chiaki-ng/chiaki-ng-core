/// @file
/// @brief Declares AiaChain, which recovers the intermediate certificates a
/// TLS server failed to send, by following each certificate's Authority
/// Information Access "CA issuers" URL.
///
/// Browsers do this and libcurl does not, so a WebSocket server with an
/// incomplete chain fails verification. The stock WebSocket provider uses
/// AiaChain after such a failure, adds what it recovers to the trusted
/// bundle and connects again, still with full verification.
#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::platform::detail {

/// @brief Downloads one issuer certificate by its AIA URL.
///
/// Returns the response body, DER or PEM, or nothing when the download
/// failed. Called on the thread running the recovery and may block for the
/// download.
using AiaFetch = std::function<std::optional<std::vector<std::byte>>(
    const std::string& url)>;

/// @brief The trust anchors a recovered chain must lead to: either root
/// certificates given directly, or OpenSSL CA file and directory locations.
struct AiaTrust {
  /// @brief A PEM bundle of trusted roots, or empty; used only when
  /// `roots` is empty.
  std::string ca_file;
  /// @brief A hashed directory of trusted roots, or empty; used only when
  /// `roots` is empty.
  std::string ca_path;
  /// @brief Trusted roots in DER, such as the Windows root store; when
  /// non-empty, the only anchors used. With no roots and no locations,
  /// OpenSSL's default paths are used.
  std::vector<std::vector<std::byte>> roots;
};

/// @brief Builds a verified path from a server's leaf certificate to a
/// trusted root by downloading missing issuers.
///
/// Stateless; every member may be called from any thread. Uses OpenSSL.
class AiaChain {
 public:
  /// @brief The most issuer certificates downloaded for one chain.
  static constexpr std::size_t max_hops = 4;
  /// @brief The first byte of a DER certificate (an ASN.1 SEQUENCE), by
  /// which DER is told from PEM.
  static constexpr std::byte der_sequence_tag{0x30};

  /// @brief Reads a certificate's CA issuers URL from its Authority
  /// Information Access extension.
  ///
  /// @param certificate_der The certificate in DER.
  /// @return The first CA issuers URI, or nothing when the certificate does
  ///     not parse, has no such entry, or the URI is empty or contains NUL.
  [[nodiscard]] static auto issuer_url(
      std::span<const std::byte> certificate_der) -> std::optional<std::string>;

  /// @brief Reads one certificate in DER or PEM and returns it as DER.
  ///
  /// @param encoded The certificate: DER when it starts with
  ///     der_sequence_tag, otherwise PEM, of which the first certificate is
  ///     taken.
  /// @return The certificate in DER, or nothing when it does not parse.
  [[nodiscard]] static auto certificate_der(std::span<const std::byte> encoded)
      -> std::optional<std::vector<std::byte>>;

  /// @brief Downloads issuers until the leaf verifies against the trust
  /// anchors, and returns the intermediates that made it verify.
  ///
  /// Follows only `http` and `https` URLs, at most max_hops of them, and
  /// blocks for each download. Verification checks the chain only, not the
  /// host name, which the later TLS connection checks.
  ///
  /// @param leaf_der The server's certificate in DER.
  /// @param fetch Downloads an issuer; must not be empty.
  /// @param trust The anchors the chain must reach.
  /// @return The intermediates between the leaf and the root, as
  ///     concatenated PEM; an empty string when the leaf verifies without
  ///     any download; or nothing when the leaf does not parse, the anchors
  ///     cannot be loaded, a download or parse fails, or no chain verifies
  ///     within max_hops.
  [[nodiscard]] static auto recover_intermediates(
      std::span<const std::byte> leaf_der, const AiaFetch& fetch,
      const AiaTrust& trust) -> std::optional<std::string>;

  /// @brief Reads the certificates of the system's root store, the trusted
  /// roots for recovery where libcurl uses the system store rather than a CA
  /// file.
  ///
  /// On Windows, where libcurl's Schannel backend trusts the system store,
  /// returns the certificates of the current user's `ROOT` store. Elsewhere it
  /// returns nothing, the CA file libcurl reports being used instead. Reads
  /// the store on every call. Defined per platform, in aia_roots_none.cpp and
  /// aia_roots_win32.cpp.
  ///
  /// @return The roots in DER; empty when the store cannot be opened, memory
  ///     runs out, or the platform has none to offer.
  static auto native_roots() -> std::vector<std::vector<std::byte>>;

 private:
  /// @brief The OpenSSL parsing, trust loading and verification behind the
  /// public functions.
  class Certificates;
};

}  // namespace libchiaking::platform::detail
