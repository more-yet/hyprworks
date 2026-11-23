#pragma once

#include <cstddef>
#include <stdexcept>
#include <string_view>

namespace hyprworks::dbus {

std::string_view take_type(std::string_view& signature);

struct BodyBudget {
    static constexpr std::size_t max_nodes = 65536, max_bytes = 8 * 1024 * 1024;
    std::size_t nodes = 0, bytes = 0;

    void node(unsigned depth) {
        if (depth > 32 || nodes == max_nodes) throw std::length_error("D-Bus value limit exceeded");
        ++nodes;
    }
    void payload(std::size_t size) {
        if (size > max_bytes - bytes) throw std::length_error("D-Bus payload exceeds 8 MiB");
        bytes += size;
    }
};

}
