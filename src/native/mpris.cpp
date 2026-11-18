#include <hyprworks/mpris.hpp>
#include "dbus.hpp"
#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <string_view>
#include <utility>

namespace hyprworks::mpris {
namespace {

using dbus::check;
using dbus::require;
constexpr char path[] = "/org/mpris/MediaPlayer2";
constexpr char root_interface[] = "org.mpris.MediaPlayer2";
constexpr char player_interface[] = "org.mpris.MediaPlayer2.Player";
constexpr char properties[] = "org.freedesktop.DBus.Properties";
constexpr char prefix[] = "org.mpris.MediaPlayer2.";

bool player_name(std::string_view name) { return name.starts_with(prefix) && name.size() > sizeof(prefix) - 1; }

std::vector<std::string> strings(sd_bus_message* message, std::size_t& bytes) {
    std::vector<std::string> result;
    require(sd_bus_message_enter_container(message, 'a', "s"));
    while (!check(sd_bus_message_at_end(message, false))) {
        if (result.size() == 128) throw std::runtime_error("MPRIS string list exceeds 128 entries");
        result.push_back(dbus::text(message, 's', bytes));
    }
    check(sd_bus_message_exit_container(message));
    return result;
}

void metadata(sd_bus_message* message, Metadata& metadata, std::size_t& bytes) {
    require(sd_bus_message_enter_container(message, 'a', "{sv}"));
    unsigned count = 0;
    while (check(sd_bus_message_enter_container(message, 'e', "sv"))) {
        if (++count > 128) throw std::runtime_error("MPRIS metadata exceeds 128 entries");
        const auto key = dbus::text(message, 's', bytes);
        std::string* text = key == "mpris:trackid" ? &metadata.track_id : key == "xesam:title" ? &metadata.title :
            key == "xesam:album" ? &metadata.album : key == "mpris:artUrl" ? &metadata.artwork_url : key == "xesam:url" ? &metadata.url : nullptr;
        auto* list = key == "xesam:artist" ? &metadata.artists : key == "xesam:albumArtist" ? &metadata.album_artists : key == "xesam:genre" ? &metadata.genres : nullptr;
        if (text) {
            const char type = key == "mpris:trackid" ? 'o' : 's';
            require(sd_bus_message_enter_container(message, 'v', type == 'o' ? "o" : "s"));
            *text = dbus::text(message, type, bytes);
            check(sd_bus_message_exit_container(message));
        } else if (list) {
            require(sd_bus_message_enter_container(message, 'v', "as"));
            *list = strings(message, bytes);
            check(sd_bus_message_exit_container(message));
        } else if (key == "mpris:length") {
            require(sd_bus_message_enter_container(message, 'v', "x"));
            std::int64_t value;
            require(sd_bus_message_read(message, "x", &value));
            if (value < 0) throw std::runtime_error("negative MPRIS track length");
            metadata.length = value;
            check(sd_bus_message_exit_container(message));
        } else require(sd_bus_message_skip(message, "v"));
        check(sd_bus_message_exit_container(message));
    }
    check(sd_bus_message_exit_container(message));
}

void read_properties(sd_bus_message* message, Player& player, bool root, std::size_t& bytes) {
    require(sd_bus_message_enter_container(message, 'a', "{sv}"));
    unsigned count = 0;
    while (check(sd_bus_message_enter_container(message, 'e', "sv"))) {
        if (++count > 128) throw std::runtime_error("MPRIS properties exceed 128 entries");
        const auto key = dbus::text(message, 's', bytes);
        std::string* text = nullptr;
        bool* boolean = nullptr;
        double* number = nullptr;
        std::vector<std::string>* list = nullptr;
        if (root) {
            if (key == "Identity") text = &player.identity;
            else if (key == "DesktopEntry") text = &player.desktop_entry;
            else if (key == "CanRaise") boolean = &player.can_raise;
            else if (key == "CanQuit") boolean = &player.can_quit;
            else if (key == "SupportedUriSchemes") list = &player.uri_schemes;
            else if (key == "SupportedMimeTypes") list = &player.mime_types;
        } else {
            if (key == "PlaybackStatus") text = &player.playback_status;
            else if (key == "LoopStatus") text = &player.loop_status.emplace();
            else if (key == "Shuffle") boolean = &player.shuffle.emplace();
            else if (key == "Volume") number = &player.volume.emplace();
            else if (key == "Rate") number = &player.rate;
            else if (key == "MinimumRate") number = &player.minimum_rate;
            else if (key == "MaximumRate") number = &player.maximum_rate;
            else if (key == "CanControl") boolean = &player.can_control;
            else if (key == "CanPlay") boolean = &player.can_play;
            else if (key == "CanPause") boolean = &player.can_pause;
            else if (key == "CanSeek") boolean = &player.can_seek;
            else if (key == "CanGoNext") boolean = &player.can_go_next;
            else if (key == "CanGoPrevious") boolean = &player.can_go_previous;
        }
        if (text || boolean || number || list || (!root && (key == "Position" || key == "Metadata"))) {
            require(sd_bus_message_enter_container(message, 'v', text ? "s" : boolean ? "b" : number ? "d" : list ? "as" : key == "Position" ? "x" : "a{sv}"));
            if (text) *text = dbus::text(message, 's', bytes);
            else if (boolean) { int value; require(sd_bus_message_read(message, "b", &value)); *boolean = value != 0; }
            else if (number) {
                require(sd_bus_message_read(message, "d", number));
                if (!std::isfinite(*number)) throw std::runtime_error("nonfinite MPRIS number");
            } else if (list) *list = strings(message, bytes);
            else if (key == "Metadata") metadata(message, player.metadata, bytes);
            else {
                std::int64_t value;
                require(sd_bus_message_read(message, "x", &value));
                if (value < 0) throw std::runtime_error("negative MPRIS position");
                player.position = value;
            }
            check(sd_bus_message_exit_container(message));
        } else require(sd_bus_message_skip(message, "v"));
        check(sd_bus_message_exit_container(message));
    }
    check(sd_bus_message_exit_container(message));
    if (!root && (player.playback_status != "Playing" && player.playback_status != "Paused" && player.playback_status != "Stopped"))
        throw std::runtime_error("invalid MPRIS playback status");
    if (!root && (player.rate == 0 || player.minimum_rate > player.maximum_rate || (player.volume && *player.volume < 0)))
        throw std::runtime_error("invalid MPRIS rate or volume");
}

}

std::optional<std::int64_t> estimated_position(const Player& player, Clock::time_point now) noexcept {
    if (!player.position) return {};
    long double position = *player.position;
    if (player.available && player.playback_status == "Playing" && now > player.sampled_at)
        position += std::chrono::duration<long double, std::micro>(now - player.sampled_at).count() * player.rate;
    const auto limit = player.metadata.length.value_or(INT64_MAX);
    if (position <= 0) return 0;
    if (position >= static_cast<long double>(limit)) return limit;
    return static_cast<std::int64_t>(position);
}

struct MediaConnection::State {
    sd_bus* bus = nullptr;
    sd_bus_slot *owners = nullptr, *properties_slot = nullptr, *seeked = nullptr, *query = nullptr, *command = nullptr;
    Status status;
    std::optional<Snapshot> snapshot;
    Snapshot pending;
    std::optional<RequestResult> result;
    PlayerKey command_player;
    std::uint64_t generation = 0, last_request = 0;
    std::size_t player = 0, bytes = 0;
    unsigned installed = 0;
    enum class Step { names, owner, root, player } step = Step::names;
    bool dirty = false, more = false;
    std::optional<Clock::time_point> deadline, refresh_at;
    Clock::time_point sent_at{};
    std::exception_ptr callback_error;

