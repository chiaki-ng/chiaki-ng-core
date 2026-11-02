#include "session_codec.hpp"

#include <algorithm>
#include <string_view>

#include "libchiaking/detail/wire_sizes.hpp"
#include "src/core/net/protocol/remote_play/rp_reason.hpp"

#include "limits.hpp"
#include "src/core/net/protocol/remote_play/console_reply.hpp"
#include "src/core/net/transport/http/status.hpp"
#include "src/core/utils/text/ascii.hpp"
#include "src/core/utils/text/base64.hpp"
#include "src/core/utils/text/integer.hpp"

namespace libchiaking::net::protocol::remote_play {
struct SessionCodec::TargetFacts {
  std::string_view path;
  std::string_view version;
};

auto SessionCodec::facts(RegistrationTarget target)
    -> Expected<TargetFacts, RequestCodecError> {
  const auto version = rp_version_string(target);
  if (!version) return unexpected(RequestCodecError::invalid_target);
  switch (target) {
    case RegistrationTarget::ps4_8:
    case RegistrationTarget::ps4_9:
      return TargetFacts{rp_session_path_ps4_legacy, *version};
    case RegistrationTarget::ps4_10:
      return TargetFacts{rp_session_path_ps4, *version};
    case RegistrationTarget::ps5_1:
      return TargetFacts{rp_session_path_ps5, *version};
    default:
      return unexpected(RequestCodecError::invalid_target);
  }
}

std::optional<RegistrationTarget> SessionCodec::retry_target(
    RegistrationTarget requested, std::string_view version) noexcept {
  const auto requested_version = RpVersion::of(requested);
  const auto family =
      requested_version ? requested_version->family() : RpVersion::Family::ps4;
  if (const auto parsed = RpVersion::parse(version, family))
    return parsed->target();
  if (version == rp_version_legacy_ps4_9_alias)
    return RegistrationTarget::ps4_9;
  return std::nullopt;
}

auto SessionCodec::decode_nonce(std::string_view text) noexcept
    -> std::optional<SecretArray<rp_nonce_size>> {
  SecretArray<rp_nonce_size> output;
  const auto length = text::base64::decode_into(
      text, {output.data(), output.size()}, text::base64::Alphabet::standard);
  if (!length || length.value() != output.size()) return std::nullopt;
  return output;
}

auto SessionCodec::encode_request(RegistrationTarget target,
                                  std::string_view host, std::uint16_t port,
                                  std::span<const std::byte> registration_key)
    -> Expected<SessionRequest, RequestCodecError> {
  const auto target_facts = facts(target);
  if (!target_facts) return unexpected(target_facts.error());
  if (!SessionRequest::Head::accepts_host(host))
    return unexpected(RequestCodecError::invalid_host);
  if (port == 0) return unexpected(RequestCodecError::invalid_port);
  if (registration_key.size() != rp_registration_key_size)
    return unexpected(RequestCodecError::invalid_key);

  SessionRequest result;
  auto& head = result.head_;
  const auto key_text = registration_key.first(static_cast<std::size_t>(
      std::ranges::find(registration_key, std::byte{0}) -
      registration_key.begin()));
  if (!head.request_line("GET", target_facts.value().path) ||
      !head.host_header(host, port) ||
      !head.header("User-Agent", "remoteplay Windows") ||
      !head.header("Connection", "close") ||
      !head.header("Content-Length", "0") ||
      !head.hex_header("RP-Registkey", key_text) ||
      !head.header("Rp-Version", target_facts.value().version) || !head.end())
    return unexpected(RequestCodecError::overflow);
  return result;
}

auto SessionCodec::parse_response(RegistrationTarget requested,
                                  std::span<const std::byte> received) noexcept
    -> Expected<SessionResponse, ResponseCodecError> {
  const auto requested_version = rp_version_string(requested);
  if (!requested_version) return unexpected(ResponseCodecError::invalid_target);
  const std::string_view input{reinterpret_cast<const char*>(received.data()),
                               received.size()};
  const auto reply = ConsoleReply::response(
      input, rp_max_header_bytes,
      ConsoleReply::Faults<ResponseCodecError>{ResponseCodecError::too_large,
                                               ResponseCodecError::invalid,
                                               ResponseCodecError::incomplete});
  if (!reply) return unexpected(reply.error());
  const auto header_size = reply->length();

  std::optional<std::string_view> nonce;
  std::optional<std::string_view> version;
  std::optional<std::uint32_t> reason;
  for (const auto& [key, value] : reply->headers()) {
    if (key == "RP-Nonce") {
      if (!nonce) nonce = value;
    } else if (text::ieq(key, "RP-Version")) {
      if (!version) version = value;
    } else if (key == "RP-Application-Reason") {
      if (!reason)
        reason = text::parse_integer<std::uint32_t>(text::trim(value), 16);
    }
  }
  const auto status =
      static_cast<std::int32_t>(static_cast<std::uint32_t>(reply->code()));

  const auto code = reason.value_or(0);
  if (status == net::HttpStatus::ok.code() && nonce) {
    auto decoded = decode_nonce(*nonce);
    if (!decoded) return unexpected(ResponseCodecError::invalid_nonce);
    return SessionResponse{SessionGrant{std::move(*decoded), header_size}};
  }
  if ((code == rp_reason_rp_version || code == rp_reason_unknown) && version &&
      *version != *requested_version)
    return SessionResponse{unexpected(SessionRefusal::version_mismatch(
        status, code, retry_target(requested, *version)))};
  if (code == rp_reason_rp_version)
    return SessionResponse{unexpected(
        SessionRefusal::version_mismatch(status, code, std::nullopt))};
  return SessionResponse{unexpected(SessionRefusal::rejected(status, code))};
}

}  // namespace libchiaking::net::protocol::remote_play
