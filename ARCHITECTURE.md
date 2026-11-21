# Architecture

Hyprworks connects native desktop state to Lua-authored surfaces. Ownership
follows resource lifetime; execution follows readiness and explicit deadlines.

## Ownership and source boundaries

```text
CLI or embedder
└── Invocation
    ├── Lua state, wake descriptor and reusable wait vector
    ├── native connections and their Lua bindings
    ├── timers, child processes, file and clock watches
    └── Gui
        ├── Wayland display, registry and protocol globals
        ├── outputs and seats
        ├── surfaces → element arrays and interaction state
        ├── presentation buffers and clipboard transfers
        ├── window catalog → capture subscriptions
        └── image worker → decode queue and icon-theme cache
```

The CLI owns process signals and the reload loop. An invocation owns one
configuration execution. Its bindings remain at stable addresses through
native callbacks, Lua finalizers and Lua shutdown.

`src/native/` owns protocol state and OS resources behind the public headers.
Connections expose readiness, deadlines, snapshots and operation results.
`src/lua/` translates values, retains Lua callbacks and delivers native changes.
`src/runtime/invocation.cpp` combines the connections into a single host-driven
loop. Its state header holds invocation-owned binding storage so construction,
native dispatch and shutdown share the same lifetime.

GUI resources live under `Gui` because they share its Wayland display and
presentation lifetime. Window observation and GUI shortcuts borrow that
display. Headless shortcuts own a display. Image decoding has a separate
execution boundary because library decoding can take time independently of
input and presentation.

The GUI's source split follows operations on that shared owner:

| File under `src/gui/` | Owns the implementation of |
|---|---|
| `gui.cpp` | Display setup/teardown, globals, outputs, poll preparation and dispatch order |
| `surface.cpp` | Surface roles and lifetime, tree parsing/commit, Lua handles and change delivery |
| `input.cpp` | Seat protocols, editing and focus input, clipboard I/O and queued input delivery |
| `render.cpp` | Measurement, arrangement, caret geometry, painting and presentation-buffer lifetime |
| `windows.cpp` | Catalog synchronization, target identities and native captures |
| `images.cpp`, `icons.cpp` | Worker execution, image publication and icon lookup |

`gui.hpp` is the internal ownership contract shared by these translation units.
Helpers used within one path stay local to its source file. Rendering consumes
the surface's element array directly. Input uses that same arrangement and
mutates the same editor state; tree commit remaps deferred input identities.
The split adds compilation boundaries while retaining direct calls and shared
owned storage.

Lua configuration owns component composition, themes, shell-level selection,
dismissal policy and retries. Native surfaces own logical focus, active
presses, editing and scrolling. Keeping these states with their resources lets
layout, input and rendering observe the same values.

## One event-loop turn

```text
prepare descriptors and earliest deadline
    → ppoll
    → consume native readiness
    → publish snapshots and queue events
    → deliver Lua callbacks
    → resolve layout and present dirty surfaces
```

`Invocation::prepare_wait` combines each connection's wait set with timers,
processes, watches and GUI readiness. `dispatch` consumes those descriptors
before Lua callbacks can close resources or reuse descriptor numbers.
Native callbacks retain data for later delivery. GUI input keeps packet order,
with interaction callbacks between packets.

Wayland preparation pairs each successful `prepare_read` with `read_events`
or `cancel_read`. Flush backpressure adds write readiness. Frame callbacks
permit presentation; buffer-release events permit buffer reuse.

Most work runs on the invocation thread. Cross-thread quit/reload requests
publish flags and wake its eventfd. Process signal handlers use a borrowed
nonblocking pipe and pending bits that survive a full pipe. The instruction
hook also observes requests while Lua is busy. Signal storage spans reloads;
termination takes precedence over reload.

## Connections and publications

A native connection owns its transport, subscriptions, in-flight requests and
deadlines. Poll readiness belongs to the generation that prepared it. Drive a
connection on one thread and dispatch a prepared wait set before mutating it.
Dispatch discards stale wait sets, preserving the current generation and its
pending work. Borrowed-display callbacks queue work for delivery after native
dispatch returns. Both borrowed and owned shortcut displays feed the connection's
bounded event queue; the invocation drains it after Wayland dispatch each turn.

