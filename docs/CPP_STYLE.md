# Design and C++ style

libchiaking is C++23, organised by domain-driven design, with every workflow
modelled as an explicit state machine. This page describes how the library is
put together and the rules its code follows, for anyone reading the source or
contributing to it.

## Domain-driven design

### Bounded contexts

The library is split by what a player does, not by protocol or by layer. Each
context is a namespace, a folder under `src/core`, a folder of public headers
under `include/libchiaking/detail`, and one umbrella header.

| Context | Namespace | Owns |
|---|---|---|
| Users | libchiaking::users | Signing in to PSN, accounts, credentials, membership |
| Consoles | libchiaking::consoles | Finding, pairing, waking and remembering consoles |
| Catalog | libchiaking::catalog | Games, ownership, and what may be streamed |
| Trophies | libchiaking::trophies | Trophy levels, trophy lists and each title's trophies |
| Messages | libchiaking::messages | Conversations with friends and their messages |
| Play | libchiaking::play | Starting a session and everything done with a live one |
| Kakusan | libchiaking::kakusan | Inviting players elsewhere into a live session |

Two smaller domains have namespaces and public headers but no umbrella
header and no entry point of their own; play composes them.

| Domain | Namespace | Owns |
|---|---|---|
| Controllers | libchiaking::controllers | Controller state, motion and feedback to a pad |
| Media | libchiaking::media | Video and audio leaving a session, and the microphone entering it |

A context reaches another only through that context's public types. Play
takes a libchiaking::consoles::Console, a libchiaking::catalog::ProductId and
a libchiaking::users::PsUser; it does not reach into how they were obtained.
When a context needs another to act for it, it asks through the port the
other context offers it, never through that context's core.

Beneath the contexts sit the things none of them own: the console protocols
in `src/core/net`, PS's services in `src/core/psn`, and encoders and other
utilities in `src/core/utils`. They serve the contexts and know nothing about
them.

### Ubiquitous language

Names come from what a player or a frontend author would say, and the same
word means the same thing in the header, the implementation, the tests and
the documentation.

- A local player takes a libchiaking::play::Seat. They do not "join pad 2".
- A call that is turned down returns a refusal that says why in domain terms:
  libchiaking::play::StartRefusal, libchiaking::play::SeatRefusal,
  libchiaking::play::LaunchRefused.
- How a session ended is a libchiaking::play::Outcome, not an error code.
- Protocol words such as Takion, Senkusha and ctrl stay in `src/core/net`.
  They do not appear in a public name. When a wire value has to cross into a
  context, an adapter translates it into a domain type first.

If a name needs a comment to explain what it means in the domain, the name is
wrong.

### Layers inside a context

Every context has the same three folders, plus the file that implements its
public entry point. A context that other contexts call into also has a
`ports/` folder.

| Folder | Holds | May depend on |
|---|---|---|
| `model/` | Values, entities, policies and lifecycle machines: the rules, with no I/O | Nothing outside the model |
| `usecases/` | One class for each thing a caller wants done | The model, and adapters through their interfaces |
| `adapters/` | Everything that talks to a console, to PS or to storage | The model, `src/core/net`, `src/core/psn`, platform ports |
| `ports/` | One class for each context that calls in, offering only what that context asks | The context's own adapters and model, and `src/core/net` and `src/core/psn` through the context's shared request helper |

- A port is named for its caller. `src/core/users/ports` holds
  libchiaking::users::CatalogCalls, libchiaking::users::ConsoleCalls,
  libchiaking::users::PlayCalls, libchiaking::users::TrophyCalls and
  libchiaking::users::MessageCalls, with libchiaking::users::MembershipCalls
  for the users context's own membership questions and
  libchiaking::users::RemotePlayAccess, the token PlayCalls lends. The
  catalog includes only its own port, and so sees nothing the play context
  may ask.
- Ports share their requests through libchiaking::users::AuthorisedRequest,
  which alone reaches PS's services; a port never calls another port.

- A use case is named for the act: `usecases::Pair`, `usecases::Find`,
  `usecases::Wake`, `usecases::Start`, `usecases::SignIn`,
  `usecases::ListGames`, `usecases::ClearForLaunch`. It receives the platform
  ports it needs by reference, sequences the model and the adapters, and
  returns a domain result.
- The model decides. An adapter never decides whether a title may be
  launched or a seat may be taken; it fetches facts and carries out what the
  model chose.
- An adapter is also the translation boundary. PS's JSON, a console's
  reply and a stored record are parsed inside the adapter that knows the
  format, and leave it as model types.
- Dependencies point inward. Model code that needs an adapter header, a
  socket or an HTTP type has put a rule in the wrong place.
