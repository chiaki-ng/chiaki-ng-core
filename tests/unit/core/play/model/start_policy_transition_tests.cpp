#include "src/core/play/model/start_policy.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using libchiaking::play::ConsoleHandover;
using libchiaking::play::ConsoleQuit;
using libchiaking::play::EndReason;
using libchiaking::play::StartPolicy;
using libchiaking::play::Where;
using Failure = StartPolicy::Failure;
using Cause = EndReason::Cause;
using namespace std::chrono_literals;

class StartPolicyTransitions : public ::testing::Test {
 protected:
  struct Row {
    std::string_view name;
    Where where;
    ConsoleHandover handover;
    bool reconnect;
  };

  static auto failures() -> std::vector<std::pair<std::string_view, Failure>> {
    return {
        {"lost", Failure{Cause::connection_lost}},
        {"refused", Failure{Cause::session_request_connection_refused}},
        {"in_use", Failure{Cause::session_request_in_use}},
        {"timeout", Failure{Cause::session_request_timeout}},
        {"left", Failure{Cause::connection_lost, std::nullopt, true}},
        {"offline", Failure{Cause::connection_lost, std::nullopt, false,
                            std::nullopt, std::nullopt, 10s}},
        {"played", Failure{Cause::connection_lost, std::nullopt, false, 5s}},
        {"played_long",
         Failure{Cause::connection_lost, std::nullopt, false, 120s}},
        {"played_refused", Failure{Cause::session_request_connection_refused,
                                   std::nullopt, false, 5s}},
        {"played_in_use",
         Failure{Cause::session_request_in_use, std::nullopt, false, 5s}},
        {"quit_refused",
         Failure{std::nullopt,
                 ConsoleQuit::Reason::session_request_connection_refused}},
        {"disconnected_in_trouble",
         Failure{std::nullopt, ConsoleQuit::Reason::console_disconnected, false,
                 5s, 3s}},
    };
  }

  static auto rows() -> std::vector<Row> {
    return {
        {"home", Where::home, ConsoleHandover::fresh, true},
        {"home_handover", Where::home, ConsoleHandover::just_left, true},
        {"home_no_reconnect", Where::home, ConsoleHandover::fresh, false},
        {"internet", Where::internet, ConsoleHandover::fresh, true},
        {"internet_handover", Where::internet, ConsoleHandover::just_left,
         true},
        {"cloud", Where::cloud, ConsoleHandover::just_left, true},
    };
  }

  static auto paired(std::string_view name) -> bool {
    constexpr std::array<std::string_view, 6> names{
        "lost", "refused", "in_use", "played", "played_long", "offline"};
    return std::ranges::find(names, name) != names.end();
  }

  static auto letter(StartPolicy::Next next) -> char {
    switch (next) {
      case StartPolicy::Next::give_up:
        return 'g';
      case StartPolicy::Next::try_again:
        return 't';
      case StartPolicy::Next::wake_then_try_again:
        return 'w';
    }
    return '?';
  }

  static auto run(const Row& row, const std::vector<Failure>& sequence)
      -> std::string {
    StartPolicy policy{row.where, row.handover, row.reconnect};
    std::string trace;
    for (const auto& failure : sequence)
      trace +=
          std::format("{}{} ", letter(policy.after(failure)), policy.attempt());
    return trace;
  }

