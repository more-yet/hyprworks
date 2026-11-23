#include <hyprworks/upower.hpp>
#include "dbus.hpp"
#include <algorithm>
#include <cmath>
#include <exception>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace hyprworks::upower {
namespace {

using dbus::check;
using dbus::require;
constexpr char service[] = "org.freedesktop.UPower";
constexpr char root[] = "/org/freedesktop/UPower";
constexpr char device_interface[] = "org.freedesktop.UPower.Device";
constexpr char display_path[] = "/org/freedesktop/UPower/devices/DisplayDevice";

void read_properties(sd_bus_message* message, Snapshot& snapshot, Device* device, std::size_t& bytes) {
    require(sd_bus_message_enter_container(message, 'a', "{sv}"));
    unsigned count = 0;
    while (check(sd_bus_message_enter_container(message, 'e', "sv")) > 0) {
        if (++count > 128)
            throw std::runtime_error("UPower property limit exceeded");
        const auto key = dbus::enter_property(message);
        if (!device) {
            if (key == "OnBattery") snapshot.on_battery = dbus::boolean(message);
            else if (key == "LidIsPresent") snapshot.lid_present = dbus::boolean(message);
            else if (key == "LidIsClosed") snapshot.lid_closed = dbus::boolean(message);
            else require(sd_bus_message_skip(message, nullptr));
        } else if (key == "Percentage") {
            device->percentage = dbus::finite_number(message);
            if (device->percentage < 0 || device->percentage > 100) throw std::runtime_error("invalid UPower percentage");
        } else if (key == "IsPresent") device->present = dbus::boolean(message);
        else if (key == "PowerSupply") device->power_supply = dbus::boolean(message);
        else if (key == "Online") device->online = dbus::boolean(message);
        else if (key == "IsRechargeable") device->rechargeable = dbus::boolean(message);
        else if (key == "Capacity") device->capacity = dbus::finite_number(message);
        else if (key == "Energy") device->energy = dbus::finite_number(message);
        else if (key == "EnergyFull") device->energy_full = dbus::finite_number(message);
        else if (key == "EnergyRate") device->energy_rate = dbus::finite_number(message);
        else if (key == "Type") require(sd_bus_message_read(message, "u", &device->type));
        else if (key == "State") require(sd_bus_message_read(message, "u", &device->state));
        else if (key == "WarningLevel") require(sd_bus_message_read(message, "u", &device->warning_level));
        else if (key == "BatteryLevel") require(sd_bus_message_read(message, "u", &device->battery_level));
        else if (key == "TimeToEmpty") require(sd_bus_message_read(message, "x", &device->time_to_empty));
        else if (key == "TimeToFull") require(sd_bus_message_read(message, "x", &device->time_to_full));
        else if (key == "Vendor") device->vendor = dbus::text(message, 's', bytes);
        else if (key == "Model") device->model = dbus::text(message, 's', bytes);
        else if (key == "IconName") device->icon_name = dbus::text(message, 's', bytes);
        else require(sd_bus_message_skip(message, nullptr));
        check(sd_bus_message_exit_container(message));
        check(sd_bus_message_exit_container(message));
    }
    check(sd_bus_message_exit_container(message));
}

}

struct PowerConnection::State {
    sd_bus* bus = nullptr;
    sd_bus_slot* signals = nullptr;
    sd_bus_slot* owner = nullptr;
    sd_bus_slot* request = nullptr;
    Status status;
    std::shared_ptr<const Snapshot> snapshot;
    Snapshot pending;
    enum class Request { root, enumerate, display, device } step = Request::root;
    std::size_t device = 0, bytes = 0;
    unsigned installed = 0;
    bool more = false;
    std::uint64_t generation = 0;
    std::optional<Clock::time_point> deadline, refresh_at;
    std::exception_ptr callback_error;

    void disconnect() noexcept {
        request = sd_bus_slot_unref(request);
        signals = sd_bus_slot_unref(signals);
        owner = sd_bus_slot_unref(owner);
        bus = sd_bus_close_unref(bus);
        deadline.reset();
        refresh_at.reset();
        more = false;
        installed = 0;
        pending = {};
        callback_error = {};
    }

    void invalidate() {
        if (!refresh_at)
            refresh_at = Clock::now() + std::chrono::milliseconds(16);
    }

    static int signal(sd_bus_message*, void* userdata, sd_bus_error*) noexcept {
        static_cast<State*>(userdata)->invalidate();
        return 0;
    }

    static int owner_changed(sd_bus_message*, void* userdata, sd_bus_error*) noexcept {
        auto& state = *static_cast<State*>(userdata);
        state.request = sd_bus_slot_unref(state.request);
        state.deadline.reset();
        state.status.phase = Phase::synchronizing;
        state.invalidate();
        return 0;
    }

    static int match_installed(sd_bus_message* message, void* userdata, sd_bus_error*) noexcept {
        auto& state = *static_cast<State*>(userdata);
        try {
            if (const auto* error = sd_bus_message_get_error(message))
                throw std::runtime_error(error->message ? error->message : error->name);
            ++state.installed;
            if (state.installed == 2)
                state.invalidate();
        } catch (...) { state.callback_error = std::current_exception(); }
        return 0;
    }

    void send() {
        const char* path = step == Request::root || step == Request::enumerate ? root : step == Request::display ? display_path : pending.devices[device].path.c_str();
        auto message = dbus::method(bus, service, path, step == Request::enumerate ? service : "org.freedesktop.DBus.Properties",
                                    step == Request::enumerate ? "EnumerateDevices" : "GetAll");
        if (step != Request::enumerate)
            check(sd_bus_message_append(message.get(), "s", step == Request::root ? service : device_interface));
        check(sd_bus_call_async(bus, &request, message.get(), reply, this, 2000000));
    }

