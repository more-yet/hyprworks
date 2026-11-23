#pragma once

#include <hyprworks/action.hpp>
#include <systemd/sd-bus.h>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <poll.h>
#include <time.h>
#include <unistd.h>

namespace hyprworks::dbus {

using Clock = std::chrono::steady_clock;
using Message = std::unique_ptr<sd_bus_message, decltype(&sd_bus_message_unref)>;

inline int check(int result) {
    if (result < 0) throw std::system_error(-result, std::generic_category(), "D-Bus");
    return result;
}

inline void require(int result) {
    if (!check(result)) throw std::runtime_error("incomplete D-Bus message");
}

inline std::string_view enter_property(sd_bus_message* message) {
    const char* name;
    require(sd_bus_message_read(message, "s", &name));
    require(sd_bus_message_enter_container(message, 'v', nullptr));
    return name;
}

inline bool boolean(sd_bus_message* message) {
    int value;
    require(sd_bus_message_read(message, "b", &value));
    return value != 0;
}

inline double finite_number(sd_bus_message* message) {
    double value;
    require(sd_bus_message_read(message, "d", &value));
    if (!std::isfinite(value)) throw std::runtime_error("nonfinite D-Bus number");
    return value;
}

inline std::string text(sd_bus_message* message, char type, std::size_t& bytes) {
    const char* value = nullptr;
    require(sd_bus_message_read_basic(message, type, &value));
    const auto length = ::strnlen(value, 16385);
    if (length > 16384 || bytes + length > 1024 * 1024) throw std::runtime_error("D-Bus string budget exceeded");
    bytes += length;
    return {value, length};
}

inline std::vector<std::string> strings(sd_bus_message* message, std::size_t& bytes) {
    std::vector<std::string> values;
    require(sd_bus_message_enter_container(message, 'a', "s"));
    while (!check(sd_bus_message_at_end(message, false))) {
        if (values.size() == 128) throw std::length_error("D-Bus string array exceeds 128 entries");
        values.push_back(text(message, 's', bytes));
    }
    check(sd_bus_message_exit_container(message));
    return values;
}

inline void open(sd_bus*& bus, const std::string& address, bool system = false) {
    std::string target = address;
    if (target.empty()) {
        if (const char* environment = std::getenv(system ? "DBUS_SYSTEM_BUS_ADDRESS" : "DBUS_SESSION_BUS_ADDRESS"))
            target = environment;
        if (target.empty())
            target = system ? "unix:path=/run/dbus/system_bus_socket" : "unix:path=/run/user/" + std::to_string(getuid()) + "/bus";
    }
    if (target.size() > 16384 || target.contains('\0')) throw std::runtime_error("invalid D-Bus address");
    check(sd_bus_new(&bus));
    check(sd_bus_set_address(bus, target.c_str()));
    check(sd_bus_set_bus_client(bus, true));
    check(sd_bus_set_method_call_timeout(bus, 2000000));
    check(sd_bus_start(bus));
}

inline bool process(sd_bus* bus, std::exception_ptr& callback_error) {
    for (unsigned count = 0; count < 64; ++count) {
        const int result = check(sd_bus_process(bus, nullptr));
        if (callback_error) std::rethrow_exception(std::exchange(callback_error, {}));
        if (result == 0) return false;
    }
    return true;
}

inline Message method(sd_bus* bus, const char* destination, const char* path, const char* interface, const char* member) {
    sd_bus_message* raw = nullptr;
    check(sd_bus_message_new_method_call(bus, &raw, destination, path, interface, member));
    return Message(raw, sd_bus_message_unref);
}

inline ActionResult action_reply(std::uint64_t id, sd_bus_message* reply) {
    const auto* error = sd_bus_message_get_error(reply);
    if (!error) return {id, ActionOutcome::acknowledged, true, {}};
    const bool uncertain = sd_bus_error_has_name(error, SD_BUS_ERROR_NO_REPLY) ||
        sd_bus_error_has_name(error, SD_BUS_ERROR_DISCONNECTED) || sd_bus_error_has_name(error, SD_BUS_ERROR_TIMEOUT);
    return {id, uncertain ? ActionOutcome::failed : ActionOutcome::rejected, uncertain, error->message ? error->message : error->name};
}

inline void prepare(sd_bus* bus, pollfd& fd, std::optional<Clock::time_point>& deadline) {
    fd = {check(sd_bus_get_fd(bus)), static_cast<short>(check(sd_bus_get_events(bus))), 0};
    std::uint64_t timeout = UINT64_MAX;
    check(sd_bus_get_timeout(bus, &timeout));
    if (timeout == UINT64_MAX) return;
    timespec monotonic{};
    if (clock_gettime(CLOCK_MONOTONIC, &monotonic) < 0) throw std::system_error(errno, std::generic_category(), "monotonic clock");
    const auto now = static_cast<std::uint64_t>(monotonic.tv_sec) * 1000000 + static_cast<std::uint64_t>(monotonic.tv_nsec) / 1000;
    const auto time = Clock::now() + std::chrono::microseconds(timeout > now ? timeout - now : 0);
    if (!deadline || time < *deadline) deadline = time;
}

}
