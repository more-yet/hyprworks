# Hyprworks

Build desktop shells for Hyprland in Lua. Create bars, docks, launchers and widgets
with support for workspaces, audio, media, notifications, tray menus, power and
desktop entries.

Hyprworks supplies native primitives, input handling and system integrations.
Themes, component composition, desktop layouts and interaction policy belong in Lua configuration.

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
local bar = hw.ui.layer {
    height = 32,
    exclusive_zone = 32,
    anchors = {top = true, left = true, right = true},
}
bar:set_content {kind = "text", text = "Hyprworks", halign = "center", valign = "center"}
```

Run your shell with `hyprworks`. Changes to `main.lua` automatically reload your shell.

Keep your Lua modules and themes beside `main.lua`. Hyprworks respects
`XDG_CONFIG_HOME`; use `hyprworks --config /path/to/main.lua` to select another configuration.
See `hyprworks --help` for command-line options.

Toolkit logging shows warnings and errors by default. Set `HT_TRACE=1` when
collecting detailed graphics and input diagnostics.

## Styling

Use `hw.ui.set_palette(table)` to set colors, fonts, rounding and the icon theme.
Apply the palette before creating content. When switching themes, apply the new
palette and rebuild your content.

Hyprworks resolves image icons from XDG icon themes, including inheritance,
`hicolor` and unthemed fallbacks. Icon images refresh when `icon_theme`, their
layout size or the output scale changes. The toolkit receives resolved paths.
Raw RGB/RGBA image sources are converted to PNG in memory and use the toolkit's
encoded-image API.

`checkbox_size` (8–64) and `checkbox_radius` (0–32, capped at size / 4) control
selection indicators; radio indicators stay circular. Selected indicators use
`accent` for the fill and `bright_text` for the mark. `focus` sets the keyboard-focus
color (default: accent), and `focus_width` (1–4) sets its width. Focus rings follow
control rounding and appear during keyboard use. Clicking retains keyboard focus
without a ring.

`textbox_padding_x` and `textbox_padding_y` set native text insets in logical
pixels (0–256, default: 3). Text, selection and caret share the inset area;
the caret follows the text line height and password fields reserve eye-button space.

Buttons accept `children` for icon and text layouts. Update a container with
`surface:set_children(id, children)`.

## Focus and input

The GUI chooses focus and dismissal per surface. `hw.ui.window` creates a regular
compositor-managed window. `hw.ui.layer` accepts `keyboard = "none"` (default),
`"on_demand"` (compositor-controlled focus), or `"exclusive"` (keyboard priority).
The compositor controls how on-demand layers receive focus.

`surface:grab_focus()` explicitly restricts input to that surface through
Hyprland's focus-grab protocol. This also prevents background scrolling.
`surface:release_focus()` removes the grab; `on_focus_cleared(surface)` reports
compositor cancellation. Configure focus grabs and surface dismissal in Lua.

In GUI invocations, `hw.hyprland.shortcuts` shares the GUI's Wayland connection
and ordered, bounded input queue. Its events include `pointer_surface` (the
surface under the pointer when the event arrived, or nil), `pointer_x` and
`pointer_y` in that surface's logical coordinates, and `surfaces`, a set
keyed by surface handles. Membership means the surface was visible at receipt
and has stayed visible through delivery. Use these event-time snapshots when
handling presses. GUI shortcuts use the GUI display; `display` is only
available to headless invocations, which own an independent connection.

The GUI can use non-consuming compositor mouse bindings and these snapshots to
choose outside-press dismissal. Bind the buttons used by your dismissal policy.
Control and shortcut callbacks retain input order. Hiding a surface invalidates
its queued control callbacks. The shared input queue holds at most 256 events;
exhaustion terminates the invocation with an error.

Surfaces accept `on_pointer_enter(surface)` and `on_pointer_leave(surface)`.
These observe entry into or exit from the surface's input region, including
movement across child controls. `surface:status().pointer_inside` reports the
current state, with `pointer_x` and `pointer_y` in surface-local logical coordinates
while the pointer is inside. Transitions are coalesced before Lua dispatch;
pointer state survives content replacement. Hiding or closing a surface clears
it and cancels pending pointer callbacks.

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
| GUI | Patched hyprtoolkit/hyprgraphics 0.6.0, xkbcommon 1.13.2+, Cairo, GLib |
| D-Bus integrations | libsystemd 262+ |
| Audio | libpulse 17+ |
| Global shortcuts | wayland-client and wayland-scanner 1.26+ |
| Desktop entries | gio-unix-2.0 2.90.1+ |

### Dependency patches

Use the `v0.6.0` tags of hyprgraphics and hyprtoolkit. Each patch applies directly
to its upstream tag:

- hyprgraphics: `patches/hyprgraphics-0.6.0-resource-lifetime.patch`
- hyprtoolkit: `patches/hyprtoolkit-0.6.0-shell.patch`

Place the patched source trees in `build/deps/hyprgraphics` and
`build/deps/hyprtoolkit`. From the Hyprworks directory, build and install them into
a private prefix:

```sh
for library in hyprgraphics hyprtoolkit; do
  PKG_CONFIG_PATH="$PWD/build/deps/prefix/lib/pkgconfig" \
    cmake -S "build/deps/$library" -B "build/deps/$library/build" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
      -DCMAKE_INSTALL_PREFIX="$PWD/build/deps/prefix" -DCMAKE_INSTALL_LIBDIR=lib \
      '-DCMAKE_INSTALL_RPATH=$ORIGIN' || break
  cmake --build "build/deps/$library/build" --target "$library" || break
  cmake --install "build/deps/$library/build" || break
done
```

These dependencies also require a C++26-capable compiler, hyprwayland-scanner and
wayland-protocols.

### Build and run

```sh
PKG_CONFIG_PATH="$PWD/build/deps/prefix/lib/pkgconfig" cmake -S . -B build/local -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib
cmake --build build/local
build/local/hyprworks
```

For packaging, stage the installation with `DESTDIR="$pkgdir" cmake --install build/local`.
The installation includes the patched graphics libraries under `lib/hyprworks`
(`/usr/lib/hyprworks` with the prefix above), using their normal filenames.
Hyprworks finds them through embedded runtime paths; keep the dependency build's
`$ORIGIN` setting so hyprtoolkit also finds its adjacent hyprgraphics copy.
Dependency headers and pkg-config files stay in the development prefix.

### C++ embedding

Use `find_package(hyprworks CONFIG REQUIRED)` and link `hyprworks::hyprworks`.
For Lua embedding, request `COMPONENTS lua` and link `hyprworks::lua`.
Both targets also support `add_subdirectory`.
