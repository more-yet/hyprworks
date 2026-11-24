#include <hyprworks/dbus.hpp>
#include <algorithm>
#include <array>
#include <cassert>
#include <iostream>
#include <utility>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

using namespace hyprworks::dbus;
using namespace std::chrono_literals;

struct Peer { Connection bus; std::vector<Event> events; };

static void until(std::array<Peer, 3>& peers, auto done) {
    const auto deadline = Clock::now() + 5s;
    while (!done()) {
        assert(Clock::now() < deadline);
        std::array<WaitSet, 3> waits;
        std::array<pollfd, 3> fds;
        int timeout = 20;
        for (std::size_t i = 0; i < peers.size(); ++i) {
            waits[i] = peers[i].bus.prepare_wait();
            fds[i] = waits[i].fd;
            if (waits[i].deadline && *waits[i].deadline <= Clock::now()) timeout = 0;
        }
        assert(poll(fds.data(), fds.size(), timeout) >= 0);
        for (std::size_t i = 0; i < peers.size(); ++i) {
            waits[i].fd.revents = fds[i].revents;
            auto update = peers[i].bus.dispatch(waits[i]);
            if (peers[i].bus.status().phase == Phase::failed) {
                std::cerr << peers[i].bus.status().error << '\n';
                std::abort();
            }
            for (auto& event : update.events) peers[i].events.push_back(std::move(event));
        }
    }
}

static bool has(const Peer& peer, EventKind kind, std::uint64_t id = 0) {
    return std::ranges::any_of(peer.events, [&](const Event& event) {
        return event.kind == kind && (!id || event.id == id);
    });
}

static Event take(Peer& peer, EventKind kind, std::uint64_t id = 0) {
    const auto found = std::ranges::find_if(peer.events, [&](const Event& event) {
        return event.kind == kind && (!id || event.id == id);
    });
    assert(found != peer.events.end());
    auto event = std::move(*found);
    peer.events.erase(found);
    return event;
}

