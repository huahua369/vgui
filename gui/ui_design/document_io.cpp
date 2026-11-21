#include "model.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <set>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace vgui::ui_design {
namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

bool fail(diagnostic* error, std::string path, std::string message)
{
    if (error) *error = {std::move(path), std::move(message)};
    return false;
}

[[noreturn]] void reject(std::string path, const char* message)
{
    throw diagnostic{std::move(path), message};
}

void append_token(std::string& path, std::string_view token)
{
    path += '/';
    for (char ch : token) {
        if (ch == '~') path += "~0";
        else if (ch == '/') path += "~1";
        else path += ch;
    }
}

std::string member_path(std::string path, std::string_view key)
{
    append_token(path, key);
    return path;
}

json parse_json(std::string_view text)
{
    struct frame {
        bool object;
        std::string token;
        std::set<std::string, std::less<>> keys{};
        std::string key{};
        std::size_t index = 0;
    };
    std::vector<frame> stack;
    stack.reserve(max_json_depth);

    // Abort on container start, before the parser adds it to the DOM. Check
    // decoded keys on arrival, before the DOM can overwrite an earlier value.
    using event = json::parse_event_t;
    return json::parse(text.begin(), text.end(), [&](int, event what, json& item) {
        switch (what) {
        case event::object_start:
        case event::array_start: {
            if (stack.size() >= max_json_depth) reject({}, "JSON nesting limit exceeded");
            std::string token;
            if (!stack.empty()) {
                auto& parent = stack.back();
                token = parent.object ? std::move(parent.key) : std::to_string(parent.index++);
            }
            stack.push_back({what == event::object_start, std::move(token)});
            break;
        }
        case event::key: {
            const auto& key = item.get_ref<const std::string&>();
            auto& current = stack.back();
            if (!current.keys.insert(key).second) {
                std::string path;
                for (std::size_t i = 1; i < stack.size(); ++i) append_token(path, stack[i].token);
                reject(member_path(std::move(path), key), "duplicate object member");
            }
            current.key = key;
            break;
        }
        case event::value:
            if (!stack.empty() && !stack.back().object) ++stack.back().index;
            break;
        case event::object_end:
        case event::array_end:
            stack.pop_back();
            break;
        }
        return true;
    });
}

void check_members(const json& object, const std::string& path,
    std::initializer_list<std::string_view> allowed)
{
    if (!object.is_object()) reject(path, "expected an object");
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (std::find(allowed.begin(), allowed.end(), it.key()) == allowed.end())
            reject(member_path(path, it.key()), "unknown member");
    }
}

const json& required(const json& object, const char* key, std::string_view path)
{
    const auto it = object.find(key);
    if (it == object.end()) reject(member_path(std::string(path), key), "missing required member");
    return *it;
}

const std::string& as_string(const json& input, std::string path)
{
    if (!input.is_string()) reject(std::move(path), "expected a string");
    return input.get_ref<const std::string&>();
}

value as_property(const json& input, std::string path)
{
    if (input.is_boolean()) return input.get<bool>();
    if (input.is_string()) return input.get_ref<const std::string&>();
    if (input.is_number()) {
        const auto number = input.get<double>();
        if (!std::isfinite(number)) reject(std::move(path), "expected a finite number");
        return number;
    }
    reject(std::move(path), "expected a boolean, number or string");
}

node decode_node(const json& input, const std::string& path)
{
    check_members(input, path, {"type", "name", "props", "children"});
    node result;
    result.type = as_string(required(input, "type", path), path + "/type");
    if (const auto name = input.find("name"); name != input.end())
        result.name = as_string(*name, path + "/name");
    if (const auto props = input.find("props"); props != input.end()) {
        const auto props_path = path + "/props";
        if (!props->is_object()) reject(props_path, "expected an object");
        for (auto it = props->begin(); it != props->end(); ++it) {
            const auto prop_path = member_path(props_path, it.key());
            const auto* desc = find_property(result.type, it.key());
            if (desc && (desc->type == value_type::items || desc->type == value_type::gradient_stops)) {
                if (!it->is_array()) reject(prop_path, desc->type == value_type::items ? "expected an item array" : "expected a color stop array");
                result.props.emplace(it.key(), it->dump());
            } else result.props.emplace(it.key(), as_property(*it, prop_path));
        }
    }
    if (const auto children = input.find("children"); children != input.end()) {
        const auto children_path = path + "/children";
        if (!children->is_array()) reject(children_path, "expected an array");
        result.children.reserve(children->size());
        for (const auto& child : *children)
            result.children.push_back(as_string(child,
                member_path(children_path, std::to_string(result.children.size()))));
    }
    return result;
}

