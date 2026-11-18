#include <hyprworks/desktop.hpp>
#include <gio/gdesktopappinfo.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <sys/inotify.h>
#include <unistd.h>

namespace hyprworks::desktop {
namespace {

using App = std::unique_ptr<GDesktopAppInfo, decltype(&g_object_unref)>;
using Text = std::unique_ptr<char, decltype(&g_free)>;
namespace fs = std::filesystem;

struct ContextScope {
    GMainContext* context;
    explicit ContextScope(GMainContext* value) : context(value) { g_main_context_push_thread_default(context); }
    ~ContextScope() { g_main_context_pop_thread_default(context); }
};

std::string copy_text(const char* value, std::size_t& bytes) {
    if (!value) return {};
    const auto length = strnlen(value, 65537);
    if (length > 65536 || (bytes += length) > 16 * 1024 * 1024) throw std::runtime_error("desktop metadata budget exceeded");
    return {value, length};
}

std::vector<std::string> copy_list(const char* const* values, std::size_t& bytes) {
    std::vector<std::string> result;
    if (!values) return result;
    for (std::size_t i = 0; values[i]; ++i) {
        if (i == 256) throw std::runtime_error("desktop metadata list limit exceeded");
        result.push_back(copy_text(values[i], bytes));
    }
    return result;
}

}

struct DesktopEntries::State {
    int fd = -1;
    std::map<int, std::vector<std::string>> watches;
    std::vector<fs::path> roots;
    std::vector<App> apps;
    std::optional<Snapshot> snapshot;
    Status status;
    bool status_pending = false, change_pending = false;
    std::optional<Clock::time_point> refresh_at;
    std::uint64_t generation = 0, revision = 0;
    bool more = false;

    void release() noexcept {
        if (fd >= 0) ::close(std::exchange(fd, -1));
        watches.clear(); apps.clear(); roots.clear(); refresh_at.reset(); more = false;
    }

