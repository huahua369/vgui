#include "ui_designer_preview.h"

#include "ui_designer_font_file.h"

#include <vgui/backend/vgui_render_target.h>
#include <vgui/editor/color_picker.h>
#include <glm/vec4.hpp>

// One private decoder in this TU, using the project's existing stb dependency.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_SIMD
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_MAX_DIMENSIONS 4096
#include <stb_image.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <numbers>
#include <span>
#include <system_error>
#include <utility>

namespace vgui::ui_design {
namespace {
constexpr int max_image_side = 4096;
constexpr std::size_t max_image_file = 16u << 20;
constexpr std::size_t max_mount_bytes = 64u << 20; // Separate encoded and RGBA budgets.
constexpr unsigned max_image_attempts = 256;

struct point { float x, y; };

bool contains(pixel_rect r, point p) {
    return p.x >= r.x && p.y >= r.y && p.x < r.x + r.width && p.y < r.y + r.height;
}

pixel_rect rect(vgui_widget* w) {
    pixel_rect r;
    vgui_widget_get_rect(w, &r.x, &r.y, &r.width, &r.height);
    return r;
}

viewport view_for(designer_preview::preset preset) {
    switch (preset) {
    case designer_preview::preset::tablet: return {768, 1024, 1};
    case designer_preview::preset::phone: return {390, 844, 1};
    default: return {1280, 720, 1};
    }
}

// Match os_window::pixel_ratio(), not SDL_GetWindowDisplayScale (UI scaling).
float pixel_ratio(SDL_Window* window) {
    int w = 0, h = 0, pw = 0, ph = 0;
    if (window && SDL_GetWindowSize(window, &w, &h) && SDL_GetWindowSizeInPixels(window, &pw, &ph)
        && w > 0 && h > 0 && pw > 0) return float(pw) / float(w);
    return 1;
}

point shell_pixels(SDL_WindowID id, float x, float y) {
    const float ratio = pixel_ratio(SDL_GetWindowFromID(id));
    return {std::round(x * ratio), std::round(y * ratio)};
}

int input_key(SDL_Keycode key) {
    switch (key) {
    case SDLK_BACKSPACE: return VGUI_KEY_BACKSPACE;
    case SDLK_TAB: return VGUI_KEY_TAB;
    case SDLK_RETURN: case SDLK_KP_ENTER: return VGUI_KEY_ENTER;
    case SDLK_ESCAPE: return VGUI_KEY_ESCAPE;
    case SDLK_DELETE: return VGUI_KEY_DELETE;
    case SDLK_LEFT: return VGUI_KEY_LEFT;
    case SDLK_RIGHT: return VGUI_KEY_RIGHT;
    case SDLK_UP: return VGUI_KEY_UP;
    case SDLK_DOWN: return VGUI_KEY_DOWN;
    case SDLK_HOME: return VGUI_KEY_HOME;
    case SDLK_END: return VGUI_KEY_END;
    case SDLK_PAGEUP: return VGUI_KEY_PAGE_UP;
    case SDLK_PAGEDOWN: return VGUI_KEY_PAGE_DOWN;
    case SDLK_INSERT: return VGUI_KEY_INSERT;
    default: break;
    }
    if (key >= SDLK_F1 && key <= SDLK_F12) return VGUI_KEY_F1 + int(key - SDLK_F1);
    return key > 0 && key < 128 ? int(key) : 0;
}

int input_mods(SDL_Keymod mods) {
    return (mods & SDL_KMOD_SHIFT ? VGUI_MOD_SHIFT : 0) |
        (mods & SDL_KMOD_CTRL ? VGUI_MOD_CTRL : 0) |
        (mods & SDL_KMOD_ALT ? VGUI_MOD_ALT : 0) |
        (mods & SDL_KMOD_GUI ? VGUI_MOD_GUI : 0);
}

using image_ptr = std::unique_ptr<vgui_texture, decltype(&vgui_texture_destroy)>;

struct image_load {
    std::vector<image_ptr> images;
    std::size_t encoded = 0, decoded = 0;
    unsigned attempts = 0;

    vgui_texture* load(vgui_context* ctx, const std::filesystem::path& file) {
        // The runtime has already resolved containment and checked regular-file status.
        if (attempts >= max_image_attempts) return nullptr;
        ++attempts;
        std::error_code ec;
        const auto size = std::filesystem::file_size(file, ec);
        if (ec || size == 0 || size > max_image_file || size > max_mount_bytes - encoded) return nullptr;
        encoded += static_cast<std::size_t>(size);
        std::ifstream stream(file, std::ios::binary);
        if (!stream) return nullptr;
        std::vector<stbi_uc> bytes(static_cast<std::size_t>(size));
        if (!stream.read(reinterpret_cast<char*>(bytes.data()), bytes.size()) ||
            stream.peek() != std::char_traits<char>::eof()) return nullptr;
        int w = 0, h = 0, channels = 0;
        const int count = static_cast<int>(bytes.size());
        if (!stbi_info_from_memory(bytes.data(), count, &w, &h, &channels) || w < 1 || h < 1 ||
            w > max_image_side || h > max_image_side) return nullptr;
        const auto rgba_bytes = std::size_t(w) * h * 4;
        if (rgba_bytes > max_mount_bytes - decoded) return nullptr;
        decoded += rgba_bytes;
        int decoded_w = 0, decoded_h = 0;
        std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(stbi_load_from_memory(
            bytes.data(), count, &decoded_w, &decoded_h, &channels, STBI_rgb_alpha), stbi_image_free);
        if (!pixels || decoded_w != w || decoded_h != h) return nullptr;
        vgui_texture_desc desc{w, h, w * 4, VGUI_TEXTURE_FORMAT_RGBA8, pixels.get()};
        image_ptr image(vgui_texture_create(ctx, &desc), vgui_texture_destroy);
        auto* result = image.get();
        if (result) images.push_back(std::move(image)); // Drop loader refs after mount; widgets retain theirs.
        return result;
    }
};

using font_ptr = std::unique_ptr<font_familys_t, decltype(&delete_font_family)>;

constexpr std::size_t max_project_font_bytes = 64u << 20;
constexpr std::size_t max_project_fonts = 32;
constexpr std::size_t max_font_attempts = 32;
constexpr char latin_alias[] = "vgui.preview.bundled.latin";
constexpr char sc_alias[] = "vgui.preview.bundled.sc";
const std::filesystem::path bundled_fonts = std::filesystem::u8path(VGUI_SOURCE_DIR) / "sample/res/fonts";

bool register_font(font_cache_cx* cache, const std::filesystem::path& file, std::size_t size, const char* alias) {
    if (!cache || size < 12 || size > max_project_font_file) return false;
    // Use the native path overload: on Windows it opens non-ASCII paths without
    // going through a code page. OVG copies the bytes into its HarfBuzz blob.
    std::ifstream stream(file, std::ios::binary);
    if (!stream) return false;
    std::vector<unsigned char> bytes(size);
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)) ||
        stream.peek() != std::char_traits<char>::eof() || stream.bad()) return false;
    if (!valid_font_file(bytes)) return false;
    return load_font_from_memory(cache, bytes.data(), static_cast<long>(size), alias);
}

struct font_version {
    std::filesystem::path path;
    std::uintmax_t size;
    std::filesystem::file_time_type modified;
    bool operator==(const font_version&) const = default;
};

} // namespace

struct designer_preview_fonts::impl {
    // OVG owns the CPU atlas pages; the shared SDL renderer owns their uploaded
    // textures and OVG cache destruction doesn't retire those textures. Keep a
    // bounded set of registrations and their atlases for the workbench lifetime
    // rather than orphaning GPU textures on every page switch/resource edit.
    // Declare the cache before all of its borrowing family lists.
    std::unique_ptr<font_cache_cx, decltype(&free_font_cache)> cache{nullptr, free_font_cache};
    font_ptr sans{nullptr, delete_font_family}, sans_sc{nullptr, delete_font_family};
    std::string fallbacks;
    std::vector<std::pair<font_version, font_ptr>> registered;
    std::vector<font_version> rejected;
    std::size_t registered_bytes = 0, mount_bytes = 0, attempts = 0;
    bool initialized = false;

    void begin_mount() { mount_bytes = attempts = 0; }

    void initialize() {
        if (initialized) return;
        initialized = true;
        cache.reset(new_font_cache());
        if (!cache) return;
        const auto load_bundled = [&](const char* name, const char* alias) {
            const auto file = bundled_fonts / name;
            std::error_code ec;
            const auto size = std::filesystem::file_size(file, ec);
            return !ec && size <= max_project_font_file && register_font(cache.get(), file, static_cast<std::size_t>(size), alias);
        };
        const bool latin = load_bundled("DejaVuSans.ttf", latin_alias);
        const bool sc = load_bundled("DesignerSansSC.ttf", sc_alias);
        if (latin) fallbacks = latin_alias;
        if (sc) {
            if (latin) fallbacks += ',';
            fallbacks += sc_alias;
        }
        if (latin) sans.reset(new_font_family(cache.get(), fallbacks.c_str(), nullptr));
        if (sc) {
            std::string sc_first = sc_alias;
            if (latin) sc_first += std::string(",") + latin_alias;
            sans_sc.reset(new_font_family(cache.get(), sc_first.c_str(), nullptr));
        }
    }

    font_familys_t* load(std::string_view key, const std::filesystem::path& file) {
        if (file.empty()) {
            initialize();
            return key == "builtin.sans" ? sans.get() : key == "builtin.sans_sc" ? sans_sc.get() : nullptr;
        }
        // Runtime has already checked containment of the canonical regular file.
        // Use the canonical file and its metadata, not the model's key: an edited
        // resource or a different project root must not keep yesterday's font.
        const auto ext = file.extension().u8string();
        if (ext.size() != 4) return nullptr;
        const auto lower = [](char8_t c) { return c >= u8'A' && c <= u8'Z' ? c - u8'A' + u8'a' : c; };
        if ((lower(ext[1]) != u8't' && lower(ext[1]) != u8'o') ||
            lower(ext[2]) != u8't' || lower(ext[3]) != u8'f') return nullptr;
        std::error_code ec;
        const auto size = std::filesystem::file_size(file, ec);
        if (ec || size < 12 || size > max_project_font_file) return nullptr;
        const auto modified = std::filesystem::last_write_time(file, ec);
        if (ec) return nullptr;
        const font_version version{file, size, modified};
        for (const auto& [existing, family] : registered)
            if (existing == version) return family.get();
        if (attempts >= max_font_attempts || registered.size() >= max_project_fonts ||
            size > max_project_font_bytes - registered_bytes || size > max_project_font_bytes - mount_bytes ||
            std::find(rejected.begin(), rejected.end(), version) != rejected.end()) return nullptr;
        initialize();
        if (!cache) return nullptr;
        const auto bytes = static_cast<std::size_t>(size);
        ++attempts;
        mount_bytes += bytes;
        const auto alias = "vgui.preview.project." + std::to_string(registered.size());
        if (!register_font(cache.get(), file, bytes, alias.c_str())) {
            // Remember recent malformed files across text-only remounts. They
            // can be retried when their file version changes.
            if (rejected.size() == max_font_attempts) rejected.erase(rejected.begin());
            rejected.push_back(version);
            return nullptr;
        }
        const auto names = fallbacks.empty() ? alias : alias + ',' + fallbacks;
        registered_bytes += bytes;
        auto& family = registered.emplace_back(version,
            font_ptr(new_font_family(cache.get(), names.c_str(), nullptr), delete_font_family)).second;
        return family.get();
    }
};

