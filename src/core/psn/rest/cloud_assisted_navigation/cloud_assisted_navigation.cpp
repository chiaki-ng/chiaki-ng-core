#include "src/core/psn/rest/cloud_assisted_navigation/cloud_assisted_navigation.hpp"

#include "src/core/net/transport/http/url.hpp"
#include "src/core/utils/json/ps_reply.hpp"
#include "src/core/utils/text/ascii.hpp"
#include "src/core/utils/text/base64.hpp"
#include "src/core/utils/text/hex.hpp"
#include "src/core/utils/text/integer.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <utility>
#include <variant>

namespace libchiaking::psn::rest {

namespace ps = json::ps;

struct CloudAssistedNavigation::ClientsReply {
  struct Title {
    using keys = ps::CamelCaseKeys;
    ps::Tolerant<std::string> np_title_id;
    ps::Tolerant<std::string> ps_platform;
    ps::Tolerant<std::variant<std::string, std::uint64_t>> concept_id;
    ps::Tolerant<std::string> last_access_date_time;
  };
  struct Installed {
    ps::Tolerant<std::vector<ps::Tolerant<Title>>> titles;
  };
  struct SystemData {
    using keys = ps::CamelCaseKeys;
    ps::Tolerant<Installed> installed_titles;
  };
  struct Device {
    using keys = ps::CamelCaseKeys;
    ps::Tolerant<std::string> name;
    ps::Tolerant<std::vector<ps::Tolerant<std::string>>> enabled_features;
  };
  struct Console {
    using keys = ps::CamelCaseKeys;
    ps::Tolerant<std::string> duid;
    ps::Tolerant<std::string> platform;
    ps::Tolerant<std::vector<ps::Tolerant<std::string>>> device_properties;
    ps::Tolerant<Device> device;
    ps::Tolerant<SystemData> system_data;
  };
  std::optional<std::vector<ps::Tolerant<Console>>> clients;
};

struct CloudAssistedNavigation::CommandReply {
  struct Action {
    struct Kind {
      using keys = ps::CamelCaseKeys;
      ps::Tolerant<std::string> action_type;
    };
    ps::Tolerant<Kind> action;
  };
  struct Failure {
    ps::Tolerant<std::int64_t> code;
  };
  struct Command {
    using keys = ps::CamelCaseKeys;
    ps::Tolerant<std::string> command_status;
    ps::Tolerant<std::vector<ps::Tolerant<Action>>> actions;
    ps::Tolerant<Failure> error;
  };
  using keys = ps::CamelCaseKeys;
  ps::Tolerant<std::vector<ps::Tolerant<Command>>> commands;
  ps::Tolerant<Command> command;
  ps::Tolerant<std::string> command_status;
  ps::Tolerant<std::vector<ps::Tolerant<Action>>> actions;
  ps::Tolerant<Failure> error;
};

struct CloudAssistedNavigation::CommandAccepted {
  using keys = ps::CamelCaseKeys;
  std::optional<SecretString> command_id;
};

struct CloudAssistedNavigation::LaunchBody {
  struct Parameters {
    using keys = ps::CamelCaseKeys;
    std::array<std::string_view, 1> title_ids;
  };
  struct Detail {
    using keys = ps::CamelCaseKeys;
    std::string_view platform;
    std::string_view duid;
    std::string_view command_type;
    Parameters parameters;
  };
  using keys = ps::CamelCaseKeys;
  Detail command_detail;
};

struct CloudAssistedNavigation::GameIntentBody {
  struct Parameters {
    using keys = ps::CamelCaseKeys;
    std::string_view game_intent;
  };
  struct Detail {
    using keys = ps::CamelCaseKeys;
    std::string_view platform;
    std::string_view duid;
    std::string_view command_type;
    Parameters parameters;
  };
  using keys = ps::CamelCaseKeys;
  Detail command_detail;
};

struct CloudAssistedNavigation::PartyData {
  using keys = ps::CamelCaseKeys;
  std::string_view session_id;
};

struct CloudAssistedNavigation::PartyBody {
  struct TypeParams {
    using keys = ps::CamelCaseKeys;
    SecretString data;
  };
  struct Parameters {
    using keys = ps::CamelCaseKeys;
    std::string_view type;
    TypeParams type_params;
  };
  struct Detail {
    using keys = ps::CamelCaseKeys;
    std::string_view platform;
    std::string_view duid;
    std::string_view command_type;
    Parameters parameters;
  };
  using keys = ps::CamelCaseKeys;
  Detail command_detail;
};

struct CloudAssistedNavigation::InitialParams {
  using keys = ps::CamelCaseKeys;
  std::int64_t account_id{};
  std::int64_t room_id{};
  std::string_view session_id;
  std::string_view client_type;
  SecretString data1;
  SecretString data2;
};

struct CloudAssistedNavigation::RemotePlayBody {
  struct Parameters {
    using keys = ps::CamelCaseKeys;
    SecretString initial_params;
  };
  struct Detail {
    using keys = ps::CamelCaseKeys;
    std::string_view command_type;
    SecretString duid;
    std::string_view message_destination;
    Parameters parameters;
    std::string_view platform;
  };
  using keys = ps::CamelCaseKeys;
  Detail command_detail;
};

auto CloudAssistedNavigation::clients_page(ClientsReply reply)
    -> Expected<ClientsPage, ListClientsError> {
  if (!reply.clients)
    return unexpected(ListClientsError{bad_reply::InvalidSchema{}});
  ClientsPage page;
  page.received = reply.clients->size();
  for (auto& slot : *reply.clients) {
    if (!slot) {
      ++page.unaddressable;
      continue;
    }
    auto& client = *slot;
    RegisteredConsole console;
    console.duid = client.duid.value_or("");
    console.platform = client.platform.value_or("");
    const auto& properties = client.device_properties;
    console.cloud_console =
        properties && std::ranges::any_of(*properties, [](const auto& entry) {
          return entry && text::trim(*entry) == "CLDK";
        });
    if (client.device) {
      console.name = client.device->name.value_or("");
      const auto& features = client.device->enabled_features;
      console.remote_play_enabled =
          features && std::ranges::any_of(*features, [](const auto& feature) {
            return feature && *feature == "remotePlay";
          });
    }
    if (client.system_data && client.system_data->installed_titles &&
        client.system_data->installed_titles->titles)
      for (auto& entry : *client.system_data->installed_titles->titles) {
        if (!entry) continue;
        auto& title = *entry;
        const auto concept_text = [&title]() -> std::string {
          if (!title.concept_id) return {};
          if (const auto* number =
                  std::get_if<std::uint64_t>(&*title.concept_id))
            return std::to_string(*number);
          return std::get<std::string>(*title.concept_id);
        };
        console.titles.push_back(InstalledTitle{
            std::move(title.np_title_id).value_or(""),
            std::move(title.ps_platform).value_or(""), concept_text(),
            std::move(title.last_access_date_time).value_or("")});
      }
    DeviceUid uid{};
    const bool named = client.duid && client.device && client.device->name &&
                       client.device->name->size() <= console_name_max_bytes &&
                       client.device->name->find('\0') == std::string::npos;
    if (named && text::decode_hex(*client.duid, uid))
      console.device_uid = uid;
    else
      ++page.unaddressable;
    page.consoles.push_back(std::move(console));
  }
  return page;
}

auto CloudAssistedNavigation::command_state(const CommandReply& reply)
    -> std::optional<CommandState> {
  const auto state_of = [](const CommandReply::Command& command) {
    CommandState state;
    state.status = std::string{text::trim(command.command_status.value_or(""))};
    if (command.actions && !command.actions->empty() &&
        command.actions->front() && command.actions->front()->action)
      state.action_type = std::string{text::trim(
          command.actions->front()->action->action_type.value_or(""))};
    if (command.error && command.error->code && *command.error->code >= 0 &&
        *command.error->code <= std::numeric_limits<std::uint32_t>::max())
      state.error_code = static_cast<std::uint32_t>(*command.error->code);
    return state;
  };
  if (reply.commands && !reply.commands->empty() && reply.commands->front())
    return state_of(*reply.commands->front());
  if (reply.command) return state_of(*reply.command);
  if (reply.command_status)
    return state_of({reply.command_status, reply.actions, reply.error});
  return std::nullopt;
}

auto CloudAssistedNavigation::frequency(const net::http::Response& response)
    -> std::optional<std::chrono::seconds> {
  const auto header = response.header(frequency_header);
  if (!header) return std::nullopt;
  const auto value = text::trim(*header);
  const auto seconds = text::parse_integer<std::uint32_t>(value);
  if (value.size() > frequency_max_digits || !seconds || *seconds == 0)
    return std::nullopt;
  return std::chrono::seconds{*seconds};
}

auto CloudAssistedNavigation::commands_query(std::string_view command_id,
                                             std::string_view platform)
    -> net::http::Query {
  net::http::Query url{commands_url};
  url.add_verbatim("platform", platform.empty() ? "PS5" : platform)
      .add("commandIds", command_id)
      .add_verbatim("includeFields", "actions,commandDetail");
  return url;
}

CloudAssistedNavigation::CloudAssistedNavigation(
    net::http::Client& client, auth::BearerToken token) noexcept
    : client_(client), token_(std::move(token)) {}

auto CloudAssistedNavigation::send(net::http::Request request)
    -> Expected<net::http::Response, ExchangeError> {
  return ExchangeError::checked(client_.send(std::move(request)),
                                token_.view());
}

auto CloudAssistedNavigation::clients_query(std::size_t offset,
                                            std::string_view platform)
    -> net::http::Query {
  net::http::Query url{clients_url};
  url.add_verbatim("platform", platform)
      .add_verbatim("includeFields", "device,systemData")
      .add_verbatim("limit", std::to_string(list_clients_page_size))
      .add_verbatim("offset", std::to_string(offset));
  return url;
}

auto CloudAssistedNavigation::list_clients(std::size_t offset, bool mobile)
    -> Expected<ClientsPage, ListClientsError> {
  net::http::Request request{list_clients_endpoint,
                             clients_query(offset, "PS5")};
  request.authorize(token_).header("Accept", json_content_type);
  if (mobile) request.header(app_type_header, app_type);
  return ListClientsError::checked_reply<ClientsReply>(
             client_.send(std::move(request)), token_.view())
      .and_then(clients_page);
}

auto CloudAssistedNavigation::list_cloud_clients(std::size_t offset,
                                                 std::string_view platform)
    -> Expected<ClientsPage, ListCloudClientsError> {
  net::http::Request request{list_cloud_clients_endpoint,
                             clients_query(offset, platform)};
  request.authorize(token_)
      .header("Accept", json_content_type)
      .header(app_type_header, app_type);
  return ListCloudClientsError::checked_reply<ClientsReply>(
             client_.send(std::move(request)), token_.view())
      .and_then(clients_page);
}

auto CloudAssistedNavigation::launch_title(
    std::string_view duid, std::string_view launch_id,
    std::chrono::milliseconds total_timeout)
    -> Expected<std::string, LaunchTitleError> {
  const LaunchBody launch{.command_detail = {.platform = "PS5",
                                             .duid = duid,
                                             .command_type = "launchTitle",
                                             .parameters = {{launch_id}}}};
  auto body = ps::write(launch);
  if (!body) return unexpected(LaunchTitleError{bad_request::InvalidMessage{}});
  auto command = send_command(launch_title_endpoint, SecretBytes{body->bytes()},
                              total_timeout, true);
  if (!command) return unexpected(LaunchTitleError{command.error()});
  return std::move(command.value());
}

auto CloudAssistedNavigation::send_game_intent(
    std::string_view duid, std::string_view platform,
    std::string_view game_intent, std::chrono::milliseconds total_timeout)
    -> Expected<std::string, GameIntentError> {
  const GameIntentBody intent{
      .command_detail = {.platform = platform.empty() ? "PS5" : platform,
                         .duid = duid,
                         .command_type = "sendGameIntent",
                         .parameters = {game_intent}}};
  auto body = ps::write(intent);
  if (!body) return unexpected(GameIntentError{bad_request::InvalidMessage{}});
  auto command = send_command(send_game_intent_endpoint,
                              SecretBytes{body->bytes()}, total_timeout, true);
  if (!command) return unexpected(GameIntentError{command.error()});
  return std::move(command.value());
}

auto CloudAssistedNavigation::join_party(
    std::string_view duid, std::string_view platform,
    std::string_view session_id, std::chrono::milliseconds total_timeout)
    -> Expected<std::string, JoinPartyError> {
  auto data = ps::write(PartyData{session_id});
  if (!data) return unexpected(JoinPartyError{bad_request::InvalidMessage{}});
  const PartyBody party{
      .command_detail = {
          .platform = platform.empty() ? "PS5" : platform,
          .duid = duid,
          .command_type = "party",
          .parameters = {.type = "sessionSharing",
                         .type_params = {std::move(data.value())}}}};
  auto body = ps::write(party);
  if (!body) return unexpected(JoinPartyError{bad_request::InvalidMessage{}});
  auto command = send_command(join_party_endpoint, SecretBytes{body->bytes()},
                              total_timeout, true);
  if (!command) return unexpected(JoinPartyError{command.error()});
  return std::move(command.value());
}

auto CloudAssistedNavigation::send_command(
    net::http::Endpoint endpoint, SecretBytes body,
    std::chrono::milliseconds total_timeout, bool mobile)
    -> Expected<std::string, SendCommandError> {
  endpoint.total_timeout = total_timeout;
  net::http::Request request{endpoint, commands_url};
  request.authorize(token_)
      .header("Accept", json_content_type)
      .body(json_content_type, std::move(body));
  if (mobile) request.header(app_type_header, app_type);
  auto accepted = SendCommandError::checked_reply<CommandAccepted>(
      client_.send(std::move(request)), token_.view());
  if (!accepted) return unexpected(std::move(accepted.error()));
  const auto command_id = text::trim(
      accepted->command_id ? accepted->command_id->view() : std::string_view{});
  if (command_id.empty())
    return unexpected(SendCommandError{bad_reply::InvalidSchema{}});
  return std::string{command_id};
}

auto CloudAssistedNavigation::polled(net::http::Endpoint endpoint,
                                     net::http::Query url,
                                     std::chrono::milliseconds total_timeout,
                                     bool mobile)
    -> Expected<net::http::Response, ExchangeError> {
  endpoint.total_timeout = total_timeout;
  net::http::Request request{endpoint, std::move(url)};
  request.authorize(token_).header("Accept", json_content_type);
  if (mobile) request.header(app_type_header, app_type);
  return send(std::move(request));
}

auto CloudAssistedNavigation::poll_command(
    std::string_view command_id, std::chrono::milliseconds total_timeout,
    bool mobile, std::string_view platform)
    -> Expected<CommandPoll, PollCommandError> {
  auto url = commands_query(command_id, platform);
  url.add_verbatim("commandStatus", "executing,completed");
  auto response =
      polled(poll_command_endpoint, std::move(url), total_timeout, mobile);
  if (!response) return unexpected(std::move(response.error()));
  const auto reply =
      json::ps::read_reply<CommandReply>(response.value().body());
  return CommandPoll{reply ? command_state(reply.value()) : std::nullopt,
                     frequency(response.value())};
}

auto CloudAssistedNavigation::poll_remote_play_command(
    std::string_view command_id, std::chrono::milliseconds total_timeout)
    -> Expected<CommandPoll, PollRemotePlayError> {
  auto response =
      polled(poll_remote_play_endpoint, commands_query(command_id, "PS5"),
             total_timeout, false);
  if (!response) return unexpected(PollRemotePlayError{response.error()});
  auto reply = PollRemotePlayError::reply_of<CommandReply>(response.value());
  if (!reply) return unexpected(std::move(reply.error()));
  return CommandPoll{command_state(reply.value()), frequency(response.value())};
}

auto CloudAssistedNavigation::start_remote_play(
    const SessionId& session, std::int64_t account_id,
    std::span<const std::byte, psn_device_uid_size> device_uid,
    std::span<const std::byte, psn_custom_data_size> data1,
    std::span<const std::byte, psn_custom_data_size> data2)
    -> Expected<std::optional<SecretString>, StartRemotePlayError> {
  InitialParams initial{.account_id = account_id,
                        .room_id = 0,
                        .session_id = session.view(),
                        .client_type = "Windows"};
  text::base64::append(initial.data1, data1, text::base64::Alphabet::standard);
  text::base64::append(initial.data2, data2, text::base64::Alphabet::standard);
  auto initial_params = ps::write(initial);
  if (!initial_params)
    return unexpected(StartRemotePlayError{bad_request::InvalidMessage{}});
  RemotePlayBody remote_play{
      .command_detail = {.command_type = "remotePlay",
                         .message_destination = "SQS",
                         .parameters = {std::move(initial_params.value())},
                         .platform = "PS5"}};
  text::append_hex(remote_play.command_detail.duid, device_uid);
  auto body = ps::write(remote_play);
  if (!body)
    return unexpected(StartRemotePlayError{bad_request::InvalidMessage{}});
  net::http::Request request{start_remote_play_endpoint, commands_url};
  request.authorize(token_)
      .body(remote_play_content_type, SecretBytes{body->bytes()})
      .header("User-Agent", remote_play_user_agent);
  auto response = send(std::move(request));
  if (!response) return unexpected(StartRemotePlayError{response.error()});
  auto accepted =
      json::ps::read_reply<CommandAccepted>(response.value().body());
  if (!accepted || !accepted->command_id) return std::optional<SecretString>{};
  const auto id = accepted->command_id->view();
  if (id.empty() || id.size() > command_id_max_bytes ||
      !std::ranges::all_of(id,
                           [](char byte) { return text::ascii_visible(byte); }))
    return std::optional<SecretString>{};
  return std::optional<SecretString>{std::move(*accepted->command_id)};
}

}  // namespace libchiaking::psn::rest
