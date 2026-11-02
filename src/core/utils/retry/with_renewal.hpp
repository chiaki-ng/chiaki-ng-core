/// @file
/// @brief Declares with_renewal, the ask, renew the credential, ask again
/// pattern used when PS refuses a token that may merely have expired.
#pragma once

#include <libchiaking/detail/expected.hpp>

#include <type_traits>

namespace libchiaking::retry {

/// @brief Asks once and, when the refusal is one that a renewed credential
/// could cure, renews it and asks exactly once more.
///
/// `ask` runs once; when it succeeds, or fails in a way `refused` does not
/// accept, its reply is the answer. Otherwise `renew` runs with the failure
/// and, when it succeeds, `ask` runs a second time and that reply is the
/// answer, whatever it is. There is never a third attempt. Everything runs
/// on the calling thread and blocks for as long as the callables do.
///
/// @tparam Ask A callable taking no arguments and returning an Expected.
/// @tparam Refused A predicate on the ask's error type.
/// @tparam Renew A callable taking the ask's error and returning
///     `Expected<void, F>` for some failure type `F`.
/// @param ask Makes the request with the current credential; it must read
///     the credential afresh each time so that the renewed one is used.
/// @param refused Says whether a failure means the credential was refused
///     and renewal is worth trying.
/// @param renew Renews the credential, given the refusal that prompted it.
/// @return The final reply of `ask`, success or failure, wrapped as the
///     value.
/// @retval F The renewal failed; `ask` was not tried again.
template <class Ask, class Refused, class Renew>
[[nodiscard]] auto with_renewal(const Ask& ask, const Refused& refused,
                                const Renew& renew)
    -> Expected<std::invoke_result_t<const Ask&>,
                typename std::invoke_result_t<
                    const Renew&, const typename std::invoke_result_t<
                                      const Ask&>::error_type&>::error_type> {
  auto reply = ask();
  if (reply || !refused(reply.error())) return reply;
  return renew(reply.error()).transform([&] { return ask(); });
}

}  // namespace libchiaking::retry
