# Hyprworks

Build desktop shells for Hyprland in Lua. Create bars, docks, launchers and widgets
with support for workspaces, audio, media, notifications, tray menus, power and
desktop entries.

Hyprworks supplies native primitives, input handling and system integrations.
Themes, component composition, desktop layouts and interaction policy belong in Lua configuration.

Version 0.2 introduces a new GUI API and Lua embedding ABI. Update shell
configurations to the API below and rebuild C++ Lua embedders against v0.2.

## D-Bus

`hw.dbus {bus = "system"}` (or `"session"`) opens an asynchronous connection.
Use it for desktop integrations and authentication agents whose behavior belongs
in configuration. Calls and exported methods run on the existing event loop.

```lua
local bus = assert(hw.dbus {bus = "system"})
bus:on_event(function(connection, event)
    if event.kind == "reply" then
        if event.error then print(event.error_name, event.error)
        else print(event.id, event.arguments[1].value) end
    end
end)
local id = assert(bus:call {
    destination = "org.freedesktop.NetworkManager",
    path = "/org/freedesktop/NetworkManager",
    interface = "org.freedesktop.DBus.Properties",
    member = "Get", signature = "ss",
    arguments = {"org.freedesktop.NetworkManager", "WirelessEnabled"},
})
```

- `call(options)` returns a string request ID, or `nil, error`. Options include
  `timeout` in milliseconds (default 5000, maximum 300000), and `interactive`
  for interactive authorization (default false).
- `subscribe(match)` returns a subscription ID. Signal events carry that ID,
  `sender`, `path`, `interface`, `member` and `arguments`.
  `unsubscribe(id)` removes the subscription.
- `export {path, interface, methods, timeout}` exports methods specified as
  `{name, input, output}`; omitted signatures mean no arguments.
  Incoming `method` events include a unique request ID and the caller's unique
  `sender`. The application must authenticate callers before returning secrets
  or accepting privileged requests.
- `reply(id, signature, arguments)` and `error(id, name, message)` complete an
  incoming request once. Replies must match the exported output signature.
  Unanswered requests expire after the export's timeout (default 120000 ms),
  return `org.freedesktop.DBus.Error.NoReply`, and produce an `expired` event.
- `cancel(id)` cancels an outstanding outgoing reply subscription; the remote
  operation may still complete. Request IDs are valid only while outstanding.
- `status()` returns `{phase, error?}`; phases are `connecting`, `ready`,
  `failed` and `closed`. `on_status(callback)` receives the connection initially
  and on transitions. `close()` releases calls, subscriptions and exports.
  A failed connection invalidates its outstanding work; reconnect explicitly.

Arguments are positional tables. D-Bus scalars use the corresponding Lua types;
`ay` uses a binary Lua string. Arrays and structs use positional tables,
dictionaries use keyed tables, and variants use
`{signature = "s", value = "text"}` in both directions. Unsigned 64-bit integers
above Lua's integer maximum use decimal strings. Unix file descriptors are
unsupported. Empty containers must still have their explicit D-Bus signature.

Each invocation retains at most eight connections. Each connection permits
64 outgoing calls, 64 subscriptions, 16 exported interfaces with up to 32 methods
each, and 32 incoming requests. Messages are bounded to 32 nesting levels,
65536 values and 8 MiB of payload; the event queue is bounded to 256 entries and
16 MiB. Incoming overload receives an error; signal/reply queue overflow fails
the connection. Closing a connection from a callback suppresses the remainder
of its event batch.

Build with `HYPRWORKS_BUILD_DBUS=OFF` to omit this integration. To check asynchronous
request ownership and expiration on an isolated session bus:

```sh
dbus-run-session -- build/local/hyprworks --headless --config tests/dbus.lua
```

## Getting started

Create `~/.config/hyprworks/main.lua`:

```lua
local hw = require("hyprworks")
local bar = assert(hw.layer {edge = "top", height = 32, exclusive = true})
assert(bar:set_content {
    kind = "row", padding = 6, gap = 12, background = "#20242aff",
    children = {
        {kind = "text", id = "clock", text = os.date("%H:%M")},
        {kind = "button", text = "Exit", on_activate = function() hw.quit() end},
    },
})
local clock = assert(bar:find("clock"))
hw.timer(1000, function() assert(clock:set {text = os.date("%H:%M")}) end, true)
```

Run your shell with `hyprworks`. Changes to `main.lua` automatically reload your shell.

Keep your Lua modules and themes beside `main.lua`. Hyprworks respects
`XDG_CONFIG_HOME`; use `hyprworks --config /path/to/main.lua` to select another configuration.
See `hyprworks --help` for command-line options.

## GUI API

Hyprworks owns its Wayland connection, event loop, element trees and buffers.
Cairo paints shared-memory buffers; Pango shapes and measures text. Dependencies
are stock system libraries. GUI functions are directly on `hw`.

