#pragma once

#include <systemd/sd-bus.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
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

inline std::string text(sd_bus_message* message, char type, std::size_t& bytes) {
    const char* value = nullptr;
    require(sd_bus_message_read_basic(message, type, &value));
    const auto length = ::strnlen(value, 16385);
    if (length > 16384 || bytes + length > 1024 * 1024) throw std::runtime_error("D-Bus string budget exceeded");
    bytes += length;
    return {value, length};
}

inline void open_session(sd_bus*& bus, const std::string& address) {
    std::string target = address;
    if (target.empty()) {
        if (const char* environment = std::getenv("DBUS_SESSION_BUS_ADDRESS")) target = environment;
        else target = "unix:path=/run/user/" + std::to_string(getuid()) + "/bus";
    }
    if (target.empty() || target.size() > 16384 || target.contains('\0')) throw std::runtime_error("invalid session bus address");
    check(sd_bus_new(&bus));
    check(sd_bus_set_address(bus, target.c_str()));
    check(sd_bus_set_bus_client(bus, true));
    check(sd_bus_set_method_call_timeout(bus, 2000000));
    check(sd_bus_start(bus));
}

inline Message method(sd_bus* bus, const char* destination, const char* path, const char* interface, const char* member) {
    sd_bus_message* raw = nullptr;
    check(sd_bus_message_new_method_call(bus, &raw, destination, path, interface, member));
    return Message(raw, sd_bus_message_unref);
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
