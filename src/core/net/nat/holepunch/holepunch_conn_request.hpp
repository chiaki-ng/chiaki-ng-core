/// @file
/// @brief Declares ConnRequest, the `connRequest` object of OFFER and ACCEPT
/// messages, and its codec.
#pragma once

#include <libchiaking/detail/secret_string.hpp>
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/secret_array.hpp"
#include "src/core/net/nat/holepunch/holepunch_candidate.hpp"
#include "src/core/net/nat/holepunch/limits.hpp"
#include "src/core/utils/json/ps_reply.hpp"
#include "src/core/utils/text/base64.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace libchiaking::net::nat::holepunch {

/// @brief The session ids of a connection request.
struct ConnRequestIds {
  /// @brief The sender's session id.
  std::uint32_t sid{};
  /// @brief The id the sender gives the other side.
  std::uint32_t peer_sid{};
};

/// @brief Why a connection request could not be read.
enum class ConnRequestError : std::uint8_t {
  /// @brief A field is missing or invalid.
  invalid_schema
};

/// @brief One side's connection request: who it is, its NAT and the candidate
/// endpoints the other side should probe.
struct ConnRequest {
  /// @brief The sender's session id; must fit 16 bits to be probed.
  std::uint32_t sid{};
  /// @brief The id given to the other side.
  std::uint32_t peer_sid{};
  /// @brief The NAT type the sender reports, as PS numbers it.
  std::uint8_t nat_type{};
  /// @brief The sender's default-route MAC; zero when not given.
  std::array<std::byte, mac_address_size> default_route_mac{};
  /// @brief The 16-byte session key, base64 on the wire.
  SecretArray<psn_custom_data_size> skey;
  /// @brief The sender's 20-byte hashed id; zero when anonymous.
  SecretArray<holepunch_hashed_id_size> local_hashed_id;
  /// @brief Endpoints to probe.
  std::vector<Candidate> candidates;
};

/// @brief A connection request as parsed, every field optional until checked.
struct ConnRequestWire {
  /// @brief Members map to camelCase JSON keys.
  using keys = json::ps::CamelCaseKeys;
  /// @brief `sid`.
  std::optional<json::ps::Number> sid;
  /// @brief `peerSid`.
  std::optional<json::ps::Number> peer_sid;
  /// @brief `natType`.
  std::optional<json::ps::Number> nat_type;
  /// @brief `defaultRouteMacAddr`, `aa:bb:cc:dd:ee:ff`.
  std::optional<std::string> default_route_mac_addr;
  /// @brief `skey`, base64. Secret; held in wiping memory.
  std::optional<SecretString> skey;
  /// @brief `localHashedId`, base64 or empty.
  std::optional<std::string> local_hashed_id;
  /// @brief `candidate`, the list of candidates.
  std::optional<std::vector<CandidateWire>> candidate;
};

/// @brief `localPeerAddr`: the sender's account and platform.
struct PeerAddress {
  /// @brief Members map to camelCase JSON keys.
  using keys = json::ps::CamelCaseKeys;
  /// @brief The PSN account id, decimal text.
  std::string account_id;
  /// @brief The platform, `REMOTE_PLAY` for this client.
  std::string_view platform;
};

/// @brief A connection request as written.
struct ConnRequestEntry {
  /// @brief Members map to camelCase JSON keys.
  using keys = json::ps::CamelCaseKeys;
  /// @brief `sid`.
  std::uint32_t sid{};
  /// @brief `peerSid`.
  std::uint32_t peer_sid{};
  /// @brief `skey`, standard base64.
  SecretString skey;
  /// @brief `natType`.
  std::uint32_t nat_type{};
  /// @brief `candidate`.
  std::vector<CandidateEntry> candidate;
  /// @brief `defaultRouteMacAddr`; always written empty.
  std::string_view default_route_mac_addr;
  /// @brief `localPeerAddr`.
  PeerAddress local_peer_addr;
  /// @brief `localHashedId`, standard base64; empty when the id is all zeros.
  SecretString local_hashed_id;
};

/// @brief Why a connection request could not be written.
enum class ConnRequestEncodeError : std::uint8_t {
  /// @brief A candidate is invalid or there are too many.
  invalid_candidate,
  /// @brief The candidate list or request exceeds its size limit.
  too_large,
};

/// @brief Reads and writes connection requests. Stateless.
class ConnRequestCodec {
 public:
  /// @brief Reads the session ids.
  ///
  /// @param conn_request The parsed request.
  /// @return The ids, wrapped to 32 bits; nothing when either is missing or
  ///     not
  ///     an integer.
  [[nodiscard]] static auto decode_ids(const ConnRequestWire& conn_request)
      -> std::optional<ConnRequestIds>;

  /// @brief Reads the NAT type.
  ///
  /// @param conn_request The parsed request.
  /// @return The type, wrapped to 8 bits; nothing when missing or not an
  ///     integer.
  [[nodiscard]] static auto decode_nat(const ConnRequestWire& conn_request)
      -> std::optional<std::uint8_t>;

  /// @brief Reads the default-route MAC, up to any NUL.
  ///
  /// @param conn_request The parsed request.
  /// @return The MAC; all zeros when the text is not 17 characters long;
  ///     nothing when absent or when 17 characters are not hex pairs joined
  ///     by
  ///     `:`.
  [[nodiscard]] static auto decode_mac(const ConnRequestWire& conn_request)
      -> std::optional<std::array<std::byte, mac_address_size>>;

  /// @brief Reads the session key.
  ///
  /// @param conn_request The parsed request.
  /// @return The 16-byte key; nothing when absent or not standard base64 of
  ///     16
  ///     bytes.
  [[nodiscard]] static auto decode_skey(const ConnRequestWire& conn_request)
      -> std::optional<SecretArray<psn_custom_data_size>>;

  /// @brief Reads the hashed id.
  ///
  /// @param conn_request The parsed request.
  /// @return The 20-byte id, all zeros when empty; nothing when absent or not
  ///     standard base64 of 20 bytes.
  [[nodiscard]] static auto decode_hashed_id(
      const ConnRequestWire& conn_request)
      -> std::optional<SecretArray<holepunch_hashed_id_size>>;

  /// @brief Reads a whole connection request.
  ///
  /// @param conn_request The parsed request.
  /// @return The request; nothing when any field fails to read or there are
  ///     more than 512 candidates.
  [[nodiscard]] static auto decode(const ConnRequestWire& conn_request)
      -> std::optional<ConnRequest>;

  /// @brief Largest connection request written: 16 KiB of JSON.
  static constexpr std::size_t max_json_bytes = 16U * 1024U;

  /// @brief Prepares a connection request for writing.
  ///
  /// @param request The request.
  /// @param account_id This user's PSN account id.
  /// @return The entry to write.
  /// @retval ConnRequestEncodeError::invalid_candidate A candidate is invalid
  ///     or there are more than 512.
  /// @retval ConnRequestEncodeError::too_large The candidates exceed 16 KiB
  ///     or
  ///     the request does.
  [[nodiscard]] static auto encode(const ConnRequest& request,
                                   std::int64_t account_id)
      -> Expected<ConnRequestEntry, ConnRequestEncodeError>;
};

}  // namespace libchiaking::net::nat::holepunch
