#include "workspace_protocol.hpp"

#include <json-c/json.h>

#include <algorithm>
#include <array>
#include <memory>
#include <new>

namespace hyprworks::hyprland::protocol {
namespace {

using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
using Tokener = std::unique_ptr<json_tokener, decltype(&json_tokener_free)>;

[[noreturn]] void invalid(std::string message, ErrorCode code = ErrorCode::protocol) {
    throw Error{code, "snapshot", std::move(message)};
}

json_object* member(json_object* object, const char* name) {
    json_object* value = nullptr;
    if (!json_object_is_type(object, json_type_object) || !json_object_object_get_ex(object, name, &value))
        invalid(std::string("missing field: ") + name);
    return value;
}

std::string text(json_object* object, const char* name) {
    auto* value = member(object, name);
    if (!json_object_is_type(value, json_type_string))
        invalid(std::string("expected string: ") + name);
    return {json_object_get_string(value), static_cast<std::size_t>(json_object_get_string_len(value))};
}

std::uint64_t integer(json_object* value, std::uint64_t maximum, const char* name) {
    if (!json_object_is_type(value, json_type_int) || json_object_get_int64(value) < 0 || json_object_get_uint64(value) > maximum)
        invalid(std::string("invalid integer: ") + name);
    return json_object_get_uint64(value);
}

bool boolean(json_object* object, const char* name) {
    auto* value = member(object, name);
    if (!json_object_is_type(value, json_type_boolean))
        invalid(std::string("expected boolean: ") + name);
    return json_object_get_boolean(value) != 0;
}

Workspace identity(json_object* object) {
    Workspace workspace;
    workspace.key.address = text(object, "address");
    workspace.name = text(object, "name");
    const auto type = text(object, "type");
    if (type == "normal")
        workspace.key.type = WorkspaceType::normal;
    else if (type == "special")
        workspace.key.type = WorkspaceType::special;
    else
        invalid("unsupported workspace type: " + type);
    if (workspace.key.address.empty() || workspace.key.address.contains('\0'))
        invalid("empty or NUL-containing workspace address");

    json_object* number = nullptr;
    if (json_object_object_get_ex(object, "id", &number)) {
        const auto id = integer(number, UINT32_MAX, "workspace id");
        if (id == 0 || workspace.key.type != WorkspaceType::normal || workspace.key.address != std::to_string(id))
            invalid("workspace number does not match its identity");
        workspace.number = static_cast<std::uint32_t>(id);
    }
    if (workspace.key.type == WorkspaceType::special &&
        (!workspace.key.address.starts_with("special:") || workspace.key.address.size() == 8))
        invalid("invalid special workspace address");
    if (workspace.key.type == WorkspaceType::normal && !workspace.number &&
        workspace.key.address.find_first_not_of("0123456789") == std::string::npos)
        invalid("numbered workspace is missing its number");
    return workspace;
}

std::optional<WorkspaceKey> reference(json_object* object, const char* name, WorkspaceType type) {
    auto* value = member(object, name);
    if (text(value, "address").empty()) {
        if (!text(value, "type").empty() || !text(value, "name").empty())
            invalid("inconsistent empty workspace reference");
        return std::nullopt;
    }
    auto workspace = identity(value);
    if (workspace.key.type != type)
        invalid("workspace reference has the wrong type");
    return std::move(workspace.key);
}

Json array(std::string_view input) {
    Tokener tokener(json_tokener_new_ex(32), json_tokener_free);
    if (!tokener)
        throw std::bad_alloc{};
    json_tokener_set_flags(tokener.get(), JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    Json root(json_tokener_parse_ex(tokener.get(), input.data(), static_cast<int>(input.size())), json_object_put);
    const auto error = json_tokener_get_error(tokener.get());
    if (error != json_tokener_success)
        invalid(json_tokener_error_desc(error), error == json_tokener_error_depth ? ErrorCode::limit_exceeded : ErrorCode::protocol);
    if (json_tokener_get_parse_end(tokener.get()) != input.size() || !json_object_is_type(root.get(), json_type_array))
        invalid("expected a complete JSON array");
    return root;
}

}

std::expected<Snapshot, Error> parse_snapshot(std::string_view reply) {
    try {
        constexpr std::string_view delimiter = "\n\n\n";
        const auto split = reply.find(delimiter);
        if (split == std::string_view::npos || reply.find(delimiter, split + delimiter.size()) != std::string_view::npos)
            invalid("expected two batch replies separated by three newlines");
        auto workspaces = array(reply.substr(0, split));
        auto outputs = array(reply.substr(split + delimiter.size()));
        const auto workspace_count = json_object_array_length(workspaces.get());
        const auto output_count = json_object_array_length(outputs.get());
        if (workspace_count > 4096 || output_count > 256)
            invalid("snapshot exceeds record limit", ErrorCode::limit_exceeded);

        Snapshot snapshot;
        snapshot.workspaces.reserve(workspace_count);
        snapshot.outputs.reserve(output_count);
        for (std::size_t i = 0; i < workspace_count; ++i) {
            auto* object = json_object_array_get_idx(workspaces.get(), i);
            auto workspace = identity(object);
            if (auto* output = member(object, "monitorID"))
                workspace.output_id = static_cast<std::int64_t>(integer(output, INT64_MAX, "monitorID"));
            snapshot.workspaces.push_back(std::move(workspace));
        }
        for (std::size_t i = 0; i < output_count; ++i) {
            auto* object = json_object_array_get_idx(outputs.get(), i);
            Output output;
            output.id = static_cast<std::int64_t>(integer(member(object, "id"), INT64_MAX, "output id"));
            output.name = text(object, "name");
            output.enabled = !boolean(object, "disabled");
            output.active_workspace = reference(object, "activeWorkspace", WorkspaceType::normal);
            output.special_workspace = reference(object, "specialWorkspace", WorkspaceType::special);
            if (boolean(object, "focused")) {
                if (snapshot.focused_output)
                    invalid("multiple focused outputs");
                snapshot.focused_output = output.id;
            }
            snapshot.outputs.push_back(std::move(output));
        }
        std::ranges::sort(snapshot.workspaces, {}, &Workspace::key);
        std::ranges::sort(snapshot.outputs, {}, &Output::id);
        for (std::size_t i = 0; i < snapshot.workspaces.size(); ++i) {
            const auto& workspace = snapshot.workspaces[i];
            if (i && workspace.key == snapshot.workspaces[i - 1].key)
                invalid("duplicate workspace key");
            if (workspace.output_id && !std::ranges::binary_search(snapshot.outputs, *workspace.output_id, {}, &Output::id))
                invalid("workspace references an unknown output");
        }
        for (std::size_t i = 0; i < snapshot.outputs.size(); ++i) {
            const auto& output = snapshot.outputs[i];
            if (i && output.id == snapshot.outputs[i - 1].id)
                invalid("duplicate output id");
            for (const auto* key : {&output.active_workspace, &output.special_workspace}) {
                if (!*key)
                    continue;
                const auto workspace = std::ranges::lower_bound(snapshot.workspaces, **key, {}, &Workspace::key);
                if (workspace == snapshot.workspaces.end() || workspace->key != **key || workspace->output_id != output.id)
                    invalid("active workspace reference does not match workspace ownership");
            }
        }
        return snapshot;
    } catch (const Error& error) {
        return std::unexpected(error);
    }
}

std::expected<std::string, Error> encode_switch(const Workspace& workspace) {
    std::string command = "/dispatch hl.dsp.focus({workspace=\"";
    if (workspace.key.type == WorkspaceType::normal && !workspace.number)
        command += "name:";
    for (const char character : workspace.key.address) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte == '"' || byte == '\\') {
            command += '\\';
            command += static_cast<char>(byte);
        } else if (byte < 32 || byte == 127) {
            command += '\\';
            command += static_cast<char>('0' + byte / 100);
            command += static_cast<char>('0' + byte / 10 % 10);
            command += static_cast<char>('0' + byte % 10);
        } else {
            command += static_cast<char>(byte);
        }
        if (command.size() + 3 > max_command_bytes)
            return std::unexpected(Error{ErrorCode::limit_exceeded, "switch", "encoded command exceeds byte limit"});
    }
    command += "\"})";
    return command;
}

std::expected<bool, Error> invalidates_workspaces(std::string_view line) {
    const auto separator = line.find(">>");
    if (separator == std::string_view::npos || separator == 0 || line.contains('\0'))
        return std::unexpected(Error{ErrorCode::protocol, "events", "malformed event line"});
    constexpr std::array names{
        "workspacev2", "focusedmonv2", "createworkspacev2", "destroyworkspacev2",
        "renameworkspace", "changeworkspaceid", "moveworkspacev2",
        "activespecialv2", "monitoraddedv2", "monitorremovedv2", "configreloaded"
    };
    return std::ranges::find(names, line.substr(0, separator)) != names.end();
}

}