Observers subscribe before obtaining state, coalesce invalidations and publish
complete snapshots. A `shared_ptr<const Snapshot>` retains a publication across
refresh, restart and connection destruction. Retaining it during Lua conversion
also protects against finalizers that close its connection.

Bindings own one replaceable callback per event kind. Status/change registration
schedules an initial delivery. A callback receives the connection so it can read
the current publication. Action results retain their request identity and
whether the remote action may have executed. Closing a binding cancels delivery
and resolves locally owned pending work.

D-Bus connections own their calls, subscriptions and exported requests.
The native side validates signatures and message bounds; Lua bindings translate
scalars, containers and variants. Exported requests remain owned until reply,
expiration or connection teardown. Configuration decides application policy
using the sender and request data.

## The Lua boundary

`binding.hpp` centralizes error translation, protected calls and registry
references. Lua errors can unwind with `longjmp`, and Lua allocation can run
user finalizers. Both affect the lifetime of native values during conversion.

Keep C++ owning locals outside the function passed to `protected_call`, and
borrow them inside it. Its return restores normal C++ unwinding before
`guarded` propagates an error. Handle construction reserves its registry slot
while allocation can reenter a constructor.

After marshalling, check the owner generation, callback reference and target
identity before delivery or commit. Resolve element handles again after a Lua
call that can replace their tree. Registry roots keep active handles reachable;
native ownership stays with the invocation or GUI.

`dispatch_callback` in `invocation.hpp` owns the common connection-callback
boundary: protected argument conversion, revalidation and delivery. Bindings
retain event ordering and payload conversion. Command replies and GUI input
retain their return-value and target-identity handling.

Operational failures return `nil, error`. Invalid Lua arguments raise errors,
and an uncaught callback error stops the invocation. Protocol failures are
reported by the connection that owns recovery state.

## Surface transactions and input

A surface stores elements in a contiguous array with parent and subtree-end
indices. Parsing produces temporary owned content. Full-tree and subtree
replacement share one splice operation:

1. Parse properties and callback references.
2. Match retained state by explicit ID and kind when preservation is requested.
3. Validate identities, content bounds and the resulting tree.
4. Allocate the destination array.
5. Transfer resources, remap editor targets and commit the new revision.

Explicit values take precedence over preserved values. A failed preparation
keeps the live tree usable. A successful structural change invalidates every
element handle on the surface. Pending presses are canceled at commit; matching
editor transactions retain their remapped target.

Seats own keyboard maps, compose state, clipboard offers and IME protocol
objects. A surface owns logical focus and one active press, including its seat
and pointer/touch source. Delayed IME and paste work carries tree and editing
revisions. Pointer and keyboard modality determine focus-ring visibility while
logical focus continues to identify the editor.

Popup role creation follows the parent's first buffer commit. Tree roots own
Hyprland focus grabs and configured descendants join the root's grab. A tree
uses either a Hyprland focus grab or XDG popup grabs. Seat-owned keyboard ancestry
survives popup retirement until native keyboard leave.

Surfaces opt into dismissal on Escape, grab clearance or focus loss. Focus-loss
dismissal arms after the tree receives native keyboard focus. Cancellation
invalidates queued interaction and editor work before close callbacks run.

Native scroll input remembers the starting offset. After input and configure
callbacks, layout clamps the final offset and dispatch delivers a coalesced
change. Programmatic mutation owns its explicit state update; interaction
callbacks describe native input changes.

## Layout, images and presentation

Layout measures intrinsic sizes, measures text under available width, then
arranges boxes. Hit testing, painting, caret geometry and IME cursor rectangles
use that arrangement. Pango owns text shaping and measurement. Cairo paints
complete frames into owned shared-memory buffers.

