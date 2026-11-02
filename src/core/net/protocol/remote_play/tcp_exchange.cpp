#include "tcp_exchange.hpp"

#include <utility>

namespace libchiaking::net::protocol::remote_play {

auto SessionReply::parse(std::span<const std::byte> received) const
    -> Expected<std::optional<SessionResponse>, ConsoleExchangeError> {
  auto parsed = SessionCodec::parse_response(target_, received);
  if (parsed) return std::optional<SessionResponse>{std::move(parsed.value())};
  switch (parsed.error()) {
    case ResponseCodecError::incomplete:
      return std::optional<SessionResponse>{};
    case ResponseCodecError::too_large:
      return unexpected(ConsoleExchangeError::too_large);
    default:
      return unexpected(ConsoleExchangeError::invalid_response);
  }
}

}  // namespace libchiaking::net::protocol::remote_play
