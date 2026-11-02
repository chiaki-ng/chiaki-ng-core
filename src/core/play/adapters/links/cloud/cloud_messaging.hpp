/// @file
/// @brief Declares CloudMessaging, the PS5 cloud service's addressed message
/// layer carried in Takion generic data transfers.
#pragma once

#include "libchiaking/detail/play/cloud_notice.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace libchiaking::play {

/// @brief Speaks the PS5 cloud service's addressed messages.
///
/// Every message is one generic data transfer: its metadata is a JSON gateway
/// header naming the address, version and request number, and its data is the
/// body. Before a client may send to an address, both sides exchange the
/// addresses they receive on at address 4. This object only builds and reads
/// transfers; the caller sends them.
class CloudMessaging {
 public:
  /// @brief The address of the address handshake.
  static constexpr std::uint32_t sync_address = 4;
  /// @brief The address the service sends game intents it could not start on
  /// the cloud console to; such an intent ends as an error.
  static constexpr std::uint32_t game_intent_address = 6;
  /// @brief The address of system notifications such as trophies.
  static constexpr std::uint32_t notification_address = 7;
  /// @brief The address of persistent banners.
  static constexpr std::uint32_t banner_address = 8;
  /// @brief The address Create button presses are sent to.
  static constexpr std::uint32_t capture_request_address = 12;
  /// @brief The address capture results arrive on.
  static constexpr std::uint32_t capture_result_address = 13;
  /// @brief The banner type of a recording in progress.
  static constexpr std::string_view recording_banner_type = "ShareRecording";
  /// @brief The size of an address handshake body.
  static constexpr std::size_t sync_size = 0xfc;
  /// @brief The size of a Create press body.
  static constexpr std::size_t capture_size = 0x418;
  /// @brief The most addresses one handshake lists.
  static constexpr std::size_t max_addresses = 30;

  /// @brief One generic data transfer to send.
  struct Transfer {
    /// @brief The JSON gateway header.
    std::string metadata;
    /// @brief The body.
    std::vector<std::byte> data;

    /// @brief Compares two transfers.
    ///
    /// @return Whether both are the same.
    bool operator==(const Transfer&) const = default;
  };

  /// @brief An addressed message the service sent.
  struct Message {
    /// @brief The address it was sent to.
    std::uint32_t address{};
    /// @brief Its version.
    std::uint32_t version{};
    /// @brief Its request number.
    std::uint32_t id{};
    /// @brief Its body.
    std::vector<std::byte> body;
  };

  /// @brief A result the service sent for one of the client's messages.
  struct Result {
    /// @brief The address of the message it answers.
    std::uint32_t address{};
    /// @brief The request number it answers.
    std::uint32_t id{};
    /// @brief The result code; 0 for success.
    std::uint32_t result{};
  };

  /// @brief What one received transfer amounted to.
  struct Received {
    /// @brief Handshake replies to send.
    std::vector<Transfer> replies;
    /// @brief The message, when it was addressed to anything but the
    /// handshake.
    std::optional<Message> message;
    /// @brief The result, when it answered one of the client's messages.
    std::optional<Result> result;
  };

  /// @brief How a Create button press was made.
  enum class CapturePress : std::uint32_t {
    /// @brief One short press.
    single_tap = 8,
    /// @brief A long press.
    long_press = 9,
    /// @brief Two short presses.
    double_tap = 79,
  };

  /// @brief Prepares the messaging layer.
  ///
  /// @param receivers The addresses this client receives on, at most
  ///     max_addresses.
  explicit CloudMessaging(std::vector<std::uint32_t> receivers);

  /// @brief Builds the opening handshake listing this client's addresses.
  ///
  /// @return The transfer to send.
  auto open() -> Transfer;
  /// @brief Reads a received transfer.
  ///
  /// @param metadata The transfer's metadata.
  /// @param data The transfer's data.
  /// @return What it amounted to; empty when it was not a gateway message.
  auto receive(std::string_view metadata, std::span<const std::byte> data)
      -> Received;
  /// @brief Whether the service has said it receives on an address.
  ///
  /// @param address The address.
  /// @return True for the handshake address and for every address the
  ///     service listed.
  auto allows(std::uint32_t address) const noexcept -> bool;
  /// @brief Builds a message to an address, numbering it.
  ///
  /// @param address The address.
  /// @param version The message version.
  /// @param body The body; 1 to 32768 bytes.
  /// @return The transfer to send.
  auto message(std::uint32_t address, std::uint32_t version,
               std::vector<std::byte> body) -> Transfer;

  /// @brief Builds a Create button press body.
  ///
  /// @param account_id The signed-in PSN account.
  /// @param press How the button was pressed.
  /// @return The capture_size body.
  static auto capture_body(std::uint64_t account_id, CapturePress press)
      -> std::vector<std::byte>;
  /// @brief Reads a system notification sent to notification_address.
  ///
  /// @param body The message body: JSON naming a use case, with a trophy's
  ///     details inside its raw data.
  /// @return The notice; empty when the body is not JSON or names a use case
  ///     this library does not show.
  static auto notice(std::span<const std::byte> body)
      -> std::optional<CloudNotice>;
  /// @brief Reads a banner sent to banner_address.
  ///
  /// @param body The message body.
  /// @return The recording banner's state; empty for other banners.
  static auto recording_banner(std::span<const std::byte> body)
      -> std::optional<RecordingBanner>;
  /// @brief Builds a gateway header for a data message.
  ///
  /// @param address The address.
  /// @param version The message version.
  /// @param id The request number.
  /// @return The JSON text, its keys in a fixed order.
  static auto data_header(std::uint32_t address, std::uint32_t version,
                          std::uint32_t id) -> std::string;