- `hw.layer {edge, width, height, output, layer, keyboard, exclusive, exclusive_zone, margin}`
  creates a panel. Edges are `top`, `bottom`, `left`, `right`; layers are
  `background`, `bottom`, `top`, `overlay`. Dimensions and margins use logical pixels.
  A zero length along the edge stretches between its opposite anchors.
  `edge = "all"` stretches across the output without reserving space.
- `hw.window {width, height, title, app_id}` creates a toplevel.
- `hw.popup {parent, x, y, width, height, grab}` creates a parent-owned popup.
  A grab requires an input callback on the parent. Each parent has at most one
  active or retiring popup; nested popups are supported.
- All surfaces accept `on_configure(surface)`, `on_close(surface)` and
  `on_grab_cleared(surface)`. `on_close` reports compositor-requested closure.
- `surface:state()` returns `closed`, `configured`, `width`, `height`, `scale`.
  Creation is asynchronous; use configured dimensions after `on_configure`.
- `surface:set_content(description)` validates and atomically replaces the tree.
  Old element handles become stale; focus and scrolling reset.
- `surface:find(id)` returns an element handle. `element:set(properties)` updates
  it, `element:state()` returns text, value and geometry, and `element:focus()`
  selects keyboard focus within its surface.
- `element:replace_children(descriptions)` atomically replaces a container's
  children. All existing element handles become stale. Surviving elements retain
  their text, scrolling and logical focus; active presses and composition reset.
- `surface:close()` is idempotent and closes popup descendants. Active surfaces
  belong to the runtime; collecting their Lua handles does not close them.
- `hw.outputs()` returns output name, description and integer scale.
  `hw.on_outputs(callback)` observes changes and schedules an initial notification.
- `hw.capabilities()` reports layer-shell, fractional-scale, text-input,
  clipboard and focus-grab support.

Constructors/mutations return a handle or `true` on success, or `nil, error` for
operational failure. Invalid argument types/values raise Lua errors. Callbacks
run serially after native dispatch; uncaught callback errors stop the runtime.
Programmatic element updates do not emit interaction callbacks.

### Elements

Kinds are `row`, `column`, `overlay`, `scroll`, `box`, `text`, `image`, `button`,
`toggle`, `slider`, and single-line `input`. Containers and buttons accept a `children` array.
Lua functions compose menus, radio groups and other shell-specific controls.

Common properties: `id`, `width`, `height`, `grow`, `padding`, `gap`, `radius`,
`background`, `color`, `accent`, `enabled`, `border_width`, `border_color`.
`x`/`y` offset the arranged position. `halign` accepts `stretch` (default),
`left`, `center`, `right`; `valign` accepts `stretch`, `top`, `center`, `bottom`.
Alignment applies outside the parent's row/column flow axis.
Dimensions default to natural size
(`-1`); `grow` shares remaining space along a row/column. Overflow clips.
Scroll containers arrange their children vertically; wheel input scrolls them.
Their `scroll` property and element state expose the vertical offset.
Use `horizontal = true` for horizontal arrangement and scrolling.
Text uses `text`, a Pango `font` description (default `"Sans 12"`), `wrap`,
and `align` (`left`, `center`, `right`).
Font sizes are capped at 256. Colors use `#RRGGBB` or `#RRGGBBAA`.

Buttons call `on_activate(element, event)`. Toggles, sliders and inputs call
`on_change(element, event)` with `{kind = "change", text, value}`. Toggle values
are 0/1; slider values are in 0..1. IDs are immutable and unique within a tree.

Images accept one `source`:

```lua
{kind = "image", width = 24, height = 24,
 source = {icon = "audio-volume-high", theme = "hicolor"}}
{kind = "image", source = {path = "/path/to/image.png"}}
{kind = "image", source = {data = encoded_png_or_svg}}
{kind = "image", source = {data = rgba_bytes, width = 2, height = 1, channels = 4}}
```

RGB/RGBA data is tightly packed and converted directly to premultiplied pixels.
PNG and SVG decode asynchronously; SVG `source.pixels` selects raster width
(default 64). Icon resolution follows XDG theme inheritance and refreshes at
layout/scale changes. Change the theme by setting a new source. Images call
`on_loaded(element, event)` with `kind = "loaded"` and an optional `error`.
Replacing a source/tree or closing its surface suppresses obsolete completions.

Limits: 32 active/retiring surfaces, 2048 elements and depth 32 per tree,
16 KiB per text, 1 MiB tree strings, 8 MiB encoded/raw input per image, image
dimensions up to 4096, 128 MiB accounted image storage and 256 MiB presentation
storage. Image jobs wait behind a bounded worker queue; busy buffers are reused
only after compositor release. Presentation-budget exhaustion reclaims idle
buffers and reports an error if capacity is still insufficient.

## Focus and input

