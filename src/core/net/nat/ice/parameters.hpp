/// @file
/// @brief Declares the ICE parameters peer-link peers exchange in their signed
/// blocks, their limits and their validation.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/secret_string.hpp"
#include "libchiaking/platform/net/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::net::nat::ice {

/// @brief Most candidates a peer exports or accepts.
inline constexpr std::size_t max_candidates = 32;
/// @brief Most extra remote endpoints the agent guesses beyond a peer's
/// reflexive ports, for a NAT that moves its ports.
inline constexpr std::size_t max_port_guesses = 32;
/// @brief Characters in an ICE ufrag: 16 lower-case hex, from 8 random bytes.
inline constexpr std::size_t ufrag_text_size = 16;
/// @brief Characters in an ICE password: 48 lower-case hex, from 24 random
/// bytes.
inline constexpr std::size_t password_text_size = 48;
/// @brief Longest candidate foundation, in characters.
inline constexpr std::size_t max_foundation_size = 32;
/// @brief Largest candidate priority: 2^31 - 1.
inline constexpr std::uint64_t max_candidate_priority = 2147483647;

/// @brief Longest candidate gathering runs, in microseconds: 15 seconds.
inline constexpr std::uint64_t gathering_timeout_us = 15'000'000;
/// @brief How often server-reflexive mappings are refreshed, in microseconds:
/// 15 seconds.
inline constexpr std::uint64_t mapping_refresh_us = 15'000'000;
/// @brief How long the host has to nominate a pair after connecting, in
/// microseconds: 30 seconds.
inline constexpr std::uint64_t connectivity_timeout_us = 30'000'000;

/// @brief Which side of a peer-link session the agent serves, which sets its
/// starting ICE role and waiting policy.
enum class Policy : std::uint8_t {
  /// @brief The session's host: starts controlling, nominates, and fails 30
  /// seconds after connect without a nomination.
  host,
  /// @brief A guest: starts controlled, never gives up on its own, and rearms
  /// a
  /// failed checklist after 5 seconds while it waits for the host.
  guest,
};

/// @brief When the agent advertises port guesses for its own mappings.
enum class PortGuessing : std::uint8_t {
  /// @brief Only when one base was seen at the same public address with
  /// different ports.
  automatic,
  /// @brief Whenever it has any server-reflexive candidate.
  forced,
};

/// @brief The kinds of candidate exchanged in peer-link blocks.
enum class CandidateType : std::uint8_t {
  /// @brief A local socket address.
  host,
  /// @brief A public address a STUN server saw for a local socket.
  server_reflexive,
};

/// @brief One ICE candidate as exported in a peer-link invitation or reply.
struct Candidate {
  /// @brief 1 to 32 characters of letters, digits, `+` or `/`; opaque to the
  /// other side.
  std::string foundation;
  /// @brief RFC 8445 priority, 1 to 2^31 - 1.
  std::uint32_t priority{};
  /// @brief The candidate's address and port.
  platform::SocketEndpoint endpoint;
  /// @brief Host or server reflexive.
  CandidateType type{CandidateType::host};
  /// @brief For a server-reflexive candidate, its base: a host candidate of
  /// the same family; unset for a host.
  platform::SocketEndpoint related;
};

/// @brief Why an ICE ufrag and password were refused.
enum class CredentialsError : std::uint8_t {
  /// @brief The ufrag is not 16 lower-case hex characters.
  invalid_ufrag,
  /// @brief The password is not 48 lower-case hex characters.
  invalid_password,
};

/// @brief An ICE ufrag and password, both lower-case hex of the right length.
///
/// Built only by make(), so every value is valid. Move-only; clone() copies
/// into new secrets.
class Credentials {
 public:
  /// @brief Checks and takes a ufrag and password.
  /// @param ufrag 16 lower-case hex characters.
  /// @param password 48 lower-case hex characters; the STUN integrity key.
  /// @return The credentials.
  /// @retval CredentialsError::invalid_ufrag The ufrag is not valid.
  /// @retval CredentialsError::invalid_password The password is not valid.
  [[nodiscard]] static auto make(SecretString ufrag, SecretString password)
      -> Expected<Credentials, CredentialsError>;

  /// @brief The ufrag, held in wiping memory.
  /// @return 16 lower-case hex characters.
  auto ufrag() const noexcept -> const SecretString& { return ufrag_; }
  /// @brief The password, held in wiping memory.
  /// @return 48 lower-case hex characters.
  auto password() const noexcept -> const SecretString& { return password_; }
  /// @brief Copies the credentials, each into a new secret.
  /// @return The copy.
  auto clone() const -> Credentials;

