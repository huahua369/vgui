#include "delivery.h"
#include "collection_schema.h"
#include "delivery_hash.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <streambuf>
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
constexpr std::string_view package_format = "vgui-ui-json-delivery";
constexpr unsigned package_version = 1;
// Separate from the 4 MiB V1 design limit. 96 MiB accommodates up to 64 MiB
// of images after Base64 encoding, plus the escaped design and metadata.
constexpr std::size_t max_package_bytes = 96 * 1024 * 1024;
constexpr std::size_t max_embedded_image_bytes = 64 * 1024 * 1024;

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

fs::path inspect_artifact(const fs::path& root, const fs::path& path, fs::file_type type,
    std::string_view label)
{
    require_entry(path, type, label, true);
    auto resolved = canonical_path(path, label);
    if (!within(root, resolved) || resolved != path)
        reject(label, "delivery entry does not have its canonical path");
    if (type == fs::file_type::regular) {
        std::error_code ec;
        const auto count = fs::hard_link_count(resolved, ec);
        if (ec) reject(label, "could not inspect file links", ec);
        if (count != 1) reject(label, "delivery hard links are not allowed");
    }
    return resolved;
}

// Only call with a fixed name or an already-validated asset name, and with
// each parent within the bundle already checked (including the assets folder).
fs::path artifact_path(const fs::path& root, std::string_view relative, fs::file_type type)
{
    return inspect_artifact(root, root / relative, type, relative);
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

std::size_t checked_file_size(const fs::path& path, std::size_t limit, std::size_t remaining,
    std::string_view label)
{
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) reject(label, "could not determine file size", ec);
    check_size(size, limit, remaining, label);
    return static_cast<std::size_t>(size);
}

template<class Consume>
std::size_t read_bytes(const fs::path& path, std::size_t limit, std::size_t& remaining,
    std::string_view label, Consume consume)
{
    checked_file_size(path, limit, remaining, label);

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

// Validate references before removing font registrations, so neither a text
// family nor an image slot can silently lose a resource from the design.
void require_embeddable_references(const document& doc)
{
    const auto check_image = [&](const std::string& key, const std::string& path) {
        if (!key.empty() && is_font_path(doc.resources.at(key)))
            reject(path, "image reference '" + key + "' points to a project font file; use directory delivery instead");
    };
    for (const auto& [id, node] : doc.nodes) {
        for (const auto& [key, value] : node.props) {
            const auto* property = find_property(node.type, key);
            if (property->type == value_type::font_key) {
                const auto& family = std::get<std::string>(value);
                if (!family.empty() && !is_builtin_font(family))
                    reject("/nodes/" + id + "/props/" + key,
                        "standalone JSON does not embed project font '" + family + "'; use directory delivery instead");
            } else if (property->type == value_type::resource_key) {
                check_image(std::get<std::string>(value), "/nodes/" + id + "/props/" + key);
            } else if (property->type == value_type::items) {
                namespace collection = detail::collection_items;
                auto items = json::parse(std::get<std::string>(value)); // already validated against the item schema
                collection::visit_icons(items, "/nodes/" + id + "/props/" + key,
                    [&](const json& icon, const std::string& path) {
                        check_image(icon.get_ref<const std::string&>(), path);
                        return true;
                    });
            }
        }
    }
}

std::size_t base64_size(std::size_t bytes)
{
    return 4 * ((bytes + 2) / 3); // bytes has already been bounded
}

constexpr std::string_view base64_alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string to_base64(std::string_view bytes)
{
    std::string encoded;
    encoded.reserve(base64_size(bytes.size()));
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const auto left = bytes.size() - i;
        const unsigned a = static_cast<unsigned char>(bytes[i]);
        const unsigned b = left > 1 ? static_cast<unsigned char>(bytes[i + 1]) : 0;
        const unsigned c = left > 2 ? static_cast<unsigned char>(bytes[i + 2]) : 0;
        encoded += base64_alphabet[a >> 2];
        encoded += base64_alphabet[((a & 3) << 4) | (b >> 4)];
        encoded += left > 1 ? base64_alphabet[((b & 15) << 2) | (c >> 6)] : '=';
        encoded += left > 2 ? base64_alphabet[c & 63] : '=';
    }
    return encoded;
}

unsigned base64_digit(char ch, std::string_view path)
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    reject(path, "invalid Base64 character or padding");
}

