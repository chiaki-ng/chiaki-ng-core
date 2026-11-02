/// @file
/// @brief Declares GameInvitation, an invitation to a game session that a
/// friend sent the user, and InvitationJoinFailure, why joining one failed.
#pragma once

#include "libchiaking/detail/users/ps_user.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace libchiaking::consoles {

/// @brief An invitation to a game session or a party, read from the
/// account's PSN notifications.
///
/// @see KnownConsoles::game_invitations
/// @ingroup api_consoles
struct GameInvitation {
  /// @brief A point in time to the millisecond, on the system clock.
  using Time = std::chrono::sys_time<std::chrono::milliseconds>;

  /// @brief What the invitation is to.
  enum class Kind : std::uint8_t {
    /// @brief A game session, joined with the game.
    game,
    /// @brief A party, which names no game and is joined on a home PS5 with
    /// KnownConsoles::join_party_on; PSN forbids it on a cloud console.
    party,
  };

  /// @brief Whether the session can be joined now, judged from its players,
  /// its size and whether it offers a way to join.
  enum class Joining : std::uint8_t {
    /// @brief The session has room and the user is not in it.
    joinable,
    /// @brief The user already plays in the session.
    already_joined,
    /// @brief The session has as many players as it allows.
    full,
    /// @brief The session has ended or gives no way to join it.
    cannot_join,
  };

  /// @brief What the invitation is to.
  Kind kind{Kind::game};
  /// @brief The session manager's id of the game session or party.
  std::string session_id;
  /// @brief The invitation's id, which a PS5 cloud session joins it with;
  /// empty when the notification did not carry one.
  std::string invitation_id;
  /// @brief The `notificationId`s of every notification of the account's
  /// list that names this session, which KnownConsoles::dismiss_invitations
  /// clears.
  std::vector<std::string> notification_ids;
  /// @brief The online id of the friend who sent it; empty when PSN did not
  /// say.
  std::string inviter;
  /// @brief The session's name as the game set it; empty when it has none.
  std::string session_name;
  /// @brief The games the session plays, such as `PPSA01234_00`, in PSN's
  /// order.
  std::vector<std::string> title_ids;
  /// @brief When the invitation was last updated; the epoch when PSN gave no
  /// readable time.
  Time sent{};
  /// @brief Whether the user has not looked at it yet.
  bool unread{};
  /// @brief The players in the session.
  std::uint32_t players{};
  /// @brief The most players the session allows; zero when PSN did not say.
  std::uint32_t max_players{};
  /// @brief Whether it can be joined now: joinable as long as the invitation
  /// has an id, unless a session read says otherwise.
  Joining joining{Joining::cannot_join};

  /// @brief Compares every field.
  friend auto operator==(const GameInvitation&, const GameInvitation&)
      -> bool = default;
};

/// @brief A message a friend sent the user, as PSN announces it. PSN's
/// announcement carries no text.
/// @ingroup api_consoles
struct MessageAlert {
  /// @brief The sender's online id.
  std::string sender;
  /// @brief The message group the message was sent in.
  std::string group_id;
  /// @brief The thread it was sent in, which messages::Messages reads and
  /// sends in; empty when the push named none.
  std::string thread_id;

  /// @brief Compares every field.
  friend auto operator==(const MessageAlert&, const MessageAlert&)
      -> bool = default;
};

/// @brief An invitation to a friend's party, as PSN pushes it. Party
/// invitations name no title; the account's list holds them as
/// GameInvitation::Kind::party a moment after the push, if at all.
/// @ingroup api_consoles
struct PartyInvitationAlert {
  /// @brief The inviter's online id.
  std::string sender;
  /// @brief The party session.
  std::string session_id;

  /// @brief Compares every field.
  friend auto operator==(const PartyInvitationAlert&,
                         const PartyInvitationAlert&) -> bool = default;
};

/// @brief Told of what arrives for the user while KnownConsoles::watch_alerts
/// runs.
/// @ingroup api_consoles
class AlertWatcher {
 public:
  /// @brief Allows deletion through the interface.
  virtual ~AlertWatcher() = default;
  /// @brief Called on the watching thread for each unread invitation that was
  /// not listed before.
  ///
  /// @param invitation The invitation. Valid for the duration of the call;
  ///     copy what is needed.
  virtual void on_invitation(const GameInvitation& invitation) = 0;
  /// @brief Called on the watching thread for each message someone else sent
  /// the user.
  ///
  /// @param message The message. Valid for the duration of the call; copy
  ///     what is needed.
  virtual void on_message(const MessageAlert& message) = 0;
  /// @brief Called on the watching thread once for each party invitation
  /// pushed to the user, or listed unread after the watch started.
  ///
  /// @param party The invitation. Valid for the duration of the call; copy
  ///     what is needed.
  virtual void on_party_invitation(const PartyInvitationAlert& party) = 0;
};

/// @brief Why joining an invitation or a party failed.
///
/// @see KnownConsoles::join_in_cloud
/// @see KnownConsoles::join_party_on
/// @ingroup api_consoles
struct InvitationJoinFailure {
  /// @brief The kinds of failure.
  enum class Reason : std::uint8_t {
    /// @brief The session id is not one the session manager uses.
    invalid_session,
    /// @brief The session has ended.
    session_ended,
    /// @brief The user already plays in the session.
    already_joined,
    /// @brief The session is full.
    full,
    /// @brief The session gives no way to join it.
    cannot_join,
    /// @brief PSN listed no PS5 cloud console for the account within a
    /// minute, which happens when no cloud stream is running.
    no_cloud_console,
    /// @brief The home console cannot take the command: it was forgotten, is
    /// a PS4, holds no pairing for the user or is not on the account.
    console_unavailable,
    /// @brief PSN rejected the join with a client error other than an
    /// authorisation refusal.
    refused,
    /// @brief The console reported that it could not join.
    failed,
    /// @brief The console did not report the join finished in time.
    timed_out,
    /// @brief The stop token was triggered.
    cancelled,
    /// @brief PSN did not answer; see InvitationJoinFailure::unanswered.
    unanswered,
  };

  /// @brief What went wrong.
  Reason reason{Reason::unanswered};
  /// @brief PSN's failure, when one was involved.
  std::optional<users::Unanswered> unanswered{};
};

}  // namespace libchiaking::consoles
