#include "src/core/psn/gk/client.hpp"

#include "src/core/net/transport/http/endpoint.hpp"
#include "src/core/net/transport/http/request.hpp"
#include "src/core/net/transport/http/response.hpp"
#include "src/core/net/transport/http/status.hpp"
#include "src/core/net/transport/http/url.hpp"
#include "src/core/psn/user_agents.hpp"
#include "src/core/utils/json/json.hpp"
#include "src/core/utils/json/ps_reply.hpp"
#include "src/core/utils/json/wire_dialect.hpp"
#include "src/core/utils/text/ascii.hpp"
#include "src/core/utils/text/utf8.hpp"

#include <charconv>
#include <string>
#include <system_error>
#include <utility>

namespace libchiaking::psn::gk {

namespace ps = json::ps;

struct Client::SpecificationHeader {
  using keys = ps::CamelCaseKeys;
  ps::Tolerant<glz::raw_json> http_user_agent;
};

struct Client::SpecificationBody {
  using keys = ps::CamelCaseKeys;
  glz::raw_json_view request_game_specification;
};

struct Client::SelectBody {
  using keys = ps::CamelCaseKeys;
  glz::raw_json_view request_game_specification;
  glz::raw_json_view ping_results;
};

struct Client::AllocateBody {
  struct Network {
    using keys = ps::CamelCaseKeys;
    int bw_kbps_sent{};
    double bw_loss{};
    int mtu{};
    int rtt{};
    int port{};
    int bw_kbps_received{};
    int bw_loss_upstream{};
    int mtu_upstream{};
  };
  using keys = ps::CamelCaseKeys;
  glz::raw_json_view request_game_specification;
  std::string_view data_center;
  Network network;
  double state_execution_time{};
  double stream_test_time{};
};

struct Client::ConfigBody {
  using keys = ps::CamelCaseKeys;
  std::string_view product;
  std::string_view platform;
  std::string_view session_id;
};

struct Client::EventHeader {
  std::string name;
};

struct Client::ClientIdsReply {
  using keys = ps::CamelCaseKeys;
  std::string gk_client_id;
  ps::Tolerant<std::string> ps3_gk_client_id;
  ps::Tolerant<std::string> stream_server_client_id;
};

struct Client::ConfigReply {
  using keys = ps::CamelCaseKeys;
  SecretString config_key;
};

struct Client::StartReply {
  using keys = ps::CamelCaseKeys;
  SecretString session_id;
};

struct Client::LockWire {
  using keys = ps::CamelCaseKeys;
  std::optional<bool> lock_acquired;
  std::optional<ps::LenientNumber<int>> poll_frequency;
};

struct Client::DatacenterRow {
  using keys = ps::CamelCaseKeys;
  std::string data_center;
  std::string public_ip;
  int port{};
  ps::LenientNumber<int> max_bandwidth;
};

struct Client::SelectReply {
  struct Network {
    std::optional<int> port;
  };
  std::optional<int> port;
  std::optional<Network> network;
};

struct Client::AllocateReply {
  struct LaunchSlot {
    using keys = ps::CamelCaseKeys;
    std::string public_ip;
    std::int32_t port{};
    std::optional<std::string> private_ip;
  };
  using keys = ps::CamelCaseKeys;
  std::optional<bool> queued;
  std::optional<bool> data_migration;
  std::optional<ps::LenientNumber<std::int32_t>> wait_time_estimate;
  std::optional<ps::LenientNumber<std::int32_t>> poll_frequency;
  std::optional<std::int32_t> data_migration_percentage_complete;
  std::optional<std::int32_t> display_queue_position;
  std::optional<std::int32_t> queue_position;
  std::optional<LaunchSlot> launch_slot;
  SecretString handshake_key;
  SecretString launch_specification;
  SecretString session_id;
};

auto Client::valid_header_value(std::string_view value) noexcept -> bool {
  return !value.empty() && value.find_first_of(std::string_view{"\0\r\n", 3}) ==
                               std::string_view::npos;
}

auto Client::spec_user_agent(const SpecificationHeader& spec)
    -> std::optional<std::string_view> {
  if (!spec.http_user_agent) return psn::cloud_stream_http_user_agent;
  const auto agent = ps::read_reply<std::string>(spec.http_user_agent->str);
  if (!agent) return std::nullopt;
  if (agent.value() == psn::cloud_stream_http_user_agent)
    return psn::cloud_stream_http_user_agent;
  if (agent.value() == psn::legacy_cloud_stream_http_user_agent)
    return psn::legacy_cloud_stream_http_user_agent;
  return std::nullopt;
}

auto Client::specification(Stage stage, std::string_view spec)
    -> Expected<Specification, Error> {
  if (spec.size() >= Client::body_limit)
    return unexpected(Error{stage, Reason::body_too_large});
  const auto header = json::read<SpecificationHeader>(spec);
  if (!header) return unexpected(Error{stage, Reason::invalid_spec});
  const auto user_agent = spec_user_agent(header.value());
  if (!user_agent) return unexpected(Error{stage, Reason::invalid_spec});
  const auto canonical =
      json::WireDialect::canonical_text(text::c_string(spec));
  if (!canonical) return unexpected(Error{stage, Reason::invalid_spec});
  SecretString text;
  text::utf8::append_repaired(text, canonical.value().view());
  return Specification{std::move(text), *user_agent};
}

auto Client::body_of(Stage stage, const auto& body)
    -> Expected<SecretString, Error> {
  auto text = ps::write(body);
  if (!text || text.value().size() > Client::body_limit)
    return unexpected(Error{stage, Reason::body_too_large});
  return std::move(text.value());
}

auto Client::keys_error(Stage stage, const SessionId& id, const ConfigKey& key)
    -> std::optional<Error> {
  if (!valid_header_value(id.view()))
    return Error{stage, Reason::invalid_session_id};
  if (!valid_header_value(key.view()))
    return Error{stage, Reason::invalid_config_key};
  return std::nullopt;
}

auto Client::session_url(const SessionId& id, std::string_view path)
    -> net::http::Query {
  SecretString base{sessions_url};
  net::http::PercentEncoding::append(base, id.view());
  base.append(path);
  return net::http::Query{base.view()};
}

auto Client::post_session(Stage stage, net::http::Client& http,
                          std::string_view label, net::http::Query url,
                          const SessionId& id, const ConfigKey& key,
                          const Specification& specification,
                          const SecretString& body)
    -> Expected<net::http::Response, Error> {
  net::http::Request request{session_endpoint(label), std::move(url)};
  request.json(body);
  request.header("Accept", "*/*");
  request.header("User-Agent", specification.user_agent);
  request.authorize(id);
  request.authorize(key);
  request.credential_label(SessionIdKind::label);
  auto sent = http.send(std::move(request));
  if (!sent) return unexpected(Error{stage, sent.error()});
  return std::move(sent.value());
}

auto Client::rotated_key(Stage stage, const net::http::Response& response)
    -> Expected<std::optional<ConfigKey>, Error> {
  const auto header = response.header(ConfigKeyKind::placement.header);
  if (!header) return std::optional<ConfigKey>{};
  const auto value = text::trim_http_space(*header);
  if (value.empty()) return std::optional<ConfigKey>{};
  if (!valid_header_value(value))
    return unexpected(
        Error{stage, Reason::invalid_session_key, response.status()});
  return std::optional<ConfigKey>{ConfigKey{value}};
}

auto Client::accepted(Stage stage, const net::http::Response& response)
    -> Expected<std::optional<ConfigKey>, Error> {
  if (net::HttpStatus{response.status()} != net::HttpStatus::ok)
    return unexpected(Error{stage, Reason::rejected, response.status()});
  return rotated_key(stage, response);
}

auto Client::event_name(const net::http::Response& response) -> std::string {
  const auto event = response.header(event_header);
  if (!event || event->empty()) return {};
  auto parsed = ps::read_reply<EventHeader>(*event);
  if (!parsed) return {};
  return std::move(parsed->name).substr(0, event_name_limit);
}

auto Client::mentions_entitlement(std::string_view body) noexcept -> bool {
  return body.find(entitlement_marker) != std::string_view::npos;
}

auto Client::client_id(std::string_view value) -> std::string {
  return std::string{value.substr(0, psn::gk_id_max_length)};
}

auto Client::decode_client_ids(std::string_view body)
    -> std::optional<ClientIds> {
  const auto reply = ps::read_reply<ClientIdsReply>(body);
  if (!reply || reply->gk_client_id.empty()) return std::nullopt;
  ClientIds ids;
  ids.gk_client_id = client_id(reply->gk_client_id);
  if (reply->ps3_gk_client_id)
    ids.ps3_gk_client_id = client_id(*reply->ps3_gk_client_id);
  if (reply->stream_server_client_id)
    ids.stream_server_client_id = client_id(*reply->stream_server_client_id);
  return ids;
}

auto Client::decode_lock(const net::http::Response& response) -> LockReply {
  LockReply result;
  const auto reply = ps::read_reply<LockWire>(response.body());
  if (!reply) return result;
  result.acquired = reply->lock_acquired.value_or(false);
  if (reply->poll_frequency && reply->poll_frequency->value() > 0)
    result.poll_seconds = reply->poll_frequency->value();
  return result;
}

auto Client::decode_datacenters(const net::http::Response& response)
    -> std::optional<std::vector<Datacenter>> {
  auto reply = ps::read_reply<std::vector<DatacenterRow>>(response.body());
  if (!reply || reply->empty()) return std::nullopt;
  std::vector<Datacenter> rows;
  rows.reserve(reply->size());
  for (auto& row : *reply)
    rows.push_back(Datacenter{std::move(row.data_center),
                              std::move(row.public_ip), row.port,
                              row.max_bandwidth.value()});
  return rows;
}

auto Client::decode_selected_port(const net::http::Response& response,
                                  int current_port) -> int {
  const auto reply = ps::read_reply<SelectReply>(response.body());
  if (!reply) return current_port;
  if (reply->port.value_or(0) > 0) return *reply->port;
  if (reply->network && reply->network->port.value_or(0) > 0)
    return *reply->network->port;
  return current_port;
}

auto Client::progress_of(const AllocateReply& reply)
    -> std::optional<AllocationProgress> {
  AllocationProgress result;
  result.queued = reply.queued.value_or(false);
  result.migrating = reply.data_migration.value_or(false);
  if (!result.queued && !result.migrating) return std::nullopt;
  const auto number = [](const auto& field) {
    return field ? field->value() : std::int32_t{};
  };
  result.wait_time_estimate = number(reply.wait_time_estimate);
  result.poll_frequency = number(reply.poll_frequency);
  result.migration_percentage =
      reply.data_migration_percentage_complete.value_or(0);
  result.queue_position =
      reply.display_queue_position.value_or(reply.queue_position.value_or(-1));
  return result;
}

auto Client::wrapper_type(std::string_view private_ip) noexcept
    -> std::uint8_t {
  constexpr std::uint8_t unknown = 1;
  const auto dot = private_ip.find_last_of('.');
  if (dot == std::string_view::npos) return unknown;
  const auto tail = private_ip.substr(dot + 1);
  int octet{};
  const auto [stop, failed] =
      std::from_chars(tail.data(), tail.data() + tail.size(), octet);
  if (failed == std::errc::invalid_argument) return 0;
  if (failed != std::errc{} || octet < 0 || octet > 255) return unknown;
  return static_cast<std::uint8_t>(octet);
}

auto Client::allocation_of(AllocateReply& reply)
    -> Expected<Allocation, Reason> {
  if (!reply.launch_slot) return unexpected(Reason::missing_launch_slot);
  auto& slot = *reply.launch_slot;
  if (!std::in_range<std::uint16_t>(slot.port))
    return unexpected(Reason::malformed);
  Allocation result;
  result.server_ip = std::move(slot.public_ip);
  result.server_port = static_cast<std::uint16_t>(slot.port);
  if (slot.private_ip) result.psn_wrapper_type = wrapper_type(*slot.private_ip);
  result.handshake_key = std::move(reply.handshake_key);
  result.launch_specification = std::move(reply.launch_specification);
  result.session_id = std::move(reply.session_id);
  return result;
}

Client::Client(net::http::Client& http) noexcept : http_(&http) {}

auto Client::client_ids(StreamingService service,
                        StreamingPlatform cloud_platform)
    -> Expected<ClientIds, Error> {
  constexpr auto stage = Stage::client_ids;
  std::string_view virt_type;
  std::string_view user_agent;
  if (service == StreamingService::legacy_cloud_stream) {
    user_agent = psn::legacy_cloud_stream_http_user_agent;
    if (cloud_platform == StreamingPlatform::ps3)
      virt_type = "konan";
    else if (cloud_platform == StreamingPlatform::ps5)
      virt_type = "cronos";
    else if (cloud_platform == StreamingPlatform::ps4)
      virt_type = "kratos";
  } else if (service == StreamingService::cloud_stream) {
    user_agent = psn::cloud_stream_http_user_agent;
    if (cloud_platform == StreamingPlatform::ps5) virt_type = "cronos";
  }
  if (virt_type.empty())
    return unexpected(Error{stage, Reason::invalid_service});

  net::http::Query url{client_ids_url};
  url.add("virtType", virt_type);
  net::http::Request request{client_ids_endpoint, std::move(url)};
  request.header("Accept", "*/*");
  request.header("User-Agent", user_agent);
  auto sent = http_->send(std::move(request));
  if (!sent) return unexpected(Error{stage, sent.error()});
  const auto& response = sent.value();
  if (net::HttpStatus{response.status()} != net::HttpStatus::ok)
    return unexpected(Error{stage, Reason::rejected, response.status()});
  auto ids = decode_client_ids(response.body());
  if (!ids)
    return unexpected(Error{stage, Reason::malformed, response.status()});
  return std::move(*ids);
}

auto Client::config(StreamingService service) -> Expected<ConfigKey, Error> {
  constexpr auto stage = Stage::config;
  ConfigBody body;
  std::string_view user_agent;
  switch (service) {
    case StreamingService::legacy_cloud_stream:
      body = {.product = "psnow", .platform = "PC"};
      user_agent = psn::legacy_cloud_stream_http_user_agent;
      break;
    case StreamingService::cloud_stream:
      body = {.product = "qlite", .platform = "qlite"};
      user_agent = psn::cloud_stream_http_user_agent;
      break;
    default:
      return unexpected(Error{stage, Reason::invalid_service});
  }
  auto text = body_of(stage, body);
  if (!text) return unexpected(text.error());
  net::http::Request request{config_endpoint, config_url};
  request.json(text.value());
  request.header("Accept", "*/*");
  request.header("User-Agent", user_agent);
  auto sent = http_->send(std::move(request));
  if (!sent) return unexpected(Error{stage, sent.error()});
  const auto& response = sent.value();
  if (net::HttpStatus{response.status()} != net::HttpStatus::ok)
    return unexpected(Error{stage, Reason::rejected, response.status()});
  auto reply = ps::read_reply<ConfigReply>(response.body());
  if (!reply || reply->config_key.empty())
    return unexpected(Error{stage, Reason::malformed, response.status()});
  return ConfigKey{std::move(reply->config_key)};
}

auto Client::start(std::string_view spec, const ConfigKey& key)
    -> Expected<Started, Error> {
  constexpr auto stage = Stage::start;
  if (!valid_header_value(key.view()))
    return unexpected(Error{stage, Reason::invalid_config_key});
  auto framed = specification(stage, spec);
  if (!framed) return unexpected(framed.error());
  auto body = body_of(stage, SpecificationBody{framed->text.view()});
  if (!body) return unexpected(body.error());

  net::http::Query url{start_url};
  url.add("npEnv", "np");
  net::http::Request request{session_endpoint("gk.start"), std::move(url)};
  request.json(body.value());
  request.header("Accept", "*/*");
  request.header("User-Agent", framed->user_agent);
  request.authorize(key);
  auto sent = http_->send(std::move(request));
  if (!sent) return unexpected(Error{stage, sent.error()});
  const auto& response = sent.value();
  if (net::HttpStatus{response.status()} != net::HttpStatus::ok)
    return unexpected(Error::rejected(stage,
                                      mentions_entitlement(response.body())
                                          ? Reason::entitlement_retry
                                          : Reason::rejected,
                                      response.status(), response.body()));
  auto reply = ps::read_reply<StartReply>(response.body());
  if (!reply || reply->session_id.empty())
    return unexpected(Error{stage, Reason::malformed, response.status()});
  auto rotated = rotated_key(stage, response);
  if (!rotated) return unexpected(rotated.error());
  return Started{SessionId{std::move(reply->session_id)},
                 std::move(rotated.value()), event_name(response)};
}

auto Client::authorize(const SessionId& id, const ConfigKey& key,
                       std::string_view spec) -> Expected<Authorized, Error> {
  constexpr auto stage = Stage::authorize;
  if (auto error = keys_error(stage, id, key)) return unexpected(*error);
  auto framed = specification(stage, spec);
  if (!framed) return unexpected(framed.error());
  auto body = body_of(stage, SpecificationBody{framed->text.view()});
  if (!body) return unexpected(body.error());
  auto sent =
      post_session(stage, *http_, "gk.authorize", session_url(id, "/authorize"),
                   id, key, *framed, body.value());
  if (!sent) return unexpected(sent.error());
  const auto& response = sent.value();
  if (net::HttpStatus{response.status()} != net::HttpStatus::ok) {
    const auto event =
        response.header(event_header).value_or(std::string_view{});
    if (event.find(psplus_marker) != std::string_view::npos ||
        response.body().find(psplus_marker) != std::string_view::npos)
      return unexpected(
          Error{stage, Reason::psplus_rejected, response.status()});
    return unexpected(Error::rejected(stage,
                                      mentions_entitlement(response.body())
                                          ? Reason::entitlement_retry
                                          : Reason::rejected,
                                      response.status(), response.body()));
  }
  auto rotated = rotated_key(stage, response);
  if (!rotated) return unexpected(rotated.error());
  return Authorized{std::move(rotated.value())};
}

auto Client::lock(const SessionId& id, const ConfigKey& key,
                  std::string_view spec) -> Expected<LockAttempt, Error> {
  constexpr auto stage = Stage::lock;
  if (auto error = keys_error(stage, id, key)) return unexpected(*error);
  auto framed = specification(stage, spec);
  if (!framed) return unexpected(framed.error());
  auto body = body_of(stage, SpecificationBody{framed->text.view()});
  if (!body) return unexpected(body.error());
  auto url = session_url(id, "/lock");
  url.add("forceLogout", "true");
  auto sent = post_session(stage, *http_, "gk.lock", std::move(url), id, key,
                           *framed, body.value());
  if (!sent) return unexpected(sent.error());
  auto rotated = accepted(stage, sent.value());
  if (!rotated) return unexpected(rotated.error());
  return LockAttempt{decode_lock(sent.value()), std::move(rotated.value()),
                     event_name(sent.value())};
}

auto Client::datacenters(const SessionId& id, const ConfigKey& key,
                         std::string_view spec)
    -> Expected<DatacenterList, Error> {
  constexpr auto stage = Stage::datacenters;
  if (auto error = keys_error(stage, id, key)) return unexpected(*error);
  auto framed = specification(stage, spec);
  if (!framed) return unexpected(framed.error());
  auto body = body_of(stage, SpecificationBody{framed->text.view()});
  if (!body) return unexpected(body.error());
  auto sent = post_session(stage, *http_, "gk.datacenters",
                           session_url(id, "/datacenters"), id, key, *framed,
                           body.value());
  if (!sent) return unexpected(sent.error());
  auto rotated = accepted(stage, sent.value());
  if (!rotated) return unexpected(rotated.error());
  auto rows = decode_datacenters(sent.value());
  if (!rows)
    return unexpected(Error{stage, Reason::malformed, sent.value().status()});
  return DatacenterList{std::move(*rows), std::move(rotated.value())};
}

auto Client::select(const SessionId& id, const ConfigKey& key,
                    std::string_view spec,
                    const std::vector<Ping>& ping_results, int current_port)
    -> Expected<Selected, Error> {
  constexpr auto stage = Stage::select;
  if (auto error = keys_error(stage, id, key)) return unexpected(*error);
  const auto pings = Ping::serialize(Ping::reported(ping_results));
  if (!pings) return unexpected(Error{stage, Reason::invalid_ping_results});
  auto framed = specification(stage, spec);
  if (!framed) return unexpected(framed.error());
  auto body = body_of(stage, SelectBody{framed->text.view(), *pings});
  if (!body) return unexpected(body.error());
  auto sent = post_session(stage, *http_, "gk.select",
                           session_url(id, "/datacenters/select"), id, key,
                           *framed, body.value());
  if (!sent) return unexpected(sent.error());
  auto rotated = accepted(stage, sent.value());
  if (!rotated) return unexpected(rotated.error());
  return Selected{decode_selected_port(sent.value(), current_port),
                  std::move(rotated.value())};
}

auto Client::allocate(const SessionId& id, const ConfigKey& key,
                      std::string_view spec, const Ping& selected,
                      int selected_port, int bitrate_kbps)
    -> Expected<AllocateState, Error> {
  constexpr auto stage = Stage::allocate;
  if (auto error = keys_error(stage, id, key)) return unexpected(*error);
  const auto name =
      std::string_view{selected.name}.substr(0, psn::gk_id_max_length);
  if (name.find('\0') != std::string_view::npos)
    return unexpected(Error{stage, Reason::invalid_allocate_body});
  auto framed = specification(stage, spec);
  if (!framed) return unexpected(framed.error());
  const auto reported = selected.reported();
  const AllocateBody allocation{
      .request_game_specification = framed->text.view(),
      .data_center = name,
      .network = {.bw_kbps_sent = bitrate_kbps,
                  .bw_loss = bw_loss,
                  .mtu = selected.mtu_in > 0 ? selected.mtu_in
                                             : Ping::default_mtu_in,
                  .rtt = reported.rtt_ms > 0 ? reported.rtt_ms
                                             : Ping::default_rtt_ms,
                  .port = selected_port,
                  .bw_kbps_received = bitrate_kbps,
                  .bw_loss_upstream = 0,
                  .mtu_upstream = selected.mtu_out > 0 ? selected.mtu_out
                                                       : Ping::default_mtu_out},
      .state_execution_time = state_execution_time_ms,
      .stream_test_time = stream_test_time_ms};
  auto body = body_of(stage, allocation);
  if (!body) return unexpected(body.error());
  auto sent =
      post_session(stage, *http_, "gk.allocate", session_url(id, "/allocate"),
                   id, key, *framed, body.value());
  if (!sent) return unexpected(sent.error());
  auto rotated = accepted(stage, sent.value());
  if (!rotated) return unexpected(rotated.error());
  auto reply = ps::read_reply<AllocateReply>(sent.value().body());
  if (!reply)
    return unexpected(Error{stage, Reason::malformed, sent.value().status()});
  if (auto progress = progress_of(*reply))
    return AllocateState{*progress, std::move(rotated.value())};
  auto success = allocation_of(*reply);
  if (!success)
    return unexpected(Error{stage, success.error(), sent.value().status()});
  return AllocateState{std::move(*success), std::move(rotated.value())};
}

}  // namespace libchiaking::psn::gk