 private:
  /// @brief Takes credentials make() has checked.
  /// @param ufrag The ufrag.
  /// @param password The password.
  Credentials(SecretString ufrag, SecretString password) noexcept;

  /// @brief Whether text is lower-case hex of an exact length.
  /// @param text The text.
  /// @param size The length required.
  /// @return True when it is.
  static auto lower_hex(std::string_view text, std::size_t size) noexcept
      -> bool;

  /// @brief The ufrag.
  SecretString ufrag_;
  /// @brief The password.
  SecretString password_;
};

/// @brief Why a set of ICE candidates or parameters was refused.
enum class ParametersError : std::uint8_t {
  /// @brief There are no candidates.
  no_candidates,
  /// @brief There are more than max_candidates.
  too_many_candidates,
  /// @brief A foundation, priority, port or address is invalid.
  invalid_candidate,
  /// @brief Two candidates share type and endpoint.
  duplicate_candidate,
  /// @brief A server-reflexive candidate's base is invalid, of another family,
  /// or not one of the host candidates.
  unbased_reflexive,
  /// @brief More than max_port_guesses port guesses.
  too_many_port_guesses,
};

/// @brief One side's ICE parameters as carried in a peer-link block:
/// credentials, candidates and how many port guesses the other side should try.
///
/// Built only by make(), so every value may be used to connect. Move-only;
/// clone() copies the secrets into new ones.
class Parameters {
 public:
  /// @brief Checks and takes one side's parameters, against RCP WIRE
  /// section 3.
  /// @param credentials The side's ufrag and password.
  /// @param candidates 1 to 32 candidates.
  /// @param port_guesses Remote endpoints to guess around the reflexive ports;
  ///     0 to 32.
  /// @return The parameters.
  /// @retval ParametersError Why candidates() or the port guesses were
  ///     refused; see check().
  [[nodiscard]] static auto make(Credentials credentials,
                                 std::vector<Candidate> candidates,
                                 std::size_t port_guesses)
      -> Expected<Parameters, ParametersError>;
  /// @brief Checks candidates against RCP WIRE section 3.
  /// @param candidates The candidates.
  /// @return Nothing when they may be exported.
  /// @retval ParametersError::no_candidates There are none.
  /// @retval ParametersError::too_many_candidates There are more than 32.
  /// @retval ParametersError::invalid_candidate A foundation, priority, port
  ///     or address is invalid.
  /// @retval ParametersError::duplicate_candidate Two share type and endpoint.
  /// @retval ParametersError::unbased_reflexive A server-reflexive candidate's
  ///     base is invalid, of another family, or not one of the hosts.
  [[nodiscard]] static auto check(std::span<const Candidate> candidates) noexcept
      -> Expected<void, ParametersError>;

  /// @brief The side's ufrag and password.
  /// @return The credentials.
  auto credentials() const noexcept -> const Credentials& {
    return credentials_;
  }
  /// @brief The side's ufrag, held in wiping memory.
  /// @return 16 lower-case hex characters.
  auto ufrag() const noexcept -> const SecretString& {
    return credentials_.ufrag();
  }
  /// @brief The side's password, held in wiping memory.
  /// @return 48 lower-case hex characters.
  auto password() const noexcept -> const SecretString& {
    return credentials_.password();
  }
  /// @brief The side's candidates.
  /// @return 1 to 32 candidates.
  auto candidates() const noexcept -> std::span<const Candidate> {
    return candidates_;
  }
  /// @brief Remote endpoints to guess around the reflexive ports.
  /// @return 0 to 32.
  auto port_guesses() const noexcept -> std::size_t { return port_guesses_; }
  /// @brief Copies the parameters, the ufrag and password into new secrets.
  /// @return The copy.
  auto clone() const -> Parameters;

 private:
  /// @brief Takes parameters make() has checked.
  /// @param credentials The ufrag and password.
  /// @param candidates The candidates.
  /// @param port_guesses The port guesses.
  Parameters(Credentials credentials, std::vector<Candidate> candidates,
             std::size_t port_guesses) noexcept;

  /// @brief Whether text is a valid foundation.
  /// @param text The text.
  /// @return True for 1 to 32 letters, digits, `+` or `/`.
  static auto foundation(std::string_view text) noexcept -> bool;
  /// @brief Whether two endpoints have the same address and port.
  /// @param left One endpoint.
  /// @param right The other.
  /// @return True when equal.
  static auto same_endpoint(const platform::SocketEndpoint& left,
                            const platform::SocketEndpoint& right) noexcept
      -> bool;

  /// @brief The ufrag and password.
  Credentials credentials_;
  /// @brief The candidates.
  std::vector<Candidate> candidates_;
  /// @brief The port guesses.
  std::size_t port_guesses_;
};

}  // namespace libchiaking::net::nat::ice
