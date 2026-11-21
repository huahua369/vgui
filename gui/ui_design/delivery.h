#pragma once

#include "model.h"

namespace vgui::ui_design {

inline constexpr std::string_view delivery_document = "design.vgui.json";
inline constexpr std::string_view delivery_manifest = "resources.vgui.json";
inline constexpr std::string_view json_delivery_suffix = ".vgui.package.json";

// The destination must not exist; its parent must already be a directory.
// Keep the input and output directory trees stable during these operations.
// Path checks do not defend against concurrent replacement of files or links.
bool export_delivery(const document& doc, const std::filesystem::path& project_root,
    const std::filesystem::path& destination, diagnostic* error = nullptr);

// Verifies the canonical document and manifest and the bytes of every listed
// asset. Only a successful verification replaces out. This is an integrity
// check, not authentication of the bundle's origin.
bool read_delivery(const std::filesystem::path& directory, document& out,
    diagnostic* error = nullptr);

struct embedded_delivery {
    document design;
    // Keyed by the resource ID in design, not its portable path.
    std::map<std::string, std::vector<std::uint8_t>, std::less<>> images;
};

// Standalone delivery preserves all non-font entries in the resource registry.
// Project font references require directory delivery. The destination must not
// exist and the project files must remain stable while exporting.
bool export_json_delivery(const document& doc, const std::filesystem::path& project_root,
    const std::filesystem::path& destination, diagnostic* error = nullptr);

// Verifies the package before replacing out. SHA-256 checks integrity, not
// origin. No files are extracted.
bool read_json_delivery(const std::filesystem::path& file, embedded_delivery& out,
    diagnostic* error = nullptr);

struct delivery_summary {
    std::size_t resource_count = 0, resource_bytes = 0, base64_bytes = 0;
};

// Checks the selected sources without reading asset contents. base64_bytes
// counts encoded image data only and is zero for a directory package.
bool summarize_delivery(const document& doc, const std::filesystem::path& project_root,
    bool embed_images, delivery_summary& out, diagnostic* error = nullptr);

} // namespace vgui::ui_design