std::vector<std::uint8_t> from_base64(std::string_view encoded, std::size_t bytes, std::string_view path)
{
    if (encoded.size() != base64_size(bytes)) reject(path, "Base64 length does not match image size");
    std::vector<std::uint8_t> decoded;
    decoded.reserve(bytes);
    for (std::size_t i = 0; i < encoded.size(); i += 4) {
        const auto left = bytes - decoded.size();
        const auto a = base64_digit(encoded[i], path), b = base64_digit(encoded[i + 1], path);
        decoded.push_back(static_cast<std::uint8_t>((a << 2) | (b >> 4)));
        if (left == 1) {
            if ((b & 15) || encoded[i + 2] != '=' || encoded[i + 3] != '=')
                reject(path, "non-canonical Base64 padding");
            break;
        }
        const auto c = base64_digit(encoded[i + 2], path);
        decoded.push_back(static_cast<std::uint8_t>(((b & 15) << 4) | (c >> 2)));
        if (left == 2) {
            if ((c & 3) || encoded[i + 3] != '=') reject(path, "non-canonical Base64 padding");
            break;
        }
        const auto d = base64_digit(encoded[i + 3], path);
        decoded.push_back(static_cast<std::uint8_t>(((c & 3) << 6) | d));
    }
    return decoded;
}

std::string sha256_text(std::string_view text)
{
    detail::sha256 hash;
    hash.update(text);
    return hash.hex_digest();
}

json image_record(std::size_t size, std::string_view hash, std::string data = {})
{
    return {{"base64", std::move(data)}, {"bytes", size}, {"sha256", hash}};
}

json package_json(std::string_view doc_text, json resources)
{
    // Store the canonical V1 JSON as text so we verify and hash the exact V1
    // bytes, with its own size and nesting limits, independently of the envelope.
    return {{"design", doc_text}, {"design_sha256", sha256_text(doc_text)}, {"format", package_format},
        {"resources", std::move(resources)}, {"version", package_version}};
}

struct planned_asset {
    std::string key;
    fs::path file;
    std::size_t bytes;
};

struct delivery_plan {
    std::string doc_text;
    std::vector<planned_asset> files;
    delivery_summary summary;
};

delivery_plan plan_delivery(const document& doc, const fs::path& project_root, bool embed_images)
{
    diagnostic issue;
    if (!validate(doc, &issue)) throw issue;
    const auto project_label = path_text(project_root);
    const auto project = canonical_path(directory_argument(project_root, project_label), project_label);
    require_entry(project, fs::file_type::directory, project_label);

    auto rewritten = doc;
    if (embed_images) {
        require_embeddable_references(doc);
        std::erase_if(rewritten.resources, [](const auto& entry) { return is_font_path(entry.second); });
    }
    std::size_t ordinal = 0;
    for (auto& [key, name] : rewritten.resources)
        name = asset_prefix(++ordinal) + portable_extension(name);

    delivery_plan plan;
    if (!encode_json(rewritten, plan.doc_text, &issue)) throw issue;
    auto remaining = embed_images ? max_embedded_image_bytes : max_delivery_bytes - plan.doc_text.size();
    plan.files.reserve(rewritten.resources.size());
    auto records = json::object();
    const detail::sha256 placeholder_hash;
    const auto placeholder_hex = placeholder_hash.hex_digest();
    for (const auto& [key, name] : rewritten.resources) {
        const auto label = "/resources/" + key;
        auto file = source_file(project, doc.resources.at(key), label);
        const auto bytes = checked_file_size(file, max_file_bytes, remaining, label);
        remaining -= bytes;
        plan.files.push_back({key, std::move(file), bytes});
        plan.summary.resource_bytes += bytes;
        if (embed_images) {
            plan.summary.base64_bytes += base64_size(bytes);
            records[key] = image_record(bytes, placeholder_hex);
        } else {
            records[key] = file_record(name, bytes, placeholder_hash);
        }
    }
    plan.summary.resource_count = plan.files.size();
    if (embed_images) {
        // Plain Base64 adds exactly its encoded length to the JSON string.
        const auto overhead = package_json(plan.doc_text, std::move(records)).dump().size() + 1;
        check_size(overhead + plan.summary.base64_bytes, max_package_bytes, max_package_bytes, "/resources");
    } else {
        const auto manifest = manifest_text(plan.doc_text, std::move(records));
        check_size(manifest.size(), max_json_bytes, remaining, delivery_manifest);
    }
    return plan;
}

