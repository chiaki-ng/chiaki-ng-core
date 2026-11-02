# Tests

Every test is a [GoogleTest](https://google.github.io/googletest/) case.
`docs/BUILDING.md` says how GoogleTest is found.

## Layout

`tests/unit` mirrors `src/`. The tests for
`src/core/trophies/usecases/read_trophies.cpp` live in
`tests/unit/core/trophies/usecases/`, one file per behaviour: the summary, the
title list, a title's trophies, the title sets. Test files end in
`_tests.cpp`. Split a file when it covers more than one behaviour, not when it
reaches some length.

Each folder that `tests/tests.cmake` names with `libchiaking_gtest_domain` is a
domain:

```cmake
libchiaking_gtest_domain(core/trophies LIBS libchiaking_trophies libchiaking_catalog)
```

That builds every `*_tests.cpp` under the folder into one executable,
`core_trophies_gtests`, and registers each case with ctest as
`core/trophies/<Suite>.<Case>`. A domain can take a few more arguments:

```cmake
libchiaking_gtest_domain(core/psn
    LIBS libchiaking_psn libchiaking_play libchiaking_net_kakusan libchiaking_diagnostics
    OPTIONAL_SOURCES graphql/query_documents_tests.cpp
    OPTIONAL_LIBS libchiaking_crypto_openssl)
```

`OPTIONAL_SOURCES`, relative to the folder, build only when every target in
`OPTIONAL_LIBS` exists, and then link those targets. `SOURCES` adds files from
outside the folder, `DEFINES` adds compile definitions, and `IF` leaves the
whole domain out when its condition is false.

A suite that must pass against more than one provider, such as the fake and
the OpenSSL secure datagram providers, is a typed suite in a `*_cases.hpp`
header, instantiated once per provider in its own `*_tests.cpp`. The OpenSSL
instantiation is an optional source, so no macro switches providers inside one
executable. `tests/unit/core/net/kakusan/udp_transport_cases.hpp` and
`tests/unit/core/kakusan/network_end_to_end_cases.hpp` work this way.

Besides the domains, `tests/tests.cmake` registers the source guards in
`tests/cmake`, `package_install_consumer`, which installs the library and
builds `tests/packaging/consumer` against it, and the Takion version checks.
The documentation lint runs in ctest as `docs_lint`.

## Writing a test

One `TEST` or `TEST_F` checks one behaviour. The suite names the thing under
test and the case finishes the sentence, so the pair reads as a statement:
`TrophySummary.ExpiresSixHoursAfterItWasKept`,
`TrophyTitleSets.AreAskedOnceForARepeatedRelease`.

Use a fixture when several cases share setup. Anything a test needs besides
its body, such as URL builders, canned replies or a scripted exchange, is a
member of the fixture or of a rig class, never a free function in the file.
Keep the strong types the code uses (`NpTitleId`, `NpCommunicationId`,
`AccountId`) rather than passing their text around.

Use `ASSERT_*` when the rest of the test cannot run if the check fails: before
dereferencing an `Expected` or `optional`, or before indexing a list whose
size you have not checked. Use `EXPECT_*` for everything else, so one run
reports every mismatch. Compare with `EXPECT_EQ(actual, expected)` rather than
`EXPECT_TRUE(a == b)`, so a failure prints both sides. A check that needs a
loop can return a `testing::AssertionResult` that says what differed.

Include `tests/support/enum_printing.hpp` in a file that compares
enumerators, directly or through its rig, so a failure prints
`Reason::rate_limited` rather than a number. When an enum lives in a namespace
the header does not list yet, add the namespace there.

Rigs and fakes report their own problems as test failures, never with
`assert`. A fake the library calls records a mismatch with `EXPECT_*` or
`ADD_FAILURE()` and carries on. Setup that cannot go on, such as a fixture
file that does not open or an id that does not parse, goes through
`test_support::Setup`, which throws, so GoogleTest fails that one test and
runs the rest.

## Shared fakes and rigs

A rig that one domain uses stays in that domain, like
`tests/unit/core/trophies/rig.hpp`. Once a second domain needs it, it moves to
`tests/support`, the `libchiaking_test_support` target, rather than being
copied or included across domains. Everything there is a class in
`test_support` or one of its area namespaces:

| Header | Holds |
|---|---|
| `setup.hpp` | `Setup`, which checks a rig's setup and unwraps an `Expected` or `optional`, throwing `SetupFailure` when it fails |
| `live_providers.hpp` | `LiveProviders`: the process threading provider, real clocks, naps and named threads, for tests that run in real time |
| `process_threading.hpp` | the process-wide threading provider |
| `clock.hpp` | `FakeClock`, a clock the test sets and advances |
| `stop_states.hpp` | `StopState` that a test raises, `AtomicStop` that another thread raises, `Stopped` that is always raised and hands out a stopped token, and `WatchedStop` that counts how often it is asked |
| `random.hpp` | `FixedRandom`, `CountingRandom`, `FailingRandom`, `SeededRandom` with a repeatable SplitMix64 sequence, and `ScriptedRandom`, which answers queued draws and counts up otherwise |
| `bytes.hpp` | `Bytes`, which writes and reads hex through the library's encoder |
| `log.hpp` | `FakeLog`, which keeps every log line |
| `thread_set.hpp` | `ThreadSet`, the threads a callback was seen on |
| `waiter.hpp` | `FakeWaiter`, a `PollWaiter` that records each wait and can advance a `FakeClock` or raise a `StopState` |
| `websocket.hpp` | `RefusingWebSocket`, which refuses every connection |
| `utc_offset.hpp` | `FixedUtcOffset`, a UTC offset the test chooses |
| `enum_printing.hpp` | a `PrintTo` that names the enumerator, in every namespace the tests compare enums from |
| `json/tree.hpp` | `JsonTree`, which parses, builds and reads generic JSON |
| `crypto/software_crypto.hpp` | `SoftwareCrypto`, a portable `CryptoProvider` in its own `libchiaking_test_crypto` library |
| `crypto/refusing.hpp` | `NoCrypto`, `CountingKeyPair` and `RefusingTakionCrypto`, providers that refuse the work |
| `api/fakes.hpp` | `test_support::api`: offline and interruptible HTTP |
| `catalog/scripted_ps.hpp` | `test_support::catalog`: `ScriptedPs`, a PlayStation HTTP fake that answers by URL fragment, `ServerTime` and `CatalogRig` |
| `catalog/idle_catalog.hpp` | `IdleCatalog`, a catalog that never fetches |
| `consoles/paired_consoles.hpp` | `test_support::consoles`: `PairedConsoles`, known consoles already paired with a user |
| `consoles/start_states.hpp` | `StartStates`, which records a title start |
| `kakusan/*.hpp` | `test_support::kakusan`: the draft-05 fixtures, control messages, loopback transports and secure datagram providers |
| `net/addresses.hpp` | `test_support::net`: `Addresses`, documentation addresses, endpoints and interfaces |
| `net/ice_network.hpp` | `Network` and `Simulation`, a simulated network with NATs for ICE agents |
| `net/simulated_network.hpp` | `SimulatedNetwork`, machines with sockets and pollers |
| `net/scripted_time_servers.hpp` | `ScriptedTimeServers`, NTP servers with a chosen skew and delay |
| `play/signed_in_player.hpp` | `test_support::play`: `SignedInPlayer`, a user signed in for play |
| `play/scripted_clearance.hpp` | `Title` and `ScriptedClearance`, a title cleared or refused for launch |
| `play/launch_rig.hpp` | `LaunchCase`, a cloud launch request, with a recording listener and a datacenter store |
| `play/scripted_cloud_ps.hpp` | `ScriptedCloudPs`, the PlayStation cloud services answering a launch |
| `play/session_rig.hpp` | `SessionRig`, a session over a scripted link, with recorded events and media |
| `play/public_household.hpp` | `PublicHousehold`, `Play` and `KnownConsoles` built through the public faces |
| `play/source_stream.hpp` | `SourceStream`, the reference source stream and its datagrams |
| `psn/*.hpp` | `test_support::psn`: a scripted `FakeHttp`, the PSN fixtures and push notifications |
| `users/signed_in.hpp` | `test_support::users`: `SignedIn`, which signs a user in over any HTTP provider |

## Running tests

```bash
cmake --build build
ctest --test-dir build
ctest --test-dir build -R '^core/trophies/'
ctest --test-dir build -R 'TrophySummary\.ExpiresSixHoursAfterItWasKept'
build/core_trophies_gtests --gtest_filter='TrophyTitleSets.*'
```

Each executable takes the usual GoogleTest flags, such as `--gtest_repeat`
and `--gtest_break_on_failure`.

`net_kakusan_real_socket_smoke` opens real UDP sockets, so ctest labels it
`network` and disables it unless the build is configured with
`-DLIBCHIAKING_NETWORK_TESTS=ON`. Run the executable directly to try it once;
it reports a missing interface as a skipped test.

Golden files live under `tests/data`. `tests/data/play/README.md` says how
the play transcripts are made and regenerated.
