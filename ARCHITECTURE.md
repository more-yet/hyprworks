# Architecture and execution paths

Hyprworks runs Lua, native connections, Wayland and mutable UI state on one
invocation thread. One bounded worker resolves icons and decodes images.
Ownership follows resource lifetime.

```text
CLI: signals and reload loop
└── Invocation: Lua, wake fd, reusable poll vector
    ├── native connections and stable Lua bindings
    ├── timers, processes, file/clock/configuration watches
    └── Gui: display, globals, outputs, seats
        ├── surfaces: element arrays, focus, presses, editor state
        ├── presentation buffers and clipboard transfers
        ├── window catalog and captures
        └── image worker, queue and icon cache
```

`include/hyprworks/` defines the native and embedding contracts.
`src/native/` owns transports, protocol validation and publications.
`src/lua/` translates arguments and results and retains callbacks.
`src/runtime/` owns their construction, scheduling and shutdown.
GUI files operate directly on their shared owner in `src/gui/gui.hpp`.

`CMakeLists.txt` selects native integrations, then builds the optional Lua
library and CLI. Wayland scanner generates protocol bindings from the installed
and repository XML definitions. Installed CMake targets expose the native and
Lua libraries; the user service starts the CLI and sends SIGHUP for reload.

## Entry, scheduling and shutdown

| Path | Execution | Branches and completion |
|---|---|---|
| CLI → `Invocation::load` | Resolve configuration → watch entry → bounded read → create GUI if selected → initialize Lua → execute script | Local `require` adds configuration watches. Loading or callback errors become diagnostics after teardown. |
| `prepare_wait` → `run` → `dispatch` | Combine active descriptors and earliest deadline → `ppoll` → consume native readiness → retain updates → deliver Lua callbacks → GUI presentation | Interrupted polling clears readiness. Deadlines drive work without an fd. Pending callbacks request an immediate turn. |
| Signal/thread/Lua request | Publish quit/reload bits → wake pipe or eventfd → loop or instruction hook observes bits | Pending signal bits survive a full pipe. Quit takes precedence over reload. |
| File change → reload | Inotify → 50 ms debounce → reload request → close invocation → reload from disk | Watch the containing directory so file replacement remains visible. Standard local module loading extends the watch set. |
| `Invocation::close` | Mark closing → stop native work → stop GUI → close Lua → release stable binding/GUI storage | Repeated close returns immediately. The wake fd remains alive until destruction for cross-thread request producers. |

Native readiness is consumed before Lua delivery can close a connection or
reuse an fd. Prepared native wait sets carry generations; stale sets are ignored.
Callbacks that add bindings extend the next delivery batch. Wayland read
preparation is paired with either `read_events` or `cancel_read`; flush
backpressure adds write readiness.

## Native integration paths

Each row follows its implementation in `src/native/` through the corresponding
Lua binding and invocation dispatch.

| Path | Input → work → output | Failure, cancellation and repeated work |
|---|---|---|
| `hyprland/workspaces.cpp`, `workspace_protocol.cpp` | Session path → event socket → subscription marker → batched workspace/output query → bounded JSON → sorted, cross-checked snapshot | Event bursts coalesce. Subscription markers retry until acknowledged or timed out. Switch requests resolve a published key, encode once, send, then report outcome and refresh. Short socket1 writes end the request; sent actions retain execution uncertainty. |
| `hyprland/shortcuts.cpp` | Validate registrations → borrow GUI display or open owned display → registry/sync → register → sync → bounded press/release queue | GUI dispatch feeds borrowed-display events; owned-display readiness feeds the same queue. Protocol loss/timeout releases registrations. Stop destroys proxies before their display. |
| `audio.cpp` | libpulse readiness → subscribe → server/output/input queries → sorted device snapshot → status/change/result callbacks | Invalidations share a refresh deadline. Action target and channel validation precede submission. Query/command failure cancels operations and preserves result identity. Stable libpulse slots survive callbacks until collection. |
| `upower.cpp` | Install owner/property/device subscriptions → enumerate → root/device properties → immutable snapshot | Refreshes coalesce. Disappearing devices schedule another refresh; transport or synchronization failure releases bus state. |
| `mpris.cpp` | Subscribe → list names → resolve owners → root/player properties → snapshot; action → owned async call → result | Owner changes invalidate targets and outstanding actions. Per-player failures stay with the player record. Transport failures complete pending actions and disconnect. |
| `tray.cpp` | Subscribe/acquire optional watcher → resolve watcher → register host → list items → resolve owners/properties → snapshot | Registration ownership is checked against the sender. Removed owners retire items and requests. Item failures remain in records; watcher absence publishes unavailable state. |
| `dbusmenu.cpp` | Subscribe to owner/layout/property changes → properties → recursive bounded layout → snapshot; show/event → async result | Refresh signals coalesce. Owner loss ends the connection. Activation requests have a bounded queue separate from command results. |
| `dbus.cpp` | Lua values → typed body → native signature/value bounds → async call, subscription or export → owned events → Lua values | Calls cancel by ID. Exported methods retain their request until reply, error, expiration or close. Unknown/stale request IDs return operation errors. |
| `notifications.cpp` | Acquire name → decode bounded notification → replace/append active record → event; deadline/action/close → removal and signal | String/binary/record budgets bound admission. Snapshot copies survive Lua finalizers. Name/transport failure releases the server; stop clears retained notifications/events. |
| `commands.cpp` | Acquire name/export method → validate command/arguments → retain request and deadline → Lua callback → reply | Requests expire or receive a reply once. Callback errors stop the invocation. Close releases pending messages and bus resources. |
| `desktop.cpp` | XDG roots → watched bounded traversal → precedence/deduplication → GIO metadata → sorted snapshot | File changes debounce rescans. Candidate watches/data commit together; failure releases transport state. Launch resolves the published entry and runs a private GLib completion loop. Scanning and launch execute synchronously. |