fs::path package_file_argument(const fs::path& path, std::string_view label)
{
    const auto leaf = path.filename();
    if (path.empty() || leaf.empty() || leaf == "." || leaf == ".." || leaf.has_root_path() ||
        path.native().find(fs::path::value_type{}) != fs::path::string_type::npos ||
        path_text(leaf).find_first_of("\\:") != std::string::npos)
        reject(label, "expected a regular delivery file name");
    const auto parent = canonical_path(path.has_parent_path() ? path.parent_path() : fs::path("."), label);
    require_entry(parent, fs::file_type::directory, label);
    return parent / leaf;
}

void publish_package(const fs::path& target, const json& package, std::string_view label)
{
    // Prepare the entire file in a uniquely reserved sibling staging directory
    // before publishing it atomically without replacing an existing destination.
    static std::atomic<unsigned> sequence{0};
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto staging_path = target.parent_path() / (".vgui-package-" + std::to_string(nonce) + '-' +
        std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + ".tmp");
    new_directory staging(staging_path);
    const auto temp = staging_path / "package";
    std::ofstream out(temp, std::ios::binary);
    if (!out || !(out << package << '\n')) reject(label, "could not write temporary package");
    close_output(out, label);
#ifdef _WIN32
    if (!MoveFileExW(temp.c_str(), target.c_str(), 0))
        reject(label, "could not publish a new delivery file",
            {static_cast<int>(GetLastError()), std::system_category()});
#else
    // POSIX rename could overwrite target. Linking creates it exclusively;
    // the staging name is removed by the directory guard.
    std::error_code ec;
    fs::create_hard_link(temp, target, ec);
    if (ec) reject(label, "could not publish a new delivery file", ec);
#endif
}

json parse_package(std::string_view text)
{
    if (text.find('\0') != std::string_view::npos) reject({}, "raw NUL in JSON package");
    // The V1 design is a single string. The envelope is shallow, with one
    // fixed-size record per asset: bound malformed DOMs before allocating them.
    std::size_t events = 0;
    using event = json::parse_event_t;
    return json::parse(text.begin(), text.end(), [&](int depth, event kind, json& value) {
        if (++events > 16 + 10 * max_resources || depth > 3 ||
            (depth == 3 && (kind == event::object_start || kind == event::array_start)))
            reject({}, "JSON package structure limit exceeded");
        if (kind == event::key && value.get_ref<const std::string&>().size() > 128)
            reject({}, "JSON package member name is too long");
        if (kind == event::value && value.is_string()) {
            const auto size = value.get_ref<const std::string&>().size();
            if ((depth == 1 && size > max_json_bytes) || size > base64_size(max_file_bytes))
                reject({}, "JSON package string size limit exceeded");
        }
        return true;
    });
}

// Compare canonical output as it is written rather than allocating another
// full-sized string containing the embedded Base64 data.
class matching_streambuf : public std::streambuf {
public:
    explicit matching_streambuf(std::string_view text) : left_(text) {}
    bool done() const { return left_.empty(); }

private:
    std::streamsize xsputn(const char* chars, std::streamsize count) override
    {
        const std::string_view written(chars, static_cast<std::size_t>(count));
        if (!left_.starts_with(written)) return 0;
        left_.remove_prefix(written.size());
        return count;
    }
    int_type overflow(int_type ch) override
    {
        if (traits_type::eq_int_type(ch, traits_type::eof())) return traits_type::not_eof(ch);
        const auto byte = traits_type::to_char_type(ch);
        return xsputn(&byte, 1) ? ch : traits_type::eof();
    }
    std::string_view left_;
};