    void scan() {
        const int next_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (next_fd < 0) throw std::system_error(errno, std::generic_category(), "watch desktop entries");
        try {
            const auto watch = [&](const fs::path& path) {
                const int descriptor = inotify_add_watch(next_fd, path.c_str(), IN_ONLYDIR | IN_CREATE | IN_DELETE |
                    IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF);
                if (descriptor < 0 && errno != ENOENT && errno != ENOTDIR)
                    throw std::system_error(errno, std::generic_category(), "watch desktop directory " + path.string());
                return descriptor;
            };
            std::map<int, std::vector<std::string>> next_watches;
            for (const auto& root : roots) {
                for (auto child = root; child.has_relative_path(); child = child.parent_path()) {
                    const auto descriptor = watch(child.parent_path());
                    if (descriptor >= 0) next_watches[descriptor].push_back(child.filename().string());
                }
            }
            std::size_t visited = 0, directories = 0, files = 0;
            std::set<std::string> claimed;
            std::vector<std::pair<std::string, fs::path>> candidates;
            for (const auto& root : roots) {
                std::vector<fs::path> paths;
                std::vector<int> ancestors;
                const auto visit = [&](auto&& visit, const fs::path& path) -> void {
                    const auto descriptor = watch(path);
                    if (descriptor < 0 || std::ranges::find(ancestors, descriptor) != ancestors.end()) return;
                    if (++directories > 4096 || ancestors.size() == 32) throw std::runtime_error("desktop directory limit exceeded");
                    next_watches[descriptor].clear();
                    ancestors.push_back(descriptor);
                    std::error_code error;
                    fs::directory_iterator iterator(path, error);
                    if (error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory) { ancestors.pop_back(); return; }
                    if (error) throw fs::filesystem_error("enumerate desktop directory", path, error);
                    for (; iterator != fs::directory_iterator{}; iterator.increment(error)) {
                        if (++visited > 32768) throw std::runtime_error("desktop traversal limit exceeded (32768 entries)");
                        const auto& item = *iterator;
                        const auto status = item.status(error);
                        if (error == std::errc::no_such_file_or_directory || error == std::errc::too_many_symbolic_link_levels) { error.clear(); continue; }
                        if (error) throw fs::filesystem_error("inspect desktop entry", item.path(), error);
                        if (fs::is_directory(status)) visit(visit, item.path());
                        else if (fs::is_regular_file(status) && item.path().extension() == ".desktop") {
                            if (++files > 8192) throw std::runtime_error("desktop entry limit exceeded (8192)");
                            paths.push_back(item.path());
                        }
                    }
                    if (error && error != std::errc::no_such_file_or_directory) throw fs::filesystem_error("enumerate desktop directory", path, error);
                    ancestors.pop_back();
                };
                visit(visit, root);
                std::ranges::sort(paths);
                for (auto& path : paths) {
                    auto id = path.lexically_relative(root).string();
                    std::ranges::replace(id, '/', '-');
                    if (claimed.insert(id).second) candidates.emplace_back(std::move(id), std::move(path));
                }
            }
            std::ranges::sort(candidates, {}, [](const auto& candidate) -> const std::string& { return candidate.first; });
            Snapshot next;
            std::vector<App> next_apps;
            next.entries.reserve(candidates.size()); next_apps.reserve(candidates.size());
            std::size_t bytes = 0;
            for (const auto& [id, path] : candidates) {
                App info(g_desktop_app_info_new_from_filename(path.c_str()), g_object_unref);
                if (!info || g_desktop_app_info_get_is_hidden(info.get())) continue;
                auto* app = G_APP_INFO(info.get());
                Entry entry;
                entry.id = copy_text(id.c_str(), bytes);
                entry.name = copy_text(g_app_info_get_display_name(app), bytes);
                entry.generic_name = copy_text(g_desktop_app_info_get_generic_name(info.get()), bytes);
                entry.comment = copy_text(g_app_info_get_description(app), bytes);
                entry.executable = copy_text(g_app_info_get_executable(app), bytes);
                entry.filename = copy_text(g_desktop_app_info_get_filename(info.get()), bytes);
                if (auto* icon = g_app_info_get_icon(app)) {
                    if (G_IS_THEMED_ICON(icon)) {
                        const auto* names = g_themed_icon_get_names(G_THEMED_ICON(icon));
                        entry.icon = copy_text(names ? names[0] : nullptr, bytes);
                    } else if (G_IS_FILE_ICON(icon)) {
                        const Text path(g_file_get_path(g_file_icon_get_file(G_FILE_ICON(icon))), g_free);
                        entry.icon = copy_text(path.get(), bytes);
                    }
                }
                entry.keywords = copy_list(g_desktop_app_info_get_keywords(info.get()), bytes);
                entry.mime_types = copy_list(g_app_info_get_supported_types(app), bytes);
                const auto categories = copy_text(g_desktop_app_info_get_categories(info.get()), bytes);
                std::size_t start = 0;
                while (start < categories.size()) {
                    auto end = categories.find(';', start);
                    if (end == std::string::npos) end = categories.size();
                    if (end != start) {
                        if (entry.categories.size() == 256) throw std::runtime_error("desktop category limit exceeded");
                        entry.categories.push_back(categories.substr(start, end - start));
                    }
                    start = end + 1;
                }
                const auto actions = copy_list(g_desktop_app_info_list_actions(info.get()), bytes);
                for (const auto& action : actions) {
                    const Text name(g_desktop_app_info_get_action_name(info.get(), action.c_str()), g_free);
                    entry.actions.push_back({action, copy_text(name.get(), bytes)});
                }
                entry.visible = g_app_info_should_show(app);
                entry.terminal = g_desktop_app_info_get_boolean(info.get(), "Terminal");
                entry.supports_files = g_app_info_supports_files(app); entry.supports_uris = g_app_info_supports_uris(app);
                next.entries.push_back(std::move(entry)); next_apps.push_back(std::move(info));
            }
            apps = std::move(next_apps);
            if (!snapshot || snapshot->entries != next.entries) {
                next.revision = ++revision; snapshot = std::move(next); change_pending = true;
            }
            if (fd >= 0) ::close(fd);
            fd = next_fd;
            watches = std::move(next_watches);
            refresh_at.reset();
            more = false;
        } catch (...) { ::close(next_fd); throw; }
    }

