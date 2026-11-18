#include <hyprworks/upower.hpp>
#include "dbus.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace hyprworks::upower {
namespace {

constexpr char service[] = "org.freedesktop.UPower";
constexpr char root[] = "/org/freedesktop/UPower";
constexpr char device_interface[] = "org.freedesktop.UPower.Device";
constexpr char display_path[] = "/org/freedesktop/UPower/devices/DisplayDevice";

int check(int result) {
    if (result < 0)
        throw std::system_error(-result, std::generic_category(), "UPower D-Bus");
    return result;
}

void require(int result) {
    if (check(result) == 0)
        throw std::runtime_error("incomplete UPower reply");
}

void read_string(sd_bus_message* message, char type, std::string& target, std::size_t& bytes) {
    const char* value = nullptr;
    require(sd_bus_message_read_basic(message, type, &value));
    const auto length = std::strlen(value);
    if (length > 16384 || bytes + length > 1024 * 1024)
        throw std::runtime_error("UPower strings exceed 16 KiB per field or 1 MiB per snapshot");
    target.assign(value, length);
    bytes += length;
}

void read_properties(sd_bus_message* message, Snapshot& snapshot, Device* device, std::size_t& bytes) {
    require(sd_bus_message_enter_container(message, 'a', "{sv}"));
    unsigned count = 0;
    while (check(sd_bus_message_enter_container(message, 'e', "sv")) > 0) {
        if (++count > 128)
            throw std::runtime_error("UPower property limit exceeded");
        const char* name = nullptr;
        require(sd_bus_message_read(message, "s", &name));
        const std::string_view key(name);
        bool* boolean = nullptr;
        double* number = nullptr;
        std::uint32_t* integer = nullptr;
        std::int64_t* seconds = nullptr;
        std::string* text = nullptr;
        if (!device) {
            if (key == "OnBattery") boolean = &snapshot.on_battery;
            else if (key == "LidIsPresent") boolean = &snapshot.lid_present;
            else if (key == "LidIsClosed") boolean = &snapshot.lid_closed;
        } else {
            if (key == "IsPresent") boolean = &device->present;
            else if (key == "PowerSupply") boolean = &device->power_supply;
            else if (key == "Online") boolean = &device->online;
            else if (key == "IsRechargeable") boolean = &device->rechargeable;
            else if (key == "Percentage") number = &device->percentage;
            else if (key == "Capacity") number = &device->capacity;
            else if (key == "Energy") number = &device->energy;
            else if (key == "EnergyFull") number = &device->energy_full;
            else if (key == "EnergyRate") number = &device->energy_rate;
            else if (key == "Type") integer = &device->type;
            else if (key == "State") integer = &device->state;
            else if (key == "WarningLevel") integer = &device->warning_level;
            else if (key == "BatteryLevel") integer = &device->battery_level;
            else if (key == "TimeToEmpty") seconds = &device->time_to_empty;
            else if (key == "TimeToFull") seconds = &device->time_to_full;
            else if (key == "Vendor") text = &device->vendor;
            else if (key == "Model") text = &device->model;
            else if (key == "IconName") text = &device->icon_name;
        }
        if (!boolean && !number && !integer && !seconds && !text)
            require(sd_bus_message_skip(message, "v"));
        else {
            require(sd_bus_message_enter_container(message, 'v', boolean ? "b" : number ? "d" : integer ? "u" : seconds ? "x" : "s"));
            if (boolean) {
                int value = 0;
                require(sd_bus_message_read(message, "b", &value));
                *boolean = value != 0;
            } else if (number) {
                require(sd_bus_message_read(message, "d", number));
                if (!std::isfinite(*number) || (key == "Percentage" && (*number < 0 || *number > 100)))
                    throw std::runtime_error("invalid UPower numeric property");
            } else if (integer)
                require(sd_bus_message_read(message, "u", integer));
            else if (seconds)
                require(sd_bus_message_read(message, "x", seconds));
            else
                read_string(message, 's', *text, bytes);
            check(sd_bus_message_exit_container(message));
        }
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
    std::optional<Snapshot> snapshot;
    Snapshot pending;
    enum class Request { root, enumerate, display, device } step = Request::root;
    std::size_t device = 0, bytes = 0;
    unsigned installed = 0;
    bool dirty = false, more = false;
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
        dirty = more = false;
        installed = 0;
        pending = {};
    }

    void invalidate() {
        dirty = true;
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
        sd_bus_message* raw = nullptr;
        check(sd_bus_message_new_method_call(bus, &raw, service, path, step == Request::enumerate ? service : "org.freedesktop.DBus.Properties",
                                            step == Request::enumerate ? "EnumerateDevices" : "GetAll"));
        std::unique_ptr<sd_bus_message, decltype(&sd_bus_message_unref)> message(raw, sd_bus_message_unref);
        if (step != Request::enumerate)
            check(sd_bus_message_append(raw, "s", step == Request::root ? service : device_interface));
        check(sd_bus_call_async(bus, &request, raw, reply, this, 2000000));
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
                    read_string(message, 'o', device.path, state.bytes);
                    if (!device.path.starts_with("/org/freedesktop/UPower/devices/"))
                        throw std::runtime_error("invalid UPower device path");
                    if (device.path != display_path)
                        state.pending.devices.push_back(std::move(device));
                }
                check(sd_bus_message_exit_container(message));
                std::ranges::sort(state.pending.devices, {}, &Device::path);
                for (std::size_t i = 1; i < state.pending.devices.size(); ++i) {
                    if (state.pending.devices[i - 1].path == state.pending.devices[i].path)
                        throw std::runtime_error("duplicate UPower device path");
                }
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
                    state.snapshot = std::move(state.pending);
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
    state.callback_error = {};
    try {
        if (address.size() > 16384 || address.contains('\0'))
            throw std::runtime_error("invalid D-Bus address");
        const char* bus_address = address.c_str();
        if (address.empty()) {
            bus_address = std::getenv("DBUS_SYSTEM_BUS_ADDRESS");
            if (!bus_address || !*bus_address)
                bus_address = "unix:path=/run/dbus/system_bus_socket";
            if (std::strlen(bus_address) > 16384)
                throw std::runtime_error("D-Bus address exceeds 16 KiB");
        }
        check(sd_bus_new(&state.bus));
        check(sd_bus_set_address(state.bus, bus_address));
        check(sd_bus_set_bus_client(state.bus, true));
        check(sd_bus_set_method_call_timeout(state.bus, 2000000));
        check(sd_bus_start(state.bus));
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
    if (!state_->bus)
        throw std::logic_error("UPower connection must be started before refresh");
    state_->invalidate();
}

WaitSet PowerConnection::prepare_wait() {
    auto& state = *state_;
    WaitSet wait;
    wait.generation = ++state.generation;
    if (!state.bus)
        return wait;
    wait.deadline = state.deadline;
    dbus::prepare(state.bus, wait.fd, wait.deadline);
    if (state.dirty && !state.request && state.installed == 2 && (!wait.deadline || *state.refresh_at < *wait.deadline))
        wait.deadline = state.refresh_at;
    if (state.more)
        wait.deadline = Clock::time_point::min();
    return wait;
}

Update PowerConnection::dispatch(const WaitSet& ready) {
    auto& state = *state_;
    if (ready.generation != state.generation || ready.fd.fd != (state.bus ? sd_bus_get_fd(state.bus) : -1))
        throw std::logic_error("dispatch requires the current UPower wait set");
    ++state.generation;
    const auto before = state.status;
    const auto revision = state.snapshot ? state.snapshot->revision : 0;
    if (!state.bus)
        return {};
    try {
        state.more = false;
        for (unsigned messages = 0; messages < 64; ++messages) {
            const int result = check(sd_bus_process(state.bus, nullptr));
            if (state.callback_error)
                std::rethrow_exception(std::exchange(state.callback_error, {}));
            if (result == 0)
                break;
            state.more = messages == 63;
        }
        const auto now = Clock::now();
        if (state.deadline && now >= *state.deadline)
            throw std::runtime_error("UPower synchronization timed out");
        if (state.dirty && !state.request && state.installed == 2 && now >= *state.refresh_at) {
            state.dirty = false;
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
const std::optional<Snapshot>& PowerConnection::snapshot() const noexcept { return state_->snapshot; }

}