Observers publish complete `shared_ptr<const Snapshot>` values. Lua conversion
holds that publication through allocation and finalization. Actions validate
their target against current published state. Native validation also serves
direct C++ callers.

Status, change and result delivery are explicit, in that order. A pending action
result remains with its binding through earlier callbacks, allowing `close()`
to return it. It transfers to a local value immediately before result delivery.
Event batches transfer to local storage before callbacks can mutate the owner.

## Host utility paths

| Path through `src/lua/host.cpp` and `src/native/host.cpp` | Execution and branches |
|---|---|
| Timer | Validate interval → monotonic deadline → coalesced elapsed ticks → callback. Stop clears the deadline; restart changes generation and clears pending ticks. |
| Process | Validate argv/options → spawn process group with owned pipes/pidfd → bounded output reads → observe exit → drain pipes → reap → result. Timeout, output exhaustion and cancellation kill the group and reap its leader; the unreaped leader reserves the group ID. |
| Detached launch | Resolve executable → fork/session/detach → exec → result pipe. Parent reaps the helper; exec failure travels back through the pipe. |
| File read | Validate path/limit → open regular file → reserve bounded size hint → read to EOF within limit → close. Growth and procfs/sysfs content are checked by the read loop. |
| File write | Validate → create temporary sibling → write/fsync → rename → directory fsync. Failure removes the uncommitted temporary; a post-rename failure reports uncertain durability. |
| File/clock watch | Inotify or realtime timerfd → coalesced pending state → callback. Clock changes rearm the timer; timezone invalidation reinstalls its watch. Close releases descriptors and callback roots. |

## GUI paths

