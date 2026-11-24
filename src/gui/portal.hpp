#pragma once

#include "windows.hpp"
#include <hyprworks/dbus.hpp>
#include <pipewire/pipewire.h>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;

namespace hyprworks::lua {

struct Gui;
struct WaitSet;
struct Image;
struct Portal;

struct PortalSource {
    std::string id, title, window;
    std::uint32_t type = 0, output = 0;
    int x = 0, y = 0, width = 0, height = 0;
    bool operator==(const PortalSource&) const = default;
};

struct PortalStream {
    Portal& owner;
    std::uint32_t type;
    int x, y, width, height;
    Capture capture;
    std::shared_ptr<Image> image;
    pw_stream* stream = nullptr;
    spa_hook listener{};
    pw_stream_state state = PW_STREAM_STATE_UNCONNECTED;
    std::uint32_t format_width = 0, format_height = 0, sequence = 0;
    std::size_t reserved = 0;
    std::optional<dbus::Clock::time_point> next_process;
    bool failed = false;
    PortalStream(Portal& owner, const PortalSource& source, bool cursor);
    ~PortalStream();
    void publish(std::shared_ptr<Image> image);
};

struct PortalSession {
    enum class Phase { created, selected, choosing, starting, streaming } phase = Phase::created;
    std::string path, app, parent, request_path;
    std::uint64_t token = 0, request = 0;
    std::uint32_t types = 1, cursor = 1;
    bool multiple = false;
    std::optional<dbus::Clock::time_point> deadline;
    std::vector<std::unique_ptr<PortalStream>> streams;
};

struct Portal {
    Gui& gui;
    dbus::Connection bus;
    dbus::WaitSet bus_wait;
    std::size_t bus_offset = 0, stream_bytes = 0;
    std::string frontend, ui_owner;
    std::uint64_t frontend_query = 0, ui_query = 0, next_token = 0, revision = 0;
    std::uint32_t types = 0;
    int fps = 30;
    bool changed = true, name_requested = false, pipewire_failed = false, attached = false;
    std::vector<PortalSource> sources;
    std::vector<PortalSession> sessions;
    pw_loop* loop = nullptr;
    pw_context* context = nullptr;
    pw_core* core = nullptr;
    spa_hook core_listener{};
    explicit Portal(Gui& gui, int fps);
    ~Portal();
    void connect_pipewire();
    void close_session(std::size_t index, std::uint32_t response = 2, std::string_view error = {});
    void refresh_sources();
    dbus::Body snapshot() const;
    void method(const dbus::Event& event);
};

void register_portal(lua_State* lua);
void prepare_portal(Portal& portal, WaitSet& wait);
void read_portal(Portal& portal, const WaitSet& wait);
void dispatch_portal(Portal& portal);

}
