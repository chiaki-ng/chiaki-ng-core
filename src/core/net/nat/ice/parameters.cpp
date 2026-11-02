#include "src/core/net/nat/ice/parameters.hpp"

#include "src/core/net/nat/ice/candidate_policy.hpp"
#include "src/core/utils/text/ascii.hpp"
#include "src/core/utils/text/hex.hpp"

#include <algorithm>
#include <utility>

namespace libchiaking::net::nat::ice {

Credentials::Credentials(SecretString ufrag, SecretString password) noexcept
    : ufrag_(std::move(ufrag)), password_(std::move(password)) {}

auto Credentials::make(SecretString ufrag, SecretString password)
    -> Expected<Credentials, CredentialsError> {
  if (!lower_hex(ufrag.view(), ufrag_text_size))
    return unexpected(CredentialsError::invalid_ufrag);
  if (!lower_hex(password.view(), password_text_size))
    return unexpected(CredentialsError::invalid_password);
  return Credentials{std::move(ufrag), std::move(password)};
}

auto Credentials::lower_hex(std::string_view text, std::size_t size) noexcept
    -> bool {
  if (text.size() != size) return false;
  return std::ranges::all_of(text, text::lower_hex_digit);
}

auto Credentials::clone() const -> Credentials {
  return Credentials{ufrag_.clone(), password_.clone()};
}

Parameters::Parameters(Credentials credentials,
                       std::vector<Candidate> candidates,
                       std::size_t port_guesses) noexcept
    : credentials_(std::move(credentials)),
      candidates_(std::move(candidates)),
      port_guesses_(port_guesses) {}

auto Parameters::foundation(std::string_view text) noexcept -> bool {
  if (text.empty() || text.size() > max_foundation_size) return false;
  return std::ranges::all_of(text, [](char symbol) {
    return text::ascii_alnum(symbol) || symbol == '+' || symbol == '/';
  });
}

auto Parameters::same_endpoint(const platform::SocketEndpoint& left,
                               const platform::SocketEndpoint& right) noexcept
    -> bool {
  return left.address == right.address && left.port == right.port;
}

auto Parameters::clone() const -> Parameters {
  return Parameters{credentials_.clone(), candidates_, port_guesses_};
}

auto Parameters::make(Credentials credentials,
                      std::vector<Candidate> candidates,
                      std::size_t port_guesses)
    -> Expected<Parameters, ParametersError> {
  if (const auto checked = check(candidates); !checked)
    return unexpected(checked.error());
  if (port_guesses > max_port_guesses)
    return unexpected(ParametersError::too_many_port_guesses);
  return Parameters{std::move(credentials), std::move(candidates),
                    port_guesses};
}

auto Parameters::check(std::span<const Candidate> candidates) noexcept
    -> Expected<void, ParametersError> {
  if (candidates.empty()) return unexpected(ParametersError::no_candidates);
  if (candidates.size() > max_candidates)
    return unexpected(ParametersError::too_many_candidates);
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    const auto& candidate = candidates[index];
    if (!foundation(candidate.foundation) || candidate.priority < 1 ||
        candidate.priority > max_candidate_priority ||
        candidate.endpoint.port == 0 ||
        !CandidatePolicy::permits(candidate.endpoint.address))
      return unexpected(ParametersError::invalid_candidate);
    for (std::size_t other = 0; other < index; ++other) {
      if (candidates[other].type == candidate.type &&
          same_endpoint(candidates[other].endpoint, candidate.endpoint))
        return unexpected(ParametersError::duplicate_candidate);
    }
    if (candidate.type != CandidateType::server_reflexive) continue;
    if (candidate.related.port == 0 ||
        !CandidatePolicy::permits(candidate.related.address) ||
        candidate.related.address.family() !=
            candidate.endpoint.address.family())
      return unexpected(ParametersError::unbased_reflexive);
    const auto based = std::any_of(
        candidates.begin(), candidates.end(), [&](const Candidate& host) {
          return host.type == CandidateType::host &&
                 same_endpoint(host.endpoint, candidate.related);
        });
    if (!based) return unexpected(ParametersError::unbased_reflexive);
  }
  return {};
}

}  // namespace libchiaking::net::nat::ice
