#include "src/core/users/ports/remote_play_access.hpp"

#include "src/core/users/adapters/ps_user_core.hpp"

#include <utility>

namespace libchiaking::users {

RemotePlayAccess::RemotePlayAccess(PsUser::Core& user,
                                   psn::auth::BearerToken token) noexcept
    : user_(&user), token_(std::move(token)) {}

void RemotePlayAccess::refused_by_service() {
  user_->distrust_remote_play_access(token_);
}

}  // namespace libchiaking::users
