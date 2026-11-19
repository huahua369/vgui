#pragma once

#include "model.h"
#include "ui_designer_font_file.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <ovg_c.h>

namespace vgui::ui_design {

struct imported_font {
    std::string relative_path; // Portable UTF-8; this is the only path to store in the document.
    std::filesystem::path copied_file; // Empty for existing files; otherwise owned by the caller for rollback.
};

namespace font_import_detail {
namespace fs = std::filesystem;

inline std::string path_text(const fs::path& path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

inline bool has_nul(const fs::path& path) {
    return path.native().find(fs::path::value_type{}) != fs::path::string_type::npos;
}

// Both names must satisfy the model's extension check before comparing types.
inline char font_type(const std::string& name) {
    const char c = name[name.size() - 3];
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

inline std::string resource_path(const fs::path& root, const fs::path& file) {
    const auto relative = file.lexically_relative(root);
    if (relative.empty() || relative.has_root_path() || *relative.begin() == "..") return {};
    auto text = path_text(relative);
    if (!is_font_path(text)) return {};
    // Use the actual model's portable resource-path rules (UTF-8, path length,
    // and component restrictions) so a successful result can be registered.
    auto doc = make_document();
    doc.resources.emplace("font", text);
    return validate(doc) ? text : std::string{};
}

inline bool identical(const fs::path& file, const std::vector<unsigned char>& bytes) {
    std::ifstream input(file, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() != static_cast<std::streamoff>(bytes.size()) || !input.seekg(0)) return false;
    std::array<char, 8192> block;
    for (std::size_t at = 0; at < bytes.size();) {
        const auto remaining = bytes.size() - at;
        const auto count = remaining < block.size() ? remaining : block.size();
        if (!input.read(block.data(), static_cast<std::streamsize>(count)) ||
            std::memcmp(block.data(), bytes.data() + at, count) != 0) return false;
        at += count;
    }
    return input.peek() == std::char_traits<char>::eof() && !input.bad();
}
} // namespace font_import_detail

// root is the canonical folder of a saved design. The caller owns document
// registration and must remove copied_file if that transaction is rejected.
// This assumes the project directories are not concurrently renamed/replaced.
inline std::optional<imported_font> import_project_font(const std::filesystem::path& root,
    const std::filesystem::path& source, std::string& error) {
    using namespace font_import_detail;
    error.clear();
    const auto fail = [&](const char* message) -> std::optional<imported_font> {
        error = message;
        return std::nullopt;
    };
    try {
        if (!root.is_absolute() || has_nul(root) || fs::canonical(root) != root || !fs::is_directory(root))
            return fail("A saved project folder is required.");
        if (has_nul(source)) return fail("Invalid font path.");
        const auto selected_name = path_text(source.filename());
        if (!is_font_path(selected_name)) return fail("Select a .ttf or .otf font.");
        const auto file = fs::canonical(source);
        const auto canonical_name = path_text(file.filename());
        if (!fs::is_regular_file(file) || !is_font_path(canonical_name) ||
            font_type(selected_name) != font_type(canonical_name))
            return fail("The selected path does not resolve to a font file of the same type.");

        std::vector<unsigned char> bytes;
        {
            std::ifstream input(file, std::ios::binary | std::ios::ate);
            if (!input) return fail("Could not read the font file.");
            const auto size = static_cast<std::streamoff>(input.tellg());
            if (size > static_cast<std::streamoff>(max_project_font_file)) return fail("The font file exceeds 32 MiB.");
            if (size <= 0) return fail("The font file is empty or unreadable.");
            bytes.resize(static_cast<std::size_t>(size));
            if (!input.seekg(0) || !input.read(reinterpret_cast<char*>(bytes.data()), size) ||
                input.peek() != std::char_traits<char>::eof() || input.bad())
                return fail("Could not read the complete font file.");
        }
        if (!valid_font_file(bytes)) return fail("The font file is invalid or incomplete.");
        {
            const std::unique_ptr<font_cache_cx, decltype(&free_font_cache)> cache(new_font_cache(), free_font_cache);
            if (!cache || !load_font_from_memory(cache.get(), bytes.data(), static_cast<long>(bytes.size()), "import"))
                return fail("Could not register the font file.");
        }

        const auto relative = file.lexically_relative(root);
        if (!relative.empty() && !relative.has_root_path() && *relative.begin() != "..") {
            auto text = resource_path(root, file);
            if (text.empty()) return fail("The font does not have a portable project-relative path.");
            return imported_font{std::move(text), {}};
        }

        const auto directory = root / "fonts";
        if (fs::symlink_status(directory).type() == fs::file_type::not_found) fs::create_directory(directory);
        // symlink_status rejects directory symlinks; canonical equality also
        // catches redirects reported as directories (e.g. Windows junctions).
        if (!fs::is_directory(fs::symlink_status(directory)) || fs::canonical(directory) != directory)
            return fail("The project fonts folder is not a local directory.");

        for (std::size_t suffix = 0;; ++suffix) {
            auto name = file.filename();
            if (suffix != 0) {
                name = file.stem();
                name += "-" + std::to_string(suffix);
                name += file.extension();
            }
            const auto target = directory / name;
            auto text = resource_path(root, target);
            if (text.empty()) return fail("The font does not have a portable project-relative path.");
            const auto status = fs::symlink_status(target);
            if (status.type() != fs::file_type::not_found) {
                // Never reuse a link, including a junction or another redirect.
                if (fs::is_regular_file(status) && fs::canonical(target) == target && identical(target, bytes))
                    return imported_font{std::move(text), {}};
                continue;
            }
            imported_font result{std::move(text), target};
            // Exclusive creation (C++23) cannot overwrite even a path that
            // appeared after the status check. Write the exact validated bytes.
            std::ofstream output(target, std::ios::binary | std::ios::noreplace);
            if (!output) {
                if (fs::symlink_status(target).type() != fs::file_type::not_found) continue;
                return fail("Could not create the project font file.");
            }
            output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            output.close();
            if (!output) {
                std::error_code ignored;
                fs::remove(target, ignored);
                return fail("Could not write the project font file.");
            }
            return result;
        }
    } catch (const std::system_error&) {
        // Leave OS/absolute paths out of the caller's localized status.
        return fail("Could not access the font or project folder.");
    }
}

} // namespace vgui::ui_design
