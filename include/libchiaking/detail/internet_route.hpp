/// @file
/// @brief Declares progress::InternetRoute, the progress report for finding a
/// route to a console over the internet.
#pragma once

#include <cstdint>
#include <optional>

namespace libchiaking::progress {

/// @brief One step of finding a direct network route to a console that is not
/// on the local network.
///
/// Reported inside connection and pairing progress while the library and the
/// console discover each other through the account's PlayStation Network
/// session and test addresses until one works. The search runs once for the
/// control connection and again for the stream; purpose says which.
struct InternetRoute {
  /// @brief Where the route search has got to, in the order the steps happen.
  enum class Step : std::uint8_t {
    /// @brief Opening the PlayStation Network session through which the
    /// console is reached.
    opening_account_session,
    /// @brief Waiting for the console to join that session.
    waiting_for_console,
    /// @brief Collecting this device's own candidate addresses.
    gathering_candidates,
    /// @brief Sending this device's candidates to the console and receiving
    /// the console's.
    exchanging_offers,
    /// @brief Probing the console's candidates to find one that answers.
    testing_candidates,
    /// @brief A candidate answered and was picked; chosen says what kind.
    route_chosen,
    /// @brief Confirming the picked route with the console.
    confirming_route,
    /// @brief Switching the control connection over to the newly found
    /// route.
    switching_control,
  };

  /// @brief Which connection the route is being found for.
  enum class Purpose : std::uint8_t {
    /// @brief The control connection, set up first.
    control,
    /// @brief The connection that carries the stream.
    stream,
  };

  /// @brief The kind of address the chosen route uses.
  enum class Kind : std::uint8_t {
    /// @brief An address on the console's local network.
    local,
    /// @brief A public address the console offered.
    public_address,
    /// @brief A public address whose port was guessed instead of known.
    port_guessed,
    /// @brief An address that was not offered as a candidate and was learnt
    /// while testing.
    discovered,
  };

  /// @brief How many candidate addresses of each kind are in play at this
  /// step.
  ///
  /// At Step::exchanging_offers these are this device's own candidates; at
  /// Step::testing_candidates they are the console's.
  struct Candidates {
    /// @brief Candidates on a local network.
    std::uint32_t local{};
    /// @brief Candidates at a public address with a known port.
    std::uint32_t public_address{};
    /// @brief Candidates at a public address whose port is a guess.
    std::uint32_t port_guessed{};

    /// @brief The number of candidates of all kinds.
    /// @return The sum of the three counts.
    constexpr auto total() const noexcept -> std::uint32_t {
      return local + public_address + port_guessed;
    }

    /// @brief Compares two counts field by field.
    friend constexpr auto operator==(const Candidates&,
                                     const Candidates&) noexcept
        -> bool = default;
  };

  /// @brief The step that has just begun.
  Step step{Step::opening_account_session};
  /// @brief The connection this search is for.
  Purpose purpose{Purpose::control};
  /// @brief Candidate counts, present at Step::exchanging_offers and
  /// Step::testing_candidates.
  std::optional<Candidates> candidates{};
  /// @brief The kind of route picked, present at Step::route_chosen and
  /// Step::confirming_route.
  std::optional<Kind> chosen{};

  /// @brief Compares two reports field by field.
  friend constexpr auto operator==(const InternetRoute&,
                                   const InternetRoute&) noexcept
      -> bool = default;
};

}  // namespace libchiaking::progress
