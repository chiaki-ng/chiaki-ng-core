#include "src/core/catalog/usecases/cloud_stream/check_exclusion.hpp"

#include "src/core/catalog/adapters/ps/cloud_exclusions.hpp"
#include "src/core/net/transport/http/client.hpp"

namespace libchiaking::catalog::usecases::cloud_stream {

CheckExclusion::CheckExclusion(platform::HttpProvider& http,
                               const platform::ClockProvider& clock,
                               const platform::ClockProvider& wall_clock,
                               platform::DiagnosticsProvider* log,
                               HeldAnswers& held) noexcept
    : http_(http), clock_(clock), wall_(wall_clock), log_(log), held_(held) {}

auto CheckExclusion::of(const std::optional<TitleId>& title,
                        const ProductId& product, const Stop& stop) -> Verdict {
  return Verdict::judged(current_list(stop), title, product);
}

auto CheckExclusion::held(const std::optional<TitleId>& title,
                          const ProductId& product) -> Verdict {
  auto list = held_.held_exclusions();
  if (!list || !list->is_fresh_at(held_.now()))
    return Verdict::judged(
        unexpected(Unanswered{Unanswered::Reason::unavailable}), title,
        product);
  return Verdict::judged(std::move(*list), title, product);
}

auto CheckExclusion::keep_current(const Stop& stop)
    -> Expected<void, Unanswered> {
  if (auto list = current_list(stop); !list)
    return unexpected(std::move(list.error()));
  return {};
}

auto CheckExclusion::current_list(const Stop& stop)
    -> Expected<catalog::cloud_stream::ExclusionList, Unanswered> {
  if (auto held = held_.held_exclusions();
      held && held->is_fresh_at(held_.now()))
    return std::move(*held);
  return held_.in_flight().exclusions.answer(
      "excluded_titles", stop,
      [&]() -> Expected<catalog::cloud_stream::ExclusionList, Unanswered> {
        if (auto held = held_.held_exclusions();
            held && held->is_fresh_at(held_.now()))
          return std::move(*held);
        return refreshed_list(stop);
      });
}

auto CheckExclusion::refreshed_list(const Stop& stop)
    -> Expected<catalog::cloud_stream::ExclusionList, Unanswered> {
  net::http::Client client{http_, clock_, stop, log_};
  auto fetched = ps::CloudExclusions{client, wall_}.fetch();
  if (fetched) held_.keep(fetched.value());
  return fetched;
}

}  // namespace libchiaking::catalog::usecases::cloud_stream