namespace {
std::string describe(const diagnostic& d) {
    return d.path.empty() ? d.message : d.path + ": " + d.message;
}

void fill(ovg_ctx_cb* cb, rvg_t* vg, pixel_rect r, unsigned color) {
    if (r.width <= 0 || r.height <= 0) return;
    cb->set_source_color(vg, color);
    cb->rectangle(vg, r.x, r.y, r.width, r.height);
    cb->fill(vg);
}

bool same_rect(pixel_rect a, pixel_rect b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

pixel_rect intersect(pixel_rect a, pixel_rect b) {
    const float x = std::max(a.x, b.x), y = std::max(a.y, b.y);
    return {x, y, std::max(0.f, std::min(a.x + a.width, b.x + b.width) - x),
        std::max(0.f, std::min(a.y + a.height, b.y + b.height) - y)};
}

std::uint32_t to_ovg(std::uint32_t argb) {
    return (argb & 0xFF00FF00u) | ((argb & 0xFFu) << 16) | ((argb >> 16) & 0xFFu);
}

bool has_area(pixel_rect r) { return r.width > 0 && r.height > 0; }

// Split the visible viewport into four strips outside the frame and the
// artboard interior, leaving the one-pixel frame clear of grid guides.
std::array<pixel_rect, 5> grid_clips(pixel_rect area, pixel_rect board, pixel_rect frame) {
    const auto covered = intersect(area, frame);
    if (!has_area(covered)) return {area};
    const float right = covered.x + covered.width, bottom = covered.y + covered.height;
    return {{
        {area.x, area.y, area.width, covered.y - area.y},
        {area.x, bottom, area.width, area.y + area.height - bottom},
        {area.x, covered.y, covered.x - area.x, covered.height},
        {right, covered.y, area.x + area.width - right, covered.height},
        intersect(area, board),
    }};
}

struct grid_range {
    std::int64_t first = 0, end = 0;
    double count() const { return double(end - first); }
};

// Include centers just outside the clip whose snapped stroke can straddle it.
grid_range grid_indices(float start, float extent, double origin, double step, float width) {
    const double pad = width * .5 + 1;
    const double first = std::ceil((start - pad - origin) / step);
    const double last = std::floor((start + extent + pad - origin) / step);
    constexpr double index_limit = 1e12; // Also bounds conversion before indexing extreme views.
    if (!(first >= -index_limit && last <= index_limit && last >= first)) return {};
    return {static_cast<std::int64_t>(first), static_cast<std::int64_t>(last) + 1};
}

float grid_edge(double center, float width) {
    return float(std::floor(center + .5) - std::floor(width * .5));
}

void draw_grid(ovg_ctx_cb* cb, rvg_t* vg, pixel_rect area, pixel_rect board, pixel_rect frame,
        viewport view, float ui_scale, const designer_grid_settings& settings, const designer_grid_colors& colors) {
    if (!std::isfinite(board.x) || !std::isfinite(board.y) || !std::isfinite(board.width) ||
        !std::isfinite(board.height) || !std::isfinite(ui_scale) || ui_scale <= 0) return;
    const auto clips = grid_clips(area, board, frame);
    if (std::none_of(clips.begin(), clips.end(), has_area)) return;

    // Spacing is in artboard units. The width is in shell units, snapped to
    // full device pixels so thin strokes and dots stay legible without MSAA.
    const double step_x = double(settings.spacing) * board.width / view.width;
    const double step_y = double(settings.spacing) * board.height / view.height;
    const float width = std::max(1.f, std::round(settings.line_width * ui_scale));
    const double min_step = std::max(settings.hide_below, width + 1.f);
    const bool dots = settings.style == designer_grid_style::dots;
    const auto layer = [&](bool major) {
        const auto color = to_ovg(major ? colors.major_color : colors.minor_color);
        if (!(color >> 24) || (!major && settings.major_every == 1)) return;
        const int stride = major ? settings.major_every : 1;
        const double sx = step_x * stride, sy = step_y * stride;
        if (std::min(sx, sy) < min_step) return;
        struct span { grid_range x, y; };
        std::array<span, clips.size()> spans{};
        double marks = 0;
        for (std::size_t i = 0; i < clips.size(); ++i) {
            const auto r = clips[i];
            if (!has_area(r)) continue;
            auto& [x, y] = spans[i];
            x = grid_indices(r.x, r.width, board.x, sx, width);
            y = grid_indices(r.y, r.height, board.y, sy, width);
            marks += dots ? x.count() * y.count() : x.count() + y.count();
        }
        // Bound generation over the whole viewport, not per row or strip.
        // The major pass has its own coarser spacing and can still be drawn.
        if (marks > (dots ? 8192 : 1024)) return;
        int queued = 0;
        const auto add = [&](pixel_rect mark, pixel_rect clip) {
            const auto r = intersect(mark, clip);
            if (!has_area(r)) return;
            cb->rectangle(vg, r.x, r.y, r.width, r.height);
            if (++queued == 64) { cb->fill(vg); queued = 0; }
        };
        const auto flush = [&] { if (queued) { cb->fill(vg); queued = 0; } };
        const auto is_major = [&](std::int64_t i) { return i % settings.major_every == 0; };
        for (std::size_t i = 0; i < clips.size(); ++i) {
            const auto r = clips[i];
            if (!has_area(r)) continue;
            const auto [xs, ys] = spans[i];
            if (dots && (xs.first == xs.end || ys.first == ys.end)) continue;
            cb->set_source_color(vg, color);
            for (auto x = xs.first; x < xs.end; ++x) {
                if (x == 0 || (!dots && !major && is_major(x))) continue;
                const float px = grid_edge(board.x + x * sx, width);
                if (!dots) { add({px, r.y, width, r.height}, r); continue; }
                for (auto y = ys.first; y < ys.end; ++y) {
                    if (y == 0 || (!major && is_major(x) && is_major(y))) continue;
                    add({px, grid_edge(board.y + y * sy, width), width, width}, r);
                }
            }
            if (!dots) {
                // Keep perpendicular lines in separate paths to avoid tessellating all their crossings.
                flush();
                for (auto y = ys.first; y < ys.end; ++y)
                    if (y != 0 && (major || !is_major(y)))
                        add({r.x, grid_edge(board.y + y * sy, width), r.width, width}, r);
            }
            flush();
        }
    };
    layer(false);
    layer(true);

    if (colors.axis_color >> 24) {
        const float x = grid_edge(board.x, width), y = grid_edge(board.y, width);
        const auto color = to_ovg(colors.axis_color);
        for (const auto r : clips) {
            if (!has_area(r)) continue;
            fill(cb, vg, intersect({x, r.y, width, r.height}, r), color);
            fill(cb, vg, intersect({r.x, y, r.width, width}, r), color);
        }
    }
}

void outline(ovg_ctx_cb* cb, rvg_t* vg, pixel_rect r, pixel_rect clip, unsigned color, float width = 2.f) {
    const float dx = std::min(r.width, width), dy = std::min(r.height, width);
    fill(cb, vg, intersect({r.x, r.y, r.width, dy}, clip), color);
    fill(cb, vg, intersect({r.x, r.y + r.height - dy, r.width, dy}, clip), color);
    fill(cb, vg, intersect({r.x, r.y, dx, r.height}, clip), color);
    fill(cb, vg, intersect({r.x + r.width - dx, r.y, dx, r.height}, clip), color);
}

struct board_transform {
    pixel_rect board;
    int width, height; // Snapshot pixels, not shell pixels.
    float scale; // Snapshot pixels per logical unit.

    pixel_rect project(pixel_rect r) const {
        const float sx = board.width / width, sy = board.height / height;
        return {board.x + r.x * sx, board.y + r.y * sy, r.width * sx, r.height * sy};
    }
    bool matches(const board_transform& other) const {
        return same_rect(board, other.board) && width == other.width && height == other.height && scale == other.scale;
    }
};

constexpr auto start_x = std::size_t(gradient_geometry_axis::start_x);
constexpr auto end_x = std::size_t(gradient_geometry_axis::end_x);
constexpr auto center_x = std::size_t(gradient_geometry_axis::center_x);
constexpr auto outer_x = std::size_t(gradient_geometry_axis::outer_x);
constexpr auto start_angle = std::size_t(gradient_geometry_axis::start_angle);
constexpr auto end_angle = std::size_t(gradient_geometry_axis::end_angle);
constexpr auto inner_radius = std::size_t(gradient_geometry_axis::inner_radius);
constexpr auto outer_radius = std::size_t(gradient_geometry_axis::outer_radius);
constexpr std::array linear_handles{start_x, end_x};
constexpr std::array radial_handles{center_x, outer_x, inner_radius, outer_radius};
constexpr std::array sweep_handles{center_x, start_angle, end_angle};
constexpr double radians_per_degree = std::numbers::pi / 180.;
constexpr double sweep_guide_ratio = .42; // Guide only, in paint pixels per short side.

bool is_angle(std::size_t axis) { return axis == start_angle || axis == end_angle; }

double sweep_guide_radius(pixel_rect node) { return sweep_guide_ratio * std::min(node.width, node.height); }

std::span<const std::size_t> shape_handles(const shape_keyframe& key) {
    switch (key.kind) {
    case shape_kind::radial: return radial_handles;
    case shape_kind::sweep: return sweep_handles;
    default: return linear_handles;
    }
}

point project_shape(const shape_keyframe& key, pixel_rect node, double x, double y) {
    const auto [xx, yx, xy, yy, tx, ty] = key.affine;
    return {float(node.x + node.width * (xx * x + xy * y + tx)),
        float(node.y + node.height * (yx * x + yy * y + ty))};
}

point shape_handle_anchor(const shape_keyframe& key, const keyframe_shape& shape,
        pixel_rect node, float short_side, std::size_t axis, double u = 1., double v = 0.) {
    const bool radius = axis >= inner_radius;
    const bool inner = axis == center_x || axis == inner_radius;
    const auto center = radius ? (inner ? center_x : outer_x) : axis;
    const double ellipse = key.ellipse ? 2. : 1.;
    // OVG stretches horizontal distances from the outer center before the brush affine.
    double x = shape[center], y = shape[center + 1];
    if (key.ellipse && inner) x = shape[outer_x] + (x - shape[outer_x]) * ellipse;
    if (radius) {
        x += u * shape[axis] * short_side / node.width * ellipse;
        y += v * shape[axis] * short_side / node.height;
    }
    return project_shape(key, node, x, y);
}

point sweep_guide_point(const shape_keyframe& key, const keyframe_shape& shape, pixel_rect node, double degrees) {
    const double angle = degrees * radians_per_degree, r = sweep_guide_radius(node);
    return project_shape(key, node, shape[center_x] + r * std::cos(angle) / node.width,
        shape[center_x + 1] + r * std::sin(angle) / node.height);
}

point knob_anchor(const shape_keyframe& key, const keyframe_shape& shape,
        pixel_rect node, float short_side, std::size_t axis, const board_transform& transform) {
    const auto p = is_angle(axis) ? sweep_guide_point(key, shape, node, shape[axis]) :
        shape_handle_anchor(key, shape, node, short_side, axis, axis == inner_radius ? -1. : 1.);
    const auto projected = transform.project({p.x, p.y, 0, 0});
    return {projected.x, projected.y};
}

float distance_sq(point a, point b) {
    const float dx = a.x - b.x, dy = a.y - b.y;
    return dx * dx + dy * dy;
}

struct shape_knob { std::size_t axis; point anchor, at; };

// Keep radial and sweep knobs accessible when guides overlap or extend off
// the board (including 0° and 360° on the same ray). Tethers show their anchors.
std::array<shape_knob, 4> place_shape_knobs(const shape_keyframe& key, const keyframe_shape& shape,
        pixel_rect node, board_transform transform, pixel_rect clip, float gap) {
    std::array<shape_knob, 4> result{};
    const auto axes = shape_handles(key);
    const float short_side = std::min(node.width, node.height);
    const float mx = std::min(gap * .5f, clip.width * .5f), my = std::min(gap * .5f, clip.height * .5f);
    const auto clamp = [&](point p) {
        return point{std::clamp(p.x, clip.x + mx, clip.x + clip.width - mx),
            std::clamp(p.y, clip.y + my, clip.y + clip.height - my)};
    };
    for (std::size_t i = 0; i < axes.size(); ++i) {
        const auto axis = axes[i];
        const auto anchor = knob_anchor(key, shape, node, short_side, axis, transform);
        const bool offset_knobs = key.kind != shape_kind::linear;
        const auto start = offset_knobs ? clamp(anchor) : anchor;
        auto& knob = result[i];
        knob = {axis, anchor, start};
        if (!offset_knobs || i == 0) continue;
        const auto separation = [&](point at) {
            float distance = gap * gap;
            for (std::size_t j = 0; j < i; ++j) distance = std::min(distance, distance_sq(at, result[j].at));
            return distance;
        };
        float clearance = separation(start);
        for (int ring = 1; ring <= 3 && clearance < gap * gap; ++ring) {
            for (const point dir : {point{1, 0}, {-1, 0}, {0, -1}, {0, 1}, {1, -1}, {-1, 1}, {-1, -1}, {1, 1}}) {
                const auto candidate = clamp({start.x + dir.x * ring * gap, start.y + dir.y * ring * gap});
                if (const float next = separation(candidate); next > clearance) {
                    knob.at = candidate;
                    clearance = next;
                    if (next >= gap * gap) break;
                }
            }
        }
    }
    return result;
}

struct gradient_stop_axis {
    point start, end;

    point at(float offset) const {
        return {std::lerp(start.x, end.x, offset), std::lerp(start.y, end.y, offset)};
    }
    double offset_delta(double dx, double dy) const {
        const double x = double(end.x) - start.x, y = double(end.y) - start.y;
        return (dx * x + dy * y) / (x * x + y * y);
    }
};

// OVG samples circles with interpolated center and radius. Joining the
// two effective circles at a common direction gives a straight t=0..1 axis.
struct radial_stop_circles {
    point inner, outer;
    float r0, r1;

    radial_stop_circles(const keyframe_shape& shape, pixel_rect node) {
        inner = {float(shape[center_x]) * node.width, float(shape[center_x + 1]) * node.height};
        outer = {float(shape[outer_x]) * node.width, float(shape[outer_x + 1]) * node.height};
        const float s = std::min(node.width, node.height);
        // Match vgui_draw_gradient's radius limits and OVG's one-pixel focal margin.
        r1 = std::max(2.f, float(shape[outer_radius]) * s);
        r0 = std::min(float(shape[inner_radius]) * s, r1 - 1.01f);
        const float dx = inner.x - outer.x, dy = inner.y - outer.y;
        const float distance = std::sqrt(dx * dx + dy * dy);
        if (distance > 0.f && distance + r0 + 1.f >= r1) {
            const float reach = r1 - r0 - 1.f;
            inner = {outer.x + (dx / distance) * reach, outer.y + (dy / distance) * reach};
        }
    }

    gradient_stop_axis spoke(const shape_keyframe& key, pixel_rect node, const board_transform& transform,
            point direction) const {
        const double ellipse = key.ellipse ? 2. : 1.;
        const auto project = [&](point center, float radius) {
            const auto x = outer.x + ellipse * (double(center.x) + direction.x * radius - outer.x);
            const auto y = double(center.y) + direction.y * radius;
            const auto p = project_shape(key, node, x / node.width, y / node.height);
            const auto at = transform.project({p.x, p.y, 0, 0});
            return point{at.x, at.y};
        };
        return {project(inner, r0), project(outer, r1)};
    }
};

using stop_knobs = std::array<std::optional<shape_knob>, max_gradient_stops>;
struct stop_ray { point anchor, normal; float inward_limit = std::numeric_limits<float>::infinity(); };

point sweep_stop_center(const shape_keyframe& key, const keyframe_shape& shape, pixel_rect node,
        const board_transform& transform) {
    return knob_anchor(key, shape, node, 0.f, center_x, transform);
}

stop_ray sweep_stop_ray(const shape_keyframe& key, const keyframe_shape& shape, pixel_rect node,
        const board_transform& transform, point center, float offset) {
    const auto p = sweep_guide_point(key, shape, node, std::lerp(shape[start_angle], shape[end_angle], double(offset)));
    const auto at = transform.project({p.x, p.y, 0, 0});
    const float dx = at.x - center.x, dy = at.y - center.y, length = std::hypot(dx, dy);
    return {{at.x, at.y}, length > 0.f ? point{dx / length, dy / length} : point{}, length};
}

std::optional<double> sweep_pointer_angle(const shape_keyframe& key, pixel_rect node,
        const board_transform& transform, point center, point pointer) {
    // Undo both the board scale and the normalized brush affine. OVG measures
    // clockwise angles in canonical paint pixels, even on a non-square widget.
    const auto [xx, yx, xy, yy, tx, ty] = key.affine;
    const double x = (double(pointer.x) - center.x) * transform.width / transform.board.width / node.width;
    const double y = (double(pointer.y) - center.y) * transform.height / transform.board.height / node.height;
    const double det = double(xx) * yy - double(xy) * yx;
    const double u = (yy * x - xy * y) * node.width / det, v = (xx * y - yx * x) * node.height / det;
    if (std::hypot(u, v) < sweep_guide_radius(node) * .02) return {};
    return std::atan2(v, u) / radians_per_degree;
}

// Stagger close stops along tethers without covering the shape controls. For a
// sweep the tethers follow rays; for a straight guide they are perpendicular.
template<class RayAt>
stop_knobs place_stop_knobs(const shape_keyframe& key, RayAt ray_at, pixel_rect clip, float radius,
        std::span<const shape_knob> geometry) {
    stop_knobs result{};
    const float inset = radius + 2.f;
    const pixel_rect room{clip.x + inset, clip.y + inset, clip.width - 2 * inset, clip.height - 2 * inset};
    if (!has_area(room)) return result;
    const float gap = 2 * inset + 2.f;
    for (std::size_t i = 0; i < key.stops.size(); ++i) {
        const auto [anchor, normal, inward_limit] = ray_at(key.stops[i].offset);
        if (inward_limit < 1.f || !contains(clip, anchor)) continue;
        for (std::size_t lane = 0; lane < 2 * max_gradient_stops; ++lane) {
            const float side = lane % 2 == 0 ? 1.f : -1.f;
            const float shift = side * (3 * radius + (lane / 2) * gap);
            if (inward_limit + shift < inset) continue; // Keep sweep swatches on their own side of the center.
            const point at{anchor.x + normal.x * shift, anchor.y + normal.y * shift};
            if (!contains(room, at) || std::ranges::any_of(geometry, [&](const auto& knob) {
                    return distance_sq(at, knob.at) < gap * gap;
                }) || std::ranges::any_of(std::span(result).first(i), [&](const auto& prev) {
                    return prev && distance_sq(at, prev->at) < gap * gap;
                })) continue;
            result[i] = shape_knob{i, anchor, at};
            break;
        }
    }
    return result;
}

stop_knobs place_axis_stop_knobs(const shape_keyframe& key, gradient_stop_axis axis, pixel_rect clip, float radius,
        std::span<const shape_knob> geometry = {}) {
    const float length = std::sqrt(distance_sq(axis.start, axis.end));
    if (length < 1.f) return {};
    const point normal{(axis.end.y - axis.start.y) / length, (axis.start.x - axis.end.x) / length};
    return place_stop_knobs(key, [&](float offset) { return stop_ray{axis.at(offset), normal}; }, clip, radius, geometry);
}

struct stop_handle_layout { gradient_stop_axis axis{}; stop_knobs knobs{}; };

stop_handle_layout layout_stop_knobs(const shape_keyframe& key, const keyframe_shape& shape, pixel_rect node,
        const board_transform& transform, pixel_rect clip, float radius, std::span<const shape_knob> geometry) {
    if (key.stops.empty()) return {};
    if (key.kind == shape_kind::linear) {
        const gradient_stop_axis axis{knob_anchor(key, shape, node, 0.f, start_x, transform),
            knob_anchor(key, shape, node, 0.f, end_x, transform)};
        return {axis, place_axis_stop_knobs(key, axis, clip, radius)};
    }
    if (key.kind == shape_kind::sweep) {
        const auto center = sweep_stop_center(key, shape, node, transform);
        return {{}, place_stop_knobs(key, [&](float offset) {
            return sweep_stop_ray(key, shape, node, transform, center, offset);
        }, clip, radius, geometry)};
    }
    const radial_stop_circles circles(shape, node);
    constexpr float diagonal = .70710678f;
    constexpr point directions[]{{0, -1}, {0, 1}, {-1, 0}, {1, 0},
        {-diagonal, -diagonal}, {diagonal, -diagonal}, {-diagonal, diagonal}, {diagonal, diagonal}};
    stop_handle_layout best;
    std::size_t visible = 0;
    // Prefer the upper spoke, switching only if another direction keeps more handles on the artboard.
    for (const auto direction : directions) {
        const auto axis = circles.spoke(key, node, transform, direction);
        auto knobs = place_axis_stop_knobs(key, axis, clip, radius, geometry);
        const auto count = std::size_t(std::ranges::count_if(knobs, [](const auto& knob) { return knob.has_value(); }));
        if (count <= visible) continue;
        best = {axis, std::move(knobs)};
        if (count == key.stops.size()) break;
        visible = count;
    }
    return best;
}

float move_stop_offset(std::span<const gradient_stop> stops, std::size_t i, double delta) {
    const float original = stops[i].offset;
    float lo = 0.f, hi = 1.f;
    if (i > 0) {
        const float prev = stops[i - 1].offset;
        lo = prev + gradient_stop_min_gap;
        if (lo - prev < gradient_stop_min_gap) lo = std::nextafter(lo, 1.f);
    }
    if (i + 1 < stops.size()) {
        const float next = stops[i + 1].offset;
        hi = next - gradient_stop_min_gap;
        if (next - hi < gradient_stop_min_gap) hi = std::nextafter(hi, 0.f);
    }
    return static_cast<float>(std::clamp(original + delta, double(std::min(lo, original)), double(std::max(hi, original))));
}

struct source_axis {
    double position;
    const property_descriptor *pos_desc, *size_desc;
};

struct geometry_source { source_axis x, y; };

enum class corner { top_left, top_right, bottom_left, bottom_right };

// Put handles on the true corners only. Clipped edges are not resize edges.
template<class F>
void visible_corners(const node_layout& node, board_transform transform, F&& visit) {
    const auto& r = node.rect_px;
    const auto& clip = node.visible_px;
    for (const auto c : {corner::top_left, corner::top_right, corner::bottom_left, corner::bottom_right}) {
        const bool left = c == corner::top_left || c == corner::bottom_left;
        const bool top = c == corner::top_left || c == corner::top_right;
        const float x = left ? r.x : r.x + r.width, y = top ? r.y : r.y + r.height;
        if (x < clip.x || x > clip.x + clip.width || y < clip.y || y > clip.y + clip.height) continue;
        const auto p = transform.project({x, y, 0, 0});
        visit(c, point{p.x, p.y});
    }
}

struct axis_edit { std::optional<double> position, size; };

axis_edit edit_axis(source_axis source, double actual_size, double delta, bool resize, bool leading) {
    if (delta == 0) return {};
    const auto& p = *source.pos_desc;
    const auto& s = *source.size_desc;
    if (!resize) {
        const double position = std::clamp(source.position + delta, p.min, p.max);
        if (position != source.position) return {position, {}};
        return {};
    }

    // Never write the auto sentinel (0). Let a pre-existing sub-unit size grow
    // from its actual extent without jumping to 1 at pointer-down.
    double min_size = std::max(s.min, std::min(actual_size, 1.0)), max_size = s.max;
    const double opposite = source.position + actual_size;
    if (leading) {
        min_size = std::max(min_size, opposite - p.max);
        max_size = std::min(max_size, opposite - p.min);
    }
    if (min_size > max_size) return {};
    const double size = std::clamp(actual_size + (leading ? -delta : delta), min_size, max_size);
    if (size == actual_size) return {};
    // Limiting the size also limits the leading position, preserving the far edge.
    return {leading ? std::optional(std::clamp(opposite - size, p.min, p.max)) : std::nullopt, size};
}

} // namespace

struct designer_preview::impl {
    const designer_strings& strings;
    vgui_context* shell = nullptr;
    vgui_widget *panel = nullptr, *toolbar = nullptr, *canvas = nullptr, *status = nullptr, *zoom_text = nullptr;
    vgui_widget *transport = nullptr, *play = nullptr, *restart = nullptr, *scrubber = nullptr, *time_text = nullptr;
    std::array<vgui_widget*, 3> presets{};
    vgui_widget *mode = nullptr, *zoom_out = nullptr, *zoom_in = nullptr, *zoom_fit = nullptr, *zoom_actual = nullptr;
    vgui_widget* timeline_toggle = nullptr;
    std::unique_ptr<designer_gradient_timeline> timeline;
    bool timeline_open = false;
    std::function<void(std::string)> select, notify;
    std::function<void(geometry_edit)> commit_geometry;
    std::map<std::string, geometry_source, std::less<>> geometry_sources;
    struct geometry_drag {
        geometry_edit change;
        geometry_source source;
        pixel_rect area, initial, visible, ghost;
        board_transform transform;
        point down;
        float dpi;
        std::optional<corner> handle;
    };
    std::optional<geometry_drag> geometry;
    struct shape_drag {
        shape_keyframe key;
        keyframe_shape shape;
        pixel_rect area, node;
        board_transform transform;
        point down;
        float dpi;
        std::size_t axis; // A center's X channel, a radius, or an angle.
        double angle_cursor = 0., angle_unwrapped = 0.;
        std::optional<gradient_stop_offset> stop;
        gradient_stop_axis stop_axis;
    };
    std::optional<shape_drag> shape_edit;
    struct stop_color_session {
        shape_keyframe key;
        std::size_t index;
        SDL_WindowID window;
        std::uint32_t color;
    };
    std::optional<stop_color_session> stop_color_edit;
    vgui_editor_kit::color_picker stop_picker;
    vgui_widget *stop_color_title = nullptr, *stop_color_ok = nullptr, *stop_color_cancel = nullptr;
    // Keep consuming the matching left-up even when the drag was canceled.
    std::optional<SDL_WindowID> edit_release;
    // Unlike the per-mount image loader, these families outlive all widgets
    // and their derived weight/slant variants in the private UI context.
    designer_preview_fonts::impl& fonts;
    std::unique_ptr<vgui_context, decltype(&vgui_destroy)> ui{nullptr, vgui_destroy};
    std::unique_ptr<ovg_ctx_cb, decltype(&free_ctx_cb)> cb{nullptr, free_ctx_cb};
    std::unique_ptr<runtime> mounted;
    std::unique_ptr<vgui_render_target, decltype(&vgui_render_target_destroy)> target{
        nullptr, vgui_render_target_destroy};
    vgpu_device_t* device = nullptr;
    vgpu_ctx_t* renderer = nullptr;
    bool has_document = false, remount = true, is_mounted = false, run = false;
    animation_span timing;
    animation_view playback;
    bool sample_requested = false;
    bool dirty = true, ready = false, fit = true;
    uint64_t revision = 0, last_paint = 0;
    std::filesystem::path root;
    std::optional<layout_snapshot> frame;
    std::string selection, pending_action;
    diagnostic mount_error;
    std::optional<std::string> render_error;
    image_load* active_load = nullptr;
    preset screen = preset::desktop;
    designer_grid_settings grid;
    designer_theme theme = default_designer_theme;
    float zoom = 1;
    point pan{};
    SDL_WindowID pointer_window = 0, run_window = 0, ime_window = 0;
    bool owns_text_input = false, dragging = false, runtime_hover = false;
    unsigned pressed = 0;
    int focus_listener = 0;
    point drag_last{};