    void disconnect() noexcept {
        query = sd_bus_slot_unref(query); command = sd_bus_slot_unref(command);
        owners = sd_bus_slot_unref(owners); properties_slot = sd_bus_slot_unref(properties_slot); seeked = sd_bus_slot_unref(seeked);
        bus = sd_bus_close_unref(bus);
        pending = {}; installed = 0; dirty = more = false;
        deadline.reset(); refresh_at.reset(); callback_error = {};
    }

    void invalidate() noexcept {
        dirty = true;
        if (!refresh_at) refresh_at = Clock::now() + std::chrono::milliseconds(16);
    }

    void publish() {
        pending.revision = snapshot ? snapshot->revision : 0;
        if (!snapshot || pending != *snapshot) {
            if (pending.revision == UINT64_MAX) throw std::runtime_error("MPRIS revision exhausted");
            ++pending.revision;
            snapshot = std::move(pending);
        }
        pending = {};
        status = {Phase::ready, {}};
        deadline.reset();
    }

    void send() {
        auto message = dbus::method(bus, step == Step::names || step == Step::owner ? "org.freedesktop.DBus" : pending.players[player].key.owner.c_str(),
            step == Step::names || step == Step::owner ? "/org/freedesktop/DBus" : path,
            step == Step::names || step == Step::owner ? "org.freedesktop.DBus" : properties,
            step == Step::names ? "ListNames" : step == Step::owner ? "GetNameOwner" : "GetAll");
        if (step != Step::names) check(sd_bus_message_append(message.get(), "s", step == Step::owner ? pending.players[player].key.name.c_str() :
                                                            step == Step::root ? root_interface : player_interface));
        sent_at = Clock::now();
        check(sd_bus_call_async(bus, &query, message.get(), reply, this, 2000000));
    }