The GUI chooses focus and dismissal per surface. `hw.layer` accepts
`keyboard = "none"` (default),
`"on_demand"` (compositor-controlled focus), or `"exclusive"` (keyboard priority).
The compositor controls how on-demand layers receive focus.

`surface:grab_focus()` explicitly restricts input to that surface through
Hyprland's focus-grab protocol.
`surface:release_focus()` removes the grab; `on_grab_cleared(surface)` reports
compositor cancellation. Configure focus grabs and surface dismissal in Lua.

`surface:set_input_region(rectangles)` accepts `{x, y, width, height}` rectangles;
an empty list makes the surface input-transparent, and `nil` restores the default.

Keyboard input supports tab navigation, key repeat, compose sequences, selection,
grapheme deletion, visual cursor movement and text-input-v3 IME transactions.
Each surface has one logical focus and one active press. A new press takes
ownership; motions, releases and cancellation from other seats or pointer/touch
sources cannot complete or alter it.
Surface `on_input(surface, event)` callbacks observe `motion`, `leave`, `button`,
`axis`, `key`, `focus`, and `blur` before element dispatch. Events carry `seat`,
`x`, `y`, `delta`, `touch`, `pressed`, `ctrl`, `alt`, `shift`, the XKB `key` name,
and the Linux `button` code for pointer buttons. Returning true consumes a key
event; pointer events still complete native press/cancellation handling.
Inputs support Ctrl+A/C/X/V. `hw.clipboard_read(function(text, error) ... end)`
starts a bounded asynchronous read; `hw.clipboard_write(text)` requires an input
callback. Transfers have a five-second deadline and a 1 MiB limit.
Inputs with `password = true` mask text and disable copying/cutting and IME
surrounding-text exchange. Keyboard composition and paste remain available.
`placeholder` displays a hint while an input is empty.

In GUI invocations, `hw.hyprland.shortcuts` shares the runtime's Wayland connection.
Its events contain `id`, `pressed`, `seconds`, and `nanoseconds`. Headless shortcuts
own their connection. Unrelated integration sources have no global event ordering.
Ordered GUI input is bounded to 256 queued events; exhaustion reports an error.

## Starting with your session

Enable the user service after creating your configuration:

```sh
systemctl --user enable --now hyprworks.service
```

The service follows your graphical session. UWSM supplies the required
`WAYLAND_DISPLAY` and `HYPRLAND_INSTANCE_SIGNATURE` environment variables.
Other session setups should export them to the user service manager.

Reload your shell or view its logs:

```sh
systemctl --user reload hyprworks.service
journalctl --user -u hyprworks.service
```

Lua configurations can also reload with `hw.reload()`.

## Building from source

Requires a C++23 compiler, CMake 4.5+, Ninja, pkg-config and these dependencies:

| Component | Dependency |
|---|---|
| Core | json-c 0.19+, Lua 5.5.1+ |
| GUI | wayland-client/scanner 1.26+, wayland-protocols 1.45+, xkbcommon 1.13.2+, Cairo, Pango, libpng, librsvg |
| D-Bus integrations | libsystemd 262+ |
| Audio | libpulse 17+ |
| Global shortcuts | wayland-client and wayland-scanner 1.26+ |
| Desktop entries | gio-unix-2.0 2.90.1+ |

### Build and run

```sh
cmake -S . -B build/local -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib
cmake --build build/local
build/local/hyprworks
```

For packaging, stage with `DESTDIR="$pkgdir" cmake --install build/local`.
`HYPRWORKS_BUILD_GUI=OFF` omits GUI dependencies. No private dependency libraries
or patches are installed.

Core ownership/concurrency checks: configure with `HYPRWORKS_BUILD_TESTS=ON`,
build, then `ctest --test-dir build/local --output-on-failure`.
`ctest --test-dir build/local -R image-worker --output-on-failure` runs the
worker checks independently of font initialization, including under ThreadSanitizer.
`build/local/hyprworks --config tests/gui.lua` exercises lifecycle behavior against
a running compositor, creating temporary windows and a layer surface.

### C++ embedding

Use `find_package(hyprworks CONFIG REQUIRED)` and link `hyprworks::hyprworks`.
For Lua embedding, request `COMPONENTS lua` and link `hyprworks::lua`.
Both targets also support `add_subdirectory`.
`hyprworks::lua::Invocation` owns load/run/close and supports
`EventLoop::headless` or `EventLoop::gui`. Only quit/reload requests are
cross-thread operations; the caller must keep the invocation alive until all
requesting threads have stopped. The GUI wait/read protocol is private.
Embedders may pass a borrowed `SignalSource` with a nonblocking wake-pipe read
descriptor. Producers atomically set its `quit`/`reload` pending bits before
writing a wake byte; the source and pipe must outlive the invocation.
The CLI uses this mechanism without blocking signals inherited by launched apps.
