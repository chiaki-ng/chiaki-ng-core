/// @file
/// @brief Declares OldServiceLaunch, the LegacyCloudStream half of a cloud
/// launch: the store and account steps that find the title's entitlement, take
/// its free licence when needed, and open the account's streaming session.
#pragma once

#include "libchiaking/detail/catalog/launch_verdict.hpp"
#include "src/core/net/transport/http/client.hpp"
#include "src/core/play/adapters/cloud_launch/launch_observer.hpp"
#include "src/core/play/adapters/links/cloud/services/old_service_title.hpp"
#include "src/core/psn/auth/auth_error.hpp"
#include "src/core/psn/rest/kamaji/kamaji_client.hpp"
#include "src/core/psn/types.hpp"
#include "src/core/users/ports/play_calls.hpp"

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/detail/users/ps_user.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::play {

/// @brief The LegacyCloudStream steps before a server is reserved.
///
/// Resolves the store title to an entitlement and platform (unless the
/// cleared licence is already known), checks the account's privacy
/// attributes, asks the licence status and, when the licence is missing and
/// the title is free, previews and takes it; then opens the account session
/// for the platform. Each step blocks on PS and reports progress out of
/// OldServiceLaunch::progress_steps. Holds the client and user by reference;
/// used on the launching thread.
class OldServiceLaunch {
 public:
  /// @brief Why a LegacyCloudStream step failed: the step, the cause, the HTTP
  /// status, and the upgrade address or price the player needs. A value.
  class Error {
   public:
    /// @brief The step that failed.
    enum class Stage : std::uint8_t {
      /// @brief Getting the anonymous title code.
      anonymous_oauth,
      /// @brief Opening the anonymous store session.
      anonymous_session,
      /// @brief Resolving the store title.
      title,
      /// @brief Getting the checkout token.
      commerce_oauth,
      /// @brief Checking the account's privacy attributes.
      account_attributes,
      /// @brief Asking the licence status.
      entitlement,
      /// @brief Previewing the licence checkout.
      checkout_preview,
      /// @brief Taking the free licence.
      checkout,
      /// @brief Opening the account's streaming session.
      account_session,
    };
    /// @brief What went wrong.
    enum class Cause : std::uint8_t {
      /// @brief The launch was stopped.
      cancelled,
      /// @brief The request could not be made, or the user is signed out.
      invalid_request,
      /// @brief PS could not be reached.
      transport,
      /// @brief PS's reply could not be read.
      invalid_response,
      /// @brief PS refused the request.
      rejected,
      /// @brief The title is not in the store.
      not_found,
      /// @brief The store title names no usable entitlement.
      no_entitlement,
      /// @brief The account's privacy settings must be completed first.
      privacy_required,
      /// @brief The licence preview was refused, for want of a subscription.
      subscription_required,
      /// @brief The licence is no longer free.
      not_free,
      /// @brief The licence is not part of any PlayStation Plus plan.
      outside_streaming_plan,
      /// @brief The licence preview was refused because the edition is sold in
      /// another store region than the account's.
      store_region,
    };

    /// @brief Builds an error.
    ///
    /// @param stage The step.
    /// @param cause What went wrong.
    /// @param http_status The HTTP status; 0 when none.
    Error(Stage stage, Cause cause, std::uint16_t http_status = 0) noexcept
        : stage_(stage), cause_(cause), http_status_(http_status) {}

    /// @brief Builds a privacy settings error with the upgrade address.
    ///
    /// @param http_status The HTTP status.
    /// @param missing_elements The account attributes PS said are missing.
    /// @return The error at the account attributes step.
    static auto privacy_required(std::uint16_t http_status,
                                 std::span<const std::string> missing_elements)
        -> Error;
    /// @brief Builds a not-free error with the price.
    ///
    /// @param display_price The price as the store shows it.
    /// @return The error at the checkout preview step.
    static auto not_free(std::string display_price) -> Error;

    /// @brief Reads the step.
    ///
    /// @return The step.
    auto stage() const noexcept -> Stage { return stage_; }
    /// @brief Reads the cause.
    ///
    /// @return The cause.
    auto cause() const noexcept -> Cause { return cause_; }
    /// @brief Reads the HTTP status.
    ///
    /// @return The status; 0 when none.
    auto http_status() const noexcept -> std::uint16_t { return http_status_; }
    /// @brief Reports whether the launch was stopped.
    ///
    /// @return `true` for Cause::cancelled.
    auto is_cancelled() const noexcept -> bool {
      return cause_ == Cause::cancelled;
    }
    /// @brief Reads the privacy settings upgrade address.
    ///
    /// @return The address; empty unless privacy settings are required.
    auto privacy_upgrade_url() const noexcept -> std::string_view {
      return privacy_upgrade_url_;
    }
    /// @brief Reads the licence price.
    ///
    /// @return The price; empty unless the licence is not free.
    auto display_price() const noexcept -> std::string_view {
      return display_price_;
    }

   private:
    /// @brief The step.
    Stage stage_;
    /// @brief The cause.
    Cause cause_;
    /// @brief The HTTP status.
    std::uint16_t http_status_{};
    /// @brief The privacy settings upgrade address.
    std::string privacy_upgrade_url_;
    /// @brief The licence price.
    std::string display_price_;
  };

  /// @brief The number of numbered steps the progress counts.
  static constexpr int progress_steps = 5;
  /// @brief The most characters of a store country or language.
  static constexpr std::size_t store_component_limit = 7;

  /// @brief What one run resolves and opens. Views into the caller's data.
  struct Request {
    /// @brief The store product id.
    std::string_view game_identifier;
    /// @brief The entitlement already known; empty to resolve it.
    std::string_view entitlement_id;
    /// @brief The platform the title streams as.
    psn::StreamingPlatform platform{psn::StreamingPlatform::ps4};
    /// @brief The store country; "US" when empty.
    std::string_view store_country;
    /// @brief The store language; "en" when empty.
    std::string_view store_language;
    /// @brief Whether the title came from the public store rather than the
    /// account's own catalog.
    bool foreign_catalog{};
    /// @brief Whether to skip the privacy attributes check.
    bool skip_account_attributes_check{};
    /// @brief Whether to resolve the title even when the entitlement is known.
    bool force_resolve{};
  };

  /// @brief How the launch behaves. Plain data.
  struct Options {
    /// @brief Whether to skip the privacy attributes check.
    bool skip_account_attributes_check{};
  };

  /// @brief Builds the steps with silent progress and default options.
  ///
  /// @param client The HTTP client; must outlive the steps.
  /// @param user The user; must outlive the steps.
  OldServiceLaunch(net::http::Client& client, users::PsUser& user);
  /// @brief Builds the steps with default options.
  ///
  /// @param client The HTTP client; must outlive the steps.
  /// @param user The user; must outlive the steps.
  /// @param report Where progress goes.
  OldServiceLaunch(net::http::Client& client, users::PsUser& user,
                   LaunchReport report);
  /// @brief Builds the steps.
  ///
  /// @param client The HTTP client; must outlive the steps.
  /// @param user The user; must outlive the steps.
  /// @param report Where progress goes.
  /// @param options How the launch behaves.
  OldServiceLaunch(net::http::Client& client, users::PsUser& user,
                   LaunchReport report, Options options);

  /// @brief Names the streaming platform of a catalog platform.
  ///
  /// @param platform The catalog platform.
  /// @return The matching streaming platform.
  static auto platform_of(catalog::Platform platform) noexcept
      -> psn::StreamingPlatform;
  /// @brief Reports whether the cleared launch already names the licence.
  ///
  /// @param cleared What the catalog cleared.
  /// @return `true` when the title is held as a known entitlement.
  static auto takes_known_licence(
      const catalog::ClearedLaunch& cleared) noexcept -> bool;
  /// @brief Reports whether a reservation refused for having no game repeats
  /// the title steps.
  ///
  /// @param cleared What the catalog cleared.
  /// @param round The round that was refused, from 0.
  /// @return `true` after the first round when the licence was taken as known,
  ///     so the second round resolves the title.
  static auto repeats_title_steps_after_refusal(
      const catalog::ClearedLaunch& cleared, unsigned round) noexcept -> bool;

  /// @brief Runs the steps for a cleared launch.
  ///
  /// @param cleared What the catalog cleared.
  /// @param round The reservation round, from 0; a later round always
  ///     resolves the title.
  /// @return As OldServiceLaunch::run.
  [[nodiscard]] auto prepare(const catalog::ClearedLaunch& cleared,
                             unsigned round)
      -> Expected<OldServiceTitle, Error>;
  /// @brief Runs the steps: resolve and license when needed, then open the
  /// account session. Blocks on PS throughout.
  ///
  /// @param request What to resolve and open.
  /// @return The title to launch; the step's error, Cause::rejected when the
  ///     account session was refused.
  [[nodiscard]] auto run(const Request& request)
      -> Expected<OldServiceTitle, Error>;

 private:
  /// @brief The step that failed.
  using Stage = Error::Stage;
  /// @brief What went wrong.
  using Cause = Error::Cause;
  /// @brief A step of the account's LegacyCloudStream calls.
  using Step = users::OldServiceRefusal::Step;

  /// @brief Builds the privacy settings upgrade address.
  ///
  /// @param missing_elements The missing account attributes.
  /// @return The address, with the attributes percent-encoded; empty when none
  ///     are missing.
  static auto upgrade_url(std::span<const std::string> missing_elements)
      -> std::string;

  /// @brief Reports a numbered step.
  ///
  /// @param activity What the launch does.
  /// @param label The step's label.
  /// @param step The step number.
  void announce(LaunchActivity activity, std::string_view label,
                int step) const;
  /// @brief Builds an error at the current step.
  ///
  /// @param cause What went wrong.
  /// @param status The HTTP status.
  /// @return The error.
  auto error(Cause cause, std::uint16_t status = 0) const -> Error;
  /// @brief Turns an HTTP failure into an error at the current step.
  ///
  /// @param failure The failure.
  /// @return Cause::cancelled, Cause::invalid_request or Cause::transport.
  auto failed(const net::http::Failure& failure) const -> Error;
  /// @brief Turns a sign-in failure into an error at the current step.
  ///
  /// @param failure The failure.
  /// @return Cause::cancelled, Cause::transport, Cause::invalid_request, or
  ///     Cause::invalid_response with the status.
  auto auth_error(const psn::auth::AuthError& failure) const -> Error;
  /// @brief Turns the account's refusal into an error at its step.
  ///
  /// @param refusal The refusal.
  /// @return The cause the refusal implies.
  auto refused(const users::OldServiceRefusal& refusal) -> Error;

  /// @brief Names the step of an account call.
  ///
  /// @param step The call's step.
  /// @return The matching step.
  static auto stage_of(Step step) noexcept -> Stage;

  /// @brief Opens the title store, resolves the title and takes its licence.
  ///
  /// @param request What to resolve.
  /// @return The title; the step's error.
  [[nodiscard]] auto resolve(const Request& request)
      -> Expected<OldServiceTitle, Error>;
  /// @brief Resolves the store title to an entitlement, falling back once to
  /// the classics region for a foreign catalog.
  ///
  /// @param request What to resolve.
  /// @return The title; Cause::not_found, Cause::rejected,
  ///     Cause::invalid_response or Cause::no_entitlement.
  [[nodiscard]] auto resolve_title(const Request& request)
      -> Expected<OldServiceTitle, Error>;
  /// @brief Checks the licence and, when missing and free, takes it.
  ///
  /// For a foreign catalog, a legacy title skips acquisition and any
  /// acquisition failure other than cancellation is ignored.
  ///
  /// @param request What to resolve.
  /// @param title The resolved title.
  /// @return The title, possibly with a replaced SKU; Cause::rejected for an
  ///     unexpected licence status; Cause::outside_streaming_plan; the
  ///     preview's or purchase's error.
  [[nodiscard]] auto license(const Request& request, OldServiceTitle title)
      -> Expected<OldServiceTitle, Error>;
  /// @brief Checks the account's privacy attributes.
  ///
  /// @return Nothing; Cause::privacy_required with the upgrade address, or the
  ///     call's error.
  [[nodiscard]] auto check_attributes() -> Expected<void, Error>;
  /// @brief Asks whether the account holds the licence.
  ///
  /// @param title The title.
  /// @return The HTTP status: 200 held, 404 missing; the call's error.
  [[nodiscard]] auto entitlement_status(const OldServiceTitle& title)
      -> Expected<std::uint16_t, Error>;
  /// @brief Previews taking the licence.
  ///
  /// @param title The title.
  /// @return The title with the preview's SKU; Cause::not_free with the price;
  ///     Cause::subscription_required; the call's error.
  [[nodiscard]] auto preview(const OldServiceTitle& title)
      -> Expected<OldServiceTitle, Error>;
  /// @brief Takes the free licence.
  ///
  /// @param title The title.
  /// @return Nothing; Cause::rejected or the call's error.
  [[nodiscard]] auto buy(const OldServiceTitle& title) -> Expected<void, Error>;

  /// @brief The HTTP client.
  net::http::Client& client_;
  /// @brief The user's authorised calls.
  users::PlayCalls user_;
  /// @brief The store session.
  std::optional<psn::rest::KamajiClient> store_;
  /// @brief The checkout session.
  std::optional<users::OldServiceCheckout> checkout_;
  /// @brief Where progress goes.
  LaunchReport report_;
  /// @brief How the launch behaves.
  Options options_;
  /// @brief The current step, named in errors.
  Stage stage_{Stage::anonymous_oauth};
};

}  // namespace libchiaking::play
