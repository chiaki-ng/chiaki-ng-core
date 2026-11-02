#include "response.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <ranges>
#include <string_view>
#include <utility>

#include <libchiaking/detail/secret_string.hpp>
#include "limits.hpp"
#include "request.hpp"
#include "src/core/net/protocol/remote_play/console_reply.hpp"
#include "src/core/net/protocol/remote_play/pairing/rp_version.hpp"
#include "src/core/net/transport/http/status.hpp"
#include "src/core/utils/text/ascii.hpp"
#include "src/core/utils/text/hex.hpp"
#include "src/core/utils/text/integer.hpp"

namespace libchiaking::net::protocol::remote_play {
template <class Buffer>
auto RegistrationResponseCodec::parse_exact_hex(Buffer& out,
                                                std::string_view hex) noexcept
    -> bool {
  const auto bytes = std::as_writable_bytes(std::span{out});
  if (text::decode_hex_into(hex, bytes) == bytes.size()) return true;
  std::ranges::fill(bytes, std::byte{0});
  return false;
}

template <std::size_t Size>
auto RegistrationResponseCodec::secret_field(std::string_view value) noexcept
    -> Expected<SecretArray<Size, char>, ResponsePayloadError> {
  if (value.size() >= Size)
    return unexpected(ResponsePayloadError::invalid_value);
  SecretArray<Size, char> field;
  std::ranges::copy(value, field.begin());
  return field;
}

template <std::size_t Size>
auto RegistrationResponseCodec::text_field(std::string_view value) noexcept
    -> std::optional<std::array<char, Size>> {
  if (value.size() >= Size) return std::nullopt;
  std::array<char, Size> field{};
  std::ranges::copy(value, field.begin());
  return field;
}

Expected<ResponseHeader, ResponseHeaderError>
RegistrationResponseCodec::parse_header(
    std::span<const std::byte> available) noexcept {
  const auto size = available.size();
  if (size > max_response_size)
    return unexpected(ResponseHeaderError::too_large);
  const std::string_view text{reinterpret_cast<const char*>(available.data()),
                              size};
  const auto reply = ConsoleReply::response(
      text, max_response_size,
      ConsoleReply::Faults<ResponseHeaderError>{
          ResponseHeaderError::too_large, ResponseHeaderError::invalid,
          ResponseHeaderError::incomplete});
  if (!reply) return unexpected(reply.error());
  const auto header_size = reply->length();

  std::optional<std::string_view> content_length;
  std::optional<std::uint32_t> reason;
  for (const auto& [key, value] : reply->headers()) {
    if (key == "Content-Length") {
      if (!content_length) content_length = value;
    } else if (key == "RP-Application-Reason") {
      reason = text::parse_integer<std::uint32_t>(text::trim(value), 16);
    }
  }

  const auto status = reply->status();
  std::uint64_t body = 0;
  if (net::HttpStatus{status} == net::HttpStatus::ok) {
    const auto declared =
        content_length
            ? text::parse_integer<std::uint64_t>(text::trim(*content_length))
                  .value_or(0)
            : 0;
    if (declared == 0) return unexpected(ResponseHeaderError::invalid);
    if (declared > max_response_size - header_size)
      return unexpected(ResponseHeaderError::too_large);
    body = declared;
  }

  return ResponseHeader{status, reason, header_size,
                        static_cast<std::size_t>(body)};
}

auto RegistrationResponseCodec::complete_lines(
    std::span<const std::byte> payload) noexcept -> std::span<const std::byte> {
  const std::string_view text{reinterpret_cast<const char*>(payload.data()),
                              payload.size()};
  const auto last_newline = text.substr(0, text.find('\0')).rfind('\n');
  return payload.first(
      last_newline == std::string_view::npos ? 0 : last_newline + 1);
}

Expected<RegisteredConsole, ResponsePayloadError>
RegistrationResponseCodec::parse_payload(
    RegistrationTarget target, std::span<const std::byte> payload) noexcept {
  using Ret = Expected<RegisteredConsole, ResponsePayloadError>;
  switch (target) {
    case RegistrationTarget::ps4_unknown:
    case RegistrationTarget::ps4_8:
    case RegistrationTarget::ps4_9:
    case RegistrationTarget::ps4_10:
    case RegistrationTarget::ps5_1:
      break;
    default:
      return unexpected(ResponsePayloadError::invalid_target);
  }

  if (payload.size() > max_response_size)
    return unexpected(ResponsePayloadError::oversize);

  SecretBytes framed;
  if (!framed.try_assign(complete_lines(payload)) ||
      !framed.try_append(std::as_bytes(std::span{payload_terminator})))
    return unexpected(ResponsePayloadError::oversize);
  const auto reply = ConsoleReply::fields(framed.view());
  if (!reply) return unexpected(ResponsePayloadError::malformed);

  RegisteredConsole console;
  console.target = target;
  const bool ps5 =
      RpVersion::of(target).transform(&RpVersion::ps5).value_or(false);
  const std::string_view mac_key = ps5 ? "PS5-Mac" : "PS4-Mac";
  const std::string_view regist_key = ps5 ? "PS5-RegistKey" : "PS4-RegistKey";
  const std::string_view nick_key = ps5 ? "PS5-Nickname" : "PS4-Nickname";
  bool mac_found = false, regist_found = false, key_found = false;

  for (const auto& h : reply->headers() | std::views::reverse) {
    if (h.name == "AP-Ssid") {
      if (auto field = text_field<console.ap_ssid.size()>(h.value))
        console.ap_ssid = *field;
    } else if (h.name == "AP-Bssid") {
      if (auto field = text_field<console.ap_bssid.size()>(h.value))
        console.ap_bssid = *field;
    } else if (h.name == "AP-Key") {
      auto field = secret_field<console.ap_key.size()>(h.value);
      if (!field) return unexpected(field.error());
      console.ap_key = std::move(field).value();
    } else if (h.name == "AP-Name") {
      if (auto field = text_field<console.ap_name.size()>(h.value))
        console.ap_name = *field;
    } else if (h.name == nick_key) {
      if (auto field = text_field<console.nickname.size()>(h.value))
        console.nickname = *field;
    } else if (h.name == regist_key) {
      console.registration_key.fill(std::byte{0});
      regist_found =
          text::decode_hex_into(h.value, console.registration_key).value_or(0) >
              0 &&
          console.registration_key[0] != std::byte{0};
      if (!regist_found) console.registration_key.fill(std::byte{0});
    } else if (h.name == "RP-KeyType")
      console.key_type =
          text::parse_integer<std::uint32_t>(text::trim(h.value)).value_or(0);
    else if (h.name == "RP-Key")
      key_found = parse_exact_hex(console.key, h.value) || key_found;
    else if (h.name == mac_key)
      mac_found = parse_exact_hex(console.mac, h.value) || mac_found;
  }

  if (!regist_found || !key_found || !mac_found)
    return unexpected(ResponsePayloadError::missing_field);
  return Ret(std::move(console));
}

auto RegistrationResponseCodec::scan_status(
    std::span<const std::byte> available) noexcept -> ResponseStatus {
  ResponseStatus result{};
  std::string_view text{reinterpret_cast<const char*>(available.data()),
                        available.size()};
  if (const auto nul = text.find('\0'); nul != std::string_view::npos)
    text = text.substr(0, nul);
  if (const auto end = text.find("\r\n\r\n"); end != std::string_view::npos)
    text = text.substr(0, end + 2);
  if (text.substr(0, net::http_version_prefix.size()) !=
      net::http_version_prefix)
    return result;
  text.remove_prefix(net::http_version_prefix.size());
  const auto line_end = text.find('\n');
  if (line_end == std::string_view::npos) return result;
  const auto status_line = text.substr(0, line_end);
  const auto code = text::parse_integer<std::uint16_t>(
      status_line.substr(0, status_line.find_first_of(" \r")));
  if (code && *code > 0 && *code <= net::HttpStatus::largest_code)
    result.status = *code;
  text.remove_prefix(line_end + 1);
  while (!text.empty()) {
    std::size_t end = 0;
    while (end < text.size() && text[end] != '\r' && text[end] != '\n') ++end;
    const auto line = text.substr(0, end);
    const bool terminated = end < text.size();
    text.remove_prefix(terminated ? end + 1 : text.size());
    if (!terminated) break;
    const auto colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) continue;
    if (line.substr(0, colon) != "RP-Application-Reason") continue;
    auto value = line.substr(colon + 1);
    if (!value.empty() && value.front() == ' ') value.remove_prefix(1);
    result.application_reason =
        text::parse_integer<std::uint32_t>(text::trim(value), 16).value_or(0);
  }
  return result;
}

}  // namespace libchiaking::net::protocol::remote_play