    impl(vgui_widget* parent, vgpu_device_t* gpu, font_familys_t* font, designer_preview_fonts::impl& cache,
            std::function<void(std::string)> s, std::function<void(std::string)> n,
            std::function<void(geometry_edit)> commit, const designer_strings& locale, gradient_timeline_commit commit_keys)
        : strings(locale), shell(parent ? vgui_widget_get_context(parent) : nullptr),
          select(std::move(s)), notify(std::move(n)), commit_geometry(std::move(commit)),
          fonts(cache), ui(vgui_create(font, 14), vgui_destroy), device(gpu) {
        if (ui) mounted = std::make_unique<runtime>(ui.get());
        if (!shell) return;
        panel = vgui_widget_add(parent, VGUI_PANEL, nullptr);
        vgui_widget_set_id(panel, "designer.preview");
        vgui_widget_set_layout(panel, VGUI_LAYOUT_COLUMN);
        vgui_widget_set_align_items(panel, VGUI_ALIGN_STRETCH);
        vgui_widget_set_grow(panel, 1);
        vgui_widget_set_padding(panel, 0, 0, 0, 0);
        vgui_widget_set_gap(panel, 0);
        vgui_widget_set_background(panel, 0xFF242933);
        toolbar = vgui_widget_add(panel, VGUI_PANEL, nullptr);
        vgui_widget_set_id(toolbar, "designer.preview.toolbar");
        vgui_widget_set_layout(toolbar, VGUI_LAYOUT_ROW);
        vgui_widget_set_size(toolbar, 0, 34);
        vgui_widget_set_padding(toolbar, 5, 3, 5, 3);
        vgui_widget_set_gap(toolbar, 4);
        vgui_widget_set_align_items(toolbar, VGUI_ALIGN_CENTER);
        vgui_widget_set_background(toolbar, 0xFF303846);
        auto button = [&](vgui_widget* parent, const char* id, const char* tip = nullptr) {
            auto* w = vgui_widget_add(parent, VGUI_BUTTON, nullptr);
            vgui_widget_set_id(w, id);
            vgui_widget_set_padding(w, 7, 2, 7, 2);
            if (tip) vgui_widget_set_tooltip(w, tip);
            vgui_widget_on_event(w, toolbar_event, this);
            return w;
        };
        presets = {button(toolbar, "designer.preview.desktop", "1280 x 720"),
            button(toolbar, "designer.preview.tablet", "768 x 1024"),
            button(toolbar, "designer.preview.phone", "390 x 844")};
        zoom_out = button(toolbar, "designer.preview.out");
        zoom_in = button(toolbar, "designer.preview.in");
        zoom_fit = button(toolbar, "designer.preview.fit");
        zoom_actual = button(toolbar, "designer.preview.actual");
        vgui_widget_set_text(zoom_actual, "1:1");
        zoom_text = vgui_widget_add(toolbar, VGUI_LABEL, nullptr);
        vgui_widget_set_id(zoom_text, "designer.preview.zoom");
        mode = button(toolbar, "designer.preview.mode");
        if (commit_keys) timeline_toggle = button(toolbar, "designer.preview.timeline");
        transport = vgui_widget_add(panel, VGUI_PANEL, nullptr);
        vgui_widget_set_id(transport, "designer.preview.animation");
        vgui_widget_set_layout(transport, VGUI_LAYOUT_ROW);
        vgui_widget_set_size(transport, 0, 34);
        vgui_widget_set_padding(transport, 5, 3, 8, 3);
        vgui_widget_set_gap(transport, 8);
        vgui_widget_set_align_items(transport, VGUI_ALIGN_CENTER);
        vgui_widget_set_background(transport, 0xFF303846);
        play = button(transport, "designer.preview.animation.play");
        vgui_widget_set_min_size(play, 64, 0);
        restart = button(transport, "designer.preview.animation.restart");
        scrubber = vgui_widget_add(transport, VGUI_SLIDER, nullptr);
        vgui_widget_set_id(scrubber, "designer.preview.animation.seek");
        vgui_widget_set_range(scrubber, 0, 1);
        vgui_widget_set_size(scrubber, 48, 24);
        vgui_widget_set_grow(scrubber, 1);
        vgui_widget_on_event(scrubber, toolbar_event, this);
        time_text = vgui_widget_add(transport, VGUI_LABEL, nullptr);
        vgui_widget_set_id(time_text, "designer.preview.animation.time");
        canvas = vgui_widget_add(panel, VGUI_CUSTOM, nullptr);
        vgui_widget_set_id(canvas, "designer.preview.canvas");
        vgui_widget_set_grow(canvas, 1);
        vgui_widget_set_focusable(canvas, 0);
        vgui_widget_set_min_size(canvas, 8, 8);
        vgui_custom_set_draw(canvas, draw_canvas, this);
        if (commit_keys) timeline = std::make_unique<designer_gradient_timeline>(panel, strings, std::move(commit_keys),
            [this](double time, bool endpoint) { seek_playhead(time, endpoint); }, notify,
            [this] { cancel_shape_edit(); mark_canvas(); },
            [this]() -> std::optional<capture_playhead> {
                if (!run) return {};
                return capture_playhead{playback.time, playback.inspect_end};
            });
        status = vgui_widget_add(panel, VGUI_LABEL, nullptr);
        vgui_widget_set_id(status, "designer.preview.status");
        vgui_widget_set_size(status, 0, 22);
        vgui_widget_set_padding(status, 6, 0, 6, 0);
        focus_listener = vgui_context_add_focus_listener(shell, shell_focus, this);
        refresh_language();
    }

