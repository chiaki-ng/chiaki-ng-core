/// @file
/// @brief Declares CloudNotice, RecordingBanner, CloudCapture and
/// CloudCaptureUpload, what a PS5 cloud service reports for the frontend to
/// show over the game.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace libchiaking::play {

/// @brief What a cloud notice is about.
enum class CloudNoticeKind : std::uint8_t {
  /// @brief A screenshot was saved.
  screenshot_saved,
  /// @brief The game does not allow a screenshot here.
  screenshot_refused,
  /// @brief A video clip was saved.
  recording_saved,
  /// @brief The game does not allow recording here.
  recording_refused,
  /// @brief Recording paused because the game entered a scene it may not
  /// record.
  recording_paused,
  /// @brief Recording resumed after such a scene.
  recording_resumed,
  /// @brief The recording reached the longest clip the service keeps.
  recording_limit_reached,
  /// @brief The service is uploading captures and cannot take another yet.
  captures_uploading,
  /// @brief A trophy was earned; see CloudNotice::grade, title and detail.
  trophy_earned,
  /// @brief A game trial is running out; CloudNotice::title is PS's text,
  /// such as "You have 5 minutes remaining on your game trial."
  trial_time_left,
};

/// @brief The grade of an earned trophy.
enum class TrophyGrade : std::uint8_t {
  /// @brief Bronze.
  bronze,
  /// @brief Silver.
  silver,
  /// @brief Gold.
  gold,
  /// @brief Platinum.
  platinum,
};

/// @brief A system notification from a PS5 cloud service.
///
/// The service sends one when the console behind the stream would show a
/// toast: a capture saved or refused, or a trophy earned.
///
/// @see SessionEvents::on_cloud_notice
struct CloudNotice {
  /// @brief What it is about.
  CloudNoticeKind kind{CloudNoticeKind::screenshot_saved};
  /// @brief The trophy's grade; bronze for other kinds.
  TrophyGrade grade{TrophyGrade::bronze};
  /// @brief The trophy's name; empty for other kinds.
  std::string title;
  /// @brief The trophy's description; empty for other kinds.
  std::string detail;

  /// @brief Compares every field.
  ///
  /// @return Whether both are the same.
  bool operator==(const CloudNotice&) const = default;
};

/// @brief The state of the banner a PS5 cloud service keeps up while it
/// records a video clip.
///
/// @see SessionEvents::on_recording_banner
struct RecordingBanner {
  /// @brief Whether a clip is being recorded.
  bool recording{};
  /// @brief The time recorded so far as the service writes it, such as
  /// "00:10"; empty when not recording.
  std::string elapsed;

  /// @brief Compares every field.
  ///
  /// @return Whether both are the same.
  bool operator==(const RecordingBanner&) const = default;
};

/// @brief A screenshot or clip the cloud console saved, with the small JPEG
/// thumbnail it sent over the stream.
///
/// @see SessionEvents::on_capture_thumbnail
struct CloudCapture {
  /// @brief The console's id for the capture, as later reports name it.
  std::string id;
  /// @brief Whether it is a video clip rather than a screenshot.
  bool video{};
  /// @brief When it was taken, as an ISO 8601 UTC time.
  std::string created;
  /// @brief The thumbnail, a JPEG image.
  std::vector<std::byte> thumbnail;

  /// @brief Compares every field.
  ///
  /// @return Whether both are the same.
  bool operator==(const CloudCapture&) const = default;
};

/// @brief A capture the cloud console finished uploading to the account's
/// cloud media gallery, from which a frontend can download it.
///
/// @see SessionEvents::on_capture_uploaded
struct CloudCaptureUpload {
  /// @brief The console's id for the capture, as in CloudCapture::id.
  std::string id;
  /// @brief The capture's id in the cloud media gallery.
  std::string gallery_id;
  /// @brief The file extension, such as ".jpg".
  std::string extension;
  /// @brief The file size in bytes.
  std::uint64_t size{};
  /// @brief Whether it is a video clip rather than a screenshot.
  bool video{};
  /// @brief When it was taken, as an ISO 8601 UTC time.
  std::string created;

  /// @brief Compares every field.
  ///
  /// @return Whether both are the same.
  bool operator==(const CloudCaptureUpload&) const = default;
};

}  // namespace libchiaking::play
