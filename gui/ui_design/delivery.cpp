#include "delivery.h"
#include "delivery_hash.h"

#include <algorithm>
#include <array>
#include <fstream>
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
#endif

namespace vgui::ui_design {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr std::string_view assets = "assets";
constexpr std::size_t max_file_bytes = 64 * 1024 * 1024;
constexpr std::size_t max_delivery_bytes = 512 * 1024 * 1024;
constexpr std::size_t max_extension_bytes = 16; // not counting the dot

[[noreturn]] void reject(std::string_view path, std::string message)
{
    throw diagnostic{std::string(path), std::move(message)};
}

[[noreturn]] void reject(std::string_view path, std::string message, const std::error_code& ec)
{
    if (ec) message += ": " + ec.message();
    reject(path, std::move(message));
}

template<class Operation>
bool with_diagnostic(diagnostic* error, Operation operation)
{
    try {
        operation();
        if (error) *error = {};
        return true;
    } catch (diagnostic& issue) {
        if (error) *error = std::move(issue);
    } catch (const std::system_error& issue) {
        if (error) *error = {{}, issue.what()};
    } catch (const json::exception& issue) {
        if (error) *error = {{}, issue.what()};
    }
    return false;
}

std::string path_text(const fs::path& path)
{
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

fs::path directory_argument(fs::path path, std::string_view label)
{
    if (path.empty() || path.native().find(fs::path::value_type{}) != fs::path::string_type::npos)
        reject(label, "invalid directory path");
    // Check the directory entry itself even when the caller supplies a trailing slash.
    while (path.has_relative_path() && path.filename().empty()) path = path.parent_path();
    return path;
}

fs::path canonical_path(const fs::path& path, std::string_view label)
{
    std::error_code ec;
    auto result = fs::canonical(path, ec);
    if (ec) reject(label, "could not resolve path", ec);
    return result;
}

bool within(const fs::path& root, const fs::path& path)
{
    auto part = path.begin();
    for (const auto& component : root) {
        if (part == path.end() || *part != component) return false;
        ++part;
    }
    return true;
}

void require_entry(const fs::path& path, fs::file_type type, std::string_view label,
    bool reject_links = false)
{
    std::error_code ec;
    const auto status = reject_links ? fs::symlink_status(path, ec) : fs::status(path, ec);
    if (ec) reject(label, "could not inspect path", ec);
    if (reject_links && fs::is_symlink(status)) reject(label, "delivery links are not allowed");
#ifdef _WIN32
    // A directory junction may not be classified as a symlink by the STL.
    if (reject_links) {
        const auto attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
            reject(label, "could not inspect file attributes",
                {static_cast<int>(GetLastError()), std::system_category()});
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
            reject(label, "delivery reparse points are not allowed");
    }
#endif
    if (status.type() != type)
        reject(label, type == fs::file_type::directory ? "expected a directory" : "expected a regular file");
}

fs::path source_file(const fs::path& root, const std::string& relative, std::string_view label)
{
    // U0 has validated the project-relative UTF-8 path.
    const fs::path path(std::u8string(relative.begin(), relative.end()));
    auto resolved = root;
    for (auto it = path.begin(); it != path.end();) {
        resolved = canonical_path(resolved / *it++, label);
        const bool file = it == path.end();
        // Check intermediate directory links too, not just the final target.
        if (!within(root, resolved) || (file && resolved == root))
            reject(label, "asset path leaves the project root: " + relative);
        require_entry(resolved, file ? fs::file_type::regular : fs::file_type::directory, label);
    }
    return resolved;
}

// Only call with a fixed name or an already-validated asset name, and with
// each parent within the bundle already checked (including the assets folder).
fs::path artifact_path(const fs::path& root, std::string_view relative, fs::file_type type)
{
    const auto path = root / relative;
    require_entry(path, type, relative, true);
    auto resolved = canonical_path(path, relative);
    if (!within(root, resolved) || resolved != path)
        reject(relative, "delivery entry does not have its canonical path");
    if (type == fs::file_type::regular) {
        std::error_code ec;
        const auto count = fs::hard_link_count(resolved, ec);
        if (ec) reject(relative, "could not inspect file links", ec);
        if (count != 1) reject(relative, "delivery hard links are not allowed");
    }
    return resolved;
}

class new_directory {
public:
    explicit new_directory(fs::path path) : path_(std::move(path))
    {
        std::error_code ec;
        if (!fs::create_directory(path_, ec))
            reject(path_text(path_), "could not create a new delivery directory", ec);
    }
    new_directory(const new_directory&) = delete;
    new_directory& operator=(const new_directory&) = delete;
    ~new_directory()
    {
        if (owned_) {
            std::error_code ignored;
            fs::remove_all(path_, ignored);
        }
    }
    void keep() { owned_ = false; }

private:
    fs::path path_;
    bool owned_ = true;
};

bool lower_or_digit(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

std::string portable_extension(std::string_view path)
{
    const auto slash = path.rfind('/');
    if (slash != std::string_view::npos) path.remove_prefix(slash + 1);
    const auto dot = path.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || path.size() - dot - 1 > max_extension_bytes)
        return "bin";
    std::string extension(path.substr(dot + 1));
    for (auto& c : extension) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (!lower_or_digit(c)) return "bin";
    }
    return extension.empty() ? "bin" : extension;
}

std::string asset_prefix(std::size_t ordinal)
{
    return std::string(assets) + '/' + std::to_string(ordinal) + '.';
}

bool valid_asset_name(std::string_view name, std::size_t ordinal)
{
    const auto prefix = asset_prefix(ordinal);
    if (!name.starts_with(prefix)) return false;
    name.remove_prefix(prefix.size());
    return !name.empty() && name.size() <= max_extension_bytes &&
        std::all_of(name.begin(), name.end(), lower_or_digit);
}

void check_size(std::uintmax_t size, std::size_t limit, std::size_t remaining, std::string_view path)
{
    if (size > limit) reject(path, "delivery file size limit exceeded");
    if (size > remaining) reject(path, "total delivery size limit exceeded");
}

template<class Consume>
std::size_t read_bytes(const fs::path& path, std::size_t limit, std::size_t& remaining,
    std::string_view label, Consume consume)
{
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) reject(label, "could not determine file size", ec);
    check_size(size, limit, remaining, label);