    ~impl() {
        if (focus_listener) vgui_context_remove_focus_listener(shell, focus_listener);
        blur();
        if (stop_picker.widget()) vgui_widget_destroy(stop_picker.widget());
        timeline.reset(); // Detach timeline callbacks before destroying its widget tree.
        if (panel) vgui_widget_destroy(panel);
        if (mounted) mounted->unmount();
        // Flush the private context's deferred CPU texture releases while the
        // shared renderer still exists. Do not touch an in-flight shell command.
        if (target && renderer && !vgpu_current_command_buffer(renderer))
            vgui_render_target_submit_context(target.get(), ui.get());
        target.reset(); // Waits for the GPU before retiring the FBO.
        mounted.reset();
        ui.reset(); // draw callback and recording allocator must still be alive.
    }

    void mark_canvas() { if (canvas) vgui_widget_mark_dirty(canvas); }

    void refresh_language() {
        if (!panel) return;
        vgui_widget_set_text(presets[0], strings.text("preview.desktop", "Desktop"));
        vgui_widget_set_text(presets[1], strings.text("preview.tablet", "Tablet"));
        vgui_widget_set_text(presets[2], strings.text("preview.phone", "Phone"));
        vgui_widget_set_text(zoom_out, strings.text("preview.zoom_out", "-"));
        vgui_widget_set_tooltip(zoom_out, strings.text("preview.zoom_out.tooltip", "Zoom out"));
        vgui_widget_set_text(zoom_in, strings.text("preview.zoom_in", "+"));
        vgui_widget_set_tooltip(zoom_in, strings.text("preview.zoom_in.tooltip", "Zoom in"));
        vgui_widget_set_text(zoom_fit, strings.text("preview.fit", "Fit"));
        vgui_widget_set_tooltip(zoom_fit, strings.text("preview.fit.tooltip", "Fit and center; middle-drag to pan"));
        const auto* actual_tip = strings.text("preview.zoom_actual.tooltip", "Reset to 100% and center");
        vgui_widget_set_tooltip(zoom_actual, actual_tip);
        vgui_widget_set_a11y_name(zoom_actual, actual_tip);
        vgui_widget_set_tooltip(mode, strings.text("preview.mode.tooltip", "Switch between Design and Run"));
        if (timeline) {
            vgui_widget_set_text(timeline_toggle, strings.text("timeline.title", "Gradient Timeline"));
            vgui_widget_set_tooltip(timeline_toggle, strings.text("timeline.toggle", "Show or hide this page's gradient animation tracks"));
            timeline->refresh_language();
        }
        refresh_stop_color_labels();
        vgui_widget_set_text(restart, strings.text("preview.animation.restart", "Restart"));
        vgui_widget_set_tooltip(restart, strings.text("preview.animation.restart.tooltip", "Play from the beginning"));
        vgui_widget_set_a11y_name(scrubber, strings.text("preview.animation.seek", "Animation playhead"));
        vgui_widget_set_tooltip(scrubber, strings.text("preview.animation.seek.tooltip",
            "Drag to pause and inspect this time range"));
        update_toolbar();
        update_status();
    }

    void update_status() {
        if (!status) return;
        std::string message;
        if (has_document && !is_mounted) {
            message = mount_error.message.empty() ?
                strings.text("preview.status.context_unavailable", "Preview context unavailable") : describe(mount_error);
        } else if (is_mounted) {
            if (const auto ds = mounted->diagnostics(); !ds.empty()) {
                message = describe(ds.front());
                if (ds.size() > 1) {
                    message += strings.text("preview.status.more_prefix", " (+");
                    message += std::to_string(ds.size() - 1);
                    message += strings.text("preview.status.more_suffix", " more)");
                }
            }
        }
        if (message.empty()) {
            if (render_error) {
                message = strings.text("preview.status.gpu_error_prefix", "Preview GPU: ");
                message += *render_error;
            } else {
                message = strings.text("preview.status.controls", "Middle-drag: pan  |  Wheel: zoom in Design  |  Ctrl+wheel: zoom in Run");
            }
        }
        vgui_widget_set_text(status, message.c_str());
        vgui_widget_set_tooltip(status, message.c_str());
    }

    void set_theme(designer_theme next) {
        if (theme == next) return;
        theme = next;
        if (!panel) return;
        const bool light = theme == designer_theme::light;
        vgui_widget_set_background(panel, light ? 0xFFF1F5FA : 0xFF242933);
        const auto bar = light ? 0xFFE7EFF8 : 0xFF303846;
        vgui_widget_set_background(toolbar, bar);
        vgui_widget_set_background(transport, bar);
        update_toolbar();
        mark_canvas(); // Shell paint only; the page texture and live input are unchanged.
    }

    void update_toolbar() {
        if (!panel) return;
        const bool light = theme == designer_theme::light;
        const vgui_color inactive = light ? 0xFFEEF4FC : 0xFF394454;
        const vgui_color active = light ? 0xFFCFE2FA : 0xFF3A668E;
        for (unsigned i = 0; i < presets.size(); ++i)
            vgui_widget_set_background(presets[i], i == static_cast<unsigned>(screen) ? active : inactive);
        vgui_widget_set_text(mode, run ? strings.text("preview.mode.run", "Run") :
            strings.text("preview.mode.design", "Design"));
        vgui_widget_set_background(mode, run ? (light ? 0xFFCBECDD : 0xFF33765A) : inactive);
        if (timeline_toggle) vgui_widget_set_background(timeline_toggle, timeline_open ? active : inactive);
        vgui_widget_set_background(play, inactive);
        vgui_widget_set_background(restart, inactive);
        update_transport();
        char label[24]{};
        if (fit) vgui_widget_set_text(zoom_text, strings.text("preview.zoom_auto", "Auto"));
        else {
            std::snprintf(label, sizeof(label), "%d%%", int(std::lround(zoom * 100)));
            vgui_widget_set_text(zoom_text, label);
        }
    }

    void update_transport() {
        if (!transport) return;
        const bool visible = run && timing.duration > 0;
        vgui_widget_set_visible(transport, visible);
        if (!visible) return;
        vgui_widget_set_text(play, playback.playing ? strings.text("preview.animation.pause", "Pause") :
            strings.text("preview.animation.play", "Play"));
        const auto end = playback.window_start + timing.duration;
        const auto progress = static_cast<float>(std::clamp((playback.time - playback.window_start) / timing.duration, 0., 1.));
        if (vgui_widget_get_value(scrubber) != progress) vgui_widget_set_value(scrubber, progress);
        char label[120];
        std::snprintf(label, sizeof(label), "%.2f s  |  %.2f–%.2f s", playback.time, playback.window_start, end);
        vgui_widget_set_text(time_text, label);
    }

    void follow_playhead() {
        if (!timing.loops) {
            playback.window_start = 0;
            playback.time = std::min(playback.time, timing.duration);
            if (playback.time == timing.duration) playback.playing = false;
        } else if (playback.time < playback.window_start || playback.time > playback.window_start + timing.duration) {
            // Move the ruler, not the clock: shorter loops keep their own phase.
            playback.window_start = playback.time - std::fmod(playback.time, timing.duration);
        }
    }

    void seek_playhead(double time, bool terminal) {
        cancel_stop_color();
        cancel_shape_edit();
        if (!run) set_run(true);
        playback.playing = false;
        playback.time = time;
        playback.inspect_end = terminal;
        follow_playhead();
        sample_requested = true;
        update_transport();
        mark_canvas();
    }

    void open_timeline(bool value) {
        if (!timeline || value == timeline_open) return;
        cancel_stop_color();
        cancel_pointer();
        timeline_open = value;
        timeline->set_visible(value);
        update_toolbar();
        mark_canvas();
    }

    static void toolbar_event(vgui_widget* w, const vgui_event* event, void* user) {
        auto& self = *static_cast<impl*>(user);
        if (w == self.scrubber) {
            if (event->type == VGUI_EVENT_VALUE_CHANGED || event->type == VGUI_EVENT_MOUSE_DOWN) {
                self.seek_playhead(self.playback.window_start + vgui_widget_get_value(w) * self.timing.duration, false);
            }
            return;
        }
        if (event->type != VGUI_EVENT_CLICK) return;
        if (w == self.play || w == self.restart) {
            self.cancel_shape_edit();
            self.mark_canvas();
            if (w == self.restart || (!self.playback.playing && !self.timing.loops &&
                    self.playback.time >= self.timing.duration)) {
                self.playback = {};
                self.sample_requested = true;
            } else {
                self.playback.playing = !self.playback.playing;
            }
            self.update_transport();
            return;
        }
        for (unsigned i = 0; i < self.presets.size(); ++i) if (w == self.presets[i]) {
            self.set_screen(static_cast<preset>(i));
            return;
        }
        if (w == self.mode) { self.set_run(!self.run); return; }
        if (w == self.timeline_toggle) { self.open_timeline(!self.timeline_open); return; }
        if (w == self.zoom_fit || w == self.zoom_actual) self.reset_zoom(w == self.zoom_fit);
        else {
            const auto area = rect(self.canvas);
            self.change_zoom(w == self.zoom_in ? 1.25f : .8f,
                {area.x + area.width / 2, area.y + area.height / 2});
        }
    }

    void restore_view(const view_state& view) {
        blur(); // Cancels the drag but leaves its matching release to be consumed.
        screen = view.screen;
        fit = view.fit;
        zoom = view.zoom;
        pan = {view.pan_x, view.pan_y};
        run = view.running;
        playback = view.animation;
        if (timeline) {
            timeline_open = view.timeline_open;
            timeline->set_visible(timeline_open);
        }
        timing = {};
        sample_requested = run;
        if (canvas) vgui_widget_set_focusable(canvas, run);
        if (mounted) mounted->unmount();
        is_mounted = false;
        remount = true; // Next sync mounts document values, not live run state.
        frame.reset();
        ready = false;
        dirty = true;
        update_toolbar();
        mark_canvas();
    }

    void reset_zoom(bool fit_view) {
        cancel_canvas_edit();
        fit = fit_view;
        zoom = 1.f;
        pan = {};
        update_toolbar();
        mark_canvas();
    }

    void set_screen(preset v) {
        if (v == screen || static_cast<unsigned>(v) >= presets.size()) return;
        cancel_pointer();
        screen = v;
        fit = true;
        pan = {};
        frame.reset();
        ready = false;
        dirty = true;
        update_toolbar();
        mark_canvas();
    }

    void set_run(bool value) {
        if (run == value) return;
        blur();
        run = value;
        playback = {};
        sample_requested = run;
        if (canvas) vgui_widget_set_focusable(canvas, run);
        if (!run) {
            remount = true; // Next sync restores document values, not live run state.
            ready = false;
            frame.reset();
        }
        update_toolbar();
        mark_canvas();
    }

    static void shell_focus(vgui_context*, vgui_widget* old, vgui_widget* next, void* user) {
        auto& self = *static_cast<impl*>(user);
        if (old != next) self.cancel_canvas_edit();
        if (old == self.canvas && next != self.canvas) self.blur(false);
    }

