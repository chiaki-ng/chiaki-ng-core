/// @file
/// @brief Declares the progress vocabulary shared by pairing, connecting and
/// cloud launch reports.
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace libchiaking::progress {

/// @brief Why an operation is paused instead of advancing.
///
/// A frontend uses it to tell the user what is being waited for.
enum class WaitReason : std::uint8_t {
  /// @brief A cloud session the account had open is still closing.
  previous_session_closing,
  /// @brief The account is queued for a cloud server.
  server_queue,
  /// @brief The cloud service is migrating the account's data before it can
  /// allocate a server.
  data_migration,
  /// @brief The frontend has been offered cloud datacenters and the user has
  /// not chosen yet.
  datacenter_choice,
  /// @brief The cloud title is not ready to start yet.
  title_not_ready,
  /// @brief The network cannot be reached at the moment.
  network_unreachable,
  /// @brief The console was sent a wake signal and is starting up.
  console_waking,
  /// @brief Waiting for the console to join the session over the internet.
  console_joining,
  /// @brief Waiting for the console to answer a pairing request.
  console_answering,
  /// @brief Resting between connection attempts.
  connection_retry,
  /// @brief The account's last cloud stream ended moments ago, and PS still
  /// refuses a new one until it has finished closing it.
  recent_cloud_session,
};

/// @brief A place in a counted sequence, such as step 3 of 9.
struct Position {
  /// @brief The current place.
  std::uint32_t number{};
  /// @brief The total the place is counted against.
  std::uint32_t of{};

  /// @brief Compares two positions field by field.
  friend constexpr auto operator==(const Position&, const Position&) noexcept
      -> bool = default;
};

/// @brief A pause in an operation, with its cause and, when known, its length.
struct Wait {
  /// @brief What is being waited for.
  WaitReason reason{WaitReason::connection_retry};
  /// @brief Time until the operation acts again, when the wait has a known
  /// end.
  std::optional<std::chrono::milliseconds> next_try_in{};

  /// @brief Compares two waits field by field.
  friend constexpr auto operator==(const Wait&, const Wait&) noexcept
      -> bool = default;
};

/// @brief The common summary every progress report can be reduced to.
///
/// Pairing, connection and cloud launch reports each offer a `status ()` that
/// returns one, so a frontend can render them all with one progress view.
struct Status {
  /// @brief The step reached out of the total, for operations with counted
  /// steps.
  std::optional<Position> position{};
  /// @brief The attempt in progress, counted from 1.
  std::uint32_t attempt{1};
  /// @brief Time spent so far, when the operation measures it.
  std::optional<std::chrono::microseconds> elapsed{};
  /// @brief The current pause, absent while the operation is advancing.
  std::optional<Wait> wait{};

  /// @brief Tells whether the operation is paused.
  /// @return True when wait is present.
  constexpr auto is_waiting() const noexcept -> bool {
    return wait.has_value();
  }

  /// @brief Compares two statuses field by field.
  friend constexpr auto operator==(const Status&, const Status&) noexcept
      -> bool = default;
};

}  // namespace libchiaking::progress