| Path | Input → work → output | Branches and lifetime |
|---|---|---|
| `gui.cpp`: display/outputs | Connect → registry synchronization → bind globals → publish completed output state → callbacks | Output `done` commits pending metadata. Scale changes invalidate affected surfaces. Protocol callbacks retain errors for the invocation boundary. |
| `surface.cpp`: create/close | Parse role/options/callbacks → resolve output/parent → allocate protocol role → retain surface → configure | Popup roles wait for the parent's first buffer commit. Dismissal cancels queued interaction/editor work before callbacks. Child roles retire before parents. |
| Tree replacement | Parse candidate → preserve explicit-ID/kind state → validate combined IDs/budgets → allocate destination → transfer resources/remap targets → commit revision | Failed preparation destroys the candidate and retains the live tree. Whole-tree commit moves its vector; subtree commit splices. Explicit values override preserved state. Structural changes invalidate element handles. |
| Element update | Copy candidate → apply supplied properties → resolve live handle → validate identity/budget → swap → invalidate affected geometry/paint | Retained strings keep their validated state; changed strings are checked at parsing. Unchanged shaping survives paint-only updates. |
| `input.cpp`: input | Queue native packet → validate seat/target generation → Lua input callback → resolve target again → mutate interaction/editor state → interaction callback | Consumed events cancel matching presses. Keyboard activation retains physical key, seat, serial and target until release. Focus changes/tree commits retire presses. |
| IME/paste | Retain tree/editor identity → receive text → resolve current editor → validate insertion and UTF-8 boundaries → replace → callback | Stale transactions are ignored. Preedit validates independently. Clipboard callbacks validate received text at delivery; editor insertion owns validation on the editing branch. |
| Clipboard transport | Retain offer → pipe → bounded nonblocking local I/O → completion/timeout → delivery | The source application receives a blocking writer. Transfers own endpoints and deadlines; SIGPIPE handling preserves the caller's signal state. |
| Scrolling/focus | Hit-test → walk matching-axis ancestors → consume available range → layout clamp → coalesced change | Focus reveal uses the same geometry. Callback-driven tree replacement requires resolving the next pending target again. |
| `render.cpp`: layout | Dirty ancestry → intrinsic measurement → width-dependent measurement → arrangement | Retained Pango configuration/natural sizes avoid shaping unchanged text. Hit testing, caret, IME rectangles and painting share arrangement. |
| Presentation | Damage → configured/frame-ready surface → reusable buffer or bounded allocation → repair region → attach/commit | Busy buffers accumulate repair metadata and remain immutable. Release permits reuse; capacity exhaustion waits. GUI retirement collects orphaned buffers once per turn; render retires resized buffers for its own surface. |
| `images.cpp`, `icons.cpp` | Account source → bounded queue → cached icon lookup → decode/premultiply → release/acquire completion → invalidate layout and notify | Superseded jobs are discarded. Queue exhaustion retries after completion wakes the loop. Errors publish with the image. Shutdown joins the worker before releasing its owner. |
| `windows.cpp`: catalog | Foreign-toplevel events → coalesced Hyprland query → Wayland synchronization → generation comparison → immutable records | A changed invalidation generation schedules another query. Session/generation identity prevents old targets from addressing restarted subscriptions. |
| Window capture | Resolve published identity → create session → collect constraints → allocate buffer → request frame → scale into independent image → callback | Validate constraints on buffer replacement. Changed constraints discard an incompatible completion; bounded retries handle constraint failures. Frame/rate deadlines pace requests. Close releases protocol/storage ownership while delivered images retain their pixels. |

Layout calls at input, callback and presentation boundaries share a dirty gate.
Intermediate callbacks can change geometry, so those calls observe distinct
states. Presentation has separate frame and buffer-release gates: one permits a
new submission, the other permits writing storage.

Length-bounded [GLib UTF-8 validation](https://docs.gtk.org/glib/func.utf8_validate.html)
also rejects embedded NUL bytes, combining both checks in one scan.

## Lua lifetime boundary

`binding.hpp` owns protected calls and registry references;
`dispatch_callback` owns argument conversion and callback revalidation.

- Keep C++ owning locals outside the function passed to `protected_call`.
  Lua errors return through `lua_pcall` before native locals unwind.
- Allocation can run Lua finalizers. Resolve action handles after parsing;
  revalidate owner generation, callback reference and target after marshalling.
- A tree candidate and an immutable snapshot hold resources through allocation.
  Registry roots keep active handles reachable; native storage owns their lifetime.
- Closing stops native resources before `lua_close`; binding and GUI storage
  remain valid while finalizers run.

Invalid arguments raise Lua errors. Operational failures return `nil, error`.
Uncaught callback errors stop the invocation. Protocol failure is handled by
the connection that owns its recovery state.

## Simplification decisions and tradeoffs

| Decision | Benefit and practical effect | Cost or constraint |
|---|---|---|
| Direct ordered callback calls | Removes synthetic event entries, kind switches and repeated selection branches | A few similar calls remain beside each binding's concrete payload conversion |
| Resolve action handles after parsing | Uses one current handle check after argument conversion and its possible finalizers | Malformed arguments can be reported before a closed-handle error |
| Validation at mutation/allocation boundaries | Skips retained-string scans, duplicate editor validation and per-frame constraint checks | Each owner must preserve the established invariant until the next mutation |
| One owner for orphan-buffer collection | Removes repeated global surface lookups during rendering | Render follows GUI retirement in the event-loop order |
| One-shot invocation cleanup | Repeated `close()` and destruction share completed teardown | Native resources and Lua storage still retire in separate lifetime phases |
| Existing invocation + image worker topology | Direct calls and bounded work keep ordering, cancellation and ownership visible | Desktop scans/launch and serial service queries retain their current latency |

Core checks cover callback/finalizer reentrancy, cancellation, stale readiness,
tree transactions, shared rendering state, clipboard transport and worker
publication. Live fixtures cover Wayland and D-Bus boundaries.
