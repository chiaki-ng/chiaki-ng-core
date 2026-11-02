/// @file
/// @brief Brings the library's Expected and unexpected into the Takion
/// namespace, so Takion code can spell its results without qualification.
#pragma once

#include <libchiaking/detail/expected.hpp>

namespace libchiaking::net::protocol::takion {

using libchiaking::Expected;
using libchiaking::Unexpected;
using libchiaking::unexpected;

}  // namespace libchiaking::net::protocol::takion