    void next_player() {
        if (++player == pending.players.size()) publish();
        else { step = Step::owner; send(); }
    }

    static int reply(sd_bus_message* message, void* data, sd_bus_error*) noexcept {
        auto& state = *static_cast<State*>(data);
        state.query = sd_bus_slot_unref(state.query);
        try {
            if (state.step == Step::names) {
                if (const auto* error = sd_bus_message_get_error(message)) throw std::runtime_error(error->message ? error->message : error->name);
                require(sd_bus_message_enter_container(message, 'a', "s"));
                unsigned count = 0;
                while (!check(sd_bus_message_at_end(message, false))) {
                    if (++count > 4096) throw std::runtime_error("D-Bus service list exceeds 4096 entries");
                    auto name = dbus::text(message, 's', state.bytes);
                    if (!player_name(name)) continue;
                    if (state.pending.players.size() == 32) throw std::runtime_error("MPRIS exceeds 32 players");
                    Player player;
                    player.key.name = std::move(name);
                    state.pending.players.push_back(std::move(player));
                }
                check(sd_bus_message_exit_container(message));
                std::ranges::sort(state.pending.players, {}, [](const Player& p) { return p.key.name; });
                if (state.pending.players.empty()) state.publish();
                else { state.step = Step::owner; state.send(); }
                return 0;
            }
            auto& player = state.pending.players[state.player];
            if (const auto* error = sd_bus_message_get_error(message)) {
                player.error = error->message ? error->message : error->name;
                state.next_player();
                return 0;
            }
            if (state.step == Step::owner) {
                player.key.owner = dbus::text(message, 's', state.bytes);
                if (!player.key.owner.starts_with(':') || !sd_bus_service_name_is_valid(player.key.owner.c_str())) throw std::runtime_error("invalid MPRIS owner");
                state.step = Step::root;
            } else {
                try {
                    read_properties(message, player, state.step == Step::root, state.bytes);
                } catch (const std::exception& error) {
                    player.error = error.what();
                    state.next_player();
                    return 0;
                }
                if (state.step == Step::player) {
                    player.available = true;
                    player.sampled_at = player.playback_status == "Playing" ? state.sent_at + (Clock::now() - state.sent_at) / 2 : Clock::time_point{};
                    state.next_player();
                    return 0;
                }
                state.step = Step::player;
            }
            state.send();
        } catch (...) { state.callback_error = std::current_exception(); }
        return 0;
    }

    static int installed_reply(sd_bus_message* message, void* data, sd_bus_error*) noexcept {
        auto& state = *static_cast<State*>(data);
        try {
            if (const auto* error = sd_bus_message_get_error(message)) throw std::runtime_error(error->message ? error->message : error->name);
            if (++state.installed == 3) { state.invalidate(); state.refresh_at = Clock::now(); }
        } catch (...) { state.callback_error = std::current_exception(); }
        return 0;
    }