    std::ifstream in(path, std::ios::binary);
    if (!in) reject(label, "could not open file for reading");
    std::array<char, 64 * 1024> buffer;
    std::size_t total = 0;
    for (;;) {
        const auto room = std::min(limit - total, remaining);
        in.read(buffer.data(), static_cast<std::streamsize>(std::min(buffer.size(), room + 1)));
        if (in.bad() || (in.fail() && !in.eof())) reject(label, "could not read file");
        const auto count = static_cast<std::size_t>(in.gcount());
        check_size(count, limit - total, remaining, label);
        if (count) consume(std::string_view(buffer.data(), count));
        total += count;
        remaining -= count;
        if (in.eof()) return total;
    }
}

std::string read_text(const fs::path& path, std::size_t& remaining, std::string_view label)
{
    std::string text;
    read_bytes(path, max_json_bytes, remaining, label, [&](std::string_view bytes) { text += bytes; });
    return text;
}

void write_bytes(std::ofstream& out, std::string_view bytes, std::string_view label)
{
    if (!out.write(bytes.data(), static_cast<std::streamsize>(bytes.size())))
        reject(label, "could not write file");
}

void close_output(std::ofstream& out, std::string_view label)
{
    out.close();
    if (!out) reject(label, "could not close output file");
}

void write_text(const fs::path& root, std::string_view label, std::string_view text, std::size_t& remaining)
{
    check_size(text.size(), max_json_bytes, remaining, label);
    std::ofstream out(root / label, std::ios::binary);
    if (!out) reject(label, "could not open file for writing");
    write_bytes(out, text, label);
    close_output(out, label);
    remaining -= text.size();
}

json file_record(std::string_view label, std::size_t bytes, const detail::sha256& hash)
{
    return {{"path", label}, {"bytes", bytes}, {"sha256", hash.hex_digest()}};
}

std::string manifest_text(std::string_view doc_text, json resources)
{
    detail::sha256 hash;
    hash.update(doc_text);
    const json manifest = {{"format", "vgui-ui-delivery"}, {"version", 1},
        {"document", file_record(delivery_document, doc_text.size(), hash)},
        {"resources", std::move(resources)}};
    return manifest.dump(2) + '\n';
}

} // namespace

