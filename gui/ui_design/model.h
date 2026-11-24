#pragma once

// Internal UI designer model. No widget handles or platform dependencies.
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace vgui::ui_design {

inline constexpr std::string_view format = "vgui-ui-design";
inline constexpr int version = 1;
inline constexpr std::size_t max_json_bytes = 4 * 1024 * 1024;
inline constexpr std::size_t max_json_depth = 32;
inline constexpr std::size_t max_nodes = 4096;
inline constexpr std::size_t max_tree_depth = 64;
inline constexpr std::size_t max_resources = 4096;
inline constexpr std::size_t append = static_cast<std::size_t>(-1);

struct diagnostic {
    std::string path; // JSON Pointer, or empty for a file/parse error.
    std::string message;
};

using value = std::variant<bool, double, std::string>;
enum class value_type { boolean, number, text, choice, resource_key, action_key, localization_key, color, font_key, items, gradient_stops, gradient_keyframes, data_key };
// Array-valued properties hold JSON text internally; page files use arrays.
inline constexpr std::size_t max_gradient_stops = 32;
struct gradient_stop {
    float offset;
    std::string color;
    bool operator==(const gradient_stop&) const = default;
};
bool decode_gradient_stops(std::string_view input, std::vector<gradient_stop>& out, std::string& error);
std::string encode_gradient_stops(std::span<const gradient_stop> stops);

inline constexpr std::size_t max_gradient_keyframes = 128;
inline constexpr float gradient_stop_min_gap = 1e-4f;
inline constexpr double gradient_keyframe_min_gap = 1e-6;

enum class gradient_geometry_axis {
    start_x, start_y, end_x, end_y, center_x, center_y, outer_x, outer_y, start_angle, end_angle,
    inner_radius, outer_radius, count
};

namespace detail {
inline constexpr double gradient_limit = 16.0;
}

struct gradient_geometry_channel {
    std::string_view key, property;
    double min, max;
    std::string_view primary_kind;
    bool also_sweep = false;

    constexpr bool applies_to(std::string_view kind) const {
        return kind == primary_kind || (also_sweep && kind == "sweep");
    }
};

inline constexpr std::array gradient_geometry_channels{
    gradient_geometry_channel{"start_x", "gradient_x0", -detail::gradient_limit, detail::gradient_limit, "linear"},
    gradient_geometry_channel{"start_y", "gradient_y0", -detail::gradient_limit, detail::gradient_limit, "linear"},
    gradient_geometry_channel{"end_x", "gradient_x1", -detail::gradient_limit, detail::gradient_limit, "linear"},
    gradient_geometry_channel{"end_y", "gradient_y1", -detail::gradient_limit, detail::gradient_limit, "linear"},
    gradient_geometry_channel{"center_x", "gradient_center_x", -detail::gradient_limit, detail::gradient_limit, "radial", true},
    gradient_geometry_channel{"center_y", "gradient_center_y", -detail::gradient_limit, detail::gradient_limit, "radial", true},
    gradient_geometry_channel{"outer_x", "gradient_outer_x", -detail::gradient_limit, detail::gradient_limit, "radial"},
    gradient_geometry_channel{"outer_y", "gradient_outer_y", -detail::gradient_limit, detail::gradient_limit, "radial"},
    gradient_geometry_channel{"start_angle", "gradient_angle0", 0, 360, "sweep"},
    gradient_geometry_channel{"end_angle", "gradient_angle1", 0, 360, "sweep"},
    gradient_geometry_channel{"inner_radius", "gradient_radius0", 0, detail::gradient_limit, "radial"},
    gradient_geometry_channel{"outer_radius", "gradient_radius1", 0, detail::gradient_limit, "radial"},
};
static_assert(gradient_geometry_channels.size() == static_cast<std::size_t>(gradient_geometry_axis::count));

struct gradient_keyframe {
    double offset = 0;
    double x = 0, y = 0; // Relative to the authored affine translation, in widget axes.
    std::string tint = "#FFFFFF";
    double opacity = 1;
    std::string interpolation = "linear"; // Outgoing segment.
    // Missing channels use authored geometry; channels for other kinds stay dormant.
    std::array<std::optional<double>, gradient_geometry_channels.size()> geometry{};
    std::vector<gradient_stop> stops; // Empty uses the authored stops.
    bool operator==(const gradient_keyframe&) const = default;
};
bool decode_gradient_keyframes(std::string_view input, std::vector<gradient_keyframe>& out, std::string& error);
std::string encode_gradient_keyframes(std::span<const gradient_keyframe> keyframes);

// Test the filename extension (.ttf or .otf, ASCII case-insensitive).
// This does not otherwise validate a resource path.
bool is_font_path(std::string_view path);
bool is_builtin_font(std::string_view key);

// Parse a concrete #RRGGBB or #RRGGBBAA into 0xAARRGGBB. Empty is not a color.
std::optional<std::uint32_t> parse_text_color(std::string_view text);

struct property_descriptor {
    std::string_view key;
    std::string_view label;
    value_type type;
    value default_value;
    double min = 0, max = 0; // For numbers.
    std::span<const std::string_view> choices{};
};

struct type_descriptor {
    std::string_view key;
    std::string_view label;
    bool container;
    std::span<const property_descriptor> properties; // Kind-specific; see common_properties().
    double initial_width, initial_height; // Used by the editor, not by V1 documents with omitted sizes.
    std::size_t max_children = max_nodes;
    bool panel_children_only = false;
};

std::span<const type_descriptor> types();
std::span<const property_descriptor> common_properties();
const type_descriptor* find_type(std::string_view type);
const property_descriptor* find_property(std::string_view type, std::string_view key);
bool accepts_child(const type_descriptor& parent, std::string_view child_type, std::size_t existing_children);
// Format an already valid collection value for the Inspector.
std::string format_items(std::string_view input);

struct node {
    std::string type;
    std::string name;
    std::map<std::string, value, std::less<>> props;
    std::vector<std::string> children;
    bool operator==(const node&) const = default;
};

struct document {
    std::string root;
    std::map<std::string, node, std::less<>> nodes;
    std::map<std::string, std::string, std::less<>> resources;
    bool operator==(const document&) const = default;
};

struct data_binding_descriptor {
    std::string_view key, target; // Binding property, then the authored fallback property.
    value_type type; // Type of the bound value, not of the key.
};
std::span<const data_binding_descriptor> data_bindings();
// Whether a binding is supported in this position, even if no key is assigned.
bool data_binding_applies(const data_binding_descriptor& binding, const node& item, const node* parent);
using data_field_map = std::map<std::string, value_type, std::less<>>;
// Collect nonempty, effective field keys from an already validated document.
data_field_map data_fields(const document& doc);
bool valid_data_value(value_type type, const value& data);

document make_document();
bool validate(const document& doc, diagnostic* error = nullptr);

// Successful loads replace the output; on failure it is untouched. Encoding
// is canonical and validates first. Files are replaced via a sibling temp file.
bool decode_json(std::string_view text, document& out, diagnostic* error = nullptr);
bool encode_json(const document& doc, std::string& out, diagnostic* error = nullptr);
bool read_document(const std::filesystem::path& path, document& out, diagnostic* error = nullptr);
bool write_document(const std::filesystem::path& path, const document& doc, diagnostic* error = nullptr);

struct insert_node { std::string id, parent; node data; std::size_t index = append; };
struct erase_node { std::string id; }; // Removes the entire subtree.
struct move_node { std::string id, parent; std::size_t index = append; }; // Index after removal.
struct copy_subtree {
    std::string id, parent;
    std::map<std::string, std::string, std::less<>> ids; // Exact old -> new IDs for the subtree.
    std::size_t index = append;
};
struct rename_node { std::string id, name; };
struct set_property { std::string id, key; std::optional<value> data; }; // nullopt restores the default.
struct set_resource { std::string key; std::optional<std::string> path; }; // nullopt removes it.
using edit = std::variant<insert_node, erase_node, move_node, copy_subtree, rename_node, set_property, set_resource>;

// Prepare an Inspector item edit. Existing image keys are kept, and project-
// relative image paths become resource keys. Commit batch together with the
// returned items value; neither output is modified on failure.
bool prepare_items_edit(const document& doc, std::string_view type, std::string_view input,
    std::string& result, std::vector<edit>& batch, std::string& reason);

class session {
public:
    session();
    const document& current() const { return live_ ? *live_ : doc_; }
    const document& committed() const { return doc_; }
    std::uint64_t revision() const { return revision_; } // Advances on observable document changes.
    bool dirty() const { return live_.has_value() || state_ != saved_state_; }
    bool can_undo() const { return live_.has_value() || cursor_ != 0; }
    bool can_redo() const { return !live_ && cursor_ < history_.size(); }

    // Apply to current(); on success, first commit any preview as its own transaction.
    // Failure leaves both the preview and committed history unchanged.
    bool apply(std::span<const edit> edits, diagnostic* error = nullptr);
    // Each batch is relative to committed(); a failure keeps the last valid preview.
    // An empty batch or a result equal to committed() removes the preview.
    bool apply_live(std::span<const edit> edits, diagnostic* error = nullptr);
    void finish_live(bool commit = true); // Commit without a revision bump, or cancel and bump once.
    bool undo(); // An active preview is committed and then undone in one step.
    bool redo(); // Disabled while a preview is active.
    // Successful load discards a preview; successful save commits it. Failure preserves it.
    bool load(const std::filesystem::path& path, diagnostic* error = nullptr);
    bool save(const std::filesystem::path& path, diagnostic* error = nullptr);

private:
    template<class T> struct change { std::string key; std::optional<T> before, after; };
    struct transaction {
        std::vector<change<node>> nodes;
        std::vector<change<std::string>> resources;
        std::uint64_t before{}, after{};
    };
    void record(document&& candidate);
    void replay(const transaction& tx, bool forward);

    document doc_;
    std::optional<document> live_;
    std::vector<transaction> history_;
    std::size_t cursor_ = 0;
    std::uint64_t revision_ = 0, state_ = 0, saved_state_ = 0, next_state_ = 0;
};

} // namespace vgui::ui_design
