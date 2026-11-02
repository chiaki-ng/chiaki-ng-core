/// @file
/// @brief Gathers the Takion client, its vocabulary, the platform crypto
/// provider and the datagram transports in one include for code that opens a
/// Takion session.
#pragma once

#include "src/core/net/protocol/takion/client.hpp"
#include "src/core/net/protocol/takion/client_types.hpp"
#include "src/core/net/protocol/takion/platform_crypto_provider.hpp"
#include "src/core/net/protocol/takion/profile.hpp"
#include "src/core/net/protocol/takion/result.hpp"
#include "src/core/net/transport/datagram/transport.hpp"
#include "src/core/net/transport/datagram/udp_transport.hpp"
