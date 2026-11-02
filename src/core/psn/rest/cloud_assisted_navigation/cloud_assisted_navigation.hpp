/// @file
/// @brief Declares CloudAssistedNavigation, the client of PSN's cloud assisted
/// navigation service: the account's PS5 consoles and their installed games,
/// and commands sent to a console through PSN (start a game, start Remote
/// Play).
///
/// The service lives under
/// `web.np.playstation.com/api/cloudAssistedNavigation/v2/users/me`. It lists
/// the consoles (`clients`) bound to the account with their device data and
/// installed titles, and queues `commands` that the console picks up over its
/// own connection to PSN, reporting their progress back.
#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/auth/credentials.hpp"
#include "src/core/psn/rest/error.hpp"
#include "src/core/psn/rest/session_manager/session_manager.hpp"

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/detail/wire_sizes.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::psn::rest {

/// @brief How many consoles one list_clients() page asks for.
inline constexpr std::size_t list_clients_page_size = 10;
/// @brief The longest console name accepted, in bytes; a console with a longer
/// name is listed but not addressable.
inline constexpr std::size_t console_name_max_bytes = 512;

/// @brief A console's 32-byte device unique id, decoded from its hex `duid`.
using DeviceUid = std::array<std::byte, psn_device_uid_size>;

/// @brief A game installed on a console, as the console reported it. Plain
/// data.
struct InstalledTitle {
  /// @brief The `npTitleId`, such as `PPSA01234_00`; empty when absent.
  std::string title_id;
  /// @brief The `psPlatform`, such as `PS5`; empty when absent.
  std::string platform;
  /// @brief The `conceptId` in decimal, whether PS sent a number or a
  /// string; empty when absent.
  std::string concept_id{};
  /// @brief The `lastAccessDateTime` as PS wrote it; empty when absent.
  std::string last_access{};
};

/// @brief A console bound to the account. Plain data.
struct RegisteredConsole {
  /// @brief The console's `duid` as hex text; empty when absent.
  std::string duid;
  /// @brief The console's `device.name`; empty when absent.
  std::string name;
  /// @brief The games in `systemData.installedTitles`, in reply order.
  std::vector<InstalledTitle> titles;
  /// @brief The decoded `duid`, present only when the console is addressable:
  /// it has a `duid` of 64 hex digits and a name of at most
  /// console_name_max_bytes with no NUL.
  std::optional<DeviceUid> device_uid;
  /// @brief Whether `device.enabledFeatures` includes `remotePlay`.
  bool remote_play_enabled{};
  /// @brief Whether `deviceProperties` includes `CLDK`, which marks the
  /// account's PS5 cloud console, the one invitations are joined on.
  bool cloud_console{};
  /// @brief The console's `platform`, such as `PS5` or `PS4`; empty when
  /// absent. Names the platform a command to this console must carry.
  std::string platform{};
};

/// @brief One page of the account's consoles. Plain data.
struct ClientsPage {
  /// @brief The consoles that could be read, in reply order.
  std::vector<RegisteredConsole> consoles;
  /// @brief How many entries the page held, readable or not; fewer than
  /// list_clients_page_size means the last page.
  std::size_t received{};
  /// @brief How many entries were unreadable or not addressable.
  std::size_t unaddressable{};
};

/// @brief Where a console command stands. Plain data.
struct CommandState {
  /// @brief The `commandStatus`, trimmed, such as `pending`, `executing`,
  /// `completed` or `failed`; empty when absent.
  std::string status;
  /// @brief The first action's `actionType`, trimmed; empty when absent.
  std::string action_type;
  /// @brief The `error.code`, when present and within 32 bits.
  std::optional<std::uint32_t> error_code;
};

/// @brief The answer to one poll of a command. Plain data.
struct CommandPoll {
  /// @brief The command's state; nothing when the reply did not say.
  std::optional<CommandState> state;
  /// @brief How long to wait before polling again, from the
  /// `x-psn-can-frequency-control` header; nothing when absent, zero or not a
  /// number of at most 9 digits.
  std::optional<std::chrono::seconds> frequency;
};

/// @brief Requests to the cloud assisted navigation service with one bearer
/// token: the app token (installed games, game starts) or the Remote Play
/// token (the account's consoles, Remote Play starts).
///
/// Holds a reference to the HTTP client, which must outlive it, and the token.
/// Every call blocks for one round trip. Not for concurrent use.
class CloudAssistedNavigation {
 public:
  /// @brief The failure of an exchange with send() or polled().
  using ExchangeError = Error<>;
  /// @brief The failure of send_command().
  using SendCommandError =
      Error<bad_reply::InvalidJson, bad_reply::InvalidSchema>;
  /// @brief The failure of start_remote_play().
  using StartRemotePlayError = Error<bad_request::InvalidMessage>;
  /// @brief The failure of poll_remote_play_command().
  using PollRemotePlayError =
      Error<bad_reply::InvalidJson, bad_reply::InvalidSchema>;
  /// @brief The failure of poll_command().
  using PollCommandError = Error<>;
  /// @brief The failure of join_party().
  using JoinPartyError =
      Error<bad_request::InvalidMessage, bad_reply::InvalidJson,
            bad_reply::InvalidSchema>;
  /// @brief The failure of send_game_intent().
  using GameIntentError =
      Error<bad_request::InvalidMessage, bad_reply::InvalidJson,
            bad_reply::InvalidSchema>;
  /// @brief The failure of launch_title().
  using LaunchTitleError =
      Error<bad_request::InvalidMessage, bad_reply::InvalidJson,
            bad_reply::InvalidSchema>;
  /// @brief The failure of list_cloud_clients().
  using ListCloudClientsError =
      Error<bad_reply::InvalidJson, bad_reply::InvalidSchema>;
  /// @brief The failure of list_clients().
  using ListClientsError =
      Error<bad_reply::InvalidJson, bad_reply::InvalidSchema>;
  /// @brief Prepares the requests.
  ///
  /// @param client The client the requests go through.
  /// @param token The bearer token to send.
  CloudAssistedNavigation(net::http::Client& client,
                          auth::BearerToken token) noexcept;

  /// @brief Reads one page of the account's PS5 consoles.
  ///
  /// GET
  /// `.../clients?platform=PS5&includeFields=device,systemData&limit=10&offset=<offset>`;
  /// each client's `duid`, `device.name`, `device.enabledFeatures` and
  /// installed titles are read, an unreadable entry counting as unaddressable
  /// rather than failing the page.
  ///
  /// @param offset Index of the first console.
  /// @param mobile Whether to send the `X-Psn-Can-Mobile-App-Type: PSAPP`
  ///     header, which the service requires when the request carries the app
  ///     token rather than the Remote Play token.
  /// @return The page.
  /// @retval bad_reply::InvalidSchema The reply has no `clients`.
  /// @retval Error A transport failure, an error status or an unreadable
  ///     reply.
  [[nodiscard]] auto list_clients(std::size_t offset, bool mobile)
      -> Expected<ClientsPage, ListClientsError>;
  /// @brief Reads one page of the account's PS5 consoles to find its cloud
  /// console.
  ///
  /// As list_clients(), with the `X-Psn-Can-Mobile-App-Type: PSAPP`
  /// header, so the cloud console is listed with its
  /// `deviceProperties`.
  ///
  /// @param offset Index of the first console.
  /// @param platform The platform to list, such as `PS5` or `PS4`; the cloud
  ///     console that runs a title is on that title's platform.
  /// @return The page.
  /// @retval bad_reply::InvalidSchema The reply has no `clients`.
  /// @retval Error A transport failure, an error status or an unreadable
  ///     reply.
  [[nodiscard]] auto list_cloud_clients(std::size_t offset,
                                        std::string_view platform)
      -> Expected<ClientsPage, ListCloudClientsError>;
  /// @brief Asks a console to start a game.
  ///
  /// POST `.../commands` with
  /// `{"commandDetail":{"platform":"PS5","duid":<duid>,
  /// "commandType":"launchTitle","parameters":{"titleIds":[<launch id>]}}}`;
  /// the reply's `commandId` names the command to poll.
  ///
  /// @param duid The console's `duid` as hex text.
  /// @param launch_id The title id to start.
  /// @param total_timeout The longest the request may take.
  /// @return The command id, trimmed.
  /// @retval bad_request::InvalidMessage The body could not be written.
  /// @retval bad_reply::InvalidSchema The reply has no command id.
  /// @retval Error A transport failure, an error status or an unreadable
  ///     reply.
  [[nodiscard]] auto launch_title(std::string_view duid,
                                  std::string_view launch_id,
                                  std::chrono::milliseconds total_timeout)
      -> Expected<std::string, LaunchTitleError>;
  /// @brief Asks a console to join a game session, which accepts an
  /// invitation.
  ///
  /// POST `.../commands` with the `X-Psn-Can-Mobile-App-Type: PSAPP`
  /// header and
  /// `{"commandDetail":{"platform":"PS5","duid":<duid>,
  /// "commandType":"sendGameIntent","parameters":{"gameIntent":<intent>}}}`;
  /// the reply's `commandId` names the command to poll with poll_command().
  ///
  /// @param duid The console's `duid` as hex text.
  /// @param platform The console's platform, such as `PS5` or `PS4`; empty
  ///     falls back to `PS5`.
  /// @param game_intent The session's `tokenizedPlayerGameIntent`.
  /// @param total_timeout The longest the request may take.
  /// @return The command id, trimmed.
  /// @retval bad_request::InvalidMessage The body could not be written.
  /// @retval bad_reply::InvalidSchema The reply has no command id.
  /// @retval Error A transport failure, an error status or an unreadable
  ///     reply.
  [[nodiscard]] auto send_game_intent(std::string_view duid,
                                      std::string_view platform,
                                      std::string_view game_intent,
                                      std::chrono::milliseconds total_timeout)
      -> Expected<std::string, GameIntentError>;
  /// @brief Asks a console to join a party, which moves the party's voice
  /// chat to the console.
  ///
  /// POST `.../commands` with the `X-Psn-Can-Mobile-App-Type: PSAPP`
  /// header and
  /// `{"commandDetail":{"platform":"PS5","duid":<duid>,"commandType":"party",
  /// "parameters":{"type":"sessionSharing","typeParams":{"data":
  /// "{\"sessionId\":<session>}"}}}}`, the data written as JSON text; the
  /// reply's `commandId` names the command to poll with poll_command().
  ///
  /// @param duid The console's `duid` as hex text.
  /// @param platform The console's platform; empty falls back to `PS5`.
  /// @param session_id The party session.
  /// @param total_timeout The longest the request may take.
  /// @return The command id, trimmed.
  /// @retval bad_request::InvalidMessage The body could not be written.
  /// @retval bad_reply::InvalidSchema The reply has no command id.
  /// @retval Error A transport failure, an error status or an unreadable
  ///     reply.
  [[nodiscard]] auto join_party(std::string_view duid,
                                std::string_view platform,
                                std::string_view session_id,
                                std::chrono::milliseconds total_timeout)
      -> Expected<std::string, JoinPartyError>;
  /// @brief Asks how a game start is going.
  ///
  /// GET `.../commands?platform=PS5&commandIds=<id>&includeFields=actions,
  /// commandDetail&commandStatus=executing,completed`. The state is read from
  /// the first of `commands`, `command`, or top-level fields; an unreadable
  /// body gives no state rather than a failure.
  ///
  /// @param command_id The command, from launch_title().
  /// @param total_timeout The longest the request may take.
  /// @param mobile Whether to send the `X-Psn-Can-Mobile-App-Type: PSAPP`
  ///     header, which the service requires to poll a command created with it
  ///     (such as send_game_intent()).
  /// @param platform The platform the command was created on, such as `PS5`
  ///     or `PS4`; empty falls back to `PS5`.
  /// @return The state, if any, and the advised polling interval.
  /// @retval Error A transport failure or an error status.
  [[nodiscard]] auto poll_command(std::string_view command_id,
                                  std::chrono::milliseconds total_timeout,
                                  bool mobile, std::string_view platform)
      -> Expected<CommandPoll, PollCommandError>;
  /// @brief Asks how a Remote Play start command is going, for diagnostics.
  ///
  /// As poll_command() without the `commandStatus` filter; an unreadable body
  /// fails.
  ///
  /// @param command_id The command, from start_remote_play().
  /// @param total_timeout The longest the request may take.
  /// @return The state, if any, and the advised polling interval.
  /// @retval Error A transport failure, an error status or an unreadable
  ///     reply.
  [[nodiscard]] auto poll_remote_play_command(
      std::string_view command_id, std::chrono::milliseconds total_timeout)
      -> Expected<CommandPoll, PollRemotePlayError>;
  /// @brief Asks a PS5 to join a Remote Play session, the internet start of a
  /// PS5 Remote Play.
  ///
  /// POST `.../commands` with the user agent `RpNetHttpUtilImpl` and
  /// `{"commandDetail":{"commandType":"remotePlay","duid":<hex>,
  /// "messageDestination":"SQS","parameters":{"initialParams":<json text>},
  /// "platform":"PS5"}}`, where the initial parameters are `{"accountId",
  /// "roomId":0,"sessionId","clientType":"Windows","data1","data2"}` with the
  /// data in base64. The reply is kept even if the stop token fires.
  ///
  /// @param session The session the console should join.
  /// @param account_id The user's account id.
  /// @param device_uid The console's device unique id.
  /// @param data1 The session's first 16-byte custom data block.
  /// @param data2 The session's second 16-byte custom data block.
  /// @return The command id when the reply named a usable one (1 to 256
  ///     visible ASCII bytes), so its progress can be polled; nothing when it
  ///     did not, which is still a success.
  /// @retval bad_request::InvalidMessage A body could not be written.
  /// @retval Error A transport failure or an error status.
  [[nodiscard]] auto start_remote_play(
      const SessionId& session, std::int64_t account_id,
      std::span<const std::byte, psn_device_uid_size> device_uid,
      std::span<const std::byte, psn_custom_data_size> data1,
      std::span<const std::byte, psn_custom_data_size> data2)
      -> Expected<std::optional<SecretString>, StartRemotePlayError>;

 private:
  /// @brief The HTTP methods.
  using Method = net::http::Method;
  /// @brief The account's consoles.
  static constexpr std::string_view clients_url =
      "https://web.np.playstation.com/api/cloudAssistedNavigation/v2/users/me/"
      "clients";
  /// @brief The account's console commands.
  static constexpr std::string_view commands_url =
      "https://web.np.playstation.com/api/cloudAssistedNavigation/v2/users/me/"
      "commands";
  /// @brief The JSON content type.
  static constexpr std::string_view json_content_type = "application/json";
  /// @brief The content type the Remote Play start sends.
  static constexpr std::string_view remote_play_content_type =
      "application/json; charset=utf-8";
  /// @brief The user agent the Remote Play start sends, the Remote Play
  /// app's.
  static constexpr std::string_view remote_play_user_agent =
      "RpNetHttpUtilImpl";
  /// @brief The header that names the calling client as a mobile client.
  static constexpr std::string_view app_type_header =
      "X-Psn-Can-Mobile-App-Type";
  /// @brief The mobile client type the service expects in app_type_header.
  static constexpr std::string_view app_type = "PSAPP";
  /// @brief The header that advises the polling interval, in seconds.
  static constexpr std::string_view frequency_header =
      "x-psn-can-frequency-control";
  /// @brief The most digits of the polling interval read.
  static constexpr std::size_t frequency_max_digits = 9;
  /// @brief The longest Remote Play command id kept, in bytes.
  static constexpr std::size_t command_id_max_bytes = 256;

  /// @brief The console list request, logged with its query.
  static constexpr net::http::Endpoint list_clients_endpoint{
      .label = "psn.cloud_assisted_navigation.list_clients",
      .log_query = true,
  };

  /// @brief The console list request that looks for the cloud console,
  /// logged with its query.
  static constexpr net::http::Endpoint list_cloud_clients_endpoint{
      .label = "psn.cloud_assisted_navigation.list_cloud_clients",
      .log_query = true,
  };

  /// @brief The game session join request.
  static constexpr net::http::Endpoint send_game_intent_endpoint{
      .label = "psn.cloud_assisted_navigation.send_game_intent",
      .method = Method::post,
  };

  /// @brief The party join request.
  static constexpr net::http::Endpoint join_party_endpoint{
      .label = "psn.cloud_assisted_navigation.join_party",
      .method = Method::post,
  };

  /// @brief The game start request.
  static constexpr net::http::Endpoint launch_title_endpoint{
      .label = "psn.cloud_assisted_navigation.launch_title",
      .method = Method::post,
      .log_query = true,
  };

  /// @brief The game start poll, logged with its query.
  static constexpr net::http::Endpoint poll_command_endpoint{
      .label = "psn.cloud_assisted_navigation.poll_command",
      .log_query = true,
  };

  /// @brief The Remote Play start poll.
  static constexpr net::http::Endpoint poll_remote_play_endpoint{
      .label = "psn.cloud_assisted_navigation.poll_remote_play_command",
  };

  /// @brief The Remote Play start request, whose reply is kept after a stop.
  static constexpr net::http::Endpoint start_remote_play_endpoint{
      .label = "psn.cloud_assisted_navigation.start_remote_play",
      .method = Method::post,
      .keep_reply_after_stop = true,
  };

  /// @brief What a poll does with a 2xx reply it cannot read.
  /// @brief The console list reply's fields, each tolerated when malformed.
  struct ClientsReply;
  /// @brief The command poll reply's fields, in any of three layouts.
  struct CommandReply;
  /// @brief A command request's `commandId`.
  struct CommandAccepted;
  /// @brief The game start body.
  struct LaunchBody;
  /// @brief The game session join body.
  struct GameIntentBody;
  /// @brief The party join body.
  struct PartyBody;
  /// @brief The party a party join names, sent as JSON text.
  struct PartyData;
  /// @brief The Remote Play start body.
  struct RemotePlayBody;
  /// @brief The Remote Play start's initial parameters, sent as JSON text.
  struct InitialParams;

  /// @brief Turns a console list reply into a page.
  ///
  /// @param reply The reply, consumed.
  /// @return The page.
  /// @retval bad_reply::InvalidSchema The reply has no `clients`.
  static auto clients_page(ClientsReply reply)
      -> Expected<ClientsPage, ListClientsError>;
  /// @brief Finds a command's state in a poll reply.
  ///
  /// @param reply The reply.
  /// @return The state of the first of `commands`, then `command`, then the
  ///     top-level fields; nothing when none is present.
  static auto command_state(const CommandReply& reply)
      -> std::optional<CommandState>;
  /// @brief Reads the advised polling interval.
  ///
  /// @param response The reply.
  /// @return The interval, as CommandPoll::frequency describes.
  [[nodiscard]] static auto frequency(const net::http::Response& response)
      -> std::optional<std::chrono::seconds>;
  /// @brief Builds the console list query.
  ///
  /// @param offset Index of the first console.
  /// @param platform The platform to list, such as `PS5` or `PS4`.
  /// @return `clients?platform=<platform>&includeFields=device,systemData&
  ///     limit=10&offset=<offset>`.
  static auto clients_query(std::size_t offset, std::string_view platform)
      -> net::http::Query;
  /// @brief Sends a command and reads its id.
  ///
  /// @param endpoint The endpoint, whose total timeout is replaced.
  /// @param body The JSON body.
  /// @param total_timeout The longest the request may take.
  /// @param mobile Whether to send the mobile client type header.
  /// @return The command id, trimmed.
  /// @retval bad_reply::InvalidSchema The reply has no command id.
  /// @retval Error A transport failure, an error status or an unreadable
  ///     reply.
  [[nodiscard]] auto send_command(net::http::Endpoint endpoint,
                                  SecretBytes body,
                                  std::chrono::milliseconds total_timeout,
                                  bool mobile)
      -> Expected<std::string, SendCommandError>;
  /// @brief Builds the query that reads one command.
  ///
  /// @param command_id The command.
  /// @param platform The command's platform, such as `PS5` or `PS4`; empty
  ///     falls back to `PS5`.
  /// @return `commands?platform=<platform>&commandIds=<id>&includeFields=
  ///     actions,commandDetail`, the id percent-encoded.
  static auto commands_query(std::string_view command_id,
                             std::string_view platform) -> net::http::Query;

  /// @brief Sends a request and accepts only a 2xx reply.
  ///
  /// @param request The request.
  /// @return The reply.
  /// @retval Error A transport failure or an error status, the token
  ///     redacted.
  [[nodiscard]] auto send(net::http::Request request)
      -> Expected<net::http::Response, ExchangeError>;
  /// @brief Sends a command poll.
  ///
  /// @param endpoint The endpoint, whose total timeout is replaced.
  /// @param url The query.
  /// @param total_timeout The longest the request may take.
  /// @param mobile Whether to send the `X-Psn-Can-Mobile-App-Type: PSAPP`
  ///     header.
  /// @return The reply.
  /// @retval ExchangeError A transport failure or an error status.
  [[nodiscard]] auto polled(net::http::Endpoint endpoint, net::http::Query url,
                            std::chrono::milliseconds total_timeout,
                            bool mobile)
      -> Expected<net::http::Response, ExchangeError>;

  /// @brief The client the requests go through.
  net::http::Client& client_;
  /// @brief The bearer token.
  auth::BearerToken token_;
};

}  // namespace libchiaking::psn::rest