- The model may use the platform's plain values, such as
  libchiaking::platform::IpAddress, and the utilities in `src/core/utils`.
  It holds no provider and nothing that locks, waits for another thread or
  reads a clock; that is an adapter.

### Values, entities and aggregates

- A value object is defined by its contents and is valid from construction.
  When not every input is valid, a static factory returns `std::optional` or
  `Expected`: libchiaking::play::SeatNumber::create,
  libchiaking::play::AccessCode::parse. It compares by value and has no
  setters.
- An entity has identity and a life: libchiaking::consoles::Console,
  libchiaking::users::PsUser, libchiaking::play::Session. It is not copyable
  and is shared by `std::shared_ptr` where more than one party holds it.
- An aggregate guards a group of these and is the only way to change them.
  libchiaking::play::Seats owns the four seats of a session;
  libchiaking::consoles::KnownConsoles owns the consoles a host remembers;
  libchiaking::users::SignedInUsers owns the signed-in players. Callers get
  snapshots out and send commands in. They never hold a reference into the
  aggregate's state.

### Ports and the composition root

The domain asks for capabilities through interfaces and never names an
operating system or a third-party library.

- Platform ports, in libchiaking::platform, are what a host machine provides:
  networking, HTTP, WebSocket, crypto, clocks, threads, randomness and
  diagnostics. Stock implementations live in `src/platform`.
- Stores are what a host keeps: libchiaking::consoles::ConsoleStore,
  libchiaking::users::CredentialStore, libchiaking::play::DatacenterStore,
  libchiaking::kakusan::DeviceStore, libchiaking::platform::RecordStore.
  The library decides what is kept and when; the host decides where.
- Outward ports are how the domain reports back:
  libchiaking::play::SessionEvents, libchiaking::play::MediaSink,
  libchiaking::consoles::PairingWatcher.
- Everything is wired in one place. libchiaking::ChiakiNg::open takes the
  platform and the stores and builds every context. No context constructs
  its own collaborators, and nothing is a global.
- Code under `src/core` does not test for an operating system. Conditional
  compilation belongs in `src/platform`.
- A public header never names `libchiaking::net` or `libchiaking::psn`. A
  type the public API shares with those layers is defined in the public
  namespace, and the layer gives it its own name with an alias under
  `src/core`, as `src/core/psn/auth/credentials.hpp` does for the users
  context's credentials.

### Public facade and private core

A public class that has real machinery behind it declares a nested
`class Core;` and holds only a pointer to it. The core is defined under
`src/` (for example `src/core/play/play_core.hpp`) and owns the use cases
and adapters.

This keeps public headers free of protocol types and of the state machine
library, and lets the inside of a context change without touching what a
frontend compiles against.

## State machines

A workflow is anything with more than one step, anything that waits, and
anything that can be cancelled or can fail part-way. Every workflow is an
explicit finite state machine. It is never a set of booleans that happen to
be read in the right order.

### One implementation