    void stop_ime() {
        if (owns_text_input) if (auto* window = SDL_GetWindowFromID(ime_window)) SDL_StopTextInput(window);
        owns_text_input = false;
        ime_window = 0;
        if (canvas) vgui_widget_set_wants_text_input(canvas, 0);
    }

    bool canvas_visible() const {
        for (auto* w = canvas; w; w = vgui_widget_get_parent(w))
            if (!vgui_widget_get_visible(w)) return false;
        return canvas != nullptr;
    }

    const node_layout* layout_of(std::string_view id) const {
        if (frame) for (const auto& node : frame->nodes) if (node.id == id) return &node;
        return nullptr;
    }

    board_transform transform_for(pixel_rect board) const {
        return {board, frame->pixel_width, frame->pixel_height, frame->view.scale};
    }

    bool geometry_is_current(pixel_rect area, pixel_rect board) const {
        const auto& g = *geometry;
        if (run || !ready || !frame || frame->revision != g.change.revision || !canvas_visible() ||
            vgui_modal_active(shell) || !same_rect(area, g.area) || !g.transform.matches(transform_for(board))) return false;
        const auto* node = layout_of(g.change.id);
        return node && node->visible && same_rect(node->rect_px, g.initial) && same_rect(node->visible_px, g.visible);
    }

    void cancel_geometry() {
        if (!geometry) return;
        geometry.reset();
        mark_canvas();
    }

    std::optional<shape_keyframe> key_at_playhead() const {
        if (!run || playback.playing || !timeline_open || !ready || !frame || !canvas_visible() ||
            vgui_modal_active(shell) || (vgui_popup_active(shell) && !stop_color_edit) ||
            vgui_context_get_open_menu(shell)) return {};
        auto key = timeline->selected_shape_key();
        if (!key || key->revision != frame->revision || key->time != playback.time ||
            key->terminal != playback.inspect_end) return {};
        const auto* node = layout_of(key->node);
        if (!node || !node->visible || !has_area(node->visible_px)) return {};
        return key;
    }

    bool shape_edit_is_current(pixel_rect area, pixel_rect board) const {
        const auto& drag = *shape_edit;
        return same_rect(area, drag.area) && key_at_playhead() == drag.key &&
            drag.transform.matches(transform_for(board)) && same_rect(layout_of(drag.key.node)->rect_px, drag.node);
    }

    void cancel_shape_edit() {
        if (!shape_edit) return;
        shape_edit.reset();
        sample_requested = true; // Resample the unmodified key, even at the same time.
        mark_canvas();
    }

    void cancel_canvas_edit() {
        cancel_geometry();
        cancel_shape_edit();
    }

    float knob_radius() const { return 6.f * vgui_context_get_ui_scale(shell); }

    void refresh_stop_color_labels() {
        if (!stop_color_title) return;
        const auto* title = strings.text("preview.stop_color", "Stop color");
        vgui_widget_set_text(stop_color_title, title);
        vgui_widget_set_a11y_name(stop_picker.widget(), title);
        vgui_widget_set_text(stop_color_ok, strings.text("common.ok", "OK"));
        vgui_widget_set_text(stop_color_cancel, strings.text("common.cancel", "Cancel"));
    }

    static void stop_color_closed(void* user, int) {
        auto& self = *static_cast<impl*>(user);
        vgui_popup_on_close(self.shell, nullptr, nullptr);
        self.stop_color_edit.reset();
        self.sample_requested = true;
        self.mark_canvas();
    }

    void cancel_stop_color() {
        if (stop_color_edit) vgui_popup_close(shell);
    }

    static void stop_color_button(vgui_widget* widget, const vgui_event* event, void* user) {
        auto& self = *static_cast<impl*>(user);
        if (event->type != VGUI_EVENT_CLICK || !self.stop_color_edit) return;
        if (widget != self.stop_color_ok || self.key_at_playhead() != self.stop_color_edit->key) {
            self.cancel_stop_color();
            return;
        }
        const auto edit = std::move(*self.stop_color_edit);
        const auto hex = self.stop_picker.value();
        // The document edit can synchronously seek and remount the preview.
        self.cancel_stop_color();
        self.timeline->edit_stop_color(edit.key, edit.index, hex);
    }

    void open_stop_color(shape_keyframe key, const shape_knob& knob, SDL_WindowID window) {
        const auto index = knob.axis;
        const auto& hex = key.stops[index].color;
        const auto color = parse_text_color(hex);
        if (!color) return;
        blur(); // Release runtime input before the shell popup takes over.
        if (!stop_picker.widget()) {
            stop_picker.set_id("designer.preview.stop_color");
            stop_picker.set_live_hex(true);
            stop_picker.build(shell);
            stop_picker.set_on_change([this](const std::string& value) {
                const auto color = parse_text_color(value);
                if (!stop_color_edit || !color || *color == stop_color_edit->color) return;
                stop_color_edit->color = *color;
                sample_requested = true;
                mark_canvas();
            });
            auto* footer = vgui_widget_add(stop_picker.widget(), VGUI_PANEL, nullptr);
            vgui_widget_set_layout(footer, VGUI_LAYOUT_ROW);
            vgui_widget_set_align_items(footer, VGUI_ALIGN_CENTER);
            vgui_widget_set_align_self(footer, VGUI_ALIGN_STRETCH);
            vgui_widget_set_padding(footer, 0, 0, 0, 0);
            vgui_widget_set_gap(footer, 6);
            vgui_widget_set_background(footer, 0);
            vgui_widget_set_border(footer, 0, 0);
            stop_color_title = vgui_widget_add(footer, VGUI_LABEL, nullptr);
            vgui_widget_set_grow(stop_color_title, 1);
            vgui_widget_set_text_align(stop_color_title, 0, .5f);
            const auto button = [&](const char* id) {
                auto* widget = vgui_widget_add(footer, VGUI_BUTTON, nullptr);
                vgui_widget_set_id(widget, id);
                vgui_widget_on_event(widget, stop_color_button, this);
                return widget;
            };
            stop_color_cancel = button("designer.preview.stop_color.cancel");
            stop_color_ok = button("designer.preview.stop_color.ok");
            refresh_stop_color_labels();
        }
        vgui_theme palette{};
        vgui_context_get_theme(shell, &palette);
        vgui_widget_set_background(stop_picker.widget(), palette.panel_bg);
        vgui_widget_set_border(stop_picker.widget(), palette.panel_border, 1);
        stop_picker.set_current(hex);
        stop_picker.set_value(hex);
        const float r = knob_radius();
        if (vgui_popup_open_anchored(shell, stop_picker.widget(), knob.at.x - r, knob.at.y - r, 2 * r, 2 * r, 4)) {
            stop_color_edit.emplace(stop_color_session{std::move(key), index, window, *color});
            vgui_popup_on_close(shell, stop_color_closed, this);
            mark_canvas();
        }
    }

    bool start_shape_edit(pixel_rect area, pixel_rect board, point p, SDL_WindowID id, float dpi, bool color_only) {
        auto key = key_at_playhead();
        if (!key) return false;
        const auto transform = transform_for(board);
        const auto node = layout_of(key->node)->rect_px;
        const float hit = knob_radius() + 2.f;
        const auto clip = intersect(area, board);
        const auto knobs = place_shape_knobs(*key, key->shape, node, transform, clip, 2 * hit + 2);
        const auto geometry = std::span(knobs).first(shape_handles(*key).size());
        const auto stops = layout_stop_knobs(*key, key->shape, node, transform, clip, knob_radius(), geometry);
        float nearest = 2 * hit * hit;
        std::optional<std::size_t> axis;
        const shape_knob* stop_knob = nullptr;
        const auto hit_knob = [&](const shape_knob& knob) {
            const float distance = distance_sq(knob.at, p);
            if (std::abs(knob.at.x - p.x) > hit || std::abs(knob.at.y - p.y) > hit || distance > nearest) return false;
            nearest = distance;
            return true;
        };
        for (const auto& knob : geometry)
            if (hit_knob(knob)) axis = knob.axis;
        for (const auto& knob : stops.knobs)
            if (knob && hit_knob(*knob)) stop_knob = &*knob;
        if (color_only) {
            if (!stop_knob) return false;
            open_stop_color(std::move(*key), *stop_knob, id);
            return true;
        }
        std::optional<gradient_stop_offset> stop;
        if (stop_knob) stop = gradient_stop_offset{stop_knob->axis, key->stops[stop_knob->axis].offset};
        if (!axis && !stop) return false;
        auto values = key->shape;
        const auto picked_axis = axis.value_or(start_x);
        double angle = !stop && is_angle(picked_axis) ? values[picked_axis] : 0.;
        double winding = angle;
        if (stop && key->kind == shape_kind::sweep) {
            const auto center = sweep_stop_center(*key, values, node, transform);
            const auto cursor = sweep_pointer_angle(*key, node, transform, center, p);
            if (!cursor) return false;
            angle = *cursor; // Use the actual grab ray so an off-center press cannot jump the stop.
        }
        shape_edit.emplace(shape_drag{std::move(*key), values, area, node, transform, p, dpi,
            picked_axis, angle, winding, stop, stops.axis});
        edit_release = id;
        if (stop) mark_canvas();
        return true;
    }

    void update_shape_edit(point p) {
        auto& drag = *shape_edit;
        const double dx = double(p.x) - drag.down.x, dy = double(p.y) - drag.down.y;
        const double slop = 3.0 * drag.dpi;
        if (drag.stop) {
            const auto i = drag.stop->index;
            const bool within_slop = dx * dx + dy * dy < slop * slop;
            double delta = 0.;
            if (drag.key.kind == shape_kind::sweep) {
                const auto center = sweep_stop_center(drag.key, drag.shape, drag.node, drag.transform);
                const auto angle = sweep_pointer_angle(drag.key, drag.node, drag.transform, center, p);
                if (!angle) return; // Keep the last usable draft while the pointer is at the center.
                drag.angle_unwrapped += std::remainder(*angle - drag.angle_cursor, 360.);
                drag.angle_cursor = *angle;
                // A near-return is a no-op, except after an actual turn across the seam.
                if (!within_slop || std::abs(drag.angle_unwrapped) > 180.)
                    delta = drag.angle_unwrapped / (drag.shape[end_angle] - drag.shape[start_angle]);
            } else if (!within_slop) {
                delta = drag.stop_axis.offset_delta(dx, dy);
            }
            const float offset = move_stop_offset(drag.key.stops, i, delta);
            if (offset != drag.stop->offset) {
                drag.stop->offset = offset;
                sample_requested = true;
                mark_canvas();
            }
            return;
        }
        auto values = drag.key.shape;
        if (dx * dx + dy * dy >= slop * slop) {
            const auto& t = drag.transform;
            const auto [xx, yx, xy, yy, tx, ty] = drag.key.affine;
            const auto a = drag.axis;
            const double ellipse = drag.key.ellipse ? 2. : 1.;
            const double sx = double(t.board.width) / t.width, sy = double(t.board.height) / t.height;
            if (a >= inner_radius) {
                // Project onto the transformed radius spoke, in shell pixels per radius unit.
                const double short_side = std::min(drag.node.width, drag.node.height) * ellipse;
                const double dir = a == inner_radius ? -1. : 1.;
                const double rx = dir * short_side * xx * sx;
                const double ry = dir * short_side * yx * drag.node.height / drag.node.width * sy;
                const double delta = (rx * dx + ry * dy) / (rx * rx + ry * ry);
                const auto& bounds = gradient_geometry_channels[a];
                // Keep the authored radii strictly ordered even after runtime float conversion.
                const double lo = a == outer_radius
                    ? std::nextafter(float(values[inner_radius]), std::numeric_limits<float>::infinity()) : bounds.min;
                const double hi = a == inner_radius ? std::nextafter(float(values[outer_radius]), 0.f) : bounds.max;
                if (std::abs(delta) > 1e-12) values[a] = std::clamp(values[a] + delta, lo, hi);
            } else {
                // Undo the board scale and brush affine before interpreting the pointer.
                const double x = dx / sx / drag.node.width, y = dy / sy / drag.node.height;
                const double det = double(xx) * yy - double(xy) * yx;
                const double u = (yy * x - xy * y) / det, v = (xx * y - yx * x) / det;
                if (is_angle(a)) {
                    // OVG measures the angle in paint pixels, not normalized widget axes.
                    // Start at the authored ray, not the shifted knob or off-center grab.
                    const double initial = values[a] * radians_per_degree, r = sweep_guide_radius(drag.node);
                    const double rx = std::cos(initial) * r + u * drag.node.width;
                    const double ry = std::sin(initial) * r + v * drag.node.height;
                    if (std::hypot(rx, ry) < r * .02) return; // At the center direction is unstable.
                    const double angle = std::atan2(ry, rx) / radians_per_degree;
                    drag.angle_unwrapped += std::remainder(angle - drag.angle_cursor, 360.);
                    drag.angle_cursor = angle;
                    const auto& bounds = gradient_geometry_channels[a];
                    values[a] = std::clamp(drag.angle_unwrapped, bounds.min, bounds.max);
                    // 0 → 360 is a valid full turn even though its rays coincide.
                    if (std::abs(float(values[end_angle]) - float(values[start_angle])) < .001f) return;
                } else {
                    // An elliptical radial inner center is stretched from its outer center.
                    const double delta[] = {u / (a == center_x ? ellipse : 1.), v};
                    for (std::size_t i = 0; i < 2; ++i) {
                        const auto& bounds = gradient_geometry_channels[a + i];
                        if (std::abs(delta[i]) > 1e-12)
                            values[a + i] = std::clamp(values[a + i] + delta[i], bounds.min, bounds.max);
                    }
                    if (drag.key.kind == shape_kind::linear && std::hypot(float(values[end_x]) - float(values[start_x]),
                            float(values[end_x + 1]) - float(values[start_x + 1])) < .0001f) return;
                }
            }
        } else if (is_angle(drag.axis)) {
            // Returning to the grab point restores the original value and winding.
            drag.angle_cursor = drag.angle_unwrapped = values[drag.axis];
        }
        if (drag.shape == values) return;
        drag.shape = values;
        sample_requested = true;
        mark_canvas();
    }