    static int owner_changed(sd_bus_message* message, void* data, sd_bus_error*) noexcept {
        auto& state = *static_cast<State*>(data);
        try {
            const char *name, *old_owner, *new_owner;
            require(sd_bus_message_read(message, "sss", &name, &old_owner, &new_owner));
            if (!player_name(name)) return 0;
            state.query = sd_bus_slot_unref(state.query);
            state.pending = {};
            if (state.snapshot) {
                const auto removed = std::erase_if(state.snapshot->players, [&](const Player& player) { return player.key.name == name; });
                if (removed) {
                    if (state.snapshot->revision == UINT64_MAX) throw std::runtime_error("MPRIS revision exhausted");
                    ++state.snapshot->revision;
                }
            }
            if (state.command && state.command_player.name == name && state.command_player.owner != new_owner) {
                state.result = RequestResult{state.last_request, Outcome::failed, true, "MPRIS player owner changed"};
                state.command = sd_bus_slot_unref(state.command);
            }
            state.status.phase = Phase::synchronizing;
            state.invalidate();
        } catch (...) { state.callback_error = std::current_exception(); }
        return 0;
    }

    static int signal(sd_bus_message* message, void* data, sd_bus_error*) noexcept {
        auto& state = *static_cast<State*>(data);
        const char* sender = sd_bus_message_get_sender(message);
        const auto owns = [&](const Player& player) { return sender && player.key.owner == sender; };
        if ((state.snapshot && std::ranges::any_of(state.snapshot->players, owns)) || std::ranges::any_of(state.pending.players, owns)) state.invalidate();
        return 0;
    }

    const Player& target(const PlayerKey& key) const {
        if (!bus || !snapshot) throw std::runtime_error("MPRIS connection is not ready");
        if (command || result) throw std::runtime_error("an MPRIS request is already outstanding");
        if (last_request == UINT64_MAX) throw std::runtime_error("MPRIS request identifiers exhausted");
        const auto found = std::ranges::find(snapshot->players, key, &Player::key);
        if (found == snapshot->players.end() || !found->available) throw std::runtime_error("MPRIS player is unavailable or its owner changed");
        return *found;
    }

    static int command_reply(sd_bus_message* message, void* data, sd_bus_error*) noexcept {
        auto& state = *static_cast<State*>(data);
        state.command = sd_bus_slot_unref(state.command);
        try {
            const auto* error = sd_bus_message_get_error(message);
            const bool uncertain = error && (sd_bus_error_has_name(error, SD_BUS_ERROR_NO_REPLY) || sd_bus_error_has_name(error, SD_BUS_ERROR_DISCONNECTED) ||
                                             sd_bus_error_has_name(error, SD_BUS_ERROR_TIMEOUT));
            state.result = RequestResult{state.last_request, !error ? Outcome::acknowledged : uncertain ? Outcome::failed : Outcome::rejected,
                !error || uncertain, error ? (error->message ? error->message : error->name) : ""};
            state.invalidate();
        } catch (...) { state.callback_error = std::current_exception(); }
        return 0;
    }

    std::uint64_t send_command(const PlayerKey& player, sd_bus_message* message) {
        command_player = player;
        check(sd_bus_call_async(bus, &command, message, command_reply, this, 2000000));
        ++generation;
        return ++last_request;
    }

