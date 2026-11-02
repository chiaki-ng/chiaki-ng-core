# libchiaking

libchiaking is chiaki-ng's protocol as a standalone C++23 library. A frontend
supplies a platform and a few stores, and gets back a small set of domain
objects that sign a player in, find their consoles and games, and start a
session on a console at home, on a console over the internet, or in the cloud.

The library is licensed under AGPL-3.0-or-later with additional permissions
for linking with OpenSSL and for distribution through application stores. It is
an unofficial project that is not endorsed or certified by PS Interactive
Entertainment LLC.

## Where to start

Everything hangs off one object, libchiaking::ChiakiNg, declared in
`libchiaking/chiaki_ng.hpp`.

1. Build a platform. libchiaking::platform::DefaultPlatform is the stock one;
   a host with its own networking, crypto or threading implements the
   provider interfaces in libchiaking::platform instead.
2. Implement the stores named by libchiaking::ChiakiNg::Stores. The library
   decides what is kept and when; the host decides where it is kept.
3. Call libchiaking::ChiakiNg::open and keep the result for the life of the
   application.
4. Ask it for the context you need: libchiaking::ChiakiNg::users,
   libchiaking::ChiakiNg::consoles, libchiaking::ChiakiNg::catalog,
   libchiaking::ChiakiNg::trophies, libchiaking::ChiakiNg::messages,
   libchiaking::ChiakiNg::play or libchiaking::ChiakiNg::kakusan.

## The contexts

The library is organised by domain, and each domain is a namespace with one
umbrella header.

| Namespace | Umbrella header | What it is for |
|---|---|---|
| libchiaking::users | `libchiaking/users.hpp` | Signing in to PSN and holding each player's credentials |
| libchiaking::consoles | `libchiaking/consoles.hpp` | Finding, registering, waking and remembering PS4 and PS5 consoles |
| libchiaking::catalog | `libchiaking/catalog.hpp` | The player's games, what they own and what can be streamed |
| libchiaking::trophies | `libchiaking/trophies.hpp` | The player's trophy level, trophy list and each title's trophies |
| libchiaking::messages | `libchiaking/messages.hpp` | Reading and sending the player's PSN messages |
| libchiaking::play | `libchiaking/play.hpp` | Starting a session and everything a frontend does with a live one |
| libchiaking::kakusan | `libchiaking/kakusan.hpp` | Letting invited players join a live session from elsewhere |
| libchiaking::platform | `libchiaking/platform/platform.hpp` | The ports a host provides, and the stock implementations |
| libchiaking::testing | `libchiaking/testing/` | Fakes and scripted doubles for exercising the API without a console |

## Playing

libchiaking::play::Play starts a session in one of three places:
libchiaking::play::Play::start_on_console,
libchiaking::play::Play::start_over_internet and
libchiaking::play::Play::start_on_cloud. Each takes the player, what to play
on, the stream libchiaking::play::Preferences, a
libchiaking::media::MediaSink that receives video and audio, and a
libchiaking::play::SessionEvents that is told what happens.

A started libchiaking::play::Session then exposes the things a player can do
while it runs: libchiaking::play::Session::controllers for input,
libchiaking::play::Session::seats for extra local players,
libchiaking::play::Session::keyboard, libchiaking::play::Session::microphone
and libchiaking::play::Session::share. How a session ended is reported as a
libchiaking::play::Outcome.

## Conventions

- A call that can fail returns `Expected<T, E>`, declared in
  `libchiaking/detail/expected.hpp`. The error type is specific to the call
  and each of its values is documented where it is declared.
- Many commands on a live session only queue a request. The reference for
  each says so and names the libchiaking::play::SessionEvents callback that
  reports the result.
- Secrets such as tokens and passcodes are held in move-only types that wipe
  their memory when destroyed.
- Headers under `libchiaking/detail/` hold the definitions the umbrella
  headers need. Their classes are part of the API, but a frontend reaches them
  through the umbrella headers and never includes a `detail/` header itself.

## More

- **Public API** in the sidebar lists each face a frontend includes and the classes it offers.
- **Internal API** in the sidebar walks the repository folder by folder, public headers and implementation alike.
- [Building libchiaking](../BUILDING.md)
- [Design and C++ style](../CPP_STYLE.md)