    static int reply(sd_bus_message* message, void* userdata, sd_bus_error*) noexcept {
        auto& state = *static_cast<State*>(userdata);
        state.request = sd_bus_slot_unref(state.request);
        try {
            if (const auto* error = sd_bus_message_get_error(message)) {
                state.status = {Phase::unavailable, error->message ? error->message : error->name};
                state.pending = {};
                state.deadline.reset();
                return 0;
            }
            if (state.step == Request::enumerate) {
                require(sd_bus_message_enter_container(message, 'a', "o"));
                unsigned count = 0;
                while (!check(sd_bus_message_at_end(message, false))) {
                    if (++count > 128)
                        throw std::runtime_error("UPower exceeds 128 devices");
                    Device device;
                    device.path = dbus::text(message, 'o', state.bytes);
                    if (device.path != display_path)
                        state.pending.devices.push_back(std::move(device));
                }
                check(sd_bus_message_exit_container(message));
                std::ranges::sort(state.pending.devices, {}, &Device::path);
                const auto duplicates = std::ranges::unique(state.pending.devices, {}, &Device::path);
                state.pending.devices.erase(duplicates.begin(), duplicates.end());
                state.step = Request::display;
            } else {
                Device* device = state.step == Request::root ? nullptr : state.step == Request::display ? &state.pending.display : &state.pending.devices[state.device];
                read_properties(message, state.pending, device, state.bytes);
                if (state.step == Request::root)
                    state.step = Request::enumerate;
                else if (state.step == Request::display)
                    state.step = Request::device;
                else
                    ++state.device;
            }
            if (state.step == Request::device && state.device == state.pending.devices.size()) {
                state.pending.revision = state.snapshot ? state.snapshot->revision : 0;
                if (!state.snapshot || state.pending != *state.snapshot) {
                    if (state.pending.revision == UINT64_MAX)
                        throw std::runtime_error("UPower snapshot revision exhausted");
                    ++state.pending.revision;
                    state.snapshot = std::make_shared<const Snapshot>(std::move(state.pending));
                }
                state.status = {Phase::ready, {}};
                state.deadline.reset();
            } else
                state.send();
        } catch (...) { state.callback_error = std::current_exception(); }
        return 0;
    }
};

PowerConnection::PowerConnection() : state_(std::make_unique<State>()) {}
PowerConnection::~PowerConnection() { stop(); }

std::expected<void, std::string> PowerConnection::start(const std::string& address) {
    stop();
    auto& state = *state_;
    state.snapshot.reset();
    try {
        dbus::open(state.bus, address, true);
        state.status = {Phase::connecting, {}};
        state.deadline = Clock::now() + std::chrono::seconds(10);
        check(sd_bus_add_match_async(state.bus, &state.signals, "type='signal',sender='org.freedesktop.UPower',path_namespace='/org/freedesktop/UPower'",
                                    State::signal, State::match_installed, &state));
        check(sd_bus_add_match_async(state.bus, &state.owner,
            "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.freedesktop.UPower'",
            State::owner_changed, State::match_installed, &state));
        return {};
    } catch (const std::exception& error) {
        state.disconnect();
        state.status = {Phase::failed, error.what()};
        return std::unexpected(state.status.error);
    }
}

void PowerConnection::stop() noexcept {
    state_->disconnect();
    state_->status = {};
    ++state_->generation;
}

void PowerConnection::refresh() {
    if (state_->bus) {
        state_->invalidate();
        ++state_->generation;
    }
}

WaitSet PowerConnection::prepare_wait() {
    auto& state = *state_;
    WaitSet wait;
    wait.generation = ++state.generation;
    if (!state.bus)
        return wait;
    wait.deadline = state.deadline;
    dbus::prepare(state.bus, wait.fd, wait.deadline);
    if (state.refresh_at && !state.request && state.installed == 2 && (!wait.deadline || *state.refresh_at < *wait.deadline))
        wait.deadline = state.refresh_at;
    if (state.more)
        wait.deadline = Clock::time_point::min();
    return wait;
}

Update PowerConnection::dispatch(const WaitSet& ready) {
    auto& state = *state_;
    if (ready.generation != state.generation || ready.fd.fd != (state.bus ? sd_bus_get_fd(state.bus) : -1))
        return {};
    ++state.generation;
    const auto before = state.status;
    const auto revision = state.snapshot ? state.snapshot->revision : 0;
    if (!state.bus)
        return {};
    try {
        state.more = dbus::process(state.bus, state.callback_error);
        const auto now = Clock::now();
        if (state.deadline && now >= *state.deadline)
            throw std::runtime_error("UPower synchronization timed out");
        if (state.refresh_at && !state.request && state.installed == 2 && now >= *state.refresh_at) {
            state.refresh_at.reset();
            state.pending = {};
            state.pending.display.path = display_path;
            state.device = state.bytes = 0;
            state.step = State::Request::root;
            state.status = {Phase::synchronizing, {}};
            state.deadline = now + std::chrono::seconds(10);
            state.send();
        }
    } catch (const std::exception& error) {
        state.disconnect();
        state.status = {Phase::failed, error.what()};
    }
    return {before != state.status, revision != (state.snapshot ? state.snapshot->revision : 0)};
}

const Status& PowerConnection::status() const noexcept { return state_->status; }
std::shared_ptr<const Snapshot> PowerConnection::snapshot() const noexcept { return state_->snapshot; }

}
