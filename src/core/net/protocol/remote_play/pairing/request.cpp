#include "request.hpp"

#include <algorithm>
#include <utility>

#include "libchiaking/detail/wire_sizes.hpp"
#include "src/core/net/protocol/remote_play/pairing/rp_version.hpp"
#include "src/core/net/protocol/remote_play/request_head.hpp"
#include "src/core/utils/bytes/byte_io.hpp"
#include "src/core/utils/text/ascii.hpp"
#include "src/core/utils/text/base64.hpp"
#include "src/core/utils/text/hex.hpp"

#include "cipher.hpp"
#include "limits.hpp"

namespace libchiaking::net::protocol::remote_play {
bool RegistrationRequestCodec::host_is_valid(std::string_view host) {
  return !host.empty() && host.size() <= max_host_length &&
         std::ranges::all_of(host, [](char c) {
           return text::hex_digit(c).has_value() || c == '.' || c == ':';
         });
}

std::string_view RegistrationRequestCodec::without_zone(std::string_view host) {
  const auto zone = host.find('%');
  if (zone == std::string_view::npos) return host;
  const auto valid_zone =
      std::ranges::all_of(host.substr(zone + 1), [](char c) {
        return text::ascii_alnum(c) || c == '_' || c == '-' || c == '.';
      });
  return valid_zone ? host.substr(0, zone) : std::string_view{};
}

auto RegistrationRequestCodec::request_path(RegistrationTarget target) noexcept
    -> std::optional<std::string_view> {
  switch (target) {
    case RegistrationTarget::ps4_8:
    case RegistrationTarget::ps4_9:
      return "/sce/rp/regist";
    case RegistrationTarget::ps4_unknown:
    case RegistrationTarget::ps4_10:
      return "/sie/ps4/rp/sess/rgst";
    case RegistrationTarget::ps5_1:
      return "/sie/ps5/rp/sess/rgst";
  }
  return std::nullopt;
}

bool RegistrationRequestCodec::valid_online(std::string_view online) {
  return online.find('\0') == std::string_view::npos;
}

bool RegistrationRequestCodec::legacy_target(RegistrationTarget target) {
  return target == RegistrationTarget::ps4_unknown ||
         target == RegistrationTarget::ps4_8 ||
         target == RegistrationTarget::ps4_9;
}

Expected<std::string, HeaderError> RegistrationRequestCodec::encode_header(
    RegistrationTarget target, std::size_t payload_size,
    std::string_view host) {
  if (payload_size == 0 || payload_size > max_payload_size)
    return unexpected(HeaderError::invalid_payload_size);
  host = without_zone(host);
  if (!host_is_valid(host)) return unexpected(HeaderError::invalid_host);
  const auto path = request_path(target);
  if (!path) return unexpected(HeaderError::invalid_target);

  RequestHead<max_header_size> head;
  if (!head.request_line("POST", *path) || !head.text(" HTTP/1.1\r\n") ||
      !head.header("HOST", host) ||
      !head.header("User-Agent", "remoteplay Windows") ||
      !head.header("Connection", "close") ||
      !head.header("Content-Length", payload_size))
    return unexpected(HeaderError::invalid_payload_size);
  if (head.size() >= payload_size)
    return unexpected(HeaderError::invalid_payload_size);
  if (const auto version = RpVersion::of(target);
      version && target != RegistrationTarget::ps4_8 &&
      !head.header("RP-Version", version->text()))
    return unexpected(HeaderError::invalid_payload_size);
  if (head.size() + 3 > payload_size || !head.end())
    return unexpected(HeaderError::invalid_payload_size);
  return std::string{head.view()};
}

Expected<SecretBytes, PayloadError> RegistrationRequestCodec::encode_payload(
    platform::CryptoProvider& provider, RegistrationTarget target,
    const RegistrationMaterial& material, std::span<const std::byte> account_id,
    std::optional<std::string_view> online_id) {
  if (!request_path(target)) return unexpected(PayloadError::invalid_argument);

  const bool online_path = legacy_target(target) && online_id.has_value();
  if (online_path) {
    constexpr std::size_t online_overhead =
        sizeof("Client-Type: Windows\r\nNp-Online-Id: \r\n") - 1;
    if (online_id->size() > inner_text_max - online_overhead ||
        !valid_online(*online_id))
      return unexpected(PayloadError::invalid_argument);
  } else if (account_id.size() != psn_account_id_size) {
    return unexpected(PayloadError::invalid_argument);
  }

  SecretArray<request_inner_size> inner;
  bytes::Writer writer{inner.span().first(inner_text_max)};
  if (online_path) {
    if (!writer.text("Client-Type: ") || !writer.text(client_type_legacy) ||
        !writer.text("\r\nNp-Online-Id: ") || !writer.text(*online_id) ||
        !writer.text("\r\n"))
      return unexpected(PayloadError::invalid_argument);
  } else {
    SecretArray<base64_account_size, char> b64;
    const auto b64_size = text::base64::encode_into(
        account_id, {b64.data(), b64.size()}, text::base64::Alphabet::standard);
    if (!b64_size) return unexpected(PayloadError::invalid_argument);
    if (!writer.text("Client-Type: ") ||
        !writer.text(legacy_target(target)
                         ? std::string_view(client_type_legacy)
                         : std::string_view(client_type_modern)) ||
        !writer.text("\r\nNp-AccountId: ") ||
        !writer.text(std::string_view{b64.data(), *b64_size}) ||
        !writer.text("\r\n"))
      return unexpected(PayloadError::invalid_argument);
  }
  const auto inner_size = writer.offset();

  auto payload = SecretBytes::zeroed(request_prefix_size + inner_size);
  const auto prefix = payload.span().first(request_prefix_size);
  std::ranges::fill(prefix, std::byte{'A'});
  const auto aeropause = material.aeropause.span();
  if (legacy_target(target)) {
    std::ranges::copy(aeropause, prefix.begin() + legacy_aeropause_offset);
  } else {
    std::ranges::copy(aeropause.last(aeropause_half_size),
                      prefix.begin() + modern_aeropause_high_offset);
    std::ranges::copy(aeropause.first(aeropause_half_size),
                      prefix.begin() + modern_aeropause_low_offset);
  }

  auto transformed = RegistrationCipher::transform(
      provider, target, material.bright.span(), material.ambassador.span(), 0,
      CipherDirection::encrypt, inner.span().first(inner_size),
      payload.span().subspan(request_prefix_size));
  if (!transformed) return unexpected(PayloadError::provider_failure);

  return payload;
}

}  // namespace libchiaking::net::protocol::remote_play
