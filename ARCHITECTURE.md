# Runtime ownership

```text
CLI: signal pipe and reload loop
└─ Invocation: Lua state, eventfd, poll vector and configuration watch
   ├─ Native connections: descriptors, protocol state, deadlines and results
   ├─ Lua bindings: active handles, callbacks and updates awaiting delivery
   └─ Gui: Wayland display, outputs, seats and presentation memory
      ├─ Surface: role, element tree, layout, focus and interaction state
      ├─ Seat: input devices, IME, clipboard and selection offers
      ├─ Window catalog: subscription, IPC query and published window identities
      ├─ Captures: sessions, frame requests and shared buffer budget
      └─ Image worker: decode queue, icon cache and image memory budget
```

Lua and native dispatch run on the invocation thread. The image worker decodes
independently. Public contracts live in `include/hyprworks/`; implementations
of native connections live in `src/native/`, Lua bindings in `src/lua/`, and
display-dependent operations in `src/gui/`.

## One turn

1. In `src/runtime/invocation.cpp`, each active connection prepares its native
   wait set. The invocation records its offset in a reusable `pollfd` vector,
   appends active descriptors, and combines deadlines. Pending delivery asks
   for an immediate turn. Timers contribute a deadline directly.
2. `ppoll` waits. The invocation reads Wayland and completes each prepared
   native wait from its own range. Native dispatch returns updates; the binding
   retains them. Borrowed-display shortcuts also consume events delivered by
   the GUI. Every prepared Wayland read ends in `read_events` or `cancel_read`.
3. Bindings deliver Lua callbacks. Observers deliver status, change, then
   result. A result belongs to the binding through the earlier callbacks, so
   `close()` can take it exactly once. Event batches move into local storage
   before delivery. GUI dispatch then processes input, lays out dirty surfaces,
   presents available buffers and initializes mapped parents' popups.

Native C++ hosts can drive a connection directly. Its wait generation and
descriptor identity tie readiness to the resource that prepared it.

## Lua lifetime

Lua argument readers use raw table fields and bound allocations while parsing.
Public native entry points also accept direct C++ input. Wire decoders validate
their own messages. Published observer snapshots are immutable; a binding pins
the snapshot while building Lua values.

Lua allocation may run author finalizers. Owning C++ locals live outside the
`protected_call` frame that can raise a Lua error. Action targets resolve
against live state after parsing. Callback delivery checks the handle,
generation and registered function across marshalling. Lua can recycle a registry
slot while the original function is still pinned on the delivery stack.

An invocation holds singleton bindings for its lifetime. For timers, processes,
watches, workspace connections, buses and menus, active slots pin the native
binding and a registry root pins the Lua handle. Closing releases the slot and
root. Userdata still held by Lua retains its own binding; a restart acquires an
active slot for that same identity. Delivery pins the binding across callbacks.

Quit and reload producers publish request bits and wake the loop. An instruction
hook observes requests during Lua execution. Quit has precedence. On shutdown
the invocation stops connections, joins the worker, releases Wayland resources,
closes Lua and releases binding storage. The eventfd lives through invocation
destruction. On reload the CLI creates a fresh invocation.

## Native connections

| Owner | Input, state and publication |
|---|---|
| Workspace connection | Subscribes to the event socket, confirms a marker, coalesces invalidations and publishes a cross-checked workspace/output snapshot. Window and workspace queries share Socket1 connect, single-write submission and bounded reply reads. A partially sent action retains execution uncertainty. |
| Window catalog | Foreign-toplevel events invalidate an IPC snapshot. A Wayland sync and invalidation generation establish when the candidate can publish. Restart creates a fresh identity namespace. |
| Audio, UPower, MPRIS, tray and D-Bus menus | Each connection owns its subscription, refresh sequence and snapshot. Property decoders read recognized variants directly into the candidate snapshot. Audio, MPRIS, tray and menu actions resolve a published target and produce `hyprworks::ActionResult`; the binding retains the result until delivery or close. |
| General D-Bus | Typed bodies account for depth, nodes and payload bytes. Calls own their async slots; exports retain the incoming message, declared result signature and deadline. A successful response consumes the request; a rejected candidate leaves it available until expiry. |
| Notifications | The server owns its bus name and active records through replacement, action, close or expiry. Lua reads pin copies across finalizers. |
| Polkit | The registered authority owns one bounded authentication request. Native code owns the helper, offered identities, retries, prompt IDs and cancellation. A private GLib context contributes descriptors to the invocation. Lua supplies identity selection and prompt UI. |
| Desktop entries | An inotify scan commits matching GIO application objects and a sorted snapshot together. Scans and launches execute on the invocation thread. |
| Process | One owner polls the pidfd and child streams. In PTY mode one master descriptor carries both directions. Queued bytes enable write readiness; pipe EOF drains the queue first. Lua delivers retained output before exit. |
| Independent jobs | An authored headless invocation owns the child and terminal. It exports application operations on the session bus. Clients connect to the owner's name. |
| Files | Replacement and exclusive copy both sync a temporary sibling and publish it by rename. A preference transaction locks a separate stable inode before reading and holds it through directory sync. `FileWatch` follows targets across replacement. |

## GUI transactions

**Element updates.** Parsing stages supplied properties for the element kind.
After Lua allocation, the setter resolves the element and applies the patch to
its current state. Validation of the combined values and content budget precedes
mutation. Supplied fields determine whether painting, arrangement or text shaping
needs work.

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
own read offset, so selection replacement and concurrent pastes cannot change
its source. An incoming file transfer owns a temporary sibling until EOF.

**Presentation.** Layout supplies geometry for painting, hit testing, scrolling
and IME. Presentation and capture share the allocation of a fixed-size mapped
Wayland buffer. A frame callback permits another presentation; buffer release
permits writing that storage. Busy buffers retain damage for later repair.

**Images and capture.** Authored icon size supplies intrinsic layout dimensions;
output scale selects raster resolution. Decode jobs release-publish completed
pixels to the invocation thread. Window and workspace capture resolve different
targets, then share session admission, buffer accounting and frame delivery.
The GUI owns eight active slots and 128 MiB of capture buffers. Delivered frames
own independent image storage. Window catalog closure retires its window
captures; workspace capture follows the workspace protocol identity.