 private:
  /// @brief The gateway message type of a data message.
  static constexpr std::uint32_t data_type = 1;
  /// @brief The gateway message type of a result.
  static constexpr std::uint32_t result_type = 2;
  /// @brief The handshake type that lists addresses.
  static constexpr std::uint32_t list_type = 1;
  /// @brief The handshake type that acknowledges the other side's addresses.
  static constexpr std::uint32_t ack_type = 2;
  /// @brief The largest body sent; longer bodies are cut.
  static constexpr std::size_t max_body = 0x8000;

  /// @brief Reads a body as JSON text.
  ///
  /// @param body The body.
  /// @return Its text without trailing NULs.
  static auto json_text(std::span<const std::byte> body) -> std::string_view;
  /// @brief Names the notice a use case shows.
  ///
  /// @param use_case The notification's use case id.
  /// @return The kind; empty for a use case this library does not show.
  static auto notice_kind(std::string_view use_case)
      -> std::optional<CloudNoticeKind>;
  /// @brief Reads a trophy grade.
  ///
  /// @param grade The grade as the service spells it.
  /// @return The grade; bronze for any other text.
  static auto trophy_grade(std::string_view grade) -> TrophyGrade;
  /// @brief Whether two address lists hold the same addresses in any order.
  ///
  /// @param left One list.
  /// @param right The other.
  /// @return True when both hold the same addresses.
  static auto same_addresses(std::vector<std::uint32_t> left,
                             std::vector<std::uint32_t> right) -> bool;
  /// @brief Builds a handshake transfer.
  ///
  /// @param type 1 to list addresses, 2 to acknowledge the other side's.
  /// @param addresses The addresses.
  /// @return The transfer.
  auto handshake(std::uint32_t type, std::span<const std::uint32_t> addresses)
      -> Transfer;
  /// @brief Reads a handshake body.
  ///
  /// @param data The body.
  /// @return Its type and addresses; empty when malformed.
  static auto handshake_of(std::span<const std::byte> data)
      -> std::optional<std::pair<std::uint32_t, std::vector<std::uint32_t>>>;

  /// @brief This client's addresses.
  std::vector<std::uint32_t> receivers_;
  /// @brief The service's addresses.
  std::vector<std::uint32_t> remote_;
  /// @brief The request number of the next message.
  std::uint32_t next_id_{1};
};

/// @brief Collects what a PS5 cloud console reports about its captures on
/// CloudMessaging::capture_result_address: chunked JPEG thumbnails, upload
/// details and progress events.
///
/// Each body starts with a 20-byte header: the account id, a constant, a kind
/// and a count. Kind 0 is a JSON progress event ("saveUgcId", "upload",
/// "complete"), kind 101 the upload details as JSON, and kind 8 one thumbnail
/// chunk: its 1-based index, JSON describing the capture, then the chunk's
/// length and bytes at a fixed place.
class CloudCaptures {
 public:
  /// @brief The kind of a progress event.
  static constexpr std::uint32_t event_kind = 0;
  /// @brief The kind of a thumbnail chunk.
  static constexpr std::uint32_t thumbnail_kind = 8;
  /// @brief The kind of the upload details.
  static constexpr std::uint32_t upload_kind = 101;
  /// @brief The size of the header before a body's own fields.
  static constexpr std::size_t header_size = 20;
  /// @brief Where a thumbnail chunk's data length is.
  static constexpr std::size_t chunk_length_at = 280;
  /// @brief Where a thumbnail chunk's data starts.
  static constexpr std::size_t chunk_data_at = 284;
  /// @brief The largest thumbnail accepted.
  static constexpr std::size_t max_thumbnail = 1U << 20U;
  /// @brief The most captures followed at once.
  static constexpr std::size_t max_pending = 8;

  /// @brief What one report completed.
  struct Received {
    /// @brief A capture whose thumbnail is now whole.
    std::optional<CloudCapture> thumbnail;
    /// @brief A capture whose upload finished.
    std::optional<CloudCaptureUpload> uploaded;
  };

  /// @brief Reads one report.
  ///
  /// @param body The message body.
  /// @return Whatever it completed; empty for partial or unknown reports.
  auto receive(std::span<const std::byte> body) -> Received;

 private:
  /// @brief A capture being followed.
  struct Pending {
    /// @brief The capture as known so far.
    CloudCapture capture;
    /// @brief How many thumbnail chunks it has.
    std::uint32_t chunks{};
    /// @brief The chunks received, by index.
    std::map<std::uint32_t, std::vector<std::byte>> parts;
    /// @brief The upload details, once sent.
    std::optional<CloudCaptureUpload> upload;
    /// @brief When it was first seen, to drop the oldest.
    std::uint64_t order{};
  };

  /// @brief Reads a fixed-size text field of a body.
  ///
  /// @param body The body.
  /// @param at Where the field starts.
  /// @param size The field's size; cut at the end of the body.
  /// @return The field's text up to its first NUL; empty when it starts past
  ///     the end.
  static auto field_text(std::span<const std::byte> body, std::size_t at,
                         std::size_t size) -> std::string_view;
  /// @brief Finds or starts following a capture, dropping the oldest when
  /// too many are followed.
  ///
  /// @param id The capture's id.
  /// @return Its record.
  auto follow(const std::string& id) -> Pending&;
  /// @brief Takes a thumbnail chunk.
  ///
  /// @param body The body.
  /// @param chunks How many chunks the thumbnail has.
  /// @return The capture when its thumbnail became whole.
  auto chunk(std::span<const std::byte> body, std::uint32_t chunks)
      -> std::optional<CloudCapture>;

  /// @brief The captures being followed, by id.
  std::map<std::string, Pending> pending_;
  /// @brief The next first-seen number.
  std::uint64_t next_order_{};
};

}  // namespace libchiaking::play