document decode_document(const json& input)
{
    check_members(input, {}, {"format", "version", "root", "nodes", "resources"});
    if (as_string(required(input, "format", {}), "/format") != format)
        reject("/format", "unknown document format");
    const auto& file_version = required(input, "version", {});
    if (!file_version.is_number_integer() || file_version != version)
        reject("/version", "expected document version 1 (integer)");

    document result;
    result.root = as_string(required(input, "root", {}), "/root");
    const auto& nodes = required(input, "nodes", {});
    if (!nodes.is_object()) reject("/nodes", "expected an object");
    if (nodes.size() > max_nodes) reject("/nodes", "too many nodes");
    for (auto it = nodes.begin(); it != nodes.end(); ++it)
        result.nodes.emplace(it.key(), decode_node(*it, member_path("/nodes", it.key())));
    if (const auto resources = input.find("resources"); resources != input.end()) {
        if (!resources->is_object()) reject("/resources", "expected an object");
        if (resources->size() > max_resources) reject("/resources", "too many resources");
        for (auto it = resources->begin(); it != resources->end(); ++it)
            result.resources.emplace(it.key(), as_string(*it, member_path("/resources", it.key())));
    }
    return result;
}

bool valid_file_path(const fs::path& path)
{
    return !path.filename().empty() &&
        path.native().find(fs::path::value_type{}) == fs::path::string_type::npos;
}

#ifdef _WIN32
using file_handle = HANDLE;
const auto invalid_handle = INVALID_HANDLE_VALUE;
std::error_code last_file_error() { return {static_cast<int>(GetLastError()), std::system_category()}; }
#else
using file_handle = int;
constexpr file_handle invalid_handle = -1;
std::error_code last_file_error() { return {errno, std::generic_category()}; }
#endif

[[noreturn]] void file_error(const char* action)
{
    throw std::system_error(last_file_error(), action);
}

std::string temp_name(fs::path::value_type target_initial)
{
    static std::atomic<std::uint32_t> sequence{0};
#ifdef _WIN32
    const auto pid = static_cast<std::uint32_t>(GetCurrentProcessId());
#else
    const auto pid = static_cast<std::uint32_t>(::getpid());
#endif
    auto number = pid * 0x9e3779b9u + sequence.fetch_add(1, std::memory_order_relaxed);
    // Use an 8.3 name so Windows cannot add a short-name alias that is the
    // destination. Distinct initials also rule out case and trailing-dot aliases.
    std::string name = "V0000000.TMP";
    if (target_initial == 'V' || target_initial == 'v') name[0] = 'W';
    for (std::size_t i = 1; i < 8; ++i, number >>= 4)
        name[i] = "0123456789ABCDEF"[number & 15];
    return name;
}

class sibling_temp {
public:
    sibling_temp() = default;
    sibling_temp(const sibling_temp&) = delete;
    sibling_temp& operator=(const sibling_temp&) = delete;
    ~sibling_temp()
    {
#ifdef _WIN32
        if (handle_ != invalid_handle) CloseHandle(handle_);
        if (owned_) DeleteFileW(path_.c_str());
#else
        if (handle_ != invalid_handle) ::close(handle_);
        if (owned_) ::unlink(path_.c_str());
#endif
    }

    void create(const fs::path& destination)
    {
        const auto directory = destination.parent_path();
        const auto filename = destination.filename().native();
        const auto first = filename.find_first_not_of(fs::path::value_type{' '});
        const auto initial = first == fs::path::string_type::npos ? fs::path::value_type{} : filename[first];
        for (int attempt = 0; attempt < 128; ++attempt) {
            path_ = directory / temp_name(initial);
#ifdef _WIN32
            handle_ = CreateFileW(path_.c_str(), GENERIC_WRITE, 0, nullptr,
                CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle_ == invalid_handle) {
                const auto code = last_file_error();
                if (code.value() == ERROR_FILE_EXISTS || code.value() == ERROR_ALREADY_EXISTS) continue;
                throw std::system_error(code, "could not create temporary document");
            }
#else
            do {
                handle_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            } while (handle_ == invalid_handle && errno == EINTR);
            if (handle_ == invalid_handle) {
                if (errno == EEXIST) continue;
                file_error("could not create temporary document");
            }
#endif
            owned_ = true;
            return;
        }
        throw std::system_error(std::make_error_code(std::errc::file_exists),
            "could not reserve a unique temporary document");
    }

