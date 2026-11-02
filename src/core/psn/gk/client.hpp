/// @file
/// @brief Declares gk::Client, the GK cloud console broker at
/// `cc.prod.gaikai.com`, and the credentials and replies of a cloud session.
///
/// A cloud launch walks GK's session state machine: client ids, a
/// configuration key, start (a session id), authorize (with the authorisation
/// codes inside the launch specification), lock (log any other session out),
/// datacenters, select (with the client's measurements), and allocate (queue
/// until a cloud console is assigned). Every session request carries the
/// session id and the configuration key as headers, and any reply may rotate
/// the key by sending a new one.
#pragma once

#include <libchiaking/detail/secret_string.hpp>
#include <libchiaking/platform/runtime/threading.hpp>
#include "libchiaking/detail/expected.hpp"
#include "src/core/net/transport/http/client.hpp"
#include "src/core/net/transport/http/credential.hpp"
#include "src/core/net/transport/http/url.hpp"
#include "src/core/psn/auth/gk_ids.hpp"
#include "src/core/psn/gk/datacenter_ping.hpp"
#include "src/core/psn/gk/error.hpp"
#include "src/core/psn/gk/measurement.hpp"
#include "src/core/utils/json/json.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace libchiaking::psn::gk {

/// @brief The OAuth client ids GK assigns a session.
using ClientIds = psn::auth::GkClientIds;

/// @brief Tag for a GK session id.
struct SessionIdKind {
  /// @brief Name used for the credential in diagnostics.
  static constexpr std::string_view label = "gk_session";
  /// @brief Sent as the `X-Gaikai-SessionId` header, unprefixed.
  static constexpr net::http::CredentialPlacement placement{
      "X-Gaikai-SessionId", ""};
  /// @brief Recorded in diagnostics as a GK session.
  static constexpr platform::HttpCredential credential =
      platform::HttpCredential::gk_session;
};

/// @brief Tag for a GK configuration key.
struct ConfigKeyKind {
  /// @brief Name used for the credential in diagnostics.
  static constexpr std::string_view label = "gk_config_key";
  /// @brief Sent, and rotated by GK, as the `X-Gaikai-Session` header,
  /// unprefixed.
  static constexpr net::http::CredentialPlacement placement{"X-Gaikai-Session",
                                                            ""};
  /// @brief Recorded in diagnostics as a GK session.
  static constexpr platform::HttpCredential credential =
      platform::HttpCredential::gk_session;
};

/// @brief A GK session id. Secret; held in wiping memory.
using SessionId = net::http::Credential<SessionIdKind>;
/// @brief A GK configuration key. Secret; held in wiping memory.
using ConfigKey = net::http::Credential<ConfigKeyKind>;

/// @brief A started session. Plain data.
struct Started {
  /// @brief The session's id.
  SessionId session_id;
  /// @brief The configuration key GK rotated to, if it did.
  std::optional<ConfigKey> rotated_key{};
  /// @brief The name in Client::event_header, up to 63 bytes; empty when none.
  std::string event_name{};
};

/// @brief An authorised session. Plain data.
struct Authorized {
  /// @brief The configuration key GK rotated to, if it did.
  std::optional<ConfigKey> rotated_key{};
};

/// @brief How long to wait before retrying the lock when GK advises
/// nothing, in seconds.
inline constexpr int default_lock_poll_seconds = 10;

/// @brief GK's answer to a lock request. Plain data.
struct LockReply {
  /// @brief The `lockAcquired` flag; false when absent or unreadable.
  bool acquired{};
  /// @brief The positive `pollFrequency`, in seconds, else the default.
  int poll_seconds{default_lock_poll_seconds};
};

/// @brief One lock request's outcome. Plain data.
struct LockAttempt {
  /// @brief The answer.
  LockReply response;
  /// @brief The configuration key GK rotated to, if it did.
  std::optional<ConfigKey> rotated_key{};
  /// @brief The name in Client::event_header, such as the session being closed.
  std::string event_name{};
};

/// @brief The datacenters offered. Plain data.
struct DatacenterList {
  /// @brief The datacenters, in reply order; never empty.
  std::vector<Datacenter> rows;
  /// @brief The configuration key GK rotated to, if it did.
  std::optional<ConfigKey> rotated_key{};
};