void require_canonical_package(const json& package, std::string_view text)
{
    matching_streambuf matching(text);
    std::ostream out(&matching);
    out << package << '\n';
    if (!out || !matching.done()) reject({}, "standalone delivery is not canonical JSON");
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

bool export_json_delivery(const document& doc, const fs::path& project_root,
    const fs::path& destination, diagnostic* error)
{
    return with_diagnostic(error, [&] {
        const auto label = path_text(destination);
        const auto target = package_file_argument(destination, label);
        const auto name = path_text(target.filename());
        if (name.size() <= json_delivery_suffix.size() || !name.ends_with(json_delivery_suffix))
            reject(label, "standalone delivery requires a new .vgui.package.json filename");
        std::error_code ec;
        if (fs::exists(fs::symlink_status(target, ec))) reject(label, "delivery destination already exists");
        if (ec && ec != std::errc::no_such_file_or_directory)
            reject(label, "could not inspect delivery destination", ec);

        const auto plan = plan_delivery(doc, project_root, true);
        auto images = json::object();
        auto remaining = max_embedded_image_bytes;
        for (const auto& asset : plan.files) {
            const auto resource = "/resources/" + asset.key;
            std::string bytes;
            bytes.reserve(asset.bytes);
            const auto size = read_bytes(asset.file, asset.bytes, remaining, resource,
                [&](std::string_view block) { bytes += block; });
            if (size != asset.bytes) reject(resource, "asset size changed while exporting");
            images[asset.key] = image_record(size, sha256_text(bytes), to_base64(bytes));
        }
        publish_package(target, package_json(plan.doc_text, std::move(images)), label);
    });
}

bool read_json_delivery(const fs::path& file, embedded_delivery& out, diagnostic* error)
{
    return with_diagnostic(error, [&] {
        const auto label = path_text(file);
        const auto target = package_file_argument(file, label);
        const auto path = inspect_artifact(target.parent_path(), target, fs::file_type::regular, label);
        const auto package = [&] {
            std::string text;
            auto remaining = max_package_bytes;
            text.reserve(checked_file_size(path, max_package_bytes, remaining, label));
            read_bytes(path, max_package_bytes, remaining, label, [&](std::string_view block) { text += block; });
            auto parsed = parse_package(text);
            require_canonical_package(parsed, text);
            return parsed;
        }();
        if (!package.is_object() || package.size() != 5 ||
            !package.contains("format") || !package.at("format").is_string() ||
            package.at("format").get_ref<const std::string&>() != package_format ||
            !package.contains("version") || !package.at("version").is_number_unsigned() ||
            package.at("version") != package_version ||
            !package.contains("design") || !package.at("design").is_string() ||
            !package.contains("design_sha256") || !package.at("design_sha256").is_string() ||
            !package.contains("resources") || !package.at("resources").is_object())
            reject(label, "invalid standalone delivery format, version or fields");

        const auto& doc_text = package.at("design").get_ref<const std::string&>();
        if (package.at("design_sha256").get_ref<const std::string&>() != sha256_text(doc_text))
            reject("/design_sha256", "standalone delivery design SHA-256 does not match");
        embedded_delivery candidate;
        diagnostic issue;
        if (!decode_json(doc_text, candidate.design, &issue))
            reject("/design" + issue.path, std::move(issue.message));
        std::string canonical;
        if (!encode_json(candidate.design, canonical, &issue))
            reject("/design" + issue.path, std::move(issue.message));
        if (doc_text != canonical) reject("/design", "document is not canonical V1 JSON");
        require_embeddable_references(candidate.design);
        const auto& images = package.at("resources");
        if (images.size() != candidate.design.resources.size())
            reject("/resources", "standalone delivery resources must match the design registry");

        std::size_t ordinal = 0, remaining = max_embedded_image_bytes;
        for (const auto& [key, name] : candidate.design.resources) {
            const auto resource = "/resources/" + key;
            // The path is metadata only: the reader never opens an asset path.
            if (!valid_asset_name(name, ++ordinal))
                reject("/design" + resource, "non-canonical delivery asset path");
            if (is_font_path(name))
                reject("/design" + resource, "standalone JSON does not accept project font assets");
            const auto it = images.find(key);
            if (it == images.end() || !it->is_object() || it->size() != 3 ||
                !it->contains("base64") || !it->at("base64").is_string() ||
                !it->contains("bytes") || !it->at("bytes").is_number_unsigned() ||
                !it->contains("sha256") || !it->at("sha256").is_string())
                reject(resource, "invalid standalone delivery image record");
            const auto size = it->at("bytes").get<json::number_unsigned_t>();
            check_size(size, max_file_bytes, remaining, resource);
            remaining -= static_cast<std::size_t>(size);
            auto bytes = from_base64(it->at("base64").get_ref<const std::string&>(),
                static_cast<std::size_t>(size), resource + "/base64");
            const auto bytes_view = bytes.empty() ? std::string_view{} :
                std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            if (it->at("sha256").get_ref<const std::string&>() != sha256_text(bytes_view))
                reject(resource + "/sha256", "standalone delivery image SHA-256 does not match");
            candidate.images.emplace(key, std::move(bytes));
        }
        out = std::move(candidate);
    });
}

bool summarize_delivery(const document& doc, const fs::path& project_root, bool embed_images,
    delivery_summary& out, diagnostic* error)
{
    return with_diagnostic(error, [&] { out = plan_delivery(doc, project_root, embed_images).summary; });
}

} // namespace vgui::ui_design
