#include <libchiaking/consoles_types.hpp>
#include <libchiaking/play_types.hpp>

#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <type_traits>

namespace play_test {
namespace {

namespace progress = libchiaking::progress;
using libchiaking::play::LaunchProgress;
using progress::Position;
using progress::Status;
using progress::Wait;
using progress::WaitReason;
using Route = progress::InternetRoute;

static_assert(std::is_same_v<LaunchProgress::Step, Position>);

constexpr Status fresh;
static_assert(fresh.attempt == 1 && !fresh.position && !fresh.elapsed &&
              !fresh.wait && !fresh.is_waiting());
constexpr Status waiting{
    Position{2, 5}, 3, std::chrono::microseconds{7},
    Wait{WaitReason::server_queue, std::chrono::seconds{15}}};
static_assert(waiting.is_waiting() &&
              waiting.wait->next_try_in == std::chrono::milliseconds{15'000});
static_assert(waiting != fresh && waiting == Status{waiting});
constexpr Wait unknown{WaitReason::console_joining};
static_assert(!unknown.next_try_in);
static_assert(unknown !=
              Wait{WaitReason::console_joining, std::chrono::milliseconds{0}});
static_assert((Position{1, 2} != Position{2, 2}));

constexpr Route opening;
static_assert(opening.step == Route::Step::opening_account_session &&
              opening.purpose == Route::Purpose::control);
static_assert(!opening.candidates && !opening.chosen);
constexpr Route::Candidates offered{1, 2, 75};
static_assert(offered.total() == 78 && Route::Candidates{}.total() == 0);
constexpr Route chosen{Route::Step::route_chosen, Route::Purpose::stream,
                       std::nullopt, Route::Kind::local};
static_assert(chosen.chosen == Route::Kind::local && chosen != opening);
static_assert((Route{Route::Step::route_chosen, Route::Purpose::stream,
                     std::nullopt, Route::Kind::discovered} != chosen));

static_assert(libchiaking::consoles::PairingStep::starting <
              libchiaking::consoles::PairingStep::opening_account_session);
static_assert(libchiaking::consoles::PairingStep::reaching_console <
              libchiaking::consoles::PairingStep::sending_request);
static_assert(libchiaking::consoles::PairingStep::sending_request <
              libchiaking::consoles::PairingStep::waiting_for_answer);
static_assert(libchiaking::consoles::PairingStep::waiting_for_answer <
              libchiaking::consoles::PairingStep::saving);

}  // namespace
}  // namespace play_test
