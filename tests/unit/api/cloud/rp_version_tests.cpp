#include "src/core/net/protocol/remote_play/pairing/rp_version.hpp"

namespace cloud_api_test {
namespace {

using libchiaking::net::protocol::remote_play::RegistrationTarget;
using libchiaking::net::protocol::remote_play::RpVersion;

static_assert(RpVersion::of(RegistrationTarget::ps4_8)->text() == "8.0");
static_assert(RpVersion::of(RegistrationTarget::ps4_10)->text() == "10.0");
static_assert(RpVersion::of(RegistrationTarget::ps5_1)->text() == "1.0");
static_assert(!RpVersion::of(RegistrationTarget::ps4_unknown));
static_assert(RpVersion::parse("1.0", RpVersion::Family::ps5)->target() ==
              RegistrationTarget::ps5_1);
static_assert(!RpVersion::parse("9.0", RpVersion::Family::ps5));
static_assert(RpVersion::parse("9.0", RpVersion::Family::ps4)->target() ==
              RegistrationTarget::ps4_9);
static_assert(RpVersion::parse("10.0", RpVersion::Family::ps4)->target() ==
              RegistrationTarget::ps4_10);
static_assert(!RpVersion::parse("10.", RpVersion::Family::ps4));
static_assert(!RpVersion::parse("", RpVersion::Family::ps4));
static_assert(!RpVersion::parse("", RpVersion::Family::ps5));

}  // namespace
}  // namespace cloud_api_test
