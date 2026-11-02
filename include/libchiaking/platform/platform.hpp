/// @file
/// @brief Umbrella header for every platform port a host can provide: crypto,
/// networking, the clock, random and threading ports, diagnostics, storage,
/// HTTP and WebSocket. The stock implementations are in
/// libchiaking/platform/defaults.hpp and libchiaking/platform/stock_providers.hpp,
/// and the conformance suites in libchiaking/testing/platform_conformance.hpp.
#pragma once

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/platform/crypto/error.hpp>
#include <libchiaking/platform/crypto/provider.hpp>
#include <libchiaking/platform/diagnostics/provider.hpp>
#include <libchiaking/platform/error.hpp>
#include <libchiaking/platform/factory.hpp>
#include <libchiaking/platform/http/provider.hpp>
#include <libchiaking/platform/net/dns.hpp>
#include <libchiaking/platform/net/error.hpp>
#include <libchiaking/platform/net/interfaces.hpp>
#include <libchiaking/platform/net/poller.hpp>
#include <libchiaking/platform/net/provider.hpp>
#include <libchiaking/platform/net/socket.hpp>
#include <libchiaking/platform/net/types.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/random_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>
#include <libchiaking/platform/storage/record_store.hpp>
#include <libchiaking/platform/websocket/provider.hpp>