    void finish_shape_edit(point p) {
        update_shape_edit(p);
        auto draft = std::move(*shape_edit);
        cancel_shape_edit();
        edit_release.reset();
        if (draft.stop) timeline->edit_stop_offset(draft.key, draft.stop->index, draft.stop->offset);
        else if (draft.shape != draft.key.shape) timeline->edit_shape(draft.key, draft.shape);
    }

    float handle_half_size() const { return 3.f * vgui_context_get_ui_scale(shell); }

    std::optional<corner> hit_handle(const node_layout& node, board_transform transform, point p) const {
        const float radius = handle_half_size() + 2.f;
        float nearest = 2 * radius * radius;
        std::optional<corner> result;
        visible_corners(node, transform, [&](corner c, point at) {
            const float dx = p.x - at.x, dy = p.y - at.y;
            const float distance = dx * dx + dy * dy;
            if (std::abs(dx) <= radius && std::abs(dy) <= radius && distance <= nearest) {
                nearest = distance;
                result = c;
            }
        });
        return result;
    }

    void start_geometry(const node_layout& node, pixel_rect area, pixel_rect board, point p,
            SDL_WindowID id, float dpi, std::optional<corner> handle = {}) {
        const auto source = geometry_sources.find(node.id);
        if (source == geometry_sources.end()) return;
        geometry = geometry_drag{{node.id, frame->revision, {}, {}, {}, {}}, source->second,
            area, node.rect_px, node.visible_px, node.rect_px, transform_for(board), p, dpi, handle};
        // Use SDL's normal auto-capture, not the physical button state: pushed
        // events and real input follow the same down/move/up path.
        edit_release = id;
    }

    void update_geometry(point p) {
        auto& g = *geometry;
        const double dx = double(p.x) - g.down.x, dy = double(p.y) - g.down.y;
        const double slop = 3.0 * g.dpi;
        axis_edit x, y;
        if (dx * dx + dy * dy >= slop * slop) {
            const auto& t = g.transform;
            const bool left = g.handle == corner::top_left || g.handle == corner::bottom_left;
            const bool top = g.handle == corner::top_left || g.handle == corner::top_right;
            x = edit_axis(g.source.x, double(g.initial.width) / t.scale,
                dx * t.width / t.board.width / t.scale, g.handle.has_value(), left);
            y = edit_axis(g.source.y, double(g.initial.height) / t.scale,
                dy * t.height / t.board.height / t.scale, g.handle.has_value(), top);
        }
        g.change.x = x.position;
        g.change.y = y.position;
        g.change.width = x.size;
        g.change.height = y.size;
        g.ghost = g.initial;
        const double scale = g.transform.scale;
        if (x.position) g.ghost.x += float((*x.position - g.source.x.position) * scale);
        if (y.position) g.ghost.y += float((*y.position - g.source.y.position) * scale);
        if (x.size) g.ghost.width = float(*x.size * scale);
        if (y.size) g.ghost.height = float(*y.size * scale);
        mark_canvas();
    }

    void finish_geometry(point p) {
        update_geometry(p);
        auto change = std::move(geometry->change);
        cancel_geometry();
        edit_release.reset();
        if (change.x || change.y || change.width || change.height)
            commit_geometry(std::move(change)); // The host may synchronously change the document.
    }

    void cancel_pointer() {
        cancel_canvas_edit();
        if (ui && (pressed || runtime_hover)) {
            vgui_input_cancel(ui.get());
            vgui_input_mouse_move(ui.get(), -1, -1);
        }
        // SDL's normal mouse auto-capture covers drags beyond the window.
        pressed = 0;
        dragging = false;
        runtime_hover = false;
        pointer_window = 0;
    }

    void blur(bool clear_shell_focus = true) {
        if (clear_shell_focus) cancel_stop_color();
        cancel_pointer();
        stop_ime();
        if (ui) vgui_set_focus(ui.get(), nullptr);
        run_window = 0;
        if (clear_shell_focus && shell && canvas && vgui_get_focus(shell) == canvas)
            vgui_set_focus(shell, nullptr);
    }

    void sync(const document& doc, uint64_t rev, const std::filesystem::path& project_root,
            std::string_view selected) {
        if (selection != selected) {
            if (geometry && geometry->change.id != selected) cancel_geometry();
            selection.assign(selected);
            mark_canvas();
        }
        if (timeline) timeline->sync(doc, rev, selected);
        const bool same_document = has_document && !remount && rev == revision;
        if (same_document && project_root == root) return;
        std::error_code ec;
        auto abs_root = project_root.empty() ? project_root : std::filesystem::absolute(project_root, ec);
        if (ec) abs_root.clear();
        if (same_document && root == abs_root) return;
        blur();
        has_document = true;
        remount = false;
        revision = rev;
        root = std::move(abs_root);
        frame.reset();
        ready = false;
        dirty = true;
        runtime_bindings bindings;
        bindings.project_root = root;
        bindings.user = this;
        bindings.load_image = [](vgui_context* ctx, std::string_view, const std::filesystem::path& file, void* user) {
            return static_cast<impl*>(user)->active_load->load(ctx, file);
        };
        bindings.load_font = [](std::string_view key, const std::filesystem::path& file, void* user) {
            return static_cast<impl*>(user)->fonts.load(key, file);
        };
        bindings.on_action = [](std::string_view id, std::string_view action, void* user) noexcept {
            try {
                auto& self = *static_cast<impl*>(user);
                self.pending_action = std::string(action) + " [" + std::string(id) + "]";
            } catch (...) { /* The runtime callback cannot throw. */ }
        };
        image_load load;
        active_load = &load;
        fonts.begin_mount();
        mount_error = {};
        is_mounted = mounted && mounted->mount(doc, rev, bindings, &mount_error);
        active_load = nullptr;
        geometry_sources.clear();
        if (!is_mounted) {
            if (mounted) mounted->unmount();
        } else if (commit_geometry) {
            collect_geometry_sources(doc);
        }
        timing = is_mounted ? mounted->animation_timing() : animation_span{};
        if (timing.duration > 0) follow_playhead();
        else playback = {};
        sample_requested = run && timing.duration > 0;
        update_transport();
        update_status();
        mark_canvas();
    }

    void collect_geometry_sources(const document& doc) {
        // The successfully mounted document is valid. Retain just model origins
        // and bounds for children of an immediate Absolute container.
        const auto property = [](const node& n, const property_descriptor& desc) -> const value& {
            const auto it = n.props.find(desc.key);
            return it == n.props.end() ? desc.default_value : it->second;
        };
        const auto axis = [&](const node& n, std::string_view pos, std::string_view size) -> source_axis {
            const auto* p = find_property(n.type, pos);
            return {std::get<double>(property(n, *p)), p, find_property(n.type, size)};
        };
        for (const auto& [id, parent] : doc.nodes) {
            const auto* layout = find_property(parent.type, "layout");
            if (parent.children.empty() || !layout || std::get<std::string>(property(parent, *layout)) != "absolute") continue;
            for (const auto& child_id : parent.children) {
                const auto& child = doc.nodes.at(child_id);
                geometry_sources.emplace(child_id, geometry_source{axis(child, "x", "width"), axis(child, "y", "height")});
            }
        }
    }

    void set_renderer(vgpu_ctx_t* ctx) {
        if (ctx == renderer) return;
        cancel_canvas_edit();
        target.reset();
        renderer = device ? ctx : nullptr;
        if (render_error) { render_error.reset(); update_status(); }
        ready = false;
        dirty = true;
        mark_canvas();
    }

    void render(float dt) {
        if (!is_mounted || remount) return;
        // Never let the standalone submit path reuse/submit the shell's cmd.
        if (renderer && vgpu_current_command_buffer(renderer)) return;
        const float step = std::isfinite(dt) ? std::clamp(dt, 0.f, .05f) : 0.f;
        const bool layout_needed = dirty || !frame || vgui_context_paint_revision(ui.get()) != last_paint ||
            vgui_context_has_active_animation(ui.get());
        if (layout_needed) {
            vgui_update(ui.get(), step);
            auto next = mounted->layout(view_for(screen));
            if (!next) { cancel_canvas_edit(); return; }
            frame = std::move(next);
        }
        if (shape_edit) {
            vgui_layout(shell);
            const auto area = rect(canvas);
            if (!shape_edit_is_current(area, artboard(area))) cancel_shape_edit();
        }
        if (stop_color_edit && key_at_playhead() != stop_color_edit->key) cancel_stop_color();
        // Run changes only paint. A requested seek samples its exact time
        // before playback can advance it on a later frame.
        if (run && timing.duration > 0) {
            const bool ticking = !sample_requested && playback.playing && std::isfinite(dt) && dt > 0;
            if (ticking) {
                playback.inspect_end = false;
                playback.time += dt;
                follow_playhead();
                update_transport();
            }
            if (sample_requested || ticking || layout_needed) {
                std::optional<gradient_shape_preview> draft;
                if (stop_color_edit) {
                    const auto& edit = *stop_color_edit;
                    if (edit.color != parse_text_color(edit.key.stops[edit.index].color))
                        draft.emplace(gradient_shape_preview{edit.key.node,
                            {{}, {}, gradient_stop_color{edit.index, edit.color}}});
                } else if (shape_edit) {
                    const auto& edit = *shape_edit;
                    if (edit.stop && edit.stop->offset != edit.key.stops[edit.stop->index].offset) {
                        draft.emplace(gradient_shape_preview{edit.key.node, {{}, edit.stop}});
                    } else if (edit.shape != edit.key.shape) {
                        draft.emplace(gradient_shape_preview{edit.key.node, {}});
                        for (std::size_t i = 0; i < draft->draft.geometry.size(); ++i)
                            if (edit.shape[i] != edit.key.shape[i]) draft->draft.geometry[i] = float(edit.shape[i]);
                    }
                }
                mounted->seek_animations(playback.time, playback.inspect_end, draft ? &*draft : nullptr);
            }
            sample_requested = false;
        }
        if (layout_needed || vgui_context_paint_revision(ui.get()) != last_paint) {
            if (renderer && !draw_target(*frame)) {
                cancel_canvas_edit();
                ready = false;
                frame.reset();
                mark_canvas();
                return;
            }
            last_paint = vgui_context_paint_revision(ui.get());
            ready = true;
            dirty = false;
            mark_canvas();
        }
        if (timeline) timeline->show_time(run ? playback.time : 0);
        if (geometry || ime_window) vgui_layout(shell);
        if (geometry) {
            const auto area = rect(canvas);
            if (!geometry_is_current(area, artboard(area))) cancel_geometry();
        }
        if (run_window) update_ime(run_window);
    }

    bool draw_target(const layout_snapshot& next) {
        if (!cb) cb.reset(new_ctx_cb());
        if (!target) {
            vgui_render_target_desc desc;
            vgui_render_target_desc_default(&desc);
            desc.width = next.pixel_width;
            desc.height = next.pixel_height;
            // Inherit the color format and effective MSAA of the window context.
            target.reset(vgui_render_target_create(shell, renderer, cb.get(), &desc));
            if (!target) return gpu_error();
        }
        int width = 0, height = 0;
        vgui_render_target_get_size(target.get(), &width, &height);
        if ((width != next.pixel_width || height != next.pixel_height) &&
            !vgui_render_target_resize(target.get(), next.pixel_width, next.pixel_height)) return gpu_error();
        if (!vgui_render_target_submit_context(target.get(), ui.get())) return gpu_error();
        // render(float) has no host pass list. An explicit wait is required by
        // render_target's standalone contract before the shell samples it.
        vgpu_wait_idle(renderer);
        if (render_error) { render_error.reset(); update_status(); }
        return true;
    }

    bool gpu_error() {
        const std::string_view message = vgui_render_target_last_error(target.get());
        if (!render_error || *render_error != message) {
            render_error.emplace(message);
            update_status();
        }
        return false;
    }

    static float fitted_scale(pixel_rect area, viewport view, float shell_scale) {
        return std::max(.01f, std::min({shell_scale, (area.width - 24 * shell_scale) / view.width,
            (area.height - 24 * shell_scale) / view.height}));
    }

    pixel_rect artboard(pixel_rect area) const {
        const auto view = view_for(screen);
        const float shell_scale = vgui_context_get_ui_scale(shell);
        const float scale = fit ? fitted_scale(area, view, shell_scale) : zoom * shell_scale;
        const float w = std::max(1.f, std::round(view.width * scale));
        const float h = std::max(1.f, std::round(view.height * scale));
        return {std::round(area.x + (area.width - w) / 2 + pan.x * shell_scale),
            std::round(area.y + (area.height - h) / 2 + pan.y * shell_scale), w, h};
    }

    void change_zoom(float factor, point anchor) {
        if (!canvas || !std::isfinite(factor)) return;
        cancel_canvas_edit();
        const auto area = rect(canvas), old = artboard(area);
        const float shell_scale = vgui_context_get_ui_scale(shell);
        const auto view = view_for(screen);
        // Pixel rounding belongs to artboard(), not the stored zoom factor.
        const float previous = fit ? fitted_scale(area, view, shell_scale) / shell_scale : zoom;
        const float requested = std::clamp(previous * factor, .05f, 4.f);
        // A button or wheel step crossing 100% lands there exactly. A step
        // starting at 100% can continue in either direction.
        const bool crosses_actual = (previous < 1.f && requested > 1.f) || (previous > 1.f && requested < 1.f);
        zoom = crosses_actual ? 1.f : requested;
        fit = false;
        const auto next = artboard(area);
        pan.x = std::clamp(pan.x + (anchor.x - next.x - (anchor.x - old.x) / old.width * next.width) / shell_scale, -8192.f, 8192.f);
        pan.y = std::clamp(pan.y + (anchor.y - next.y - (anchor.y - old.y) / old.height * next.height) / shell_scale, -8192.f, 8192.f);
        update_toolbar();
        mark_canvas();
    }

