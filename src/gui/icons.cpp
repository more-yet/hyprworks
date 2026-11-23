#include "icons.hpp"

#include <hyprworks/host.hpp>
#include <glib.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <sys/stat.h>

namespace hyprworks::lua {
namespace {

bool component(const std::string& name) {
    return !name.empty() && name != "." && name != ".." && name.find_first_of("/\\") == std::string::npos;
}

bool file_exists(const std::string& path) {
    struct stat status;
    return ::stat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode);
}

IconTheme read_theme(const std::string& name) {
    if (!component(name)) throw std::runtime_error("invalid icon theme name");
    IconTheme result;
    result.name = name;
    std::vector<std::string> roots{std::string(g_get_home_dir()) + "/.icons"};
    std::vector<std::string> data_paths{g_get_user_data_dir()};
    for (const char* const* path = g_get_system_data_dirs(); *path; ++path) {
        if (data_paths.size() == 64) throw std::runtime_error("icon search exceeds 64 data paths");
        data_paths.emplace_back(*path);
    }
    for (const auto& path : data_paths) {
        roots.push_back(path + "/icons");
        result.fallback_paths.push_back(path + "/icons");
        result.fallback_paths.push_back(path + "/pixmaps");
    }
    result.fallback_paths.insert(result.fallback_paths.begin(), roots.front());

    std::vector<std::string> pending{"hicolor", name}, visited;
    std::size_t bytes = 0;
    while (!pending.empty()) {
        auto theme = std::move(pending.back());
        pending.pop_back();
        if (!component(theme) || std::ranges::find(visited, theme) != visited.end()) continue;
        if (visited.size() == 64) throw std::runtime_error("icon inheritance exceeds 64 themes");
        visited.push_back(theme);
        const std::unique_ptr<GKeyFile, decltype(&g_key_file_unref)> index(g_key_file_new(), g_key_file_unref);
        g_key_file_set_list_separator(index.get(), ',');
        bool loaded = false;
        for (const auto& root : roots) {
            const auto text = host::read_file(root + "/" + theme + "/index.theme", 1024 * 1024);
            if (!text) continue;
            bytes += text->size();
            if (bytes > 4 * 1024 * 1024) throw std::runtime_error("icon theme indexes exceed 4 MiB");
            if (g_key_file_load_from_data(index.get(), text->data(), text->size(), G_KEY_FILE_NONE, nullptr)) {
                loaded = true;
                break;
            }
        }
        if (!loaded) continue;
        const auto number = [&](const char* group, const char* key, double fallback) {
            return g_key_file_has_key(index.get(), group, key, nullptr) ?
                g_key_file_get_double(index.get(), group, key, nullptr) : fallback;
        };
        const auto root_begin = result.search_roots.size();
        for (const auto& root : roots) result.search_roots.push_back(root + "/" + theme);
        for (const char* key : {"Directories", "ScaledDirectories"}) {
            const std::unique_ptr<gchar*, decltype(&g_strfreev)> directories(
                g_key_file_get_string_list(index.get(), "Icon Theme", key, nullptr, nullptr), g_strfreev);
            for (char** directory = directories.get(); directory && *directory; ++directory) {
                const char* section = g_strstrip(*directory);
                const auto relative = std::filesystem::path(section).lexically_normal();
                if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") continue;
                const double size = number(section, "Size", 0), scale = number(section, "Scale", 1);
                const std::unique_ptr<gchar, decltype(&g_free)> type(
                    g_key_file_get_string(index.get(), section, "Type", nullptr), g_free);
                double minimum = size, maximum = size;
                if (type && std::string_view(type.get()) == "Scalable") {
                    minimum = number(section, "MinSize", size);
                    maximum = number(section, "MaxSize", size);
                } else if (!type || std::string_view(type.get()) == "Threshold") {
                    const double threshold = number(section, "Threshold", 2);
                    minimum = std::max(0.0, size - threshold);
                    maximum = size + threshold;
                }
                minimum *= scale;
                maximum *= scale;
                if (size <= 0 || scale <= 0 || !std::isfinite(minimum) || !std::isfinite(maximum) || minimum < 0 || maximum < minimum) continue;
                for (std::size_t root = root_begin; root < result.search_roots.size(); ++root) {
                    if (result.directories.size() == 16384) throw std::runtime_error("icon theme exceeds 16384 directories");
                    result.directories.push_back({result.search_roots[root] + "/" + relative.string(),
                        minimum, maximum, visited.size(), root});
                }
            }
        }
        const std::unique_ptr<gchar*, decltype(&g_strfreev)> inherits(
            g_key_file_get_string_list(index.get(), "Icon Theme", "Inherits", nullptr, nullptr), g_strfreev);
        const auto begin = pending.size();
        for (char** parent = inherits.get(); parent && *parent; ++parent) {
            if (pending.size() == 64) throw std::runtime_error("icon inheritance exceeds 64 pending themes");
            pending.emplace_back(g_strstrip(*parent));
        }
        std::reverse(pending.begin() + static_cast<std::ptrdiff_t>(begin), pending.end());
    }
    return result;
}

}

std::string icon_path(std::optional<IconTheme>& cache, const std::string& theme, const std::string& name, int pixels) {
    if (name.starts_with('/')) return file_exists(name) ? name : std::string{};
    if (!component(name)) return {};
    if (!cache || cache->name != theme) cache = read_theme(theme);
    const auto key = std::pair{name, pixels};
    if (const auto found = cache->paths.find(key); found != cache->paths.end()) return found->second;
    std::string path;
    double best = std::numeric_limits<double>::max();
    std::size_t selected_theme = 0;
    bool upscaled = true;
    std::vector<std::optional<bool>> roots_present(cache->search_roots.size());
    for (const auto& directory : cache->directories) {
        if (!path.empty() && directory.theme != selected_theme) break;
        const double distance = std::max({0.0, directory.minimum - pixels, pixels - directory.maximum});
        const bool upscale = pixels > directory.maximum;
        if (distance > best || (distance == best && (upscale || !upscaled))) continue;
        auto& present = roots_present[directory.root];
        if (!present) {
            struct stat status;
            present = ::stat(cache->search_roots[directory.root].c_str(), &status) == 0 && S_ISDIR(status.st_mode);
        }
        if (!*present) continue;
        for (const char* extension : {".svg", ".png"}) {
            auto candidate = directory.path + "/" + name + extension;
            if (!file_exists(candidate)) continue;
            path = std::move(candidate);
            best = distance;
            upscaled = upscale;
            selected_theme = directory.theme;
            break;
        }
    }
    if (path.empty()) {
        for (const auto& directory : cache->fallback_paths) {
            for (const char* extension : {".svg", ".png"}) {
                auto candidate = directory + "/" + name + extension;
                if (file_exists(candidate)) { path = std::move(candidate); break; }
            }
            if (!path.empty()) break;
        }
    }
    if (cache->paths.size() == 4096) cache->paths.clear();
    cache->paths.emplace(key, path);
    return path;
}

}