    void write_and_close(std::string_view text)
    {
        while (!text.empty()) {
#ifdef _WIN32
            DWORD count = 0;
            if (!WriteFile(handle_, text.data(), static_cast<DWORD>(text.size()), &count, nullptr))
                file_error("could not write temporary document");
#else
            const auto count = ::write(handle_, text.data(), text.size());
            if (count < 0) {
                if (errno == EINTR) continue;
                file_error("could not write temporary document");
            }
#endif
            if (!count) throw std::system_error(std::make_error_code(std::errc::io_error),
                "short write to temporary document");
            text.remove_prefix(static_cast<std::size_t>(count));
        }
#ifdef _WIN32
        if (!FlushFileBuffers(handle_)) file_error("could not flush temporary document");
        if (!CloseHandle(handle_)) file_error("could not close temporary document");
        handle_ = invalid_handle;
#else
        int status;
        do { status = ::fsync(handle_); } while (status < 0 && errno == EINTR);
        if (status < 0) file_error("could not flush temporary document");
        // Do not retry close: on some systems the descriptor is already released.
        if (::close(std::exchange(handle_, invalid_handle)) < 0)
            file_error("could not close temporary document");
#endif
    }

    void replace(const fs::path& destination)
    {
#ifdef _WIN32
        if (!MoveFileExW(path_.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING))
#else
        if (::rename(path_.c_str(), destination.c_str()) != 0)
#endif
            file_error("could not replace document");
        owned_ = false;
    }

private:
    fs::path path_;
    file_handle handle_ = invalid_handle;
    bool owned_ = false;
};

} // namespace

bool decode_json(std::string_view text, document& out, diagnostic* error)
{
    if (text.size() > max_json_bytes) return fail(error, {}, "JSON size limit exceeded");
    if (text.empty()) return fail(error, {}, "empty JSON input");
    // nlohmann's lexer can treat a raw NUL as EOF, including after a valid value.
    if (text.find('\0') != std::string_view::npos) return fail(error, {}, "raw NUL in JSON input");
    try {
        auto candidate = decode_document(parse_json(text));
        if (!validate(candidate, error)) return false;
        out = std::move(candidate);
        return true;
    } catch (diagnostic& issue) {
        return fail(error, std::move(issue.path), std::move(issue.message));
    } catch (const json::exception& issue) {
        return fail(error, {}, issue.what());
    }
}

std::string format_items(std::string_view input)
{
    return json::parse(input.begin(), input.end()).dump(2);
}

bool encode_json(const document& doc, std::string& out, diagnostic* error)
{
    if (!validate(doc, error)) return false;
    try {
        json nodes = json::object();
        for (const auto& [id, item] : doc.nodes) {
            json props = json::object();
            for (const auto& [key, prop] : item.props) {
                const auto kind = find_property(item.type, key)->type;
                if (kind == value_type::items || kind == value_type::gradient_stops) {
                    const auto& text = std::get<std::string>(prop);
                    props[key] = json::parse(text.begin(), text.end());
                } else std::visit([&](const auto& data) { props[key] = data; }, prop);
            }
            nodes[id] = {{"type", item.type}, {"name", item.name},
                {"props", std::move(props)}, {"children", item.children}};
        }
        const json root = {{"format", format}, {"version", version}, {"root", doc.root},
            {"nodes", std::move(nodes)}, {"resources", doc.resources}};
        auto text = root.dump(2);
        if (text.size() >= max_json_bytes) return fail(error, {}, "JSON size limit exceeded");
        text += '\n';
        out = std::move(text);
        return true;
    } catch (const json::exception& issue) {
        return fail(error, {}, issue.what());
    }
}

bool read_document(const fs::path& path, document& out, diagnostic* error)
{
    if (!valid_file_path(path)) return fail(error, {}, "invalid document path");
    std::ifstream input(path, std::ios::binary);
    if (!input) return fail(error, {}, "could not open document for reading");
    std::string text;
    char buffer[8192];
    while (input) {
        const auto remaining = max_json_bytes - text.size();
        input.read(buffer, static_cast<std::streamsize>(std::min(sizeof(buffer), remaining + 1)));
        const auto count = static_cast<std::size_t>(input.gcount());
        if (count > remaining) return fail(error, {}, "JSON size limit exceeded");
        text.append(buffer, count);
    }
    if (input.bad() || !input.eof()) return fail(error, {}, "could not read document");
    return decode_json(text, out, error);
}

bool write_document(const fs::path& path, const document& doc, diagnostic* error)
{
    if (!valid_file_path(path)) return fail(error, {}, "invalid document path");
    std::string text;
    if (!encode_json(doc, text, error)) return false;
    try {
        const auto destination = fs::absolute(path);
        sibling_temp temp;
        temp.create(destination);
        temp.write_and_close(text);
        temp.replace(destination);
        return true;
    } catch (const std::system_error& issue) {
        return fail(error, {}, issue.what());
    }
}

} // namespace vgui::ui_design