/// @brief A selection's outcome. Plain data.
struct Selected {
  /// @brief The port GK chose (`port`, else `network.port`), else the one
  /// proposed.
  int port{};
  /// @brief The configuration key GK rotated to, if it did.
  std::optional<ConfigKey> rotated_key{};
};

/// @brief An allocation still in the queue or migrating data. Plain data.
struct AllocationProgress {
  /// @brief The `dataMigration` flag: the user's data is being moved.
  bool migrating{};
  /// @brief The `queued` flag: the user is waiting for a console.
  bool queued{};
  /// @brief The `waitTimeEstimate` as GK states it; 0 when absent.
  std::int32_t wait_time_estimate{};
  /// @brief The `pollFrequency`, in seconds; 0 when absent.
  std::int32_t poll_frequency{};
  /// @brief The `dataMigrationPercentageComplete`, 0 to 100; 0 when absent.
  std::int32_t migration_percentage{};
  /// @brief The `displayQueuePosition`, else `queuePosition`; -1 when
  /// neither.
  std::int32_t queue_position{-1};
};

/// @brief An assigned cloud console. Plain data; holds secrets.
struct Allocation {
  /// @brief The launch slot's `publicIp`.
  std::string server_ip;
  /// @brief The launch slot's `port`, checked to fit 16 bits; 0 when GK
  /// named none.
  std::uint16_t server_port{};
  /// @brief The `handshakeKey` the stream is opened with.
  SecretString handshake_key;
  /// @brief The `launchSpecification` GK returns for the stream.
  SecretString launch_specification;
  /// @brief The stream's `sessionId`.
  SecretString session_id;
  /// @brief The Takion envelope type, the last octet of the slot's
  /// `privateIp`: 0 (no PSN envelope) when it is not a number; 1 when there
  /// is no private address, it has no `.`, or the octet is out of range.
  std::uint8_t psn_wrapper_type{1};
};

/// @brief Still waiting, or assigned.
using AllocateOutcome = std::variant<AllocationProgress, Allocation>;

/// @brief One allocate request's outcome. Plain data.
struct AllocateState {
  /// @brief Progress or the allocation.
  AllocateOutcome outcome;
  /// @brief The configuration key GK rotated to, if it did.
  std::optional<ConfigKey> rotated_key{};
};

/// @brief GK's session requests.
///
/// Each call is one request that blocks for its round trip and keeps the reply
/// even if the stop token fires mid-request, so a session state already
/// reached on GK is not lost. The caller owns the state machine: it carries
/// the session id and the latest configuration key from step to step, adopting
/// each rotated key. Holds a pointer to the HTTP client, which must outlive it.
/// Not for concurrent use.
class Client {
 public:
  /// @brief GK's broker, also named as `cloudEndpoint` in launch
  /// specifications.
  static constexpr std::string_view cloud_endpoint =
      "https://cc.prod.gaikai.com";
  /// @brief The largest body sent or reply accepted, in bytes.
  static constexpr std::size_t body_limit = json::max_document_bytes;

  /// @brief Prepares the requests.
  ///
  /// @param http The client the requests go through; must outlive this
  ///     object.
  explicit Client(net::http::Client& http) noexcept;