  static auto pins()
      -> std::vector<std::pair<std::string_view, std::string_view>> {
    return {
        {"home.lost", "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"home.refused",
         "w2 t3 t4 w5 t6 t7 w8 t9 t10 w11 t12 t13 g13 g13 g13 g13 "},
        {"home.in_use", "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"home.timeout",
         "w2 t3 t4 t5 t6 t7 t8 t9 t10 t11 t12 t13 g13 g13 g13 g13 "},
        {"home.left", "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"home.offline", "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"home.played",
         "w2 t3 t4 t5 t6 t7 t8 t9 t10 t11 t12 t13 g13 g13 g13 g13 "},
        {"home.played_long",
         "w2 t3 t4 t5 t6 t7 t8 t9 t10 t11 t12 t13 t14 t15 t16 t17 "},
        {"home.played_refused",
         "w2 t3 t4 w5 t6 t7 w8 t9 t10 w11 t12 t13 g13 g13 g13 g13 "},
        {"home.played_in_use",
         "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"home.quit_refused",
         "w2 t3 t4 w5 t6 t7 w8 t9 t10 w11 t12 t13 g13 g13 g13 g13 "},
        {"home.disconnected_in_trouble",
         "w2 t3 t4 t5 t6 t7 t8 t9 t10 t11 t12 t13 g13 g13 g13 g13 "},
        {"home.lost+lost", "g1 g1 g1 g1 g1 g1 "},
        {"home.lost+refused", "g1 g1 g1 g1 g1 g1 "},
        {"home.lost+in_use", "g1 g1 g1 g1 g1 g1 "},
        {"home.lost+offline", "g1 g1 g1 g1 g1 g1 "},
        {"home.lost+played", "g1 g1 g1 g1 g1 g1 "},
        {"home.lost+played_long", "g1 g1 g1 g1 g1 g1 "},
        {"home.refused+lost", "w2 g2 g2 g2 g2 g2 "},
        {"home.refused+refused", "w2 t3 t4 w5 t6 t7 "},
        {"home.refused+in_use", "w2 g2 g2 g2 g2 g2 "},
        {"home.refused+offline", "w2 g2 g2 g2 g2 g2 "},
        {"home.refused+played", "w2 t3 t4 t5 t6 t7 "},
        {"home.refused+played_long", "w2 t3 t4 t5 t6 t7 "},
        {"home.in_use+lost", "g1 g1 g1 g1 g1 g1 "},
        {"home.in_use+refused", "g1 g1 g1 g1 g1 g1 "},
        {"home.in_use+in_use", "g1 g1 g1 g1 g1 g1 "},
        {"home.in_use+offline", "g1 g1 g1 g1 g1 g1 "},
        {"home.in_use+played", "g1 g1 g1 g1 g1 g1 "},
        {"home.in_use+played_long", "g1 g1 g1 g1 g1 g1 "},
        {"home.offline+lost", "g1 g1 g1 g1 g1 g1 "},
        {"home.offline+refused", "g1 g1 g1 g1 g1 g1 "},
        {"home.offline+in_use", "g1 g1 g1 g1 g1 g1 "},
        {"home.offline+offline", "g1 g1 g1 g1 g1 g1 "},
        {"home.offline+played", "g1 g1 g1 g1 g1 g1 "},
        {"home.offline+played_long", "g1 g1 g1 g1 g1 g1 "},
        {"home.played+lost", "w2 t3 t4 t5 t6 t7 "},
        {"home.played+refused", "w2 t3 t4 w5 t6 t7 "},
        {"home.played+in_use", "w2 t3 t4 t5 t6 t7 "},
        {"home.played+offline", "w2 t3 t4 t5 t6 t7 "},
        {"home.played+played", "w2 t3 t4 t5 t6 t7 "},
        {"home.played+played_long", "w2 t3 t4 t5 t6 t7 "},
        {"home.played_long+lost", "w2 t3 t4 t5 t6 t7 "},
        {"home.played_long+refused", "w2 t3 t4 t5 t6 t7 "},
        {"home.played_long+in_use", "w2 t3 t4 t5 t6 t7 "},
        {"home.played_long+offline", "w2 t3 t4 t5 t6 t7 "},
        {"home.played_long+played", "w2 t3 t4 t5 t6 t7 "},
        {"home.played_long+played_long", "w2 t3 t4 t5 t6 t7 "},
        {"home_handover.lost",
         "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"home_handover.refused",
         "w2 t3 t4 w5 t6 t7 w8 t9 t10 w11 t12 t13 g13 g13 g13 g13 "},
        {"home_handover.in_use",
         "t2 t3 t4 t5 t6 t7 g7 g7 g7 g7 g7 g7 g7 g7 g7 g7 "},
        {"home_handover.timeout",
         "w2 t3 t4 t5 t6 t7 t8 t9 t10 t11 t12 t13 g13 g13 g13 g13 "},
        {"home_handover.left",
         "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"home_handover.offline",
         "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"home_handover.played",
         "w2 t3 t4 t5 t6 t7 t8 t9 t10 t11 t12 t13 g13 g13 g13 g13 "},
        {"home_handover.played_long",
         "w2 t3 t4 t5 t6 t7 t8 t9 t10 t11 t12 t13 t14 t15 t16 t17 "},
        {"home_handover.played_refused",
         "w2 t3 t4 w5 t6 t7 w8 t9 t10 w11 t12 t13 g13 g13 g13 g13 "},
        {"home_handover.played_in_use",
         "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"home_handover.quit_refused",
         "w2 t3 t4 w5 t6 t7 w8 t9 t10 w11 t12 t13 g13 g13 g13 g13 "},
        {"home_handover.disconnected_in_trouble",
         "w2 t3 t4 t5 t6 t7 t8 t9 t10 t11 t12 t13 g13 g13 g13 g13 "},
        {"home_handover.lost+lost", "g1 g1 g1 g1 g1 g1 "},
        {"home_handover.lost+refused", "g1 g1 g1 g1 g1 g1 "},
        {"home_handover.lost+in_use", "g1 g1 g1 g1 g1 g1 "},
        {"home_handover.lost+offline", "g1 g1 g1 g1 g1 g1 "},
        {"home_handover.lost+played", "g1 g1 g1 g1 g1 g1 "},
        {"home_handover.lost+played_long", "g1 g1 g1 g1 g1 g1 "},
        {"home_handover.refused+lost", "w2 g2 g2 g2 g2 g2 "},
        {"home_handover.refused+refused", "w2 t3 t4 w5 t6 t7 "},
        {"home_handover.refused+in_use", "w2 t3 t4 t5 t6 t7 "},
        {"home_handover.refused+offline", "w2 g2 g2 g2 g2 g2 "},
        {"home_handover.refused+played", "w2 t3 t4 t5 t6 t7 "},
        {"home_handover.refused+played_long", "w2 t3 t4 t5 t6 t7 "},
        {"home_handover.in_use+lost", "t2 g2 g2 g2 g2 g2 "},
        {"home_handover.in_use+refused", "t2 w3 t4 t5 t6 t7 "},
        {"home_handover.in_use+in_use", "t2 t3 t4 t5 t6 t7 "},
        {"home_handover.in_use+offline", "t2 g2 g2 g2 g2 g2 "},
        {"home_handover.in_use+played", "t2 w3 t4 t5 t6 t7 "},
        {"home_handover.in_use+played_long", "t2 w3 t4 t5 t6 t7 "},
        {"home_handover.offline+lost", "g1 g1 g1 g1 g1 g1 "},
        {"home_handover.offline+refused", "g1 g1 g1 g1 g1 g1 "},
        {"home_handover.offline+in_use", "g1 g1 g1 g1 g1 g1 "},
        {"home_handover.offline+offline", "g1 g1 g1 g1 g1 g1 "},
        {"home_handover.offline+played", "g1 g1 g1 g1 g1 g1 "},
        {"home_handover.offline+played_long", "g1 g1 g1 g1 g1 g1 "},
        {"home_handover.played+lost", "w2 t3 t4 t5 t6 t7 "},
        {"home_handover.played+refused", "w2 t3 t4 w5 t6 t7 "},
        {"home_handover.played+in_use", "w2 t3 t4 t5 t6 t7 "},
        {"home_handover.played+offline", "w2 t3 t4 t5 t6 t7 "},
        {"home_handover.played+played", "w2 t3 t4 t5 t6 t7 "},
        {"home_handover.played+played_long", "w2 t3 t4 t5 t6 t7 "},
        {"home_handover.played_long+lost", "w2 t3 t4 t5 t6 t7 "},
        {"home_handover.played_long+refused", "w2 t3 t4 t5 t6 t7 "},
        {"home_handover.played_long+in_use", "w2 t3 t4 t5 t6 t7 "},
        {"home_handover.played_long+offline", "w2 t3 t4 t5 t6 t7 "},
        {"home_handover.played_long+played", "w2 t3 t4 t5 t6 t7 "},
        {"home_handover.played_long+played_long", "w2 t3 t4 t5 t6 t7 "},
        {"home_no_reconnect.lost",
         "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"home_no_reconnect.refused",
         "w2 t3 t4 w5 t6 t7 w8 t9 t10 w11 t12 t13 g13 g13 g13 g13 "},
        {"home_no_reconnect.in_use",
         "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"home_no_reconnect.timeout",
         "w2 t3 t4 t5 t6 t7 t8 t9 t10 t11 t12 t13 g13 g13 g13 g13 "},
        {"home_no_reconnect.left",
         "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"home_no_reconnect.offline",
         "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"home_no_reconnect.played",
         "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"home_no_reconnect.played_long",
         "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"home_no_reconnect.played_refused",
         "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"home_no_reconnect.played_in_use",
         "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"home_no_reconnect.quit_refused",
         "w2 t3 t4 w5 t6 t7 w8 t9 t10 w11 t12 t13 g13 g13 g13 g13 "},
        {"home_no_reconnect.disconnected_in_trouble",
         "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"home_no_reconnect.lost+lost", "g1 g1 g1 g1 g1 g1 "},
        {"home_no_reconnect.lost+refused", "g1 g1 g1 g1 g1 g1 "},
        {"home_no_reconnect.lost+in_use", "g1 g1 g1 g1 g1 g1 "},
        {"home_no_reconnect.lost+offline", "g1 g1 g1 g1 g1 g1 "},
        {"home_no_reconnect.lost+played", "g1 g1 g1 g1 g1 g1 "},
        {"home_no_reconnect.lost+played_long", "g1 g1 g1 g1 g1 g1 "},
        {"home_no_reconnect.refused+lost", "w2 g2 g2 g2 g2 g2 "},
        {"home_no_reconnect.refused+refused", "w2 t3 t4 w5 t6 t7 "},
        {"home_no_reconnect.refused+in_use", "w2 g2 g2 g2 g2 g2 "},
        {"home_no_reconnect.refused+offline", "w2 g2 g2 g2 g2 g2 "},
        {"home_no_reconnect.refused+played", "w2 g2 g2 g2 g2 g2 "},
        {"home_no_reconnect.refused+played_long", "w2 g2 g2 g2 g2 g2 "},
        {"home_no_reconnect.in_use+lost", "g1 g1 g1 g1 g1 g1 "},
        {"home_no_reconnect.in_use+refused", "g1 g1 g1 g1 g1 g1 "},
        {"home_no_reconnect.in_use+in_use", "g1 g1 g1 g1 g1 g1 "},
        {"home_no_reconnect.in_use+offline", "g1 g1 g1 g1 g1 g1 "},
        {"home_no_reconnect.in_use+played", "g1 g1 g1 g1 g1 g1 "},
        {"home_no_reconnect.in_use+played_long", "g1 g1 g1 g1 g1 g1 "},
        {"home_no_reconnect.offline+lost", "g1 g1 g1 g1 g1 g1 "},
        {"home_no_reconnect.offline+refused", "g1 g1 g1 g1 g1 g1 "},
        {"home_no_reconnect.offline+in_use", "g1 g1 g1 g1 g1 g1 "},
        {"home_no_reconnect.offline+offline", "g1 g1 g1 g1 g1 g1 "},
        {"home_no_reconnect.offline+played", "g1 g1 g1 g1 g1 g1 "},
        {"home_no_reconnect.offline+played_long", "g1 g1 g1 g1 g1 g1 "},
        {"home_no_reconnect.played+lost", "g1 g1 g1 g1 g1 g1 "},
        {"home_no_reconnect.played+refused", "g1 g1 g1 g1 g1 g1 "},
        {"home_no_reconnect.played+in_use", "g1 g1 g1 g1 g1 g1 "},
        {"home_no_reconnect.played+offline", "g1 g1 g1 g1 g1 g1 "},
        {"home_no_reconnect.played+played", "g1 g1 g1 g1 g1 g1 "},
        {"home_no_reconnect.played+played_long", "g1 g1 g1 g1 g1 g1 "},
        {"home_no_reconnect.played_long+lost", "g1 g1 g1 g1 g1 g1 "},
        {"home_no_reconnect.played_long+refused", "g1 g1 g1 g1 g1 g1 "},
        {"home_no_reconnect.played_long+in_use", "g1 g1 g1 g1 g1 g1 "},
        {"home_no_reconnect.played_long+offline", "g1 g1 g1 g1 g1 g1 "},
        {"home_no_reconnect.played_long+played", "g1 g1 g1 g1 g1 g1 "},
        {"home_no_reconnect.played_long+played_long", "g1 g1 g1 g1 g1 g1 "},
        {"internet.lost", "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"internet.refused",
         "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"internet.in_use", "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"internet.timeout",
         "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"internet.left", "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"internet.offline",
         "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"internet.played", "t2 t3 t4 g4 g4 g4 g4 g4 g4 g4 g4 g4 g4 g4 g4 g4 "},
        {"internet.played_long",
         "t2 t3 t4 t5 t6 t7 t8 t9 t10 t11 t12 t13 t14 t15 t16 t17 "},
        {"internet.played_refused",
         "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"internet.played_in_use",
         "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"internet.quit_refused",
         "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"internet.disconnected_in_trouble",
         "t2 t3 t4 g4 g4 g4 g4 g4 g4 g4 g4 g4 g4 g4 g4 g4 "},
        {"internet.lost+lost", "g1 g1 g1 g1 g1 g1 "},
        {"internet.lost+refused", "g1 g1 g1 g1 g1 g1 "},
        {"internet.lost+in_use", "g1 g1 g1 g1 g1 g1 "},
        {"internet.lost+offline", "g1 g1 g1 g1 g1 g1 "},
        {"internet.lost+played", "g1 g1 g1 g1 g1 g1 "},
        {"internet.lost+played_long", "g1 g1 g1 g1 g1 g1 "},
        {"internet.refused+lost", "g1 g1 g1 g1 g1 g1 "},
        {"internet.refused+refused", "g1 g1 g1 g1 g1 g1 "},
        {"internet.refused+in_use", "g1 g1 g1 g1 g1 g1 "},
        {"internet.refused+offline", "g1 g1 g1 g1 g1 g1 "},
        {"internet.refused+played", "g1 g1 g1 g1 g1 g1 "},
        {"internet.refused+played_long", "g1 g1 g1 g1 g1 g1 "},
        {"internet.in_use+lost", "g1 g1 g1 g1 g1 g1 "},
        {"internet.in_use+refused", "g1 g1 g1 g1 g1 g1 "},
        {"internet.in_use+in_use", "g1 g1 g1 g1 g1 g1 "},
        {"internet.in_use+offline", "g1 g1 g1 g1 g1 g1 "},
        {"internet.in_use+played", "g1 g1 g1 g1 g1 g1 "},
        {"internet.in_use+played_long", "g1 g1 g1 g1 g1 g1 "},
        {"internet.offline+lost", "g1 g1 g1 g1 g1 g1 "},
        {"internet.offline+refused", "g1 g1 g1 g1 g1 g1 "},
        {"internet.offline+in_use", "g1 g1 g1 g1 g1 g1 "},
        {"internet.offline+offline", "g1 g1 g1 g1 g1 g1 "},
        {"internet.offline+played", "g1 g1 g1 g1 g1 g1 "},
        {"internet.offline+played_long", "g1 g1 g1 g1 g1 g1 "},
        {"internet.played+lost", "t2 t3 t4 g4 g4 g4 "},
        {"internet.played+refused", "t2 g2 g2 g2 g2 g2 "},
        {"internet.played+in_use", "t2 t3 t4 t5 t6 t7 "},
        {"internet.played+offline", "t2 t3 t4 t5 t6 t7 "},
        {"internet.played+played", "t2 t3 t4 g4 g4 g4 "},
        {"internet.played+played_long", "t2 t3 t4 t5 t6 t7 "},
        {"internet.played_long+lost", "t2 t3 t4 t5 t6 t7 "},
        {"internet.played_long+refused", "t2 g2 g2 g2 g2 g2 "},
        {"internet.played_long+in_use", "t2 t3 t4 t5 t6 t7 "},
        {"internet.played_long+offline", "t2 t3 t4 t5 t6 t7 "},
        {"internet.played_long+played", "t2 t3 t4 t5 t6 t7 "},
        {"internet.played_long+played_long", "t2 t3 t4 t5 t6 t7 "},
        {"internet_handover.lost",
         "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"internet_handover.refused",
         "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"internet_handover.in_use",
         "t2 t3 t4 t5 t6 t7 g7 g7 g7 g7 g7 g7 g7 g7 g7 g7 "},
        {"internet_handover.timeout",
         "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"internet_handover.left",
         "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"internet_handover.offline",
         "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"internet_handover.played",
         "t2 t3 t4 g4 g4 g4 g4 g4 g4 g4 g4 g4 g4 g4 g4 g4 "},
        {"internet_handover.played_long",
         "t2 t3 t4 t5 t6 t7 t8 t9 t10 t11 t12 t13 t14 t15 t16 t17 "},
        {"internet_handover.played_refused",
         "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"internet_handover.played_in_use",
         "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"internet_handover.quit_refused",
         "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"internet_handover.disconnected_in_trouble",
         "t2 t3 t4 g4 g4 g4 g4 g4 g4 g4 g4 g4 g4 g4 g4 g4 "},
        {"internet_handover.lost+lost", "g1 g1 g1 g1 g1 g1 "},
        {"internet_handover.lost+refused", "g1 g1 g1 g1 g1 g1 "},
        {"internet_handover.lost+in_use", "g1 g1 g1 g1 g1 g1 "},
        {"internet_handover.lost+offline", "g1 g1 g1 g1 g1 g1 "},
        {"internet_handover.lost+played", "g1 g1 g1 g1 g1 g1 "},
        {"internet_handover.lost+played_long", "g1 g1 g1 g1 g1 g1 "},
        {"internet_handover.refused+lost", "g1 g1 g1 g1 g1 g1 "},
        {"internet_handover.refused+refused", "g1 g1 g1 g1 g1 g1 "},
        {"internet_handover.refused+in_use", "g1 g1 g1 g1 g1 g1 "},
        {"internet_handover.refused+offline", "g1 g1 g1 g1 g1 g1 "},
        {"internet_handover.refused+played", "g1 g1 g1 g1 g1 g1 "},
        {"internet_handover.refused+played_long", "g1 g1 g1 g1 g1 g1 "},
        {"internet_handover.in_use+lost", "t2 g2 g2 g2 g2 g2 "},
        {"internet_handover.in_use+refused", "t2 g2 g2 g2 g2 g2 "},
        {"internet_handover.in_use+in_use", "t2 t3 t4 t5 t6 t7 "},
        {"internet_handover.in_use+offline", "t2 g2 g2 g2 g2 g2 "},
        {"internet_handover.in_use+played", "t2 t3 t4 t5 t6 t7 "},
        {"internet_handover.in_use+played_long", "t2 t3 t4 t5 t6 t7 "},
        {"internet_handover.offline+lost", "g1 g1 g1 g1 g1 g1 "},
        {"internet_handover.offline+refused", "g1 g1 g1 g1 g1 g1 "},
        {"internet_handover.offline+in_use", "g1 g1 g1 g1 g1 g1 "},
        {"internet_handover.offline+offline", "g1 g1 g1 g1 g1 g1 "},
        {"internet_handover.offline+played", "g1 g1 g1 g1 g1 g1 "},
        {"internet_handover.offline+played_long", "g1 g1 g1 g1 g1 g1 "},
        {"internet_handover.played+lost", "t2 t3 t4 g4 g4 g4 "},
        {"internet_handover.played+refused", "t2 g2 g2 g2 g2 g2 "},
        {"internet_handover.played+in_use", "t2 t3 t4 t5 t6 t7 "},
        {"internet_handover.played+offline", "t2 t3 t4 t5 t6 t7 "},
        {"internet_handover.played+played", "t2 t3 t4 g4 g4 g4 "},
        {"internet_handover.played+played_long", "t2 t3 t4 t5 t6 t7 "},
        {"internet_handover.played_long+lost", "t2 t3 t4 t5 t6 t7 "},
        {"internet_handover.played_long+refused", "t2 g2 g2 g2 g2 g2 "},
        {"internet_handover.played_long+in_use", "t2 t3 t4 t5 t6 t7 "},
        {"internet_handover.played_long+offline", "t2 t3 t4 t5 t6 t7 "},
        {"internet_handover.played_long+played", "t2 t3 t4 t5 t6 t7 "},
        {"internet_handover.played_long+played_long", "t2 t3 t4 t5 t6 t7 "},
        {"cloud.lost", "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"cloud.refused", "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"cloud.in_use", "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"cloud.timeout", "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"cloud.left", "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"cloud.offline", "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"cloud.played", "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"cloud.played_long",
         "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"cloud.played_refused",
         "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"cloud.played_in_use",
         "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"cloud.quit_refused",
         "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"cloud.disconnected_in_trouble",
         "g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 g1 "},
        {"cloud.lost+lost", "g1 g1 g1 g1 g1 g1 "},
        {"cloud.lost+refused", "g1 g1 g1 g1 g1 g1 "},
        {"cloud.lost+in_use", "g1 g1 g1 g1 g1 g1 "},
        {"cloud.lost+offline", "g1 g1 g1 g1 g1 g1 "},
        {"cloud.lost+played", "g1 g1 g1 g1 g1 g1 "},
        {"cloud.lost+played_long", "g1 g1 g1 g1 g1 g1 "},
        {"cloud.refused+lost", "g1 g1 g1 g1 g1 g1 "},
        {"cloud.refused+refused", "g1 g1 g1 g1 g1 g1 "},
        {"cloud.refused+in_use", "g1 g1 g1 g1 g1 g1 "},
        {"cloud.refused+offline", "g1 g1 g1 g1 g1 g1 "},
        {"cloud.refused+played", "g1 g1 g1 g1 g1 g1 "},
        {"cloud.refused+played_long", "g1 g1 g1 g1 g1 g1 "},
        {"cloud.in_use+lost", "g1 g1 g1 g1 g1 g1 "},
        {"cloud.in_use+refused", "g1 g1 g1 g1 g1 g1 "},
        {"cloud.in_use+in_use", "g1 g1 g1 g1 g1 g1 "},
        {"cloud.in_use+offline", "g1 g1 g1 g1 g1 g1 "},
        {"cloud.in_use+played", "g1 g1 g1 g1 g1 g1 "},
        {"cloud.in_use+played_long", "g1 g1 g1 g1 g1 g1 "},
        {"cloud.offline+lost", "g1 g1 g1 g1 g1 g1 "},
        {"cloud.offline+refused", "g1 g1 g1 g1 g1 g1 "},
        {"cloud.offline+in_use", "g1 g1 g1 g1 g1 g1 "},
        {"cloud.offline+offline", "g1 g1 g1 g1 g1 g1 "},
        {"cloud.offline+played", "g1 g1 g1 g1 g1 g1 "},
        {"cloud.offline+played_long", "g1 g1 g1 g1 g1 g1 "},
        {"cloud.played+lost", "g1 g1 g1 g1 g1 g1 "},
        {"cloud.played+refused", "g1 g1 g1 g1 g1 g1 "},
        {"cloud.played+in_use", "g1 g1 g1 g1 g1 g1 "},
        {"cloud.played+offline", "g1 g1 g1 g1 g1 g1 "},
        {"cloud.played+played", "g1 g1 g1 g1 g1 g1 "},
        {"cloud.played+played_long", "g1 g1 g1 g1 g1 g1 "},
        {"cloud.played_long+lost", "g1 g1 g1 g1 g1 g1 "},
        {"cloud.played_long+refused", "g1 g1 g1 g1 g1 g1 "},
        {"cloud.played_long+in_use", "g1 g1 g1 g1 g1 g1 "},
        {"cloud.played_long+offline", "g1 g1 g1 g1 g1 g1 "},
        {"cloud.played_long+played", "g1 g1 g1 g1 g1 g1 "},
        {"cloud.played_long+played_long", "g1 g1 g1 g1 g1 g1 "},
    };
  }
};

TEST_F(StartPolicyTransitions, EveryFailureSequenceEndsWhereTheTableSays) {
  const auto table = pins();
  std::size_t matched = 0;
  const auto all = failures();
  for (const auto& row : rows()) {
    for (const auto& [name, failure] : all) {
      const auto actual = run(row, std::vector<Failure>(16, failure));
      const auto key = std::string{row.name} + "." + std::string{name};
      const auto pin = std::ranges::find_if(
          table, [&](const auto& item) { return item.first == key; });
      if (pin == table.end()) {
        ADD_FAILURE() << "{\"" << key << "\", \"" << actual << "\"},";
        continue;
      }
      ++matched;
      EXPECT_EQ(actual, pin->second) << key;
    }
    for (const auto& [first, opening] : all) {
      if (!paired(first)) continue;
      for (const auto& [second, closing] : all) {
        if (!paired(second)) continue;
        std::vector<Failure> sequence{opening, closing, opening,
                                      closing, closing, opening};
        const auto actual = run(row, sequence);
        const auto key = std::string{row.name} + "." + std::string{first} +
                         "+" + std::string{second};
        const auto pin = std::ranges::find_if(
            table, [&](const auto& item) { return item.first == key; });
        if (pin == table.end()) {
          ADD_FAILURE() << "{\"" << key << "\", \"" << actual << "\"},";
          continue;
        }
        ++matched;
        EXPECT_EQ(actual, pin->second) << key;
      }
    }
  }
  EXPECT_EQ(matched, table.size());
}

}  // namespace
