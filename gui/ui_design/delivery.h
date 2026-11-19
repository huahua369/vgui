#pragma once

#include "model.h"

namespace vgui::ui_design {

inline constexpr std::string_view delivery_document = "design.vgui.json";
inline constexpr std::string_view delivery_manifest = "resources.vgui.json";

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

} // namespace vgui::ui_design
