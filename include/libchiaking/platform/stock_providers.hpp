/// @file
/// @brief Umbrella header for the stock providers a frontend composes a
/// platform from: DefaultPlatform, the OpenSSL, BoringSSL and libnx crypto
/// providers, the libcurl HTTP and WebSocket providers, the OpenSSL DTLS
/// provider and Horizon threading, with the provider interfaces from
/// platform.hpp.
#pragma once

#include <libchiaking/platform/crypto/boringssl/provider.hpp>
#include <libchiaking/platform/crypto/libnx/provider.hpp>
#include <libchiaking/platform/crypto/openssl/provider.hpp>
#include <libchiaking/platform/curl/services.hpp>
#include <libchiaking/platform/default.hpp>
#include <libchiaking/platform/defaults.hpp>
#include <libchiaking/platform/dtls/openssl/provider.hpp>
#include <libchiaking/platform/http/curl/multi_provider.hpp>
#include <libchiaking/platform/http/curl/provider.hpp>
#include <libchiaking/platform/http/curl/selected_provider.hpp>
#include <libchiaking/platform/platform.hpp>
#include <libchiaking/platform/runtime/horizon_threading.hpp>
#include <libchiaking/platform/websocket/curl/provider.hpp>
