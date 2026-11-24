# Hyprworks architecture

Lua owns desktop composition, appearance, interaction choices and application
decisions. Native code owns protocol state, rendering, asynchronous execution
and resource lifetimes. Shell entries, dialogs, preferences and themes live in
the authored configuration directory. The runtime package supplies service
units and D-Bus activation files.

## Ownership

```text
CLI: signal pipe and reload loop
└─ Invocation: Lua state, eventfd, poll vector and configuration watch
   ├─ Native connections: descriptors, protocol state, deadlines and results
   ├─ Lua bindings: active handles, callbacks and updates awaiting delivery
   └─ Gui: Wayland display, outputs, seats and presentation memory
      ├─ Surface: role, element tree, layout, focus and interaction state
      ├─ Seat: input devices, IME, clipboard and selection offers
       ├─ Window catalog: subscription, IPC query and published window identities
       ├─ Captures: native sessions, frame requests and capture buffer budget
       ├─ Capture bindings: Lua handles, callbacks and native capture references
       ├─ Portal: authenticated peers, sessions and PipeWire connection
       │  └─ Streams: capture references, images, PipeWire nodes and buffers
       └─ Images: image memory budget, decode queue and decode thread
          └─ Decode thread: icon-theme cache
```

Lua and native dispatch run on the invocation thread. The decode thread works
independently. Public contracts live in `include/hyprworks/`; implementations
of native connections live in `src/native/`, Lua bindings in `src/lua/`, and
display-dependent operations in `src/gui/`.

## Invocation and callback delivery

The CLI resolves the entry file and creates an invocation. `load()` reads the
script, installs the `require` searcher for watched local modules, and starts
the selected event loop's resources. Each reload runs a fresh Lua state.

1. In `src/runtime/invocation.cpp`, each active connection prepares its native
   wait set. The invocation records its offset in a reusable `pollfd` vector,
   appends active descriptors, and combines deadlines. Pending delivery asks
   for an immediate turn. Timers contribute a deadline directly.
2. `ppoll` waits. The invocation reads Wayland and completes each prepared
   native wait from its own range. Native dispatch returns updates; the binding
   retains them. Borrowed-display shortcuts also consume events delivered by
   the GUI. Every prepared Wayland read ends in `read_events` or `cancel_read`.
3. Bindings deliver Lua callbacks. Observers deliver status, change, then
   result. A result belongs to the binding through argument marshalling. After
   callback admission, delivery consumes it immediately before calling Lua;
   `close()` can take it exactly once before that point. With no result callback,
   the delivery turn discards the result. Event batches move into local storage
   before delivery. GUI dispatch then processes input, lays out dirty surfaces,
   presents available buffers and initializes mapped parents' popups.

Native C++ hosts can drive a connection directly. Its wait generation and
descriptor identity tie readiness to the resource that prepared it.

Quit and reload producers set request bits and wake the invocation. A Lua
instruction hook observes requests during scripts and callbacks; quit takes
precedence. Shutdown stops connections and children, joins the decode thread,
retires popup trees child-first and releases the display. Lua finalizers run
while binding storage and the image budget are still alive. The eventfd closes last.

## Validation and Lua lifetime

Lua readers bound allocations and translate raw table fields to native values.
Public native entry points validate inputs from C++ callers. Wire decoders own
message framing, field types and size limits. The D-Bus property reader advances
the message cursor; each observer consumes its fields and skips extensions.
Audio, UPower, MPRIS, tray and menu observers commit a changed candidate as an
immutable snapshot and release the candidate storage. Lua pins the published
snapshot while building a table.

Lua allocation may run author finalizers. Owning C++ locals live outside the
`protected_call` frame that can raise a Lua error. Action targets resolve
against live state after parsing. Callback delivery checks the handle,
generation and registered function across marshalling. Lua can recycle a registry
slot while the original function is still pinned on the delivery stack.
Polkit additionally resolves request and prompt identity at callback admission.
Process output advances its delivery offset at that same point.

An invocation holds singleton bindings for its lifetime. For timers, processes,
watches, workspace connections, buses and menus, active slots pin the native
binding and a registry root pins the Lua handle. Closing releases the slot and
root. Userdata still held by Lua retains its own binding; a restart acquires an
active slot for that same identity. Delivery pins the binding across callbacks.