  /// @brief Asks for the session's OAuth client ids.
  ///
  /// GET `https://cc.prod.gaikai.com/v1/client_ids?virtType=<type>` with the
  /// service's user agent; `konan` for a PS3, `kratos` for a PS4 and `cronos`
  /// for a PS5 game.
  ///
  /// @param service The streaming service.
  /// @param cloud_platform The console the game runs on.
  /// @return The ids, each cut to gk_id_max_length.
  /// @retval Error::Reason::invalid_service The service does not stream the
  ///     platform.
  /// @retval Error::Reason::rejected The status is not 200.
  /// @retval Error::Reason::malformed No `gkClientId`.
  /// @retval Error A transport failure or cancellation.
  [[nodiscard]] auto client_ids(StreamingService service,
                                StreamingPlatform cloud_platform)
      -> Expected<ClientIds, Error>;
  /// @brief Asks for a configuration key.
  ///
  /// POST `https://config.cc.prod.gaikai.com/v1/config` with
  /// `{"product":"psnow","platform":"PC"}` or `{"product":"qlite",
  /// "platform":"qlite"}`.
  ///
  /// @param service The streaming service.
  /// @return The `configKey`.
  /// @retval Error::Reason::rejected The status is not 200.
  /// @retval Error::Reason::malformed No key.
  /// @retval Error A transport failure, cancellation or unknown service.
  [[nodiscard]] auto config(StreamingService service)
      -> Expected<ConfigKey, Error>;
  /// @brief Starts a session.
  ///
  /// POST `.../v1/sessions/start?npEnv=np` with the configuration key and
  /// `{"requestGameSpecification":<spec>}`.
  ///
  /// @param spec The launch specification, without codes.
  /// @param key The configuration key.
  /// @return The session id, a rotated key and the event name.
  /// @retval Error::Reason::entitlement_retry Rejected with
  ///     `noGameForEntitlement`.
  /// @retval Error::Reason::rejected Another status than 200; the body is
  ///     kept.
  /// @retval Error::Reason::malformed No `sessionId`.
  /// @retval Error The failures of specification() and the transport.
  [[nodiscard]] auto start(std::string_view spec, const ConfigKey& key)
      -> Expected<Started, Error>;
  /// @brief Authorises the session.
  ///
  /// POST `.../v1/sessions/<id>/authorize` with
  /// `{"requestGameSpecification":<spec>}`, the spec carrying the codes.
  ///
  /// @param id The session id.
  /// @param key The configuration key.
  /// @param spec The launch specification with the authorisation codes.
  /// @return A rotated key, if any.
  /// @retval Error::Reason::psplus_rejected Rejected for want of PlayStation
  ///     Plus.
  /// @retval Error::Reason::entitlement_retry Rejected with
  ///     `noGameForEntitlement`.
  /// @retval Error::Reason::rejected Another status than 200; the body is
  ///     kept.
  /// @retval Error The failures of keys_error(), specification(), the
  ///     transport and rotated_key().
  [[nodiscard]] auto authorize(const SessionId& id, const ConfigKey& key,
                               std::string_view spec)
      -> Expected<Authorized, Error>;
  /// @brief Locks the account to the session, logging out any other.
  ///
  /// POST `.../v1/sessions/<id>/lock?forceLogout=true`; `lockAcquired` and
  /// `pollFrequency` are read.
  ///
  /// @param id The session id.
  /// @param key The configuration key.
  /// @param spec The launch specification.
  /// @return Whether the lock was taken, when to retry, a rotated key and the
  ///     event name.
  /// @retval Error::Reason::rejected The status is not 200.
  /// @retval Error The failures of keys_error(), specification(), the
  ///     transport and rotated_key().
  [[nodiscard]] auto lock(const SessionId& id, const ConfigKey& key,
                          std::string_view spec)
      -> Expected<LockAttempt, Error>;
  /// @brief Lists the datacenters offered.
  ///
  /// POST `.../v1/sessions/<id>/datacenters`; an array of `dataCenter`,
  /// `publicIp`, `port` and `maxBandwidth` is read.
  ///
  /// @param id The session id.
  /// @param key The configuration key.
  /// @param spec The launch specification.
  /// @return The datacenters and a rotated key.
  /// @retval Error::Reason::rejected The status is not 200.
  /// @retval Error::Reason::malformed The list is unreadable or empty.
  /// @retval Error The failures of keys_error(), specification(), the
  ///     transport and rotated_key().
  [[nodiscard]] auto datacenters(const SessionId& id, const ConfigKey& key,
                                 std::string_view spec)
      -> Expected<DatacenterList, Error>;
  /// @brief Reports the measurements and selects a datacenter.
  ///
  /// POST `.../v1/sessions/<id>/datacenters/select` with
  /// `{"requestGameSpecification":<spec>,"pingResults":<report>}`, the
  /// report's round-trip times lowered as Ping::reported() does.
  ///
  /// @param id The session id.
  /// @param key The configuration key.
  /// @param spec The launch specification.
  /// @param ping_results The measurements.
  /// @param current_port The port proposed.
  /// @return The port GK chose and a rotated key.
  /// @retval Error::Reason::invalid_ping_results The report could not be
  ///     written.
  /// @retval Error::Reason::rejected The status is not 200.
  /// @retval Error The failures of keys_error(), specification(), the
  ///     transport and rotated_key().
  [[nodiscard]] auto select(const SessionId& id, const ConfigKey& key,
                            std::string_view spec,
                            const std::vector<Ping>& ping_results,
                            int current_port) -> Expected<Selected, Error>;
  /// @brief Asks for a cloud console in the selected datacenter.
  ///
  /// POST `.../v1/sessions/<id>/allocate` with the spec, the datacenter's
  /// name and the network as measured (bitrate both ways, 0.1 % loss, MTUs
  /// and the reported round-trip time, with defaults when unmeasured) and the
  /// fixed timings. The reply is progress (`queued` or
  /// `dataMigration`) or a `launchSlot`.
  ///
  /// @param id The session id.
  /// @param key The configuration key.
  /// @param spec The launch specification.
  /// @param selected The selected datacenter's measurement.
  /// @param selected_port The port to stream from.
  /// @param bitrate_kbps The bitrate to declare, in kbit/s.
  /// @return Progress or the allocation, and a rotated key.
  /// @retval Error::Reason::invalid_allocate_body The datacenter's name holds
  ///     a NUL.
  /// @retval Error::Reason::rejected The status is not 200.
  /// @retval Error::Reason::malformed The reply is unreadable, or the launch
  ///     slot's `port` does not fit 16 bits.
  /// @retval Error::Reason::missing_launch_slot Neither progress nor a slot.
  /// @retval Error The failures of keys_error(), specification(), the
  ///     transport and rotated_key().
  [[nodiscard]] auto allocate(const SessionId& id, const ConfigKey& key,
                              std::string_view spec, const Ping& selected,
                              int selected_port, int bitrate_kbps)
      -> Expected<AllocateState, Error>;

