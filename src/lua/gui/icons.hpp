#pragma once

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace hyprworks::lua {

struct IconTheme {
    struct Directory {
        std::string path;
        double minimum, maximum;
        std::size_t theme;
    };
    std::string name;
    std::vector<Directory> directories;
    std::vector<std::string> fallback_paths;
    std::map<std::pair<std::string, int>, std::string> paths;
};

std::string icon_path(std::optional<IconTheme>& cache, const std::string& theme, const std::string& name, int pixels);

}