## Native connections

| Owner | Input, state and publication |
|---|---|
| Workspace connection | Subscribes to the event socket, confirms a marker, coalesces invalidations and publishes a cross-checked workspace/output snapshot. `src/native/hyprland/protocol.cpp` owns Socket1 connect, single-write submission and bounded reply reads for both workspace and window queries. A partially sent action retains execution uncertainty. |
| Window catalog | Foreign-toplevel events invalidate an IPC snapshot. A Wayland sync and invalidation generation establish when the candidate can publish. Restart creates a fresh identity namespace. |
| Audio, UPower, MPRIS, tray and D-Bus menus | Each connection owns its subscription, refresh sequence and snapshot. Property decoders read recognized variants directly into the candidate snapshot. Audio, MPRIS, tray and menu actions resolve a published target and produce `hyprworks::ActionResult`; the binding retains the result until delivery or close. |
| General D-Bus | Typed bodies account for depth, nodes, payload bytes and owned FDs. Calls and name operations own async slots. Exported interfaces retain introspection tables, encoded properties and incoming requests with result signatures and deadlines. An internal broker subscription publishes peer disappearance and name loss. |
| Notifications | The server owns its bus name and active records through replacement, action, close or expiry. Lua reads pin copies across finalizers. |
| Polkit | The registered authority owns one bounded authentication request. Native code owns the helper, offered identities, retries, prompt IDs and cancellation. A private GLib context contributes descriptors to the invocation. Lua supplies identity selection and prompt UI. |
| Desktop entries | An inotify scan commits matching GIO application objects and a sorted snapshot together. Scans and launches execute on the invocation thread. |
| Process | One owner polls the pidfd and child streams. In PTY mode one master descriptor carries both directions. Queued bytes enable write readiness; pipe EOF drains the queue first. Lua delivers retained output before exit. |

### D-Bus resource lifetimes

An exported interface owns its vtable and the strings borrowed by sd-bus.
Property admission validates and encodes each value into a sealed message that
owns its descriptor duplicates. Reads copy that message into the reply.
Replacement encodes the new value, adjusts the interface's retained-byte,
value and descriptor totals, and submits `PropertiesChanged` before committing.
The property's signature storage remains stable for the vtable's lifetime.

Name acquisition and release complete asynchronously. Acquisition requires an
available name. Broker ownership events and operation replies update the
owned-name snapshot; uncertain operation timeouts close the connection,
releasing its names.

Incoming requests retain the message independently of the exported slot.
Removing an interface first retires its requests. The internal `NameOwnerChanged`
match retires disconnected callers' requests and publishes unique-name departure
for longer-lived protocol sessions. Request IDs increase across connection
restarts. Reply tracking cancellation is local; each protocol owns its remote
cancellation semantics.

Received FDs are duplicated with close-on-exec before their messages can be
released. Value copies share native ownership. Lua FD userdata owns a reference,
and close or finalization releases only that reference. Transport messages hold
sd-bus's own descriptor duplicates. Shutdown releases slots, requests, exported
values and transport resources; descriptors already delivered to a caller remain
owned by that caller.

## GUI state and transactions

**Element updates.** Parsing stages supplied properties for the element kind.
After Lua allocation, the setter resolves the element and applies the patch to
its current state. Validation of the combined values and content budget precedes
mutation. Layout consumes the committed font and image state and settles
geometry, including focused-editor scrolling. Paint and IME consume that geometry.

**Tree replacement.** A candidate owns its elements, callbacks and images during
parsing. Admission bounds depth, count and content bytes. Preservation matches
explicit ID and kind. The splice resolves IDs, slider links, focus and live
editor targets before transferring resources. A structural commit advances the
revision; a failed candidate releases its own resources.

**Input.** The seat identifies the originating surface and input generation.
Dispatch resolves the target around author callbacks. Focus and structural
changes cancel obsolete presses. Clipboard and IME completions carry editor
identity through asynchronous delivery. Text edits validate UTF-8 boundaries
and the resulting size at mutation.

**Clipboard.** An input seat owns its MIME offer independently of any surface.
An outgoing transfer pins the selected bytes or file descriptor and has its
own read offset. Concurrent pastes each finish against the selection they
started with. An incoming file transfer owns a temporary sibling until EOF.