Machines are written with [boost-ext/sml](https://github.com/boost-ext/sml),
vendored in `src/lib`. There is no second, project-owned state machine
framework. SML is an implementation detail: no public header includes it and
no public type exposes one of its states.

### The shape of a machine

`src/core/consoles/model/pairing/home_pairing_lifecycle.hpp` is the reference
example. A machine has its own namespace and defines, in this order:

1. State types, one empty struct per state.
2. Event types, one struct per thing that can happen, each carrying the
   attempt it belongs to.
3. Effect types, one struct per thing the machine can ask for, gathered in a
   `std::variant`.
4. A context holding the queue of effects, the current attempt, the reason
   for the pending ending, and counters for stale and unexpected events.
5. The transition table.
6. A small owning class with `process (event)` and a `state ()` that projects
   the machine's state onto a plain enum.

```cpp
return make_transition_table (
*state<IdleState> + event<Start> / begin_knock = state<KnockingState>,
state<KnockingState> + event<KnockAnswered> / wait = state<SettlingState>,
state<KnockingState> + event<Cancel> / begin_cleanup = state<ClosingState>,
state<KnockingState> + event<Failed> / begin_cleanup = state<ClosingState>,
state<ClosingState> + event<Closed>[paired] / publish = state<PairedState>);
```

### Transitions decide, effects do

An action in the table never opens a socket, sends a request, reads a clock
or touches storage. It records an effect. The component that owns the
machine runs the effect and feeds the result back as a new event.

```text
event -> transition -> new state and effects
                            |
                            v
                       effect runner
                            |
                            v
                       result event
```

This is what makes a machine testable without a network: a test feeds events
and asserts on states and effects, as `home_pairing_lifecycle_tests.cpp`
does. It is also why the machine belongs in `model/` while its effect runner
belongs in `adapters/`.

### Rules

- States and events are closed sets of types. A transition happens only
  through a typed event.
- Every asynchronous result names the attempt that started it. A result for
  an attempt that is no longer current is counted as stale and dropped. It
  cannot revive a workflow that was cancelled or restarted.
- An event the current state does not expect is counted and reported as
  refused. It is never treated as success and never ignored silently.
- Failure and cancellation are listed per source state, because what has to
  be released depends on how far the workflow got.
- Cleanup is a state. A machine that acquired something passes through a
  closing state on every path out, including failure and cancellation, and
  reaches its terminal state only when cleanup has reported back.
- A machine has one owner. Only the owner calls `process`, reads the state or
  destroys the machine. Other threads, timers and callbacks hand their events
  to the owner; they do not call into the machine.
- An action does not call `process` again. Follow-on work is an effect that
  returns as an event after the current transition has finished.
- Machines stay small. A larger workflow is several machines coordinated
  through events and results, not one table covering every combination of
  console, route and service.
- Control and lifecycle go through the machine. Video, audio and controller
  data do not; they move through their own bounded queues once a stream is
  running.
- State that a caller needs is projected onto a stable enum or snapshot,
  such as libchiaking::play::SessionState. The machine's own representation
  is never stored or exposed.

### Where they are

| Workflow | Machine |
|---|---|
| Pairing a console at home | `src/core/consoles/model/pairing/home_pairing_lifecycle.hpp` |
| A live session | `src/core/play/model/session_lifecycle.hpp` |
| Bringing a home link up | `src/core/play/adapters/links/home_link_progress.hpp` |
| Reconnecting a lost link | `src/core/play/adapters/links/reconnecting_link.hpp` |
| Starting over the internet | `src/core/play/adapters/links/internet/internet_start.hpp` |
| Hole punching | `src/core/play/adapters/links/internet/wire/` |
| PSN signalling | `src/core/psn/remote_play/` |
| The control channel | `src/core/net/protocol/remote_play/ctrl_lifecycle.hpp` |
| Starting the control channel at home | `src/core/net/protocol/remote_play/stream/local_ctrl.hpp` |
| A request to the console over TCP | `src/core/net/protocol/remote_play/tcp_exchange.hpp` |
| Moving control onto RUDP | `src/core/net/protocol/remote_play/rudp_switch.hpp` |
| The RUDP handshake | `src/core/net/transport/rudp/rudp_handshake_lifecycle.hpp` |
| The stream handshake | `src/core/net/protocol/remote_play/stream/active_handshake_lifecycle.hpp` |
| A calibrated home stream | `src/core/net/protocol/remote_play/stream/local_start_lifecycle.hpp` |
| Takion handshake and client | `src/core/net/protocol/takion/` |
| Takion version negotiation | `src/core/net/protocol/takion/client/version_negotiation.hpp` |
| Outgoing generic data | `src/core/net/protocol/takion/generic_data_lifecycle.hpp` |
| Senkusha | `src/core/net/protocol/senkusha/lifecycle.hpp` |

## Recurring patterns

### Commands queue, events report

A command on a live session checks what it can at once, queues the request
and returns. The result arrives later through an outward port.
libchiaking::play::Seats::add_player returns as soon as the request is
queued, and the answer comes through libchiaking::play::SessionEvents. The
documentation of every such call names the event that completes it.

### Refusals are values

A call that can be turned down returns `Expected<T, E>`, or
`std::optional<E>` when it has no value to give on success, where `E` belongs
to that call and lists only the reasons that call can give. There is no shared
error enum, no status code beside loose result fields and no exception across
the public boundary.

### Snapshots out

Reading state returns a copy that is consistent at the moment of the call,
such as libchiaking::play::Seat or libchiaking::play::Statistics. A caller is
never handed a reference that another thread may be changing.

### Secrets are a type

Tokens, passcodes and session material live in move-only types that wipe
their memory on destruction, from the moment they are parsed until they are
spent. Copying one is an explicit `clone ()`.

### Cancellation is passed in

A call that may run for a long time takes a
libchiaking::platform::StopToken. The caller decides when to stop; the
workflow turns that into its cancel event and still runs its cleanup.

### Doubles ship with the library

libchiaking::testing holds in-memory stores, a manual clock, an in-memory
network, a fake console and libchiaking::play::testing::ScriptedLink. Tests
drive the public API against these doubles. Conformance checks in
libchiaking::platform::conformance let a host run the same expectations
against its own providers.

## Ownership and lifetime: RAII

- A resource is wrapped by its owning type the moment it is acquired, and released in exactly one place: that type's destructor. Resources include secret bytes, handles, sockets, crypto contexts, log scopes, in-flight markers and subscriptions.
- Prefer composing owning members (rule of zero) over hand-written destructors. Write the rule of five only for a type that is itself a resource.
- No guard classes that reach into another type's fields to clean them up. No free `wipe()`/`cleanup()` overloads.
- Secrets (tokens, npsso, auth codes, cookies, session material, request and response bodies) live in one move-only wiping type from creation to destruction. They are never copied through a plain `std::string` or `std::vector`. A copy is an explicit `clone()`. The only `volatile` or `explicit_bzero` code is the single secure-zero primitive.

## Types

- A type with state or rules is a class with private members, valid as soon as it is constructed. If construction can fail, use a static factory returning `Expected<T, E>`, not a constructor plus `is_valid()`.
- An aggregate `struct` is only for plain data with no rules, such as an endpoint descriptor or a cache key.
- One-argument constructors are `explicit`. Mark functions `const` when they don't mutate, and `noexcept` where it is true and matters.
- Use strong types for distinct values (`BearerToken`, `Npsso`, `AuthCode`, `SessionCookie`), not `string_view`s that share a label.
- Split god objects into focused classes and compose them.

## Errors and results

- Failure comes back as `Expected<T, E>` or `std::optional`. A call with no value on success may return `std::optional<E>`, empty when it succeeded. Never a status enum beside loose result fields, `nullptr` meaning error, out-params, or a mutable context struct with sticky flags set as a side effect.
- An HTTP status is not a transport failure. A response carries its status, and each client maps statuses to its own domain error.
- Methods return parsed domain types (`CreatedSession`, `TitleDetails`), not raw replies. Parsing lives in the class that knows the format; callers make decisions.

## Headers and files

- A header declares the class that owns the behaviour. No C-style headers of free functions plus loose structs they fill in.
- A file is either a class or it is not. In a class's files every function is a member of that class, private or static where it is internal. No free helper functions and no anonymous-namespace helpers scattered around a class's `.cpp`. A helper that does not belong to the class belongs to another class, or to the one shared utility that already owns that job.
- Free functions exist only in utility files that hold no class (`json::`, `text::`), and each such utility exists once.
- Files follow meaning, not size budgets. A function with one caller is a private member of the class that calls it. A request body is built inside the method that sends it, not in a separate `*_codec` file. A new file needs a reason beyond "keep each file small".
- Use namespaces, not prefixes: `psn::auth::Authenticator`, not `psn::psn_authorize` or `PsnPs5StartEnvelope` inside `namespace psn`.

## Formats and helpers

- JSON, URLs and form bodies are built with the `json::` builder or a `text::` encoder and then serialised. Never concatenated from hand-escaped fragments into fixed buffers with `memcpy` and sticky `valid_` flags.
- Each encoder (hex, base64, URL, form) and each helper (case-insensitive compare, ASCII classification) exists once. Use it instead of writing another.
- Prefer the standard library (`std::ranges`, `std::span`, `std::string_view`, `std::to_chars`) over hand-rolled loops.
- Cancellation is the exception: a call takes libchiaking::platform::StopToken, never `std::stop_token`.

## Documentation

- Every declaration in `include/libchiaking` carries a Doxygen comment, and `tools/docs/docs.py lint` fails when one does not. That covers each header (`@file`), class, enum and enum value, type alias, public or protected member, and free function. Namespaces are described once, in `docs/doxygen/namespaces.dox`.
- Write `///` lines above the declaration, starting with `@brief`. Add `@param`, `@return` and `@retval` where a caller needs them, and keep lines within 80 columns.
- Say what the signature cannot: what the value means, when a call is valid, each failure and its cause, whether a call blocks or only queues and what reports the result, who owns what, and how PS4, PS5 and cloud differ.
- Change the comment in the same commit as the behaviour it describes.
- These are the only comments. Files under `src/` may carry the same `///` comments, and `tools/docs/docs.py lint-internal` checks the ones they have; files under `tests/` are not documented this way. Inline notes, rationale paragraphs and section dividers stay out of the source.

## Attributes

- `final` only on classes with virtual functions.
- `[[nodiscard]]` on error-carrying returns. Put it on `Expected` itself rather than repeating it on every getter.
- `virtual` interfaces only where a real second implementation exists, meaning platform providers and test fakes.

## Tests

- Test through the public interface. Wire bytes are asserted through the client against a fake provider. If a test needs to reach inside a class, that class is hiding a second class.