    void fail(const std::string& error) {
        release(); status = {Phase::failed, error}; status_pending = true;
    }
};

DesktopEntries::DesktopEntries() : state_(std::make_unique<State>()) {}
DesktopEntries::~DesktopEntries() { state_->release(); }

std::expected<void, std::string> DesktopEntries::start() {
    stop(); auto& state = *state_;
    try {
        state.roots.emplace_back(fs::path(g_get_user_data_dir()) / "applications");
        for (const auto* path = g_get_system_data_dirs(); *path; ++path) {
            if (state.roots.size() == 64) throw std::runtime_error("desktop search path limit exceeded (64)");
            state.roots.emplace_back(fs::path(*path) / "applications");
        }
        for (auto& root : state.roots) root = fs::absolute(root).lexically_normal();
        state.scan();
    }
    catch (const std::exception& error) { state.fail(error.what()); return std::unexpected(state.status.error); }
    state.status = {Phase::ready, {}}; state.status_pending = true;
    return {};
}

void DesktopEntries::stop() noexcept {
    auto& state = *state_; ++state.generation; state.release();
    state.status = {}; state.status_pending = true;
}
void DesktopEntries::refresh() { if (state_->fd >= 0) state_->refresh_at = Clock::time_point::min(); }
const Status& DesktopEntries::status() const noexcept { return state_->status; }
const std::optional<Snapshot>& DesktopEntries::snapshot() const noexcept { return state_->snapshot; }

WaitSet DesktopEntries::prepare_wait() {
    auto& state = *state_; WaitSet wait; wait.generation = ++state.generation;
    wait.fd = {state.fd, POLLIN, 0};
    wait.deadline = state.refresh_at;
    if (state.more || state.status_pending || state.change_pending) wait.deadline = Clock::time_point::min();
    return wait;
}

Update DesktopEntries::dispatch(const WaitSet& ready) {
    auto& state = *state_;
    if (ready.generation != state.generation || ready.fd.fd != state.fd)
        throw std::logic_error("dispatch requires the current desktop-entry wait set");
    ++state.generation;
    if (state.fd >= 0) try {
        if (ready.fd.revents & (POLLERR | POLLHUP | POLLNVAL)) throw std::runtime_error("desktop directory watch failed");
        state.more = false;
        alignas(inotify_event) std::array<char, 65536> events;
        for (unsigned batch = 0; batch < 4; ++batch) {
            const auto size = ::read(state.fd, events.data(), events.size());
            if (size < 0) {
                if (errno == EAGAIN) break;
                if (errno == EINTR) continue;
                throw std::system_error(errno, std::generic_category(), "read desktop changes");
            }
            for (std::size_t offset = 0; offset < static_cast<std::size_t>(size);) {
                const auto& event = *reinterpret_cast<const inotify_event*>(events.data() + offset);
                const auto found = state.watches.find(event.wd);
                if ((event.mask & IN_Q_OVERFLOW) || (found != state.watches.end() &&
                    (event.len == 0 || found->second.empty() || std::ranges::find(found->second, event.name) != found->second.end()))) {
                    if (!state.refresh_at) state.refresh_at = Clock::now() + std::chrono::milliseconds(50);
                }
                offset += sizeof(inotify_event) + event.len;
            }
            state.more = batch == 3;
        }
        if (state.refresh_at && Clock::now() >= *state.refresh_at) state.scan();
    } catch (const std::exception& error) { state.fail(error.what()); }
    return {std::exchange(state.status_pending, false), std::exchange(state.change_pending, false)};
}

std::expected<void, std::string> DesktopEntries::launch(const std::string& id, std::span<const std::string> uris, const std::string& action) {
    auto& state = *state_;
    if (state.status.phase != Phase::ready) return std::unexpected("desktop entries are not ready");
    if (uris.size() > 256) return std::unexpected("desktop launch exceeds 256 URIs");
    std::size_t bytes = 0;
    for (const auto& uri : uris)
        if (uri.size() > 65536 || uri.contains('\0') || (bytes += uri.size()) > 1024 * 1024)
            return std::unexpected("invalid desktop launch URI or URI budget exceeded");
    const auto found = std::ranges::lower_bound(state.snapshot->entries, id, {}, &Entry::id);
    if (found == state.snapshot->entries.end() || found->id != id) return std::unexpected("unknown desktop entry");
    auto* info = state.apps[static_cast<std::size_t>(found - state.snapshot->entries.begin())].get();
    if (!action.empty()) {
        if (!uris.empty()) return std::unexpected("desktop actions do not accept URIs");
        if (std::ranges::find(found->actions, action, &Action::id) == found->actions.end()) return std::unexpected("unknown desktop action");
    }
    const std::unique_ptr<GMainContext, decltype(&g_main_context_unref)> context(g_main_context_new(), g_main_context_unref);
    ContextScope scope(context.get());
    GList* list = nullptr;
    for (const auto& uri : uris) list = g_list_prepend(list, const_cast<char*>(uri.c_str()));
    const std::unique_ptr<GList, decltype(&g_list_free)> arguments(g_list_reverse(list), g_list_free);
    struct Completion { bool done = false, success = false; GError* error = nullptr; } completion;
    const std::unique_ptr<GCancellable, decltype(&g_object_unref)> cancel(g_cancellable_new(), g_object_unref);
    const auto destroy_source = [](GSource* source) { g_source_destroy(source); g_source_unref(source); };
    const std::unique_ptr<GSource, decltype(destroy_source)> timeout(g_timeout_source_new(5000), destroy_source);
    g_source_set_callback(timeout.get(), [](void* data) -> gboolean { g_cancellable_cancel(G_CANCELLABLE(data)); return G_SOURCE_REMOVE; }, cancel.get(), nullptr);
    g_source_attach(timeout.get(), context.get());
    if (action.empty()) {
        g_app_info_launch_uris_async(G_APP_INFO(info), arguments.get(), nullptr, cancel.get(), [](GObject* object, GAsyncResult* result, void* data) {
            auto& completion = *static_cast<Completion*>(data);
            completion.success = g_app_info_launch_uris_finish(G_APP_INFO(object), result, &completion.error);
            completion.done = true;
        }, &completion);
    } else {
        g_desktop_app_info_launch_action(info, action.c_str(), nullptr);
        const std::unique_ptr<GDBusConnection, decltype(&g_object_unref)> bus(g_bus_get_sync(G_BUS_TYPE_SESSION, cancel.get(), nullptr), g_object_unref);
        if (bus) {
            g_dbus_connection_flush(bus.get(), cancel.get(), [](GObject* object, GAsyncResult* result, void* data) {
                auto& completion = *static_cast<Completion*>(data);
                completion.success = g_dbus_connection_flush_finish(G_DBUS_CONNECTION(object), result, &completion.error);
                completion.done = true;
            }, &completion);
        } else { completion.done = true; completion.success = true; }
    }
    // GIO's activation callbacks retain this context until completion.
    while (!completion.done) g_main_context_iteration(context.get(), true);
    const std::unique_ptr<GError, decltype(&g_error_free)> error(completion.error, g_error_free);
    if (!completion.success) return std::unexpected(error ? error->message : "desktop launch failed");
    return {};
}

}