  /// @brief Tells whether the launch is being cancelled.
  ///
  /// @return Whether the client's stop token fired.
  auto stop_requested() const noexcept -> bool {
    return http_->stop_requested();
  }
  /// @brief Reads the client's stop token.
  ///
  /// @return The token.
  auto stop_token() const noexcept -> const platform::StopToken& {
    return http_->stop_token();
  }

 private:
  /// @brief Why a step failed.
  using Reason = Error::Reason;
  /// @brief The launch stage.
  using Stage = Error::Stage;
  /// @brief An HTTP endpoint.
  using Endpoint = net::http::Endpoint;
  /// @brief The HTTP methods.
  using Method = net::http::Method;

  /// @brief A launch specification ready to send.
  struct Specification {
    /// @brief The specification as canonical JSON, invalid UTF-8 repaired.
    SecretString text;
    /// @brief The user agent it names, which the requests are sent with.
    std::string_view user_agent;
  };
  /// @brief The specification's `httpUserAgent`.
  struct SpecificationHeader;
  /// @brief A body of only `requestGameSpecification`.
  struct SpecificationBody;
  /// @brief The select body.
  struct SelectBody;
  /// @brief The allocate body.
  struct AllocateBody;
  /// @brief The config body.
  struct ConfigBody;
  /// @brief The `name` in the event_header JSON.
  struct EventHeader;
  /// @brief The client ids reply.
  struct ClientIdsReply;
  /// @brief The config reply's `configKey`.
  struct ConfigReply;
  /// @brief The start reply's `sessionId`.
  struct StartReply;
  /// @brief The lock reply.
  struct LockWire;
  /// @brief One datacenter in the list.
  struct DatacenterRow;
  /// @brief The select reply's port.
  struct SelectReply;
  /// @brief The allocate reply.
  struct AllocateReply;

  /// @brief Describes a session POST.
  ///
  /// @param label The diagnostic label.
  /// @return An endpoint that logs its query, keeps its reply after a stop
  ///     and accepts body_limit bytes.
  static constexpr auto session_endpoint(std::string_view label) noexcept
      -> Endpoint {
    return Endpoint{.label = label,
                    .method = Method::post,
                    .log_query = true,
                    .keep_reply_after_stop = true,
                    .maximum_response_bytes = Client::body_limit};
  }

