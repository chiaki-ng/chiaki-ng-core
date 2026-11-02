# Building libchiaking

## Requirements

* CMake 3.24 or newer, Ninja (recommended; the Makefile uses it when it is on
  `PATH` and falls back to CMake's default generator otherwise) and a C++23 compiler whose standard library has
  `<format>` and `<expected>`: GCC 16 or newer, Clang 17 or newer (Android
  NDK r26 or newer), or Visual Studio 2022. Configuring stops with an error
  naming these minimums when the compiler falls short.
* Optional providers, each picked up automatically when found:
  * OpenSSL 3 (`libcrypto`) or BoringSSL for crypto;
  * curl 7.86 or newer built with WebSocket support (`ws`, `wss`) for HTTP and
    WebSocket;
  * miniupnpc for UPnP port mapping.
* For the tests only, GoogleTest. An installed GoogleTest (for example
  `brew install googletest`, `apt install libgtest-dev`, or a prefix on
  `CMAKE_PREFIX_PATH`) is used when `find_package(GTest)` finds one;
  otherwise the copy vendored under `tests/lib/googletest` is built, so the
  tests need no network. No library and no installed package depends on it.

## Configure and build

The root Makefile builds the library on its own using the CMake target graph:

```bash
make                 # Release libraries in build-standalone, without tests
make test            # build and run tests in build-standalone-tests
make docs            # generate Doxygen HTML in build/docs/html/index.html
make docs-serve      # generate and serve docs at http://127.0.0.1:8000
make docs-lint       # check public API documentation
make docs-lint-internal AREA=users  # check internal header documentation
make install PREFIX=/opt/libchiaking
make clean           # clean compiled outputs, retaining CMake configuration
```

The documentation targets need Python 3 and Doxygen; Graphviz is optional.
Use `make help` for targets and overrides. For example:

```bash
make BUILD_TYPE=Debug JOBS=8
make CMAKE_ARGS='-DCURL_ROOT=/opt/homebrew/opt/curl'
make test CTEST_ARGS='-R package_install_consumer'
```

`JOBS` defaults to the number of host processors and `GENERATOR` to Ninja when
it is installed. A build directory keeps the generator it was first configured
with, so delete an older Makefiles-based directory before switching.
`BUILD_DIR`, `TEST_BUILD_DIR`, `CMAKE`, `CTEST`, and `PYTHON` are also
overridable. Build directories resolve from the calling directory. The docs
targets always write to the repository's `build/docs` directory.

For a local preview, run `make docs-serve` and open
[localhost:8000](http://localhost:8000). Override the port with
`make docs-serve PORT=8080`. Stop the server with Ctrl-C.

The equivalent direct CMake commands are:

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

A plain configure builds every library with whatever providers it finds.
The public API is C++ only.

| Target | Needs |
|---|---|
| `libchiaking` (the core) | nothing; the Kakusan sources only with `LIBCHIAKING_KAKUSAN` (ON by default), and without it `ChiakiNg::kakusan` answers `Kakusan::Unavailable::not_built` |
| `libchiaking_gateway`, `libchiaking_chiaki_ng` | the core |
| `libchiaking_crypto_openssl` / `libchiaking_crypto_boringssl` | the crypto provider |
| `libchiaking_dtls_openssl` | OpenSSL with `libssl` 3.2 or newer |
| `libchiaking_curl` | curl |
| `libchiaking_default_platform` (`platform::DefaultPlatform`) | a crypto provider and curl |
| `libchiaking_media_opus` | Opus |

The installed package reports each optional target as a component of the same
name without the `libchiaking_` prefix, for example `default_platform`, and
`kakusan` when the core was built with it.

## Per-platform builds

Each platform has its own target. It builds on that platform's own host, in
`build/<platform>`; the only cross build is macOS to iOS.

```bash
make darwin            # macOS, in build/darwin
make linux             # Linux, in build/linux
make windows           # Windows (MSYS2 or Git Bash make), in build/windows
make ios DEPS_PREFIX=/path/to/ios-prefix
make ios-simulator DEPS_PREFIX=/path/to/simulator-prefix
make darwin-test       # also linux-test and windows-test
```

A target run on the wrong host stops before configuring. `DEPS_PREFIX` puts
a dependency prefix (OpenSSL, curl, Opus, miniupnpc) on
`CMAKE_PREFIX_PATH` for any platform. The iOS targets require it. `GENERATOR`, `MACOS_DEPLOYMENT_TARGET`,
`IOS_DEPLOYMENT_TARGET` (both 26.0 by default) and `IOS_ARCHITECTURES` (arm64)
are overridable, and `CMAKE_ARGS`, `BUILD_TYPE` and `JOBS` apply as above.

## Takion messages

The Takion control messages are encoded with [nanopb](https://github.com/nanopb/nanopb),
vendored under `src/lib/nanopb`. The generated `takion.pb.c` and `takion.pb.h`
are committed under `src/core/net/protocol/takion/generated`, so building needs
no protobuf compiler. After changing `proto/takion.proto` or
`proto/takion.options`, regenerate them with a nanopb 0.4.9.2 checkout whose
`requirements.txt` is installed:

```sh
python3 tools/proto/generate.py --nanopb /path/to/nanopb
```

The script stores every `string` field as bytes, so embedded zero bytes
survive, and every enum field as `int32`, so values the schema does not name
keep their number, as C++ protobuf does.

## Build files

`CMakeLists.txt` only orders the pieces under `cmake/`:

| Path | Holds |
|---|---|
| `cmake/Toolchain.cmake` | C++23 checks and the strict warning set |
| `cmake/platform/select.cmake` | picks one platform file from the target system |
| `cmake/platform/<platform>.cmake` | that platform's sources, libraries and definitions |
| `cmake/platform/<platform>.mk` | that platform's make target arguments |
| `cmake/Vendored.cmake`, `cmake/Takion.cmake` | vendored libraries, nanopb among them, and the generated Takion version header |
| `cmake/Core.cmake` | the `libchiaking` core target and its public header set |
| `cmake/ChiakiNg.cmake` | the `ChiakiNg` assembly over the core and the default platform |
| `cmake/providers/` | crypto, curl, gateway, Opus and the default platform assembly |
| `cmake/Install.cmake`, `cmake/Docs.cmake` | the installed package and the docs targets |

Platform differences live only in `cmake/platform/`. The core and provider files
read the `LIBCHIAKING_PLATFORM_*` variables the platform file sets and never
test the platform themselves.

## API reference

The public headers carry Doxygen comments. With Doxygen installed (and
Graphviz for the class diagrams):

```bash
python3 tools/docs/docs.py build
```

writes the reference to `build/docs/html/index.html`. The same command is the
`docs` CMake target.

Every public declaration must be documented. The check that enforces it is

```bash
python3 tools/docs/docs.py lint
```

also available as the `docs-lint` target, and run by `ctest` as `docs_lint`
when the tests are built. It fails when a header has no
`@file` block, when a class, function, member, type alias or enum value has no
comment, when a documented function leaves a parameter out, or when a
namespace is not described in `docs/doxygen/namespaces.dox`.

The same command also reports every public function in `include/` that has
no `@param` for a named parameter or no `@return` for a non-void result,
followed by a count. The report does not fail the check yet;
`python3 tools/docs/docs.py lint --strict-signatures` makes those gaps fail
it too.

Internal headers under `src/core` and `src/platform` are being documented
area by area. Their check is

```bash
python3 tools/docs/docs.py lint-internal
python3 tools/docs/docs.py lint-internal --area users
python3 tools/docs/docs.py lint-internal --area src/core/net/protocol
```

It applies the public rules to every `.hpp` and `.h` there, private members
included, using `docs/doxygen/Doxyfile.lint.internal`. Namespaces that exist
only in `src` are described in `docs/doxygen/internal_namespaces.dox`, so they
stay out of the public reference. `@param` and `@return` gaps fail it
outright. It prints each problem, then a count per area (each directory of
`src/core`, plus `platform`) and a total, and exits non-zero when any remain.
`--area` takes an area name or any directory under `src`. The `src/lib`
vendored code is never read.

The check is the `docs-lint-internal` target, and it runs in `ctest` as
`docs_lint_internal` by default. Configure with
`-DLIBCHIAKING_DOCS_LINT_INTERNAL=OFF` to leave it out.

Both commands run Doxygen again when it dies from a signal. Doxygen 1.18
does so at random while reading Markdown pages and `.dox` files.

## Options

| Option | Default | Effect |
|---|---|---|
| `LIBCHIAKING_CRYPTO_OPENSSL` | ON when OpenSSL is found and BoringSSL is off | OpenSSL crypto provider |
| `LIBCHIAKING_CRYPTO_BORINGSSL` | OFF | BoringSSL crypto provider; mutually exclusive with OpenSSL |
| `LIBCHIAKING_CRYPTO_LIBNX` | ON on the Switch platform, OFF elsewhere | libnx crypto provider for Switch homebrew; needs an AArch64 compiler with the cryptographic extension. On an AArch64 host with OpenSSL, the tests also build its portable GMAC and ECDH half to check it against OpenSSL |
| `LIBCHIAKING_CURL` | ON when curl with WS and WSS is found | curl HTTP and WebSocket providers |
| `LIBCHIAKING_MINIUPNPC` | ON | UPnP gateway provider when miniupnpc is found |
| `LIBCHIAKING_TAKION_VERSION_OVERRIDE` | 0 | Takion version every session uses instead of the negotiated one; a per-session `takion_version_override` still wins, and 0 keeps negotiation. A pinned build disables the seven scripted-console tests that assert negotiated versions, so run the full suite on a default build |
| `BUILD_TESTING` | ON when libchiaking is the top-level project | build and register the tests |
| `LIBCHIAKING_BUILD_TESTS` | OFF | build the tests when libchiaking is a subproject |
| `LIBCHIAKING_CHIAKI_NG_REFERENCE` | empty | chiaki-ng checkout for the reference parity tests |
| `LIBCHIAKING_VENDORED_GTEST` | OFF | build the tests against the vendored GoogleTest even when one is installed |

Setting an option explicitly always wins over detection. Turning a provider on
that cannot be found is a configure error.

## Host builds

Apple clang is the default compiler on macOS. To build and test with
Homebrew GCC instead, give it its own build directory:

```bash
cmake -S . -B build-gcc -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DLIBCHIAKING_NETWORK_TESTS=OFF \
    -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3 \
    -DCMAKE_C_COMPILER=gcc-16 -DCMAKE_CXX_COMPILER=g++-16
cmake --build build-gcc
ctest --test-dir build-gcc
```

GCC links against libstdc++, while Homebrew's Protobuf and Abseil are built
with Apple clang against libc++. Every target that links Protobuf therefore
fails to link under GCC on macOS until Protobuf and Abseil are built with the
same GCC; the targets that do not use Protobuf build and run. A Homebrew
GoogleTest has the same problem, so configure a GCC build with
`-DLIBCHIAKING_VENDORED_GTEST=ON`.

## Pointing at providers

Each provider honours the usual CMake `<Package>_ROOT` variables. For curl,
`CURL_ROOT` alone is enough: libchiaking puts `${CURL_ROOT}/lib/pkgconfig` in
front of `PKG_CONFIG_PATH` before looking for curl, so the WebSocket protocol
check reads the same curl that is linked rather than a system copy.

macOS with Homebrew:

```bash
cmake -S . -B build -G Ninja \
    -DCURL_ROOT=/opt/homebrew/opt/curl \
    -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3
```

BoringSSL installs an `OpenSSLConfig.cmake`; point CMake at it:

```bash
cmake -S . -B build -G Ninja \
    -DLIBCHIAKING_CRYPTO_BORINGSSL=ON \
    -DOpenSSL_DIR=/path/to/boringssl/lib/cmake/OpenSSL \
    -DCURL_ROOT=/opt/homebrew/opt/curl
```

With BoringSSL the curl WebSocket provider is built without the AIA
intermediate-certificate fetch, which is OpenSSL-only.

## Sanitizers

```bash
cmake -S . -B build-asan -G Ninja \
    -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
    -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake -S . -B build-tsan -G Ninja \
    -DCMAKE_C_FLAGS=-fsanitize=thread -DCMAKE_CXX_FLAGS=-fsanitize=thread \
    -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread
```

The `LinkSecrets` cases in `core_play_default_platform_gtests` replace
`operator new` and `operator delete` to scan freed memory for the bearer token
and the NPSSO; under a sanitizer they keep the sanitizer's allocator and check
only the API results.

## Installing and consuming

```bash
cmake --install build --prefix /opt/libchiaking
```

This installs the static libraries, every header under `include/libchiaking`
and a CMake package:

```cmake
find_package(libchiaking 0.1 REQUIRED COMPONENTS default_platform)
target_link_libraries(app PRIVATE libchiaking::libchiaking libchiaking::default_platform)
```

The package finds Protobuf, Threads and whichever of OpenSSL, BoringSSL, curl
and Opus libchiaking was built with, so pass the same `*_ROOT` hints to the
consumer. Every library in the table above is exported as `libchiaking::` plus
its name without the `libchiaking_` prefix, for example `libchiaking::chiaki_ng`
or `libchiaking::default_platform`, and the core as `libchiaking::libchiaking`; the optional ones only when built. The same
`libchiaking::` names are available as aliases when libchiaking is added with
`add_subdirectory`.

`package_install_consumer` installs the build tree into a scratch prefix,
builds `tests/packaging/consumer` against it with `find_package`, and runs it.