**Presentation.** Layout supplies geometry for painting, hit testing, scrolling
and IME. Presentation and capture share the allocation of a fixed-size mapped
Wayland buffer. A frame callback permits another presentation; buffer release
permits writing that storage. Busy buffers retain damage for later repair.
Each surface releases its own Wayland roles and Lua references. The GUI retires
child slots before their parent so popup ancestry stays valid during teardown.

## Images and capture

`Gui::images` owns the 128 MiB image budget, bounded decode queue and thread.
Each image charges input and pixel storage to that budget and releases the
charge on destruction. Raw-image admission validates dimensions, channels and
byte count together. The queue holds up to 32 jobs and drops queued images whose
only remaining owner is the queue.

Authored icon size supplies intrinsic layout dimensions; output scale selects
raster resolution. The decode thread owns its icon-theme cache and mutates a
submitted image until release-publishing `ready`. The invocation acquires that
flag before reading pixels or reporting completion. Completion delivery resolves
the surface revision, element callback and current image. Shutdown joins the
thread before releasing the GUI's images; the budget lives through Lua finalization.

Window, workspace and output capture share native admission, buffer accounting,
frame progression and teardown. Admission resolves the live source identity,
reserves one of the GUI's eight capture slots and publishes the session.
Completed constraint batches establish valid dimensions and shared-memory
formats; the capture loop allocates within the 128 MiB budget and consumes
those constraints. Frame transforms are validated on arrival.

Each Lua capture binding owns its handle, callback and native capture reference.
It allocates Lua state before resolving the source, then admits the session
and registers delivery. Native admission enforces capture capacity. Dispatch
pins the binding through argument allocation and callback admission. Catalog
teardown releases its captures and Lua registry roots together. Portal streams
hold native capture references directly. Completed images retain independent
ownership in the image budget; workspace captures follow workspace identity.

## Portal sessions and PipeWire

`src/gui/portal.cpp` owns ScreenCast protocol state inside a persistent
Wayland-connected invocation. Its D-Bus connection exports the standard backend
and a selector control interface. Broker ownership snapshots authenticate the
desktop portal frontend and the authored selector by unique bus name.

Session admission validates identity and source options. Start retains the
request and exports its cancellation object. The selector attaches under its
current bus identity and submits source IDs with their publication revision.
Sessions survive selector reconnection.

The portal owns the PipeWire connection. A selection admits all captures and
stream nodes before committing; failed admission releases its temporary resources.

The PipeWire loop contributes a descriptor to the invocation's poll set.
Callbacks run on the invocation thread. Streams own their latest image,
negotiated format and buffer reservation. The first image connects the node;
resizing renegotiates its format. Frame deadlines copy the latest image into
mapped BGRA buffers. Start publishes the registered node IDs.

Request cancellation, source loss, session closure and frontend disappearance
converge on session cleanup: complete the pending request, retire exported
objects, close captures, destroy PipeWire streams and release retained images.
GUI shutdown destroys the portal before releasing capture globals and the
Wayland display. The public frontend owns application permissions and the
restricted PipeWire remote.

## Processes and files

Process startup owns temporary descriptors and spawn attributes until the child
and pidfd are admitted. Failure closes those resources and reaps any spawned
child. The process then owns its child, streams, bounded input queue and retained
output through completion or cancellation. The unreaped leader reserves its
process-group identity until cleanup. Lua output delivery precedes exit delivery.

An authored headless invocation can own a child and terminal for returning UI
clients. Its exported session-bus operations define the application's reads,
commands and cancellation policy.

File replacement and exclusive copy sync a temporary sibling and publish it by
rename. A preference transaction locks a separate stable inode before reading
and holds it through the Lua edit callback, replacement and directory sync.
Callback failure releases the lock and preserves the original file.
`FileWatch` follows targets across replacement.

## Verification

Core checks exercise callback reentrancy, registry-slot reuse, result ownership,
stale identities, atomic tree and file publication, buffer lifetimes, reload and
shutdown. Private-bus fixtures exercise service ownership, request retirement,
descriptor transport and reconnecting clients. Live Wayland fixtures exercise
surface teardown and capture publication. `image-worker` isolates decode queue,
cancellation and cross-thread publication checks from font initialization.
