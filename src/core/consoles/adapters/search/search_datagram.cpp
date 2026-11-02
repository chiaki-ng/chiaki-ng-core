#include "src/core/consoles/adapters/search/search_datagram.hpp"

#include "src/core/net/protocol/remote_play/console_reply.hpp"
#include "src/core/net/transport/http/status.hpp"
#include "src/core/utils/text/ascii.hpp"
#include "src/core/utils/text/integer.hpp"

#include <algorithm>
#include <utility>

namespace libchiaking::consoles {

auto SearchDatagram::valid_request_port(std::string_view value) noexcept
    -> bool {
  const auto port = text::parse_integer<std::uint16_t>(text::trim(value));
  return port && *port != 0;
}

auto SearchDatagram::header_value(std::string_view text) noexcept
    -> std::string_view {
  const auto start = std::ranges::find_if_not(text, text::http_space);
  return text.substr(static_cast<std::size_t>(start - text.begin()));
}

auto SearchDatagram::usable_source(platform::IpAddress source) noexcept
    -> bool {
  constexpr std::uint32_t first_octet_shift = 24;
  constexpr std::uint32_t first_multicast_octet = 224;
  const auto first_octet = source.ipv4_number() >> first_octet_shift;
  return first_octet != 0 && first_octet < first_multicast_octet;
}

auto SearchDatagram::refusal(std::span<const std::byte> datagram,
                             platform::IpAddress source) noexcept
    -> std::optional<Refusal> {
  if (datagram.size() > reply_limit) return Refusal::oversized;
  if (!source.is_ipv4()) return Refusal::not_ipv4;
  if (!usable_source(source)) return Refusal::unusable_source;
  std::string_view rest{reinterpret_cast<const char*>(datagram.data()),
                        datagram.size()};
  if (rest.find('\0') != std::string_view::npos) return Refusal::embedded_nul;
  std::array<bool, limited_headers.size()> seen{};
  const auto status_end = rest.find('\n');
  rest = status_end == std::string_view::npos ? std::string_view{}
                                              : rest.substr(status_end + 1);
  while (!rest.empty()) {
    const auto line_end = rest.find('\n');
    auto line = rest.substr(0, line_end);
    rest = line_end == std::string_view::npos ? std::string_view{}
                                              : rest.substr(line_end + 1);
    if (line.ends_with('\r')) line.remove_suffix(1);
    if (line.empty()) continue;
    const auto colon = line.find(':');
    if (colon == std::string_view::npos) return Refusal::line_without_colon;
    const auto key = line.substr(0, colon);
    const auto value = header_value(line.substr(colon + 1));
    const auto header = std::ranges::find_if(limited_headers,
                                             [key](const LimitedHeader& known) {
                                               return text::ieq(known.key, key);
                                             });
    if (header == limited_headers.end()) continue;
    if (std::exchange(
            seen[static_cast<std::size_t>(header - limited_headers.begin())],
            true))
      return Refusal::duplicate_header;
    if (header->key == request_port_key) {
      if (!valid_request_port(value)) return Refusal::invalid_request_port;
    } else if (value.size() > header->limit) {
      return Refusal::field_too_long;
    }
  }
  return std::nullopt;
}

auto SearchDatagram::parse(std::span<const std::byte> datagram,
                           platform::IpAddress source)
    -> std::optional<Fields> {
  const std::string_view text{reinterpret_cast<const char*>(datagram.data()),
                              datagram.size()};
  std::string framed{text.substr(0, text.find('\0'))};
  framed.erase(framed.find_last_of(reply_terminator) + 1);
  framed.push_back(reply_terminator);
  const auto reply = net::protocol::remote_play::ConsoleReply::response(framed);
  if (!reply || reply->headers().empty()) return std::nullopt;
  const auto code = reply->code();

  Fields fields;
  fields.address = source;
  if (code == net::HttpStatus::ok.code())
    fields.status = Status::ready;
  else if (code == standby_status)
    fields.status = Status::standby;

  std::array<bool, 8> seen{};
  const auto assign = [&seen](std::size_t slot, std::string& field,
                              std::string_view value) {
    if (!std::exchange(seen[slot], true)) field = value;
  };
  for (const auto& [key, value] : reply->headers()) {
    if (key == "system-version")
      assign(0, fields.system_version, value);
    else if (key == "device-discovery-protocol-version")
      assign(1, fields.protocol_version, value);
    else if (key == request_port_key) {
      if (!std::exchange(seen[2], true))
        fields.request_port =
            text::parse_integer<std::uint16_t>(text::trim(value)).value_or(0);
    } else if (key == "host-name")
      assign(3, fields.name, value);
    else if (key == "host-type")
      assign(4, fields.type, value);
    else if (key == "host-id")
      assign(5, fields.id, value);
    else if (key == "running-app-titleid")
      assign(6, fields.running_app_title_id, value);
    else if (key == "running-app-name")
      assign(7, fields.running_app_name, value);
  }
  return fields;
}

auto SearchDatagram::from(const Fields& fields) -> std::optional<SearchReply> {
  const bool ps5 = fields.type == ps5_type;
  if ((!ps5 && fields.type != ps4_type) || fields.id.size() != id_length ||
      fields.name.size() > console_name_limit ||
      !ConsoleName::printable(fields.name) ||
      fields.system_version.size() > console_system_version_limit ||
      !ConsoleName::printable(fields.system_version))
    return std::nullopt;
  const auto hardware_id = HardwareId::parse(fields.id);
  if (!hardware_id) return std::nullopt;
  std::optional<RunningTitle> running_title;
  if (!fields.running_app_title_id.empty() &&
      ConsoleName::printable(fields.running_app_title_id) &&
      ConsoleName::printable(fields.running_app_name))
    running_title =
        RunningTitle{fields.running_app_title_id, fields.running_app_name};
  return SearchReply::create(
      *hardware_id,
      Firmware::from_system_version(ps5 ? Generation::ps5 : Generation::ps4,
                                    text::trim(fields.system_version)),
      ConsoleName::create(fields.name, ConsoleName::Source::discovered),
      fields.address, fields.status, fields.request_port, fields.system_version,
      std::move(running_title));
}

auto SearchDatagram::admit(std::span<const std::byte> datagram,
                           platform::IpAddress source)
    -> Expected<SearchReply, Refusal> {
  if (const auto refused = refusal(datagram, source))
    return unexpected(*refused);
  const auto fields = parse(datagram, source);
  if (!fields) return unexpected(Refusal::malformed);
  auto reply = from(*fields);
  if (!reply) return unexpected(Refusal::not_a_console);
  return std::move(*reply);
}

}  // namespace libchiaking::consoles