int main() {
    std::array<Peer, 3> peers;
    auto& server = peers[0];
    auto& client = peers[1];
    auto& rival = peers[2];
    for (auto& peer : peers) assert(peer.bus.start(false));
    until(peers, [&] { return std::ranges::all_of(peers, [](const Peer& peer) { return peer.bus.status().phase == Phase::ready; }); });
    const std::string name = "org.hyprworks.Core", path = "/org/hyprworks/Core", interface = name;
    assert(server.bus.request_name(name));
    until(peers, [&] { return has(server, EventKind::name_acquired); });
    take(server, EventKind::name_acquired);
    assert(server.bus.owned_names() == std::vector<std::string>{name});
    assert(rival.bus.request_name(name));
    until(peers, [&] { return has(rival, EventKind::name_failed); });
    assert(take(rival, EventKind::name_failed).error_name == "org.freedesktop.DBus.Error.NameExists");
    assert(rival.bus.owned_names().empty());

    assert(server.bus.export_interface(path, interface,
        {{"Hold", "", ""}, {"Fd", "h", "h"}}, {},
        {{"State", {"s", std::string("idle")}, false},
         {"Retained", {"s", std::string(32768, 'x')}, true}}, 2s));
    assert(server.bus.export_interface(path + "/request", interface, {{"Hold", "", ""}}, {}, {}, 2s));
    auto call = [&](Peer& peer, const std::string& object, const std::string& iface, const std::string& member, Body body = {}) {
        auto id = peer.bus.call({name, object, iface, member, std::move(body), 3s});
        assert(id);
        return *id;
    };
    const auto subscription = client.bus.subscribe("type='signal',path='/org/hyprworks/Core'");
    assert(subscription);
    // The round trip also orders match installation before publication.
    const auto initial = call(client, path, "org.freedesktop.DBus.Properties", "Get",
        {{"s", interface}, {"s", std::string("State")}});
    until(peers, [&] { return has(client, EventKind::reply, initial); });
    assert(take(client, EventKind::reply, initial).error.empty());
    assert(!server.bus.set_property(path, interface, "State", {"u", std::uint64_t{2}}));
    assert(server.bus.set_property(path, interface, "State", {"s", std::string("sharing")}));
    assert(!server.bus.set_property(path, interface, "State", {"s", std::string(32769, 'x')}));
    until(peers, [&] { return has(client, EventKind::signal); });
    assert(take(client, EventKind::signal).member == "PropertiesChanged");
    for (unsigned read = 0; read < 2; ++read) {
        const auto get = call(client, path, "org.freedesktop.DBus.Properties", "Get",
            {{"s", interface}, {"s", std::string("State")}});
        until(peers, [&] { return has(client, EventKind::reply, get); });
        auto property = take(client, EventKind::reply, get);
        assert(property.error.empty());
        assert(std::get<std::string>(std::get<Body>(property.arguments[0].data)[0].data) == "sharing");
    }
    const int original = memfd_create("hyprworks-dbus-test", MFD_CLOEXEC);
    assert(original >= 0 && write(original, "secret", 6) == 6);
    auto descriptor = FileDescriptor::duplicate(original);
    assert(descriptor && descriptor->get() != original);
    close(original);
    auto property_fd = FileDescriptor::duplicate(descriptor->get());
    assert(property_fd);
    const int property_number = property_fd->get();
    assert(server.bus.export_interface(path + "/descriptor", interface, {}, {},
        {{"Descriptor", {"h", *property_fd}, true}}, 1s));
    *property_fd = {};
    assert(fcntl(property_number, F_GETFD) == -1 && errno == EBADF);
    const auto read_fd = call(client, path + "/descriptor", "org.freedesktop.DBus.Properties", "Get",
        {{"s", interface}, {"s", std::string("Descriptor")}});
    until(peers, [&] { return has(client, EventKind::reply, read_fd); });
    auto property_reply = take(client, EventKind::reply, read_fd);
    assert(property_reply.error.empty());
    const int retained_property_fd = std::get<FileDescriptor>(std::get<Body>(property_reply.arguments[0].data)[0].data).get();
    assert(server.bus.unexport(path + "/descriptor", interface));
    char property_bytes[6];
    assert(pread(retained_property_fd, property_bytes, sizeof(property_bytes), 0) == 6);
    assert(std::string_view(property_bytes, 6) == "secret");
    property_reply.arguments.clear();
    assert(fcntl(retained_property_fd, F_GETFD) == -1 && errno == EBADF);
    const auto fd_call = call(client, path, interface, "Fd", {{"h", *descriptor}});
    descriptor = FileDescriptor{};
    until(peers, [&] { return has(server, EventKind::method); });
    auto fd_request = take(server, EventKind::method);
    auto retained = std::get<FileDescriptor>(fd_request.arguments[0].data);
    const int received_fd = retained.get();
    assert(fcntl(received_fd, F_GETFD) & FD_CLOEXEC);
    assert(server.bus.reply(fd_request.id, fd_request.arguments));
    fd_request.arguments.clear();
    until(peers, [&] { return has(client, EventKind::reply, fd_call); });
    auto fd_reply = take(client, EventKind::reply, fd_call);
    assert(fd_reply.error.empty());
    const int returned_fd = std::get<FileDescriptor>(fd_reply.arguments[0].data).get();
    char contents[6];
    assert(pread(returned_fd, contents, sizeof(contents), 0) == 6);
    assert(std::string_view(contents, 6) == "secret");
    retained = {};
    assert(fcntl(received_fd, F_GETFD) == -1 && errno == EBADF);

    const auto removed_call = call(client, path + "/request", interface, "Hold");
    const auto live_call = call(client, path, interface, "Hold");
    until(peers, [&] {
        return std::ranges::count_if(server.events, [](const Event& event) { return event.kind == EventKind::method; }) == 2;
    });
    auto removed = take(server, EventKind::method);
    auto live = take(server, EventKind::method);
    assert(removed.path == path + "/request");
    assert(server.bus.unexport(path + "/request", interface));
    assert(!server.bus.reply(removed.id, {}));
    assert(!server.bus.error(removed.id, "org.hyprworks.Error", "stale"));
    assert(!server.bus.reply(live.id, {{"s", std::string("invalid reply")}}));
    assert(server.bus.reply(live.id, {}));
    until(peers, [&] { return has(client, EventKind::reply, removed_call) && has(client, EventKind::reply, live_call); });
    assert(take(client, EventKind::reply, removed_call).error_name == "org.freedesktop.DBus.Error.UnknownObject");
    assert(take(client, EventKind::reply, live_call).error.empty());
    assert(take(server, EventKind::expired, removed.id).id == removed.id);
    assert(server.bus.export_interface(path + "/request", interface, {{"Hold", "", ""}}, {}, {}, 2s));
    assert(!server.bus.reply(removed.id, {}));

    const auto gone_call = call(client, path, interface, "Hold");
    (void)gone_call;
    until(peers, [&] { return has(server, EventKind::method); });
    auto gone = take(server, EventKind::method);
    client.bus.stop();
    until(peers, [&] { return has(server, EventKind::expired, gone.id); });
    assert(take(server, EventKind::expired, gone.id).error_name == "org.freedesktop.DBus.Error.Disconnected");
    assert(!server.bus.reply(gone.id, {}));
    assert(has(server, EventKind::peer_gone));
    // Received descriptors remain owned after message and connection teardown.
    assert(pread(returned_fd, contents, sizeof(contents), 0) == 6);
    fd_reply.arguments.clear();
    assert(fcntl(returned_fd, F_GETFD) == -1 && errno == EBADF);

    assert(server.bus.release_name(name));
    until(peers, [&] { return has(server, EventKind::name_lost); });
    take(server, EventKind::name_lost);
    assert(server.bus.owned_names().empty());
    assert(rival.bus.request_name(name));
    until(peers, [&] { return has(rival, EventKind::name_acquired); });
    take(rival, EventKind::name_acquired);
    rival.bus.stop();
    assert(server.bus.request_name(name));
    until(peers, [&] { return has(server, EventKind::name_acquired); });
    take(server, EventKind::name_acquired);

    assert(client.bus.start(false));
    const auto shutdown_call = call(client, path, interface, "Hold");
    until(peers, [&] { return has(server, EventKind::method); });
    auto shutdown = take(server, EventKind::method);
    const auto stale_wait = server.bus.prepare_wait();
    server.bus.stop();
    assert(server.bus.start(false));
    assert(server.bus.dispatch(stale_wait).events.empty());
    assert(!server.bus.reply(shutdown.id, {}));
    until(peers, [&] { return has(client, EventKind::reply, shutdown_call); });
    assert(!take(client, EventKind::reply, shutdown_call).error_name.empty());
}