bool export_delivery(const document& doc, const fs::path& project_root,
    const fs::path& destination, diagnostic* error)
{
    return with_diagnostic(error, [&] {
        diagnostic issue;
        if (!validate(doc, &issue)) throw issue;
        const auto project_label = path_text(project_root);
        const auto project = canonical_path(directory_argument(project_root, project_label), project_label);
        require_entry(project, fs::file_type::directory, project_label);

        auto rewritten = doc;
        std::size_t ordinal = 0;
        for (auto& [key, name] : rewritten.resources)
            name = asset_prefix(++ordinal) + portable_extension(name);
        std::string doc_text;
        if (!encode_json(rewritten, doc_text, &issue)) throw issue;

        const auto destination_label = path_text(destination);
        const auto target = directory_argument(destination, destination_label);
        const auto leaf = target.filename();
        if (leaf.empty() || leaf == "." || leaf == ".." || leaf.has_root_path())
            reject(destination_label, "expected a new delivery directory name");
        const auto parent = canonical_path(target.has_parent_path() ? target.parent_path() : fs::path("."),
            destination_label);
        require_entry(parent, fs::file_type::directory, destination_label);
        const auto directory = parent / leaf;
        new_directory output(directory);
        const auto root = canonical_path(directory, destination_label);
        if (!within(parent, root) || root != directory)
            reject(destination_label, "delivery directory does not have its canonical path");
        if (!rewritten.resources.empty()) {
            std::error_code ec;
            if (!fs::create_directory(root / assets, ec)) reject(assets, "could not create assets directory", ec);
            artifact_path(root, assets, fs::file_type::directory);
        }

        auto remaining = max_delivery_bytes;
        write_text(root, delivery_document, doc_text, remaining);
        auto resources = json::object();
        for (const auto& [key, name] : rewritten.resources) {
            const auto label = "/resources/" + key;
            const auto source = source_file(project, doc.resources.at(key), label);
            std::ofstream out(root / name, std::ios::binary);
            if (!out) reject(name, "could not open asset for writing");
            detail::sha256 hash;
            const auto bytes = read_bytes(source, max_file_bytes, remaining, label, [&](std::string_view block) {
                write_bytes(out, block, name);
                hash.update(block);
            });
            close_output(out, name);
            resources[key] = file_record(name, bytes, hash);
        }
        write_text(root, delivery_manifest, manifest_text(doc_text, std::move(resources)), remaining);
        output.keep();
    });
}

bool read_delivery(const fs::path& directory, document& out, diagnostic* error)
{
    return with_diagnostic(error, [&] {
        const auto label = path_text(directory);
        auto requested = directory_argument(directory, label);
        while (requested.has_parent_path() && requested.filename() == ".")
            requested = directory_argument(requested.parent_path(), label);
        require_entry(requested, fs::file_type::directory, label, true);
        const auto root = canonical_path(requested, label);
        const auto design_path = artifact_path(root, delivery_document, fs::file_type::regular);
        const auto manifest_path = artifact_path(root, delivery_manifest, fs::file_type::regular);

        auto remaining = max_delivery_bytes;
        const auto doc_text = read_text(design_path, remaining, delivery_document);
        document candidate;
        diagnostic issue;
        if (!decode_json(doc_text, candidate, &issue))
            reject(std::string(delivery_document) + issue.path, std::move(issue.message));
        std::string canonical;
        if (!encode_json(candidate, canonical, &issue))
            reject(std::string(delivery_document) + issue.path, std::move(issue.message));
        if (doc_text != canonical) reject(delivery_document, "document is not canonical V1 JSON");
        if (!candidate.resources.empty()) artifact_path(root, assets, fs::file_type::directory);

        auto resources = json::object();
        std::size_t ordinal = 0;
        for (const auto& [key, name] : candidate.resources) {
            if (!valid_asset_name(name, ++ordinal))
                reject(std::string(delivery_document) + "/resources/" + key, "non-canonical delivery asset path");
            const auto file = artifact_path(root, name, fs::file_type::regular);
            detail::sha256 hash;
            const auto bytes = read_bytes(file, max_file_bytes, remaining, name,
                [&](std::string_view block) { hash.update(block); });
            resources[key] = file_record(name, bytes, hash);
        }
        // Never parse or trust paths or sizes from the supplied manifest.
        const auto expected = manifest_text(doc_text, std::move(resources));
        if (read_text(manifest_path, remaining, delivery_manifest) != expected)
            reject(delivery_manifest, "manifest does not match the canonical file list, sizes and SHA-256 hashes");
        out = std::move(candidate);
    });
}

} // namespace vgui::ui_design
