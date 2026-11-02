#include "src/platform/net/win32_error.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

namespace {

using libchiaking::platform::NetError;
using libchiaking::platform::detail::normalize_wsa_error;

TEST(WsaErrors, PendingWorkMeansTheCallWouldBlock) {
  EXPECT_EQ(normalize_wsa_error(WSAEWOULDBLOCK), NetError::would_block);
  EXPECT_EQ(normalize_wsa_error(WSAEINPROGRESS), NetError::would_block);
  EXPECT_EQ(normalize_wsa_error(WSAEALREADY), NetError::would_block);
}

TEST(WsaErrors, ConnectionFailuresKeepTheirMeaning) {
  EXPECT_EQ(normalize_wsa_error(WSAECONNREFUSED), NetError::connection_refused);
  EXPECT_EQ(normalize_wsa_error(WSAECONNRESET), NetError::connection_reset);
  EXPECT_EQ(normalize_wsa_error(WSAECONNABORTED), NetError::connection_aborted);
  EXPECT_EQ(normalize_wsa_error(WSAENOTCONN), NetError::not_connected);
  EXPECT_EQ(normalize_wsa_error(WSAEISCONN), NetError::already_connected);
  EXPECT_EQ(normalize_wsa_error(WSAENETRESET), NetError::connection_reset);
  EXPECT_EQ(normalize_wsa_error(WSAESHUTDOWN), NetError::connection_reset);
  EXPECT_EQ(normalize_wsa_error(WSAETIMEDOUT), NetError::timed_out);
  EXPECT_EQ(normalize_wsa_error(WSAEINTR), NetError::interrupted);
}

TEST(WsaErrors, AddressAndRouteFailuresKeepTheirMeaning) {
  EXPECT_EQ(normalize_wsa_error(WSAEADDRINUSE), NetError::address_in_use);
  EXPECT_EQ(normalize_wsa_error(WSAEADDRNOTAVAIL),
            NetError::address_not_available);
  EXPECT_EQ(normalize_wsa_error(WSAENETUNREACH), NetError::network_unreachable);
  EXPECT_EQ(normalize_wsa_error(WSAEHOSTUNREACH), NetError::host_unreachable);
  EXPECT_EQ(normalize_wsa_error(WSAEHOSTDOWN), NetError::host_unreachable);
  EXPECT_EQ(normalize_wsa_error(WSAENETDOWN), NetError::network_down);
  EXPECT_EQ(normalize_wsa_error(WSAEACCES), NetError::permission_denied);
}

TEST(WsaErrors, BadArgumentsAreInvalid) {
  EXPECT_EQ(normalize_wsa_error(WSAEINVAL), NetError::invalid_argument);
  EXPECT_EQ(normalize_wsa_error(WSAEFAULT), NetError::invalid_argument);
  EXPECT_EQ(normalize_wsa_error(WSAEDESTADDRREQ), NetError::invalid_argument);
  EXPECT_EQ(normalize_wsa_error(WSAEMSGSIZE), NetError::message_too_large);
}

TEST(WsaErrors, MissingFeaturesAreUnsupported) {
  EXPECT_EQ(normalize_wsa_error(WSAENOPROTOOPT), NetError::not_supported);
  EXPECT_EQ(normalize_wsa_error(WSAEOPNOTSUPP), NetError::not_supported);
  EXPECT_EQ(normalize_wsa_error(WSAEAFNOSUPPORT), NetError::not_supported);
  EXPECT_EQ(normalize_wsa_error(WSAEPFNOSUPPORT), NetError::not_supported);
  EXPECT_EQ(normalize_wsa_error(WSAEPROTONOSUPPORT), NetError::not_supported);
  EXPECT_EQ(normalize_wsa_error(WSAESOCKTNOSUPPORT), NetError::not_supported);
}

TEST(WsaErrors, ExhaustedResourcesAreReportedAsSuch) {
  EXPECT_EQ(normalize_wsa_error(WSAENOBUFS), NetError::no_buffers);
  EXPECT_EQ(normalize_wsa_error(WSA_NOT_ENOUGH_MEMORY), NetError::no_buffers);
  EXPECT_EQ(normalize_wsa_error(WSAEMFILE), NetError::resource_exhausted);
}

TEST(WsaErrors, AnUnknownCodeIsASystemFailure) {
  EXPECT_EQ(normalize_wsa_error(0x7fff1234), NetError::system_failure);
}

}  // namespace