    void draw_shape_handles(ovg_ctx_cb* cb, rvg_t* vg, pixel_rect area, pixel_rect board) const {
        const auto key = key_at_playhead();
        const auto clip = intersect(area, board);
        if (!key || !has_area(clip)) return;
        const auto t = transform_for(board);
        const auto node = layout_of(key->node)->rect_px;
        const float short_side = std::min(node.width, node.height);
        const auto& values = shape_edit ? shape_edit->shape : key->shape;
        const float h = knob_radius();
        const auto knobs = place_shape_knobs(*key, values, node, t, clip, 2 * (h + 2.f) + 2);
        const auto visible_knobs = std::span(knobs).first(shape_handles(*key).size());
        auto [axis, stops] = layout_stop_knobs(*key, values, node, t, clip, h, visible_knobs);
        const auto moving = shape_edit ? shape_edit->stop : std::nullopt;
        if (moving && stops[moving->index]) {
            auto& knob = *stops[moving->index];
            // Keep the other handles in their rows during the gesture.
            if (key->kind == shape_kind::sweep) {
                const auto center = sweep_stop_center(*key, values, node, t);
                const auto initial = sweep_stop_ray(*key, values, node, t, center, key->stops[moving->index].offset);
                const auto ray = sweep_stop_ray(*key, values, node, t, center, moving->offset);
                const float lane = (knob.at.x - knob.anchor.x) * initial.normal.x + (knob.at.y - knob.anchor.y) * initial.normal.y;
                const float shift = std::max(lane, h + 2.f - ray.inward_limit);
                knob.anchor = ray.anchor;
                knob.at = {ray.anchor.x + ray.normal.x * shift, ray.anchor.y + ray.normal.y * shift};
            } else {
                const auto anchor = axis.at(moving->offset);
                knob.at.x += anchor.x - knob.anchor.x;
                knob.at.y += anchor.y - knob.anchor.y;
                knob.anchor = anchor;
            }
        }
        constexpr unsigned ink = 0xFF202936u, accent = 0xFFB7F0FFu, blue = 0xFFFFE1A0u; // OVG: AABBGGRR.
        const auto color_for = [&](std::size_t axis) {
            return (key->kind == shape_kind::radial && (axis == center_x || axis == inner_radius)) ||
                axis == start_angle ? blue : accent;
        };
        if (cb->clip_rect && cb->get_clip_rect && cb->set_clip_rect) {
            // OVG save/restore does not preserve the scissor. Keep and intersect
            // the canvas's inherited clip just as the preview texture does.
            glm::ivec4 old{};
            cb->get_clip_rect(vg, &old);
            auto bounds = clip;
            if (old.z > 0 && old.w > 0)
                bounds = intersect(bounds, {float(old.x), float(old.y), float(old.z), float(old.w)});
            if (has_area(bounds)) {
                const int x = int(std::floor(bounds.x)), y = int(std::floor(bounds.y));
                cb->clip_rect(vg, x, y, int(std::ceil(bounds.x + bounds.width)) - x,
                    int(std::ceil(bounds.y + bounds.height)) - y);
                const auto stroke = [&](unsigned color, auto path) {
                    for (const float width : {3.f, 1.f}) {
                        cb->set_source_color(vg, width == 3.f ? ink : color);
                        cb->set_line_width(vg, width);
                        path();
                        cb->stroke(vg);
                    }
                };
                const auto segment = [&](point a, point b) { cb->move_to(vg, a.x, a.y); cb->line_to(vg, b.x, b.y); };
                if (key->kind == shape_kind::linear) {
                    stroke(accent, [&] { segment(knobs[0].at, knobs[1].at); });
                } else if (key->kind == shape_kind::radial) {
                    for (const auto radius : {outer_radius, inner_radius}) {
                        if (values[radius] <= 0.) continue;
                        stroke(color_for(radius), [&] {
                            constexpr int segments = 64;
                            for (int i = 0; i < segments; ++i) {
                                const double angle = 2 * std::numbers::pi * i / segments;
                                const auto p = shape_handle_anchor(*key, values, node, short_side, radius,
                                    std::cos(angle), std::sin(angle));
                                const auto at = t.project({p.x, p.y, 0, 0});
                                if (i == 0) cb->move_to(vg, at.x, at.y);
                                else cb->line_to(vg, at.x, at.y);
                            }
                            cb->close_path(vg);
                        });
                    }
                } else {
                    const double start = values[start_angle], span = values[end_angle] - start;
                    stroke(accent, [&] {
                        // Show the authored directed span, including reversed and full turns.
                        const int segments = std::max(1, int(std::ceil(std::abs(span) / 5.)));
                        for (int i = 0; i <= segments; ++i) {
                            const auto p = sweep_guide_point(*key, values, node, start + span * i / segments);
                            const auto at = t.project({p.x, p.y, 0, 0});
                            if (i == 0) cb->move_to(vg, at.x, at.y);
                            else cb->line_to(vg, at.x, at.y);
                        }
                    });
                    for (const auto& knob : visible_knobs.subspan(1))
                        stroke(color_for(knob.axis), [&] { segment(knobs[0].anchor, knob.anchor); });
                }
                constexpr unsigned stop_ink = 0xFFE7DECDu;
                if (key->kind == shape_kind::radial && std::ranges::any_of(stops, [](const auto& stop) { return stop.has_value(); }))
                    stroke(stop_ink, [&] { segment(axis.start, axis.end); });
                for (const auto& stop : stops)
                    if (stop) stroke(stop_ink, [&] { segment(stop->anchor, stop->at); });
                for (const auto& knob : visible_knobs)
                    if (distance_sq(knob.at, knob.anchor) > 1.f)
                        stroke(color_for(knob.axis), [&] { segment(knob.anchor, knob.at); });
                cb->set_clip_rect(vg, &old);
            }
        }
        const auto box = [&](point at, float w, float height, unsigned color) {
            fill(cb, vg, intersect({at.x - w, at.y - height, 2 * w, 2 * height}, clip), color);
        };
        for (const auto& knob : visible_knobs) {
            const auto at = knob.at;
            const auto color = color_for(knob.axis);
            if (knob.axis == center_x || knob.axis == outer_x) { // Cross for a center.
                box(at, h + 1, 2.5f, ink);
                box(at, 2.5f, h + 1, ink);
                box(at, h, 1.5f, color);
                box(at, 1.5f, h, color);
            } else { // Square for a radius, angle, or linear endpoint.
                box(at, h + 1, h + 1, ink);
                box(at, h, h, color);
                if (knob.axis != start_x && knob.axis != start_angle) box(at, h * .5f, h * .5f, ink);
            }
        }
        const auto swatch = [&](const shape_knob& stop, bool active) {
            const auto i = stop.axis;
            const auto at = stop.at;
            const float r = h * .75f;
            const bool picking = stop_color_edit && stop_color_edit->index == i;
            box(at, r + 2.f, r + 2.f, active || picking ? accent : ink);
            box(at, r + 1.f, r + 1.f, 0xFFE7DECDu);
            box(at, r, r, 0xFFAAAAAAu);
            fill(cb, vg, intersect({at.x - r, at.y - r, r, r}, clip), 0xFFE8E8E8u);
            fill(cb, vg, intersect({at.x, at.y, r, r}, clip), 0xFFE8E8E8u);
            box(at, r, r, to_ovg(picking ? stop_color_edit->color : parse_text_color(key->stops[i].color).value_or(0)));
        };
        for (const auto& stop : stops)
            if (stop && (!moving || moving->index != stop->axis)) swatch(*stop, false);
        if (moving && stops[moving->index]) swatch(*stops[moving->index], true);
    }

    static void draw_canvas(vgui_widget* w, ovg_ctx_cb* cb, rvg_t* vg,
            float x, float y, float width, float height, float, void* user) {
        const auto& self = *static_cast<impl*>(user);
        const pixel_rect area{x, y, width, height};
        const bool light = self.theme == designer_theme::light;
        const auto& grid_appearance = self.grid.colors(self.theme);
        const auto matte = self.run ? (light ? 0xFFE4ECF7u : 0xFF191D24u) : grid_appearance.background;
        fill(cb, vg, area, to_ovg(matte));
        if (!self.ready || !self.frame) return;
        const auto board = self.artboard(area);
        const auto visible = intersect(board, area);
        const pixel_rect board_frame{board.x - 1, board.y - 1, board.width + 2, board.height + 2};
        outline(cb, vg, board_frame, area, to_ovg(light ? 0xFF849BB6u : 0xFF535E6Eu), 1.f);
        if (self.run) {
            fill(cb, vg, visible, to_ovg(light ? 0xFFD7E2F0u : 0xFF262B35u));
        } else if (self.grid.visible) {
            // Guides are part of the Design backdrop. The preview texture's
            // alpha determines which parts, if any, show through the page.
            draw_grid(cb, vg, area, board, board_frame, view_for(self.screen),
                vgui_context_get_ui_scale(self.shell), self.grid, grid_appearance);
        }
        if (self.target && has_area(visible))
            vgui_custom_draw_texture_fit(w, cb, vg, vgui_render_target_texture(self.target.get()),
                board.x, board.y, board.width, board.height, VGUI_IMAGE_FIT_STRETCH, 0xFFFFFFFF);
        if (self.run) {
            self.draw_shape_handles(cb, vg, area, board);
            return;
        }
        const auto t = self.transform_for(board);
        constexpr auto accent = 0xFFFFC963u; // OVG uses 0xAABBGGRR.
        if (const auto* selected = self.layout_of(self.selection); selected && selected->visible) {
            outline(cb, vg, t.project(selected->visible_px), area, accent);
            if (self.geometry_sources.contains(selected->id)) {
                const float h = self.handle_half_size();
                visible_corners(*selected, t, [&](corner, point at) {
                    fill(cb, vg, {at.x - h - 1, at.y - h - 1, 2 * h + 2, 2 * h + 2}, 0xFF553820);
                    fill(cb, vg, {at.x - h, at.y - h, 2 * h, 2 * h}, accent);
                });
            }
        }
        if (self.geometry && self.geometry_is_current(area, board)) {
            const auto& g = *self.geometry;
            if (g.change.x || g.change.y || g.change.width || g.change.height) {
                const auto ghost = t.project(g.ghost), clip = intersect(area, board);
                fill(cb, vg, intersect(ghost, clip), 0x29FFECC4);
                outline(cb, vg, ghost, clip, 0xF5FFF2DA);
            }
        }
    }

    point preview_pixels(pixel_rect board, point p) const {
        return {(p.x - board.x) * frame->pixel_width / board.width,
            (p.y - board.y) * frame->pixel_height / board.height};
    }

    std::optional<canvas_control_drop> control_drop_at(const document& doc, std::uint64_t rev,
        const type_descriptor& type, point p) const {
        if (run || remount || !ready || !frame || frame->revision != rev || doc.nodes.size() >= max_nodes ||
            !std::isfinite(p.x) || !std::isfinite(p.y) || vgui_hit_test(shell, p.x, p.y) != canvas) return {};
        const auto area = rect(canvas), board = artboard(area), clip = intersect(area, board);
        if (!contains(clip, p)) return {};
        const auto transform = transform_for(board);
        const auto at = preview_pixels(board, p);
        // Hit the topmost visible node, then walk up to a compatible container.
        const auto hit = std::find_if(frame->nodes.rbegin(), frame->nodes.rend(), [&](const node_layout& item) {
            return item.visible && contains(item.visible_px, at);
        });
        if (hit == frame->nodes.rend()) return {};
        for (auto* w = mounted->widget(hit->id); w && w != vgui_root(ui.get()); w = vgui_widget_get_parent(w)) {
            const auto found = doc.nodes.find(vgui_widget_get_id(w));
            if (found == doc.nodes.end()) continue;
            const auto& parent = found->second;
            if (!accepts_child(*find_type(parent.type), type.key, parent.children.size())) continue;
            const auto* target = layout_of(found->first);
            if (!target || !target->visible || !contains(target->visible_px, at)) continue;
            const auto limit = intersect(clip, transform.project(target->visible_px));
            const auto property = [&](std::string_view key) -> const value& {
                const auto entry = parent.props.find(key);
                return entry == parent.props.end() ? find_property(parent.type, key)->default_value : entry->second;
            };
            if (parent.type != "panel" || std::get<std::string>(property("layout")) != "absolute")
                return canvas_control_drop{target->id, {}, limit};

            const auto number = [&](std::string_view key) { return std::get<double>(property(key)); };
            const bool scrolling = std::get<bool>(property("scrollable_x")) || std::get<bool>(property("scrollable_y"));
            const double border = scrolling ? number("border_width") : 0.;
            const auto scale = frame->view.scale;
            float sx = 0, sy = 0;
            vgui_panel_get_scroll(w, &sx, &sy);
            // The absolute layout origin is the panel's inner padding minus scroll.
            const auto left = target->rect_px.x + std::max(border, number("pad_left")) * scale - sx;
            const auto top = target->rect_px.y + std::max(border, number("pad_top")) * scale - sy;
            const auto position = [&](double offset, std::string_view axis) {
                return std::clamp(std::round(offset / scale), 0., find_property(type.key, axis)->max);
            };
            const auto x = position(at.x - left, "x"), y = position(at.y - top, "y");
            const auto bounds = transform.project({float(left + x * scale), float(top + y * scale),
                float(type.initial_width * scale), float(type.initial_height * scale)});
            return canvas_control_drop{target->id, std::pair{x, y}, intersect(limit, bounds)};
        }
        return {};
    }

