# Hyprworks architecture

Lua owns desktop composition, appearance and application decisions. Native code
owns protocols, rendering, asynchronous work and resource lifetimes. Authored
entries, dialogs, preferences and themes live in the configuration directory.

The three user services belong to `graphical-session.target`: the shell,
Polkit agent and persistent portal backend. The shell owns the portal selector
UI and its bus name; the backend owns capture sessions and streams. After a reload,
the shell attaches to the backend and rebuilds its UI from the current sessions.

## Ownership

```text
CLI: signals and reload loop
└─ Invocation: Lua state, wake descriptor, poll set and configuration watch
   ├─ Bindings: native connections, callbacks and pending delivery
   └─ Gui: Wayland display, protocol globals and memory budgets
      ├─ Surfaces: element trees, layout and interaction state
      ├─ Seats: input, IME and clipboard offers
      ├─ Window catalog: subscription, IPC query and published identities
      ├─ Capture bindings: Lua references and an owned native capture
      ├─ Portal: authenticated peers, sessions and PipeWire connection
      │  └─ Streams: an owned native capture, latest image and PipeWire node
      └─ Images: bounded decode queue, worker and icon-theme cache
```

Each capture has one owner. The GUI indexes active captures for protocol events,
capacity accounting and shutdown; closing a capture removes it from that index.
Delivered images have independent shared ownership.

Public C++ contracts live in `include/hyprworks/`, protocol implementations in
`src/native/`, Lua translation in `src/lua/`, and display operations in `src/gui/`.
Each native connection can also be driven by a C++ host.

## Execution and shutdown

Lua, protocol dispatch and PipeWire callbacks run on the invocation thread.
The image decoder is the sole worker thread.

Each event-loop turn prepares active resources into one reusable poll vector,
waits, reads native events, then delivers Lua callbacks and presents dirty surfaces.
Prepared descriptors and deadlines belong to that turn. Public native wait sets
carry generations to reject readiness from a replaced resource. Every prepared
Wayland read completes through `read_events` or `cancel_read`.

Quit and reload producers publish request bits before waking the invocation.
A Lua instruction hook observes requests during scripts; quit takes precedence.
Reload creates a fresh invocation.

Shutdown stops native connections and children, joins the decoder, closes portal
streams and captures, retires surfaces child-first, and releases the display.
Lua finalizers run while binding storage and image accounting are alive.
The wake descriptor closes after producers have finished.

## Admission and publication

Lua readers establish types and bound allocation while translating arguments.
Native public entry points enforce their operation contracts; wire readers establish
protocol types, framing and resource bounds. Consumers use the admitted data.
Property readers consume recognized fields; the shared D-Bus reader skips extensions.
sd-bus validates addresses and exported declarations at transport admission.

Observers assemble candidates and publish changed immutable snapshots. Lua pins
the publication while building tables. Window queries publish after a Wayland
sync confirms their invalidation generation; workspace snapshots cross-check
workspace and output identities.

Surface updates stage properties and validate the combined content before mutation.
Tree commits resolve IDs, slider links and preserved editor state before transferring
resources. Structural commits advance the revision. Layout supplies geometry to
painting, hit testing, scrolling and IME.

State that can change asynchronously is resolved at use: action targets, source
identities, editor revisions and callback registrations.

## Lua delivery

Lua allocation can run author finalizers. Owning C++ locals stay outside the
`protected_call` frame whose Lua errors use longjmp. Delivery pins its binding
and callback across argument construction, then checks identity before calling Lua.

Observers deliver status, change, then result. A result stays with the binding
until callback admission or `close()`, which consumes it once. Event batches move
into local storage before delivery. Polkit checks request and prompt identity
at admission; process output advances its delivery offset there.

Singleton bindings live through Lua shutdown. Multi-instance bindings are pinned
by active slots and Lua userdata; closing releases the slot and registry root.
Restart acquires an active slot for the same userdata identity.

## Protocol resources

D-Bus connections own calls, subscriptions, names, exports and incoming requests.
Exports retain vtables and their borrowed strings. Properties are encoded into
sealed messages that own descriptor duplicates; replacement commits after
admission and signal submission. Requests retain their messages until reply,
expiry, interface removal or caller departure. Received FDs are duplicated before
message release, and delivered values retain their own ownership.

Portal sessions authenticate the desktop frontend and shell through current bus
ownership. Selection admits all captures and PipeWire streams before committing.
The portal polls its PipeWire loop alongside D-Bus and Wayland. Streams retain
the latest image, negotiate dimensions and publish frames on bounded deadlines.
Cancellation, source loss, caller departure and shutdown converge on session cleanup.

Polkit owns the authority registration, helper, offered identities, retries and
prompt IDs. Its private GLib context contributes descriptors to the same poll set.
Lua supplies the identity and prompt UI.

## Buffers, images and transfers

Presentation and capture own mapped Wayland buffers within separate budgets.
A frame callback permits presentation; buffer release permits reuse. Busy buffers
accumulate damage for later repair. Capture constraint batches establish dimensions
and formats before allocation; completed frames publish independent images.

Images charge input and pixel storage to a shared budget. The bounded decode queue
drops abandoned work. The worker mutates each image until release-publishing
`ready`; the invocation acquires that flag before consuming pixels or completion.
Image accounting outlives Lua finalization.

Each clipboard transfer pins its selected bytes or file descriptor through
completion. Incoming file transfers own a temporary sibling until EOF.
Concurrent transfers retain the selection they started with.

## Processes and files

A process owns its child, pidfd, streams, bounded input queue and retained output.
Startup failure closes temporary descriptors and reaps a spawned child.
The unreaped leader reserves its process-group identity through cancellation.
Lua receives output before exit delivery.

File replacement syncs a temporary sibling and publishes it by rename.
Preference transactions hold a stable lock inode through reading, the Lua edit
callback, replacement and directory sync. Callback failure preserves the original file
and releases the lock. File watches follow atomic replacement.

## Verification

Core checks protect reentrancy, stale identities, transaction integrity, bounded
queues and resource lifetimes. Private-bus fixtures exercise ownership, request
retirement and descriptor transport. Live fixtures exercise Wayland teardown and
portal reconnection. `image-worker` isolates cross-thread publication and shutdown.