Submitted presentation buffers stay immutable until compositor release.
Resizing and closing can retire busy buffers; their storage remains accounted
until release. Frame readiness and reusable storage jointly determine when a
dirty surface can render. Per-surface buffer counts and a GUI-wide byte budget
bound storage; compositor release resumes surfaces waiting for capacity.

Outputs publish complete pending state at `wl_output.done`. XDG-output v3 adds
logical geometry to the same publication; surfaces retain entered-output IDs
and use native fractional scale or the highest entered integer scale.

Each image request owns its source and accounted storage. The worker borrows
the image-work owner, resolves icons and decodes pixels. Release/acquire
publication makes completed pixels or errors visible on the invocation thread.
Superseded work is discarded, and completion wakes the loop to admit waiting
jobs. Shutdown joins the worker before releasing its owner.

Clipboard transfers own their local endpoints and deadline. Receiving uses a
nonblocking read endpoint and gives the source application a blocking write
endpoint. Child processes similarly own their pipes, process group and reaping;
the unreaped leader reserves the group ID through cancellation.

## Window observation and capture

The GUI-owned window catalog combines foreign-toplevel notifications with
asynchronous Hyprland IPC. Ext notifications include metadata changes; wlr
notifications include focus changes. A Wayland synchronization after each IPC
reply establishes whether the candidate still matches its invalidation
generation. Current candidates become immutable publications.

Hyprland socket1 treats its first read as the request boundary. A short send
ends that query. Both workspace and window IPC follow this contract.

Catalog identities combine the subscription generation with the compositor's
stable window identity. Activation addresses and capture identifiers serve
their respective protocols. Restart creates a fresh identity generation.

A capture owns one source buffer and one frame request. Damage and rate-limit
deadlines pace subsequent requests. Completed frames are scaled into independent
immutable images before Lua delivery. Their pixels remain valid after the capture
closes. Capture cancellation releases protocol resources and cancels pending
callbacks; image references determine delivered-frame lifetime.

## Shared development contracts

- **Ownership:** keep mutable state, cleanup and cancellation with the resource
  owner. Borrowed objects outlive every user, including shutdown callbacks.
- **Identity:** carry generations or revisions across deferred work. Resolve
  targets at delivery and commit boundaries.
- **Publication:** expose complete state. Snapshot readers and submitted-buffer
  readers retain immutable data for their access lifetime.
- **Units:** GUI geometry and scroll offsets use logical pixels; buffers use
  physical pixels; editor offsets use UTF-8 bytes. Convert at those boundaries.
- **Scheduling:** events invalidate state; deadlines serve coalescing, pacing,
  retries and time-dependent work. Clock/timezone notifications recompute
  wall-clock displays while ordinary timers use monotonic time.
- **Bounds:** enforce input, queue and storage budgets where work is admitted.
  Keep numeric limits beside their implementation. Exhaustion follows that
  owner's error or backpressure path.
- **Failure:** preserve usable state until commit and release resources on every
  completion, failure and cancellation path.

For an integration change, follow its public contract through native state,
wait preparation, dispatch, Lua conversion and teardown. For a GUI change,
follow parsing through transaction, layout, input and painting. Share behavior
at the boundary that owns its invariant.

## Teardown and verification

Close marks owners closed, stops native work and cancels pending delivery.
The GUI joins its image worker, destroys popup roles before parents, releases
buffers and protocol objects, then disconnects the display. Lua closes while
binding and GUI storage still exist for finalizers. Storage is reclaimed last.
A reload closes the invocation and repeats construction from disk. The standard
Lua file searcher supplies local module paths to the invocation's file watch.
Edits are debounced into a restart request; SIGHUP and `hw.reload()` request one
directly. Configuration errors follow normal teardown and process failure.

Core checks in `tests/invocation_core.cpp` and `tests/gui_core.cpp` exercise
cross-thread requests, teardown, reentrant mutation, transaction integrity,
clipboard transport and worker publication. Live Lua fixtures exercise the
compositor and D-Bus boundaries. The core checks link the runtime implementation
used by the executable. Keep checks focused on these shared contracts.