  /// @brief The client ids request.
  static constexpr Endpoint client_ids_endpoint{
      .label = "gk.client_ids",
      .method = Method::get,
      .log_query = true,
      .keep_reply_after_stop = true,
      .maximum_response_bytes = Client::body_limit};
  /// @brief The config request.
  static constexpr Endpoint config_endpoint{
      .label = "gk.config",
      .method = Method::post,
      .keep_reply_after_stop = true,
      .maximum_response_bytes = Client::body_limit};

  /// @brief The sessions collection; the session id follows.
  static constexpr std::string_view sessions_url =
      "https://cc.prod.gaikai.com/v1/sessions/";
  /// @brief The session start.
  static constexpr std::string_view start_url =
      "https://cc.prod.gaikai.com/v1/sessions/start";
  /// @brief The client ids.
  static constexpr std::string_view client_ids_url =
      "https://cc.prod.gaikai.com/v1/client_ids";
  /// @brief The configuration service.
  static constexpr std::string_view config_url =
      "https://config.cc.prod.gaikai.com/v1/config";

  static_assert(sessions_url.starts_with(Client::cloud_endpoint));
  static_assert(start_url.starts_with(sessions_url));
  static_assert(client_ids_url.starts_with(Client::cloud_endpoint));

  /// @brief The header GK names events in.
  static constexpr std::string_view event_header = "x-gaikai-event";
  /// @brief The longest event name kept, in bytes.
  static constexpr std::size_t event_name_limit = 63;
  /// @brief The code that means PlayStation Plus is required.
  static constexpr std::string_view psplus_marker = "002.2001";
  /// @brief The text that means GK found no game for the entitlement.
  static constexpr std::string_view entitlement_marker = "noGameForEntitlement";
  /// @brief The packet loss declared at allocation, as a fraction.
  static constexpr double bw_loss = 0.001;
  /// @brief The state execution time declared at allocation, in ms.
  static constexpr double state_execution_time_ms = 5974.7632;
  /// @brief The stream test time declared at allocation, in ms.
  static constexpr double stream_test_time_ms = 11262.8423;