    void update_ime(SDL_WindowID id) {
        if (!canvas || !frame || !run || vgui_get_focus(shell) != canvas ||
            !vgui_context_wants_text_input(ui.get())) { stop_ime(); return; }
        vgui_widget_set_wants_text_input(canvas, 1);
        if (ime_window && ime_window != id) stop_ime();
        ime_window = id;
        if (auto* window = SDL_GetWindowFromID(id)) {
            if (!SDL_TextInputActive(window)) owns_text_input = SDL_StartTextInput(window);
            int x = 0, y = 0, w = 0, h = 0;
            if (vgui_context_get_text_input_rect(ui.get(), &x, &y, &w, &h)) {
                const auto area = rect(canvas), board = artboard(area);
                const float sx = board.width / frame->pixel_width, sy = board.height / frame->pixel_height;
                const float px = std::clamp(board.x + x * sx, area.x, area.x + area.width);
                const float py = std::clamp(board.y + y * sy, area.y, area.y + area.height);
                const float ratio = pixel_ratio(window);
                SDL_Rect ime{int(std::lround(px / ratio)), int(std::lround(py / ratio)),
                    std::max(1, int(std::lround(w * sx / ratio))), std::max(1, int(std::lround(h * sy / ratio)))};
                vgui_widget_set_text_input_rect(canvas, px - area.x, py - area.y, w * sx, h * sy);
                SDL_SetTextInputArea(window, &ime, 0);
            }
        }
    }

    bool event(const SDL_Event& e) {
        if (!canvas) return false;
        switch (e.type) {
        case SDL_EVENT_WINDOW_FOCUS_LOST:
        case SDL_EVENT_WINDOW_HIDDEN:
        case SDL_EVENT_WINDOW_MINIMIZED:
            if (run_window == e.window.windowID || pointer_window == e.window.windowID ||
                edit_release == e.window.windowID || (stop_color_edit && stop_color_edit->window == e.window.windowID)) blur();
            return false;
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
            if (edit_release == e.window.windowID) cancel_canvas_edit();
            return false;
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            if (run_window == e.window.windowID && frame && !pressed) {
                vgui_input_mouse_move(ui.get(), -1, -1);
                runtime_hover = false;
            }
            return false;
        case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP:
        case SDL_EVENT_TEXT_INPUT: case SDL_EVENT_TEXT_EDITING:
            return key_event(e);
        default: break;
        }

        SDL_WindowID id;
        point p;
        switch (e.type) {
        case SDL_EVENT_MOUSE_MOTION:
            id = e.motion.windowID; p = shell_pixels(id, e.motion.x, e.motion.y); break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
            id = e.button.windowID; p = shell_pixels(id, e.button.x, e.button.y); break;
        case SDL_EVENT_MOUSE_WHEEL:
            id = e.wheel.windowID; p = shell_pixels(id, e.wheel.mouse_x, e.wheel.mouse_y); break;
        default: return false;
        }
        if ((dragging || pressed) && id != pointer_window) return false;
        if ((geometry || shape_edit) && edit_release != id) return false;
        const bool left_down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT;
        const bool left_up = e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT;
        if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
            if (!left_up || edit_release != id) return false;
            cancel_canvas_edit();
            edit_release.reset();
            return true;
        }
        vgui_layout(shell);
        const auto area = rect(canvas), board = artboard(area);
        const float dpi = pixel_ratio(SDL_GetWindowFromID(id));
        if (geometry && (dpi != geometry->dpi || !geometry_is_current(area, board))) cancel_geometry();
        if (shape_edit && (dpi != shape_edit->dpi || !shape_edit_is_current(area, board))) cancel_shape_edit();
        if (left_up && edit_release == id) {
            if (geometry) finish_geometry(p);
            else if (shape_edit) finish_shape_edit(p);
            else edit_release.reset();
            return true;
        }
        if (left_down) {
            cancel_canvas_edit();
            edit_release.reset();
        } else if ((geometry || shape_edit) && e.type == SDL_EVENT_MOUSE_MOTION) {
            if (geometry) update_geometry(p);
            else update_shape_edit(p);
            return true;
        } else if ((geometry || shape_edit) && e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            // An intervening button press must not turn a pan or shell click
            // into an edit on the later left release.
            cancel_canvas_edit();
        }
        const bool over_canvas = contains(area, p) && vgui_hit_test(shell, p.x, p.y) == canvas;
        const bool inside = over_canvas && ready && frame && contains(board, p);
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !inside && !pressed && !dragging &&
            (run_window || vgui_get_focus(shell) == canvas)) blur();

        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_MIDDLE && over_canvas && !pressed) {
            dragging = true;
            pointer_window = id;
            drag_last = p;
            return true;
        }
        if (dragging) {
            if (e.type == SDL_EVENT_MOUSE_MOTION) {
                const float scale = vgui_context_get_ui_scale(shell);
                pan.x = std::clamp(pan.x + (p.x - drag_last.x) / scale, -8192.f, 8192.f);
                pan.y = std::clamp(pan.y + (p.y - drag_last.y) / scale, -8192.f, 8192.f);
                drag_last = p;
                mark_canvas();
            } else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_MIDDLE) cancel_pointer();
            return true;
        }
        if (e.type == SDL_EVENT_MOUSE_WHEEL && over_canvas && (!run || (SDL_GetModState() & SDL_KMOD_CTRL))) {
            const float dir = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.f : 1.f;
            change_zoom(std::pow(1.15f, std::clamp(dir * e.wheel.y, -12.f, 12.f)), p);
            return true;
        }
        if (!run) {
            if (left_down && over_canvas) {
                if (ready && frame) {
                    if (geometry_sources.contains(selection)) {
                        if (const auto* selected = layout_of(selection); selected && selected->visible) {
                            if (const auto handle = hit_handle(*selected, transform_for(board), p)) {
                                start_geometry(*selected, area, board, p, id, dpi, handle);
                                return true;
                            }
                        }
                    }
                    std::string hit;
                    if (inside) {
                        const auto t = transform_for(board);
                        // Later siblings paint on top; reverse preorder visits children first.
                        for (auto it = frame->nodes.rbegin(); it != frame->nodes.rend(); ++it) {
                            if (!it->visible) continue;
                            const auto r = t.project(it->visible_px);
                            // SDL positions are rounded to shell pixels. Round the bounds
                            // in that space too so a click on a displayed leading edge
                            // cannot miss the child and start dragging its parent.
                            if (p.x >= std::round(r.x) && p.y >= std::round(r.y) &&
                                p.x < std::round(r.x + r.width) && p.y < std::round(r.y + r.height)) {
                                hit = it->id;
                                start_geometry(*it, area, board, p, id, dpi);
                                break;
                            }
                        }
                    }
                    if (select) select(std::move(hit));
                }
                return true;
            }
            return false;
        }
        if (!frame || !ready) return false;
        const bool right_down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_RIGHT;
        if ((left_down || right_down) && inside && !pressed && start_shape_edit(area, board, p, id, dpi, right_down))
            return true;
        const point at = preview_pixels(board, p);
        switch (e.type) {
        case SDL_EVENT_MOUSE_MOTION:
            if (!pressed && !inside && !runtime_hover) return false;
            vgui_input_mouse_move(ui.get(), inside || pressed ? at.x : -1, inside || pressed ? at.y : -1);
            runtime_hover = inside;
            update_ime(run_window);
            return inside || pressed;
        case SDL_EVENT_MOUSE_WHEEL:
            if (!inside) return false;
            vgui_input_mouse_move(ui.get(), at.x, at.y);
            runtime_hover = true;
            if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) vgui_input_mouse_wheel(ui.get(), -e.wheel.x, -e.wheel.y);
            else vgui_input_mouse_wheel(ui.get(), e.wheel.x, e.wheel.y);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            if (!inside || e.button.button < 1 || e.button.button > 8) return false;
            vgui_set_focus(shell, canvas);
            run_window = pointer_window = id;
            pressed |= 1u << e.button.button;
            runtime_hover = true;
            vgui_input_mouse_down(ui.get(), e.button.button, at.x, at.y, e.button.clicks);
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            if (e.button.button < 1 || e.button.button > 8 || !(pressed & (1u << e.button.button))) return false;
            pressed &= ~(1u << e.button.button);
            if (!pressed) pointer_window = 0;
            // A release over shell chrome must not activate a clipped widget.
            vgui_input_mouse_up(ui.get(), e.button.button, inside ? at.x : -1, inside ? at.y : -1, e.button.clicks);
            runtime_hover = inside;
            break;
        }
        default: return false;
        }
        update_ime(run_window);
        notify_action();
        return true;
    }

    bool key_event(const SDL_Event& e) {
        if ((geometry || shape_edit) && e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE &&
            edit_release == e.key.windowID) {
            cancel_canvas_edit();
            return true;
        }
        if (!run || !ready || !frame || vgui_get_focus(shell) != canvas) return false;
        SDL_WindowID id = e.type == SDL_EVENT_TEXT_INPUT ? e.text.windowID :
            e.type == SDL_EVENT_TEXT_EDITING ? e.edit.windowID : e.key.windowID;
        if (run_window != 0 && id != run_window) return false;
        run_window = id;
        bool handled = false;
        if (e.type == SDL_EVENT_TEXT_INPUT) handled = vgui_input_text(ui.get(), e.text.text ? e.text.text : "") != 0;
        else if (e.type == SDL_EVENT_TEXT_EDITING) handled = vgui_input_editing(ui.get(), e.edit.text ? e.edit.text : "", e.edit.start, e.edit.length) != 0;
        else {
            const int key = input_key(e.key.key);
            const bool down = e.type == SDL_EVENT_KEY_DOWN;
            handled = vgui_input_key(ui.get(), key, down, input_mods(e.key.mod)) != 0;
            if (!handled && down && key == VGUI_KEY_ESCAPE) { blur(); return true; }
            // Don't let the shell's shortcuts act on edits inside the preview.
            handled = handled || vgui_context_wants_text_input(ui.get());
        }
        update_ime(id);
        notify_action();
        return handled;
    }

    void notify_action() {
        auto text = std::exchange(pending_action, {});
        if (!text.empty() && notify)
            notify(strings.text("preview.status.action_prefix", "Action: ") + std::move(text));
    }
};

designer_preview_fonts::designer_preview_fonts() : p_(std::make_unique<impl>()) {}
designer_preview_fonts::~designer_preview_fonts() = default;

designer_preview::designer_preview(vgui_widget* parent, vgpu_device_t* gpu, font_familys_t* font,
        designer_preview_fonts& fonts,
        std::function<void(std::string)> select, std::function<void(std::string)> notify,
        std::function<void(geometry_edit)> commit_geometry, const designer_strings& strings, gradient_timeline_commit commit_keys)
    : p_(std::make_unique<impl>(parent, gpu, font, *fonts.p_, std::move(select), std::move(notify),
        std::move(commit_geometry), strings, std::move(commit_keys))) {}
designer_preview::~designer_preview() = default;
void designer_preview::sync(const document& doc, uint64_t rev, const std::filesystem::path& root, std::string_view selection) {
    p_->sync(doc, rev, root, selection);
}
void designer_preview::set_render_context(vgpu_ctx_t* ctx) { p_->set_renderer(ctx); }
void designer_preview::render(float dt) { p_->render(dt); }
bool designer_preview::handle_event(const SDL_Event& e) { return p_->event(e); }
void designer_preview::cancel_interaction() { p_->blur(); }
void designer_preview::refresh_language() { p_->refresh_language(); }
vgui_widget* designer_preview::widget() const { return p_->panel; }
void designer_preview::set_control_drop_target(const vgui_drop_target& target) {
    vgui_widget_set_drop_target(p_->canvas, &target);
}
std::optional<canvas_control_drop> designer_preview::control_drop_at(const document& doc, std::uint64_t rev,
    const type_descriptor& type, float x, float y) const {
    return p_->control_drop_at(doc, rev, type, {x, y});
}
designer_preview::view_state designer_preview::capture_view() const {
    return {p_->screen, p_->fit, p_->zoom, p_->pan.x, p_->pan.y, p_->run, p_->playback, p_->timeline_open};
}
void designer_preview::restore_view(const view_state& view) { p_->restore_view(view); }
void designer_preview::set_preset(preset v) { p_->set_screen(v); }
designer_preview::preset designer_preview::current_preset() const { return p_->screen; }
void designer_preview::reset_zoom(bool fit) { p_->reset_zoom(fit); }
void designer_preview::set_running(bool run) { p_->set_run(run); }
bool designer_preview::running() const { return p_->run; }

void designer_preview::set_theme(designer_theme theme) { p_->set_theme(theme); }

void designer_preview::set_grid_settings(designer_grid_settings settings) {
    const designer_grid_settings defaults;
    const auto valid = [](float value, float lo, float hi) { return value >= lo && value <= hi; };
    if (settings.style != designer_grid_style::lines && settings.style != designer_grid_style::dots)
        settings.style = defaults.style;
    if (!valid(settings.spacing, designer_grid_min_spacing, designer_grid_max_spacing))
        settings.spacing = defaults.spacing;
    if (settings.major_every < designer_grid_min_major_every || settings.major_every > designer_grid_max_major_every)
        settings.major_every = defaults.major_every;
    if (!valid(settings.hide_below, designer_grid_min_hide_below, designer_grid_max_hide_below))
        settings.hide_below = defaults.hide_below;
    if (!valid(settings.line_width, designer_grid_min_line_width, designer_grid_max_line_width))
        settings.line_width = defaults.line_width;
    if (settings == p_->grid) return;
    p_->grid = settings;
    if (!p_->run) p_->mark_canvas(); // Shell paint only; keep the runtime frame and input state.
}

designer_grid_settings designer_preview::grid_settings() const { return p_->grid; }
const layout_snapshot* designer_preview::snapshot() const { return p_->frame ? &*p_->frame : nullptr; }

std::optional<pixel_rect> designer_preview::artboard_rect_px() const {
    if (!p_->canvas || !p_->ready || !p_->frame) return std::nullopt;
    return p_->artboard(rect(p_->canvas));
}

std::optional<std::string> designer_preview::edit_text(std::string_view id) const {
    auto* widget = p_->mounted ? p_->mounted->widget(id) : nullptr;
    if (!widget || vgui_widget_get_kind(widget) != VGUI_EDIT) return std::nullopt;
    return std::string(vgui_edit_get_text(widget));
}

} // namespace vgui::ui_design
