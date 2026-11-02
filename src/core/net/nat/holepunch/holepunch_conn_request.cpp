#include "src/core/net/nat/holepunch/holepunch_conn_request.hpp"
#include "src/core/utils/text/integer.hpp"

#include <algorithm>

namespace libchiaking::net::nat::holepunch {

auto ConnRequestCodec::decode_ids(const ConnRequestWire& conn_request)
    -> std::optional<ConnRequestIds> {
  const auto sid_value =
      MessageSchemaCodec::wrapped<std::uint32_t>(conn_request.sid);
  const auto peer_sid_value =
      MessageSchemaCodec::wrapped<std::uint32_t>(conn_request.peer_sid);
  if (!sid_value || !peer_sid_value) return std::nullopt;
  return ConnRequestIds{*sid_value, *peer_sid_value};
}

auto ConnRequestCodec::decode_nat(const ConnRequestWire& conn_request)
    -> std::optional<std::uint8_t> {
  return MessageSchemaCodec::wrapped<std::uint8_t>(conn_request.nat_type);
}

auto ConnRequestCodec::decode_mac(const ConnRequestWire& conn_request)
    -> std::optional<std::array<std::byte, mac_address_size>> {
  if (!conn_request.default_route_mac_addr) return std::nullopt;
  auto value =
      std::optional<std::string_view>{*conn_request.default_route_mac_addr};
  *value = value->substr(0, value->find('\0'));
  std::array<std::byte, mac_address_size> mac{};
  if (value->size() != mac_address_text_length) return mac;

  for (std::size_t index = 0; index < mac.size(); ++index) {
    const auto octet =
        text::parse_integer<std::uint8_t>(value->substr(index * 3, 2), 16);
    if (!octet || (index + 1 < mac.size() && (*value)[index * 3 + 2] != ':'))
      return std::nullopt;
    mac[index] = static_cast<std::byte>(*octet);
  }
  return mac;
}

auto ConnRequestCodec::decode_skey(const ConnRequestWire& conn_request)
    -> std::optional<SecretArray<psn_custom_data_size>> {
  const auto& value = conn_request.skey;
  if (!value) return std::nullopt;
  SecretArray<psn_custom_data_size> skey{};
  if (!text::base64::decode_into(value->view(),
                                 std::span<std::byte>(skey.data(), skey.size()),
                                 text::base64::Alphabet::standard))
    return std::nullopt;
  return skey;
}

auto ConnRequestCodec::decode_hashed_id(const ConnRequestWire& conn_request)
    -> std::optional<SecretArray<holepunch_hashed_id_size>> {
  const auto& value = conn_request.local_hashed_id;
  if (!value) return std::nullopt;
  SecretArray<holepunch_hashed_id_size> hashed_id{};
  if (!value->empty() &&
      !text::base64::decode_into(
          *value, std::span<std::byte>(hashed_id.data(), hashed_id.size()),
          text::base64::Alphabet::standard))
    return std::nullopt;
  return hashed_id;
}

auto ConnRequestCodec::decode(const ConnRequestWire& conn_request)
    -> std::optional<ConnRequest> {
  const auto ids = decode_ids(conn_request);
  const auto nat_type = decode_nat(conn_request);
  const auto mac = decode_mac(conn_request);
  auto skey = decode_skey(conn_request);
  auto hashed_id = decode_hashed_id(conn_request);
  auto candidates = CandidateCodec::decode_list(conn_request.candidate);
  if (!ids || !nat_type || !mac || !skey || !hashed_id || !candidates)
    return std::nullopt;
  return ConnRequest{ids->sid,
                     ids->peer_sid,
                     *nat_type,
                     *mac,
                     std::move(*skey),
                     std::move(*hashed_id),
                     std::move(*candidates)};
}

auto ConnRequestCodec::encode(const ConnRequest& request,
                              std::int64_t account_id)
    -> Expected<ConnRequestEntry, ConnRequestEncodeError> {
  auto candidates = CandidateCodec::encode_list(request.candidates);
  if (!candidates) {
    return unexpected(candidates.error() == CandidateListEncodeError::too_large
                          ? ConnRequestEncodeError::too_large
                          : ConnRequestEncodeError::invalid_candidate);
  }
  const bool anonymous =
      std::ranges::all_of(request.local_hashed_id,
                          [](std::byte byte) { return byte == std::byte{}; });
  ConnRequestEntry document{
      .sid = request.sid,
      .peer_sid = request.peer_sid,
      .skey = text::base64::encode<SecretString>(
          {request.skey.data(), psn_custom_data_size},
          text::base64::Alphabet::standard),
      .nat_type = request.nat_type,
      .candidate = std::move(candidates.value()),
      .default_route_mac_addr = {},
      .local_peer_addr = {std::to_string(account_id), "REMOTE_PLAY"},
      .local_hashed_id = anonymous ? SecretString{}
                                   : text::base64::encode<SecretString>(
                                         {request.local_hashed_id.data(),
                                          holepunch_hashed_id_size},
                                         text::base64::Alphabet::standard)};
  const auto written =
      json::ps::write<json::ps::body_options_with_nulls>(document);
  if (!written || written.value().size() > max_json_bytes)
    return unexpected(ConnRequestEncodeError::too_large);
  return document;
}

}  // namespace libchiaking::net::nat::holepunch
