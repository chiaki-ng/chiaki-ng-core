#pragma once

#include <magic_enum/magic_enum.hpp>

#include <ostream>
#include <type_traits>
#include <utility>

namespace test_support {

template <class Enum>
  requires std::is_enum_v<Enum>
void PrintTo(const Enum& value, std::ostream* out) {
  *out << magic_enum::enum_type_name<Enum>();
  if (const auto name = magic_enum::enum_name(value); !name.empty())
    *out << "::" << name;
  else
    *out << '(' << +std::to_underlying(value) << ')';
}

}  // namespace test_support

namespace libchiaking {
using test_support::PrintTo;
}
namespace libchiaking::catalog {
using test_support::PrintTo;
}
namespace libchiaking::catalog::cloud_stream {
using test_support::PrintTo;
}
namespace libchiaking::catalog::legacy_cloud_stream {
using test_support::PrintTo;
}
namespace libchiaking::catalog::ps {
using test_support::PrintTo;
}
namespace libchiaking::catalog::usecases::cloud_stream {
using test_support::PrintTo;
}
namespace libchiaking::catalog::usecases::legacy_cloud_stream {
using test_support::PrintTo;
}
namespace libchiaking::cloud {
using test_support::PrintTo;
}
namespace libchiaking::concurrency {
using test_support::PrintTo;
}
namespace libchiaking::consoles {
using test_support::PrintTo;
}
namespace libchiaking::consoles::home_pairing_lifecycle {
using test_support::PrintTo;
}
namespace libchiaking::consoles::usecases {
using test_support::PrintTo;
}
namespace libchiaking::containers {
using test_support::PrintTo;
}
namespace libchiaking::controllers {
using test_support::PrintTo;
}
namespace libchiaking::diagnostics {
using test_support::PrintTo;
}
namespace libchiaking::json {
using test_support::PrintTo;
}
namespace libchiaking::kakusan {
using test_support::PrintTo;
}
namespace libchiaking::kakusan::testing {
using test_support::PrintTo;
}
namespace libchiaking::kakusan::usecases {
using test_support::PrintTo;
}
namespace libchiaking::media {
using test_support::PrintTo;
}
namespace libchiaking::messages {
using test_support::PrintTo;
}
namespace libchiaking::net::http {
using test_support::PrintTo;
}
namespace libchiaking::net::kakusan {
using test_support::PrintTo;
}
namespace libchiaking::net::kakusan::control {
using test_support::PrintTo;
}
namespace libchiaking::net::nat::holepunch {
using test_support::PrintTo;
}
namespace libchiaking::net::nat::ice {
using test_support::PrintTo;
}
namespace libchiaking::net::nat::stun {
using test_support::PrintTo;
}
namespace libchiaking::net::ntp {
using test_support::PrintTo;
}
namespace libchiaking::net::protocol::remote_play {
using test_support::PrintTo;
}
namespace libchiaking::net::protocol::remote_play::active_handshake_lifecycle {
using test_support::PrintTo;
}
namespace libchiaking::net::protocol::remote_play::ctrl_lifecycle_detail {
using test_support::PrintTo;
}
namespace libchiaking::net::protocol::remote_play::local_ctrl_lifecycle {
using test_support::PrintTo;
}
namespace libchiaking::net::protocol::remote_play::local_pipeline_lifecycle {
using test_support::PrintTo;
}
namespace libchiaking::net::protocol::remote_play::tcp_exchange_lifecycle {
using test_support::PrintTo;
}
namespace libchiaking::net::protocol::remote_play::transport_switch_detail {
using test_support::PrintTo;
}
namespace libchiaking::net::protocol::senkusha {
using test_support::PrintTo;
}
namespace libchiaking::net::protocol::senkusha::lifecycle_detail {
using test_support::PrintTo;
}
namespace libchiaking::net::protocol::takion {
using test_support::PrintTo;
}
namespace libchiaking::net::protocol::takion::client_detail {
using test_support::PrintTo;
}
namespace libchiaking::net::protocol::takion::generic_data_lifecycle {
using test_support::PrintTo;
}
namespace libchiaking::net::protocol::takion::handshake_detail {
using test_support::PrintTo;
}
namespace libchiaking::net::transport::datagram {
using test_support::PrintTo;
}
namespace libchiaking::net::transport::rudp {
using test_support::PrintTo;
}
namespace libchiaking::platform {
using test_support::PrintTo;
}
namespace libchiaking::play {
using test_support::PrintTo;
}
namespace libchiaking::play::home_link_progress {
using test_support::PrintTo;
}
namespace libchiaking::play::internet {
using test_support::PrintTo;
}
namespace libchiaking::play::internet::holepunch {
using test_support::PrintTo;
}
namespace libchiaking::play::internet::remote_ctrl {
using test_support::PrintTo;
}
namespace libchiaking::play::internet_route_order {
using test_support::PrintTo;
}
namespace libchiaking::play::reconnection {
using test_support::PrintTo;
}
namespace libchiaking::play::usecases {
using test_support::PrintTo;
}
namespace libchiaking::progress {
using test_support::PrintTo;
}
namespace libchiaking::psn {
using test_support::PrintTo;
}
namespace libchiaking::psn::account {
using test_support::PrintTo;
}
namespace libchiaking::psn::auth {
using test_support::PrintTo;
}
namespace libchiaking::psn::gk {
using test_support::PrintTo;
}
namespace libchiaking::psn::graphql {
using test_support::PrintTo;
}
namespace libchiaking::psn::remote_play {
using test_support::PrintTo;
}
namespace libchiaking::psn::rest {
using test_support::PrintTo;
}
namespace libchiaking::testing {
using test_support::PrintTo;
}
namespace libchiaking::text {
using test_support::PrintTo;
}
namespace libchiaking::text::base64 {
using test_support::PrintTo;
}
namespace libchiaking::trophies {
using test_support::PrintTo;
}
namespace libchiaking::users {
using test_support::PrintTo;
}