  /// @brief Checks a value before it is sent as a header.
  ///
  /// @param value The value.
  /// @return True when it is not empty and holds no NUL, CR or LF.
  static auto valid_header_value(std::string_view value) noexcept -> bool;
  /// @brief Finds the user agent a specification names.
  ///
  /// @param spec The specification's header fields.
  /// @return The PS5 cloud streaming agent when none is named; the PS5 cloud
  ///     streaming or LegacyCloudStream agent when named; nothing for any
  ///     other.
  static auto spec_user_agent(const SpecificationHeader& spec)
      -> std::optional<std::string_view>;
  /// @brief Prepares a specification for sending.
  ///
  /// @param stage The stage, for the failure.
  /// @param spec The specification.
  /// @return Its canonical text and user agent.
  /// @retval Error::Reason::body_too_large It is body_limit bytes or more.
  /// @retval Error::Reason::invalid_spec It is not JSON, names an unknown
  ///     user agent, or cannot be canonicalised.
  [[nodiscard]] static auto specification(Stage stage, std::string_view spec)
      -> Expected<Specification, Error>;
  /// @brief Writes a body.
  ///
  /// @param stage The stage, for the failure.
  /// @param body The body.
  /// @return The JSON.
  /// @retval Error::Reason::body_too_large It could not be written or exceeds
  ///     body_limit.
  [[nodiscard]] static auto body_of(Stage stage, const auto& body)
      -> Expected<SecretString, Error>;
  /// @brief Checks the session headers.
  ///
  /// @param stage The stage, for the failure.
  /// @param id The session id.
  /// @param key The configuration key.
  /// @return invalid_session_id or invalid_config_key, or nothing when both
  ///     can be sent.
  static auto keys_error(Stage stage, const SessionId& id, const ConfigKey& key)
      -> std::optional<Error>;
  /// @brief Sends a session POST with both session headers and the
  /// specification's user agent.
  ///
  /// @param stage The stage, for the failure.
  /// @param http The client.
  /// @param label The diagnostic label.
  /// @param url The URL.
  /// @param id The session id.
  /// @param key The configuration key.
  /// @param specification The specification, for its user agent.
  /// @param body The body.
  /// @return The reply, whatever its status.
  /// @retval Error A transport failure or cancellation.
  [[nodiscard]] static auto post_session(
      Stage stage, net::http::Client& http, std::string_view label,
      net::http::Query url, const SessionId& id, const ConfigKey& key,
      const Specification& specification, const SecretString& body)
      -> Expected<net::http::Response, Error>;
  /// @brief Builds a URL under a session.
  ///
  /// @param id The session id, percent-encoded into the path.
  /// @param path What follows, such as `/lock`.
  /// @return The URL.
  static auto session_url(const SessionId& id, std::string_view path)
      -> net::http::Query;
  /// @brief Reads a rotated configuration key.
  ///
  /// @param stage The stage, for the failure.
  /// @param response The reply.
  /// @return The trimmed `X-Gaikai-Session` value, or nothing when absent or
  ///     blank.
  /// @retval Error::Reason::invalid_session_key It cannot be sent as a
  ///     header.
  [[nodiscard]] static auto rotated_key(Stage stage,
                                        const net::http::Response& response)
      -> Expected<std::optional<ConfigKey>, Error>;
  /// @brief Accepts a 200 reply and reads its rotated key.
  ///
  /// @param stage The stage, for the failure.
  /// @param response The reply.
  /// @return The rotated key, if any.
  /// @retval Error::Reason::rejected The status is not 200.
  /// @retval Error::Reason::invalid_session_key The key cannot be sent.
  [[nodiscard]] static auto accepted(Stage stage,
                                     const net::http::Response& response)
      -> Expected<std::optional<ConfigKey>, Error>;
  /// @brief Reads the event a reply names.
  ///
  /// @param response The reply.
  /// @return The `name` in the event_header JSON, up to 63 bytes; empty when
  ///     absent or unreadable.
  static auto event_name(const net::http::Response& response) -> std::string;
  /// @brief Tells whether a rejection says no game was found for the
  /// entitlement.
  ///
  /// @param body The reply body.
  /// @return True when it mentions `noGameForEntitlement`.
  static auto mentions_entitlement(std::string_view body) noexcept -> bool;
  /// @brief Cuts a client id to gk_id_max_length.
  ///
  /// @param value The id.
  /// @return The kept part.
  static auto client_id(std::string_view value) -> std::string;
  /// @brief Reads the client ids reply.
  ///
  /// @param body The reply.
  /// @return The ids; nothing when unreadable or without `gkClientId`.
  static auto decode_client_ids(std::string_view body)
      -> std::optional<ClientIds>;
  /// @brief Reads the lock reply.
  ///
  /// @param response The reply.
  /// @return The answer; not acquired, default poll, when unreadable.
  static auto decode_lock(const net::http::Response& response) -> LockReply;
  /// @brief Reads the datacenter list.
  ///
  /// @param response The reply.
  /// @return The datacenters; nothing when unreadable or empty.
  static auto decode_datacenters(const net::http::Response& response)
      -> std::optional<std::vector<Datacenter>>;
  /// @brief Reads the port GK chose.
  ///
  /// @param response The reply.
  /// @param current_port The port proposed.
  /// @return A positive `port`, else a positive `network.port`, else
  ///     `current_port`.
  static auto decode_selected_port(const net::http::Response& response,
                                   int current_port) -> int;
  /// @brief Reads allocation progress.
  ///
  /// @param reply The reply.
  /// @return The progress; nothing when neither queued nor migrating.
  static auto progress_of(const AllocateReply& reply)
      -> std::optional<AllocationProgress>;
  /// @brief Reads the Takion envelope type from a private address.
  ///
  /// @param private_ip The slot's `privateIp`.
  /// @return Its last octet; 0 when that is not a number; 1 when there is no
  ///     `.` or the octet is out of range.
  static auto wrapper_type(std::string_view private_ip) noexcept
      -> std::uint8_t;
  /// @brief Reads an assigned console.
  ///
  /// @param reply The reply, moved from.
  /// @return The allocation.
  /// @retval Reason::missing_launch_slot The reply has no launch slot.
  /// @retval Reason::malformed The slot's `port` is outside 0 to 65535.
  [[nodiscard]] static auto allocation_of(AllocateReply& reply)
      -> Expected<Allocation, Reason>;

  /// @brief The client the requests go through; never null.
  net::http::Client* http_;
};

}  // namespace libchiaking::psn::gk