    std::uint64_t set_property(const PlayerKey& player, const char* name, const char* signature, const void* value) {
        auto message = dbus::method(bus, player.owner.c_str(), path, properties, "Set");
        check(sd_bus_message_append(message.get(), "ss", player_interface, name));
        check(sd_bus_message_open_container(message.get(), 'v', signature));
        check(sd_bus_message_append_basic(message.get(), *signature, value));
        check(sd_bus_message_close_container(message.get()));
        return send_command(player, message.get());
    }
};

MediaConnection::MediaConnection() : state_(std::make_unique<State>()) {}
MediaConnection::~MediaConnection() { stop(); }

std::expected<void, std::string> MediaConnection::start(const std::string& address) {
    auto& state = *state_;
    if (state.bus) return std::unexpected("MPRIS connection is already running");
    stop(); state.snapshot.reset();
    try {
        dbus::open_session(state.bus, address);
        state.status = {Phase::connecting, {}};
        state.deadline = Clock::now() + std::chrono::seconds(10);
        check(sd_bus_add_match_async(state.bus, &state.owners,
            "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0namespace='org.mpris.MediaPlayer2'",
            State::owner_changed, State::installed_reply, &state));
        check(sd_bus_add_match_async(state.bus, &state.properties_slot,
            "type='signal',path='/org/mpris/MediaPlayer2',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged'",
            State::signal, State::installed_reply, &state));
        check(sd_bus_add_match_async(state.bus, &state.seeked,
            "type='signal',path='/org/mpris/MediaPlayer2',interface='org.mpris.MediaPlayer2.Player',member='Seeked'",
            State::signal, State::installed_reply, &state));
        return {};
    } catch (const std::exception& error) {
        state.disconnect(); state.status = {Phase::failed, error.what()};
        return std::unexpected(state.status.error);
    }
}

std::optional<RequestResult> MediaConnection::stop() noexcept {
    auto& state = *state_;
    auto result = std::exchange(state.result, {});
    if (state.command && !result) result = RequestResult{state.last_request, Outcome::cancelled, true, {}};
    state.disconnect(); state.status = {}; ++state.generation;
    return result;
}

std::expected<void, std::string> MediaConnection::refresh() {
    if (!state_->bus) return std::unexpected("MPRIS connection is not running");
    state_->invalidate(); ++state_->generation;
    return {};
}

std::expected<std::uint64_t, std::string> MediaConnection::action(const PlayerKey& key, Action action) {
    try {
        const auto& player = state_->target(key);
        const char* member = nullptr;
        bool allowed = false;
        bool root = false;
        switch (action) {
            case Action::play: member = "Play"; allowed = player.can_play; break;
            case Action::pause: member = "Pause"; allowed = player.can_pause; break;
            case Action::play_pause: member = "PlayPause"; allowed = player.can_pause && (player.playback_status == "Playing" || player.can_play); break;
            case Action::stop: member = "Stop"; allowed = true; break;
            case Action::next: member = "Next"; allowed = player.can_go_next; break;
            case Action::previous: member = "Previous"; allowed = player.can_go_previous; break;
            case Action::raise: member = "Raise"; allowed = player.can_raise; root = true; break;
            case Action::quit: member = "Quit"; allowed = player.can_quit; root = true; break;
        }
        if (!member || !allowed || (!root && !player.can_control)) throw std::runtime_error("MPRIS action is not supported");
        auto message = dbus::method(state_->bus, key.owner.c_str(), path, root ? root_interface : player_interface, member);
        return state_->send_command(key, message.get());
    } catch (const std::exception& error) { return std::unexpected(error.what()); }
}

std::expected<std::uint64_t, std::string> MediaConnection::seek(const PlayerKey& key, std::int64_t offset) {
    try {
        const auto& player = state_->target(key);
        if (!player.can_control || !player.can_seek) throw std::runtime_error("MPRIS seeking is not supported");
        auto message = dbus::method(state_->bus, key.owner.c_str(), path, player_interface, "Seek");
        check(sd_bus_message_append(message.get(), "x", offset));
        return state_->send_command(key, message.get());
    } catch (const std::exception& error) { return std::unexpected(error.what()); }
}

std::expected<std::uint64_t, std::string> MediaConnection::set_position(const PlayerKey& key, const std::string& track_id, std::int64_t position) {
    try {
        const auto& player = state_->target(key);
        if (!player.can_control || !player.can_seek) throw std::runtime_error("MPRIS seeking is not supported");
        if (track_id.empty() || track_id != player.metadata.track_id || track_id == "/org/mpris/MediaPlayer2/TrackList/NoTrack" ||
            position < 0 || (player.metadata.length && position > *player.metadata.length)) throw std::runtime_error("stale track or invalid MPRIS position");
        auto message = dbus::method(state_->bus, key.owner.c_str(), path, player_interface, "SetPosition");
        check(sd_bus_message_append(message.get(), "ox", track_id.c_str(), position));
        return state_->send_command(key, message.get());
    } catch (const std::exception& error) { return std::unexpected(error.what()); }
}

std::expected<std::uint64_t, std::string> MediaConnection::set_volume(const PlayerKey& key, double volume) {
    try {
        const auto& player = state_->target(key);
        if (!player.can_control || !player.volume) throw std::runtime_error("MPRIS volume is not supported");
        if (!std::isfinite(volume) || volume < 0) throw std::runtime_error("MPRIS volume must be finite and nonnegative");
        return state_->set_property(key, "Volume", "d", &volume);
    } catch (const std::exception& error) { return std::unexpected(error.what()); }
}

std::expected<std::uint64_t, std::string> MediaConnection::set_rate(const PlayerKey& key, double rate) {
    try {
        const auto& player = state_->target(key);
        if (!player.can_control || !std::isfinite(rate) || rate == 0 || rate < player.minimum_rate || rate > player.maximum_rate)
            throw std::runtime_error("unsupported MPRIS playback rate");
        return state_->set_property(key, "Rate", "d", &rate);
    } catch (const std::exception& error) { return std::unexpected(error.what()); }
}

std::expected<std::uint64_t, std::string> MediaConnection::set_shuffle(const PlayerKey& key, bool shuffle) {
    try {
        const auto& player = state_->target(key);
        if (!player.can_control || !player.shuffle) throw std::runtime_error("MPRIS shuffle is not supported");
        const int value = shuffle;
        return state_->set_property(key, "Shuffle", "b", &value);
    } catch (const std::exception& error) { return std::unexpected(error.what()); }
}

std::expected<std::uint64_t, std::string> MediaConnection::set_loop_status(const PlayerKey& key, const std::string& loop_status) {
    try {
        const auto& player = state_->target(key);
        if (!player.can_control || !player.loop_status || (loop_status != "None" && loop_status != "Track" && loop_status != "Playlist"))
            throw std::runtime_error("unsupported MPRIS loop status");
        return state_->set_property(key, "LoopStatus", "s", loop_status.c_str());
    } catch (const std::exception& error) { return std::unexpected(error.what()); }
}

std::expected<std::uint64_t, std::string> MediaConnection::open_uri(const PlayerKey& key, const std::string& uri) {
    try {
        const auto& player = state_->target(key);
        const auto colon = uri.find(':');
        if (!player.can_control || uri.empty() || uri.size() > 16384 || uri.contains('\0') || colon == std::string::npos ||
            std::ranges::find(player.uri_schemes, uri.substr(0, colon)) == player.uri_schemes.end()) throw std::runtime_error("unsupported MPRIS URI");
        auto message = dbus::method(state_->bus, key.owner.c_str(), path, player_interface, "OpenUri");
        check(sd_bus_message_append(message.get(), "s", uri.c_str()));
        return state_->send_command(key, message.get());
    } catch (const std::exception& error) { return std::unexpected(error.what()); }
}

WaitSet MediaConnection::prepare_wait() {
    auto& state = *state_;
    WaitSet wait;
    wait.generation = ++state.generation;
    if (!state.bus) return wait;
    wait.deadline = state.deadline;
    dbus::prepare(state.bus, wait.fd, wait.deadline);
    if (state.dirty && !state.query && state.installed == 3 && (!wait.deadline || *state.refresh_at < *wait.deadline)) wait.deadline = state.refresh_at;
    if (state.more || state.result) wait.deadline = Clock::time_point::min();
    return wait;
}

Update MediaConnection::dispatch(const WaitSet& ready) {
    auto& state = *state_;
    if (ready.generation != state.generation || ready.fd.fd != (state.bus ? sd_bus_get_fd(state.bus) : -1))
        throw std::logic_error("dispatch requires the current MPRIS wait set");
    ++state.generation;
    const auto before = state.status;
    const auto revision = state.snapshot ? state.snapshot->revision : 0;
    if (!state.bus) return {};
    try {
        state.more = false;
        for (unsigned count = 0; count < 64; ++count) {
            const int result = check(sd_bus_process(state.bus, nullptr));
            if (state.callback_error) std::rethrow_exception(std::exchange(state.callback_error, {}));
            if (!result) break;
            state.more = count == 63;
        }
        const auto now = Clock::now();
        if (state.deadline && now >= *state.deadline) throw std::runtime_error("MPRIS synchronization timed out");
        if (state.dirty && !state.query && state.installed == 3 && now >= *state.refresh_at) {
            state.dirty = false; state.refresh_at.reset(); state.pending = {}; state.player = state.bytes = 0;
            state.step = State::Step::names;
            state.status = {Phase::synchronizing, {}};
            if (!state.deadline) state.deadline = now + std::chrono::seconds(10);
            state.send();
        }
    } catch (const std::exception& error) {
        if (state.command && !state.result) state.result = RequestResult{state.last_request, Outcome::failed, true, error.what()};
        state.disconnect(); state.status = {Phase::failed, error.what()};
    }
    return {before != state.status, revision != (state.snapshot ? state.snapshot->revision : 0), std::exchange(state.result, {})};
}

const Status& MediaConnection::status() const noexcept { return state_->status; }
const std::optional<Snapshot>& MediaConnection::snapshot() const noexcept { return state_->snapshot; }

}
