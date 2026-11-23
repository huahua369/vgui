// U1 runtime contract tests: core-only, no window or GPU required.
#include "runtime.h"

#include <glm/glm.hpp>
#include <ovg_c.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#undef near // Legacy Windows memory-model macro.
#endif

namespace ui = vgui::ui_design;
namespace fs = std::filesystem;
using namespace std::string_literals;

namespace {
void check(bool ok, std::string_view message) {
    if (!ok) throw std::runtime_error(std::string(message));
}
bool near(float a, float b) { return std::fabs(a - b) < 0.01f; }
const ui::node_layout& node(const ui::layout_snapshot& snapshot, std::string_view id) {
    for (const auto& n : snapshot.nodes) if (n.id == id) return n;
    throw std::runtime_error("missing snapshot node: " + std::string(id));
}
void same_rect(const ui::pixel_rect& a, const ui::pixel_rect& b) {
    check(near(a.x,b.x) && near(a.y,b.y) && near(a.width,b.width) && near(a.height,b.height),
        "snapshot rect differs from actual VGUI layout");
}
ui::document fixture() {
    return {"root", {
        {"root", {"panel", "", {{"layout", "column"s}, {"gap", 5.0},
            {"pad_left", 10.0}, {"pad_right", 10.0}, {"pad_top", 10.0},
            {"align_items", "stretch"s}}, {"row", "field", "photo"}}},
        {"row", {"panel", "", {{"layout", "row"s}, {"height", 30.0}, {"gap", 5.0}}, {"heading", "submit"}}},
        {"heading", {"label", "", {{"text", "Title"s}, {"width", 60.0}, {"height", 30.0}}, {}}},
        {"submit", {"button", "", {{"text", "Send"s}, {"action", "form.send"s}, {"width", 80.0}, {"height", 30.0}}, {}}},
        {"field", {"edit", "", {{"text", "original"s}, {"height", 30.0}}, {}}},
        {"photo", {"image", "", {{"resource", "missing"s}, {"width", 40.0}, {"height", 20.0}}, {}}}
    }, {{"missing", "assets/not-present.png"}}};
}
void test_label_orientation() {
    ui::document doc;
    ui::diagnostic error;
    check(ui::decode_json(R"({"format":"vgui-ui-design","version":1,"root":"root","nodes":{
        "root":{"type":"panel","children":["default","vertical","horizontal"]},
        "default":{"type":"label","props":{"text":"纵向标签"}},
        "vertical":{"type":"label","props":{"text":"纵向标签","text_orientation":"vertical","x":100}},
        "horizontal":{"type":"label","props":{"text":"纵向标签","text_orientation":"horizontal","x":200}}
    }})", doc, &error), "decode labels with optional text orientation");
    std::unique_ptr<vgui_context, decltype(&vgui_destroy)> ctx(vgui_create(nullptr, 16), &vgui_destroy);
    check(bool(ctx), "create label context");
    ui::runtime rt(ctx.get());
    check(rt.mount(doc, 1, {}, &error), "mount label orientations");
    check(vgui_label_get_text_orientation(rt.widget("default")) == VGUI_LABEL_TEXT_HORIZONTAL &&
        vgui_label_get_text_orientation(rt.widget("horizontal")) == VGUI_LABEL_TEXT_HORIZONTAL &&
        vgui_label_get_text_orientation(rt.widget("vertical")) == VGUI_LABEL_TEXT_VERTICAL,
        "runtime must apply serialized orientation and preserve the old default");
    for (const float scale : {1.0f, 1.5f}) {
        const auto layout = rt.layout({320, 240, scale});
        check(layout.has_value(), "layout the vertical label");
        const auto unset = node(*layout, "default").rect_px.height;
        check(unset > 0 && near(node(*layout, "horizontal").rect_px.height, unset) &&
            near(node(*layout, "vertical").rect_px.height, unset * 4), "vertical labels must auto-size for one glyph per row");
    }
}

void test_font_properties() {
    const struct {
        const char* id;
        const char* type;
        float size;
        const char* weight;
        int expected_weight;
        const char* slant;
        int expected_slant;
        const char* color;
    } fonts[] = {
        {"heading", "label", 23.5f, "semi_bold", VGUI_FONT_WEIGHT_SEMI_BOLD, "italic", VGUI_FONT_SLANT_ITALIC, "#aB01fE"},
        {"submit", "button", 17.f, "bold", VGUI_FONT_WEIGHT_BOLD, "oblique", VGUI_FONT_SLANT_OBLIQUE, "#12345680"},
        {"field", "edit", 21.f, "medium", VGUI_FONT_WEIGHT_MEDIUM, "normal", VGUI_FONT_SLANT_NORMAL, "#987654"},
        {"caption", "label", 15.f, "regular", VGUI_FONT_WEIGHT_REGULAR, "default", VGUI_FONT_SLANT_DEFAULT, ""},
        {"inherited", "edit", 0.f, "default", VGUI_FONT_WEIGHT_DEFAULT, "default", VGUI_FONT_SLANT_DEFAULT, ""},
    };
    auto doc = ui::make_document();
    for (const auto& f : fonts) {
        doc.nodes.at(doc.root).children.push_back(f.id);
        doc.nodes.emplace(f.id, ui::node{f.type, "", {{"text", "Sample"s}, {"font_size", double(f.size)},
            {"font_weight", std::string(f.weight)}, {"font_slant", std::string(f.slant)},
            {"text_color", std::string(f.color)}}, {}});
    }
    const auto original = doc;

    std::unique_ptr<vgui_context, decltype(&vgui_destroy)> ctx(vgui_create(nullptr, 18), &vgui_destroy);
    check(bool(ctx), "create font test context");
    ui::runtime rt(ctx.get());
    check(rt.mount(doc, 1), "mount font overrides");
    check(doc == original, "mount changed font properties in document");
    for (const auto [scale, context_size] : {std::pair{1.f, 18.f}, {2.f, 20.f}, {1.f, 20.f}}) {
        // Inherited size follows both the host font and DPI; overrides follow only DPI.
        vgui_context_set_font(ctx.get(), nullptr, context_size);
        check(rt.layout({240, 180, scale}).has_value(), "layout font overrides");
        for (const auto& f : fonts) {
            const auto detail = std::string(f.id) + " at scale " + std::to_string(scale);
            auto* widget = rt.widget(f.id);
            check(widget, detail + ": missing widget");
            check(near(vgui_widget_get_font_size(widget), (f.size == 0 ? context_size : f.size) * scale),
                detail + ": incorrect effective font size");
            check(vgui_widget_get_font_weight(widget) == f.expected_weight, detail + ": incorrect font weight");
            check(vgui_widget_get_font_slant(widget) == f.expected_slant, detail + ": incorrect font slant");
        }
    }
    check(doc == original, "layout changed font properties in document");
}

struct action_log { int calls = 0; std::string id, action; };
void on_action(std::string_view id, std::string_view action, void* user) noexcept {
    auto& log = *static_cast<action_log*>(user);
    ++log.calls;
    log.id = id;
    log.action = action;
}
class temp_dir {
public:
    temp_dir() {
        const auto parent = fs::temp_directory_path();
        std::random_device random;
        for (int attempt = 0; attempt < 32; ++attempt) {
            auto candidate = parent / ("vgui-u1-" + std::to_string(random()) + "-" + std::to_string(random()));
            std::error_code ec;
            if (fs::create_directory(candidate, ec)) {
                path = std::move(candidate);
                return;
            }
            if (ec && ec != std::errc::file_exists)
                throw fs::filesystem_error("create test directory", candidate, ec);
        }
        throw std::runtime_error("could not reserve a unique test directory");
    }
    temp_dir(const temp_dir&) = delete;
    temp_dir& operator=(const temp_dir&) = delete;
    ~temp_dir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    fs::path path;
};

void test_font_families() {
    temp_dir temp;
    const auto assets = temp.path / "assets";
    fs::create_directories(assets / "directory.otf");
    const auto fixtures = fs::path(__FILE__).parent_path() / "res/fonts";
    fs::copy_file(fixtures / "DesignerSansSC.ttf", assets / "Custom.TtF");
    std::ofstream(assets / "broken.OTF").put('!');

    std::unique_ptr<font_cache_cx, decltype(&free_font_cache)> cache(new_font_cache(), free_font_cache);
    check(cache && load_font_from_path(cache.get(), (fixtures / "DejaVuSans.ttf").string().c_str(), "RuntimeSans") &&
        load_font_from_path(cache.get(), (assets / "Custom.TtF").string().c_str(), "RuntimeSansSC"), "load font fixtures");
    using font_ptr = std::unique_ptr<font_familys_t, decltype(&delete_font_family)>;
    font_ptr sans(new_font_family(cache.get(), "RuntimeSans", nullptr), delete_font_family);
    font_ptr sans_sc(new_font_family(cache.get(), "RuntimeSansSC", nullptr), delete_font_family);
    check(sans && sans->count && sans_sc && sans_sc->count, "resolve font fixtures");

    using font_call = std::pair<std::string, fs::path>;
    struct font_host {
        font_familys_t* sans;
        font_familys_t* custom;
        std::vector<font_call> calls;
    } host{sans.get(), sans_sc.get(), {}};
    ui::runtime_bindings bindings;
    bindings.project_root = assets / "..";
    bindings.user = &host;
    bindings.load_font = [](std::string_view key, const fs::path& file, void* user) -> font_familys_t* {
        auto& h = *static_cast<font_host*>(user);
        h.calls.emplace_back(key, file);
        return key == "builtin.sans" ? h.sans : key == "custom" ? h.custom : nullptr;
    };
    auto doc = ui::make_document();
    doc.resources = {{"custom", "assets/Custom.TtF"}, {"broken", "assets/broken.OTF"},
        {"directory", "assets/directory.otf"}, {"missing", "assets/missing.ttf"}, {"image", "image.png"},
        {"unused", "assets/Custom.TtF"}};
    static constexpr char text[] = "Sample iii WWW";
    const auto add = [&](const char* id, const char* type, const char* font) {
        doc.nodes.at(doc.root).children.push_back(id);
        doc.nodes.emplace(id, ui::node{type, "", {{"text", std::string(text)}, {"font_family", std::string(font)},
            {"font_weight", "bold"s}, {"font_slant", "italic"s}}, {}});
    };
    add("custom", "label", "custom");
    add("repeat", "edit", "custom");
    add("builtin", "button", "builtin.sans");
    add("builtin-sc", "label", "builtin.sans_sc");
    add("broken", "label", "broken");
    add("broken-repeat", "button", "broken");
    add("directory", "label", "directory");
    add("missing", "label", "missing");
    add("inherit", "label", "");

    std::unique_ptr<vgui_context, decltype(&vgui_destroy)> ctx(vgui_create(sans.get(), 20), &vgui_destroy);
    check(bool(ctx), "create family test context");
    ui::runtime rt(ctx.get());
    const std::vector<font_call> expected{{"custom", fs::canonical(assets / "Custom.TtF")}, {"builtin.sans", {}},
        {"builtin.sans_sc", {}}, {"broken", fs::canonical(assets / "broken.OTF")}};
    for (std::uint64_t revision : {1, 2}) {
        host.calls.clear();
        check(rt.mount(doc, revision, bindings), "mount font families");
        check(std::ranges::is_permutation(host.calls, expected), "load only referenced keys once per mount, with canonical or empty paths");
        check(vgui_widget_get_font_family(rt.widget("custom")) == sans_sc.get() &&
            vgui_widget_get_font_family(rt.widget("repeat")) == sans_sc.get() &&
            vgui_widget_get_font_family(rt.widget("builtin")) == sans.get(), "bind borrowed fonts to label, edit and button");
        check(!vgui_widget_get_font_family(rt.widget("inherit")) && rt.diagnostics().size() == 5, "only unavailable fonts are diagnosed");
        for (const auto* id : {"broken", "broken-repeat", "directory", "missing", "builtin-sc"}) {
            check(!vgui_widget_get_font_family(rt.widget(id)), "unavailable font must inherit the context family");
            const auto path = "/nodes/"s + id + "/props/font_family";
            check(std::ranges::any_of(rt.diagnostics(), [&](const auto& d) { return d.path == path && !d.message.empty(); }),
                "missing font diagnostic: " + path);
        }
    }
    auto invalid = doc;
    ui::diagnostic error;
    for (const auto* key : {"typo", "image"}) {
        invalid.nodes.at("custom").props.at("font_family") = std::string(key);
        check(!ui::validate(invalid, &error) && error.path == "/nodes/custom/props/font_family",
            "reject unknown key or non-font resource");
    }

    const auto width = [](const font_familys_t* font) { return vg_measure_text_width(font, 20, text, sizeof(text) - 1); };
    const auto measure = [](vgui_widget* widget) { return vgui_widget_measure_text_styled(widget, text, 0, 0, 0); };
    font_ptr styled_sans(font_familys_derive(sans.get(), VGUI_FONT_WEIGHT_BOLD, VGUI_FONT_SLANT_ITALIC), delete_font_family);
    font_ptr styled_sc(font_familys_derive(sans_sc.get(), VGUI_FONT_WEIGHT_BOLD, VGUI_FONT_SLANT_ITALIC), delete_font_family);
    check(styled_sans && styled_sc && !near(width(styled_sans.get()), width(styled_sc.get())), "test variants must have distinct widths");
    auto* label = rt.widget("custom");
    check(near(measure(label), width(styled_sc.get())) && near(measure(rt.widget("broken")), width(styled_sans.get())),
        "measurement derives weight/slant from override or inherited family");
    vgui_widget_set_font_family(label, sans.get());
    check(near(measure(label), width(styled_sans.get())), "family switch invalidates the widget's derived variant");
    vgui_widget_set_font_family(label, nullptr);
    check(!vgui_widget_get_font_family(label) && near(measure(label), width(styled_sans.get())), "clearing the override restores inheritance");

    vgui_context_set_font(ctx.get(), nullptr, 20);
    struct text_capture : rvg_t { font_familys_t* family = nullptr; } capture{};
    ovg_ctx_cb callbacks{};
    callbacks.add_text = [](rvg_t* vg, text_st_t*, text_style_t* style, text_box_rt*) {
        static_cast<text_capture*>(vg)->family = style->family;
    };
    vgui_custom_draw_text(rt.widget("repeat"), &callbacks, &capture, text, 0, 0, 240, 40, 0xffffffff, 0, 0);
    check(capture.family && near(width(capture.family), width(styled_sc.get())),
        "render uses the styled override even with no context family");
}

// The runtime delegates decoding to its host. Use raw RGBA fixtures to load
// actual file data and attach real CPU textures without SDL.
constexpr int image_width = 19, image_height = 13;
void write_image(const fs::path& file, bool valid = true, int width = image_width, int height = image_height) {
    const std::vector<char> pixels(std::size_t(width) * height * 4, 0x55);
    std::ofstream out(file, std::ios::binary);
    out.write(pixels.data(), valid ? pixels.size() : 1);
    out.close();
    check(bool(out), "write image fixture");
}

struct image_call {
    vgui_context* ctx;
    std::string key;
    fs::path file;
    bool operator==(const image_call&) const = default;
};
struct image_loader {
    std::vector<image_call> calls;
    std::map<std::string, std::pair<int, int>, std::less<>> sizes;
    std::vector<std::unique_ptr<vgui_texture, decltype(&vgui_texture_destroy)>> textures;
    action_log actions;

    static vgui_texture* load(vgui_context* ctx, std::string_view key, const fs::path& file, void* user) {
        auto& self = *static_cast<image_loader*>(user);
        self.calls.push_back({ctx, std::string(key), file});
        const auto size = self.sizes.find(key);
        const auto [width, height] = size == self.sizes.end() ? std::pair{image_width, image_height} : size->second;
        std::vector<char> pixels(std::size_t(width) * height * 4);
        std::ifstream in(file, std::ios::binary);
        if (!in.read(pixels.data(), pixels.size()) || in.peek() != std::char_traits<char>::eof())
            return nullptr;
        const vgui_texture_desc desc{width, height, 0, VGUI_TEXTURE_FORMAT_RGBA8, pixels.data()};
        self.textures.emplace_back(vgui_texture_create(ctx, &desc), &vgui_texture_destroy);
        check(bool(self.textures.back()), "create CPU image texture");
        return self.textures.back().get();
    }

    ui::runtime_bindings bindings(const fs::path& root) {
        ui::runtime_bindings result;
        result.project_root = root;
        result.load_image = load;
        result.user = this;
        return result;
    }
};

void add_image(ui::document& doc, const std::string& id, const std::string& key) {
    doc.nodes.at(doc.root).children.push_back(id);
    doc.nodes.emplace(id, ui::node{"image", "", {{"resource", key}}, {}});
}

void check_loaded(ui::runtime& rt, const ui::layout_snapshot& snapshot, const char* id,
    int expected_width = image_width, int expected_height = image_height) {
    check(!node(snapshot, id).image_placeholder, std::string(id) + ": unexpected placeholder");
    float width = 0, height = 0;
    // A missing set_texture call also fails this check, even if placeholder
    // flags are correct: dimensions are implicit and come from the texture.
    vgui_widget_get_intrinsic_size(rt.widget(id), &width, &height);
    check(near(width, expected_width) && near(height, expected_height), std::string(id) + ": CPU texture not attached");
}

void check_placeholder(const ui::runtime& rt, const ui::layout_snapshot& snapshot, const char* id,
    const char* property = "resource") {
    check(node(snapshot, id).image_placeholder, std::string(id) + ": missing image placeholder");
    const auto path = std::string("/nodes/") + id + "/props/" + property;
    for (const auto& d : rt.diagnostics()) if (d.path == path && !d.message.empty()) return;
    throw std::runtime_error("missing resource diagnostic for " + path);
}

void test_image_loading() {
    temp_dir temp;
    const auto project = temp.path / "project";
    const auto assets = project / "assets";
    fs::create_directories(assets);
    write_image(assets / "image.rgba");
    write_image(assets / "truncated.rgba", false);

    auto doc = ui::make_document();
    doc.resources = {{"image", "assets/image.rgba"}, {"bad", "assets/truncated.rgba"},
        {"missing", "assets/not-present.rgba"}, {"directory", "assets"}};
    add_image(doc, "photo", "image");
    add_image(doc, "repeat", "image");
    add_image(doc, "bad", "bad");
    add_image(doc, "bad-repeat", "bad");
    add_image(doc, "missing", "missing");
    add_image(doc, "directory", "directory");

    std::unique_ptr<vgui_context, decltype(&vgui_destroy)> ctx(vgui_create(nullptr, 16), &vgui_destroy);
    check(bool(ctx), "create image test context");
    image_loader loader;
    ui::runtime rt(ctx.get());
    const auto bindings = loader.bindings(assets / "..");
    const std::vector<image_call> expected = {{ctx.get(), "image", fs::canonical(assets / "image.rgba")},
        {ctx.get(), "bad", fs::canonical(assets / "truncated.rgba")}};
    check(rt.mount(doc, 1, bindings), "mount image fixtures");
    check(std::ranges::is_permutation(loader.calls, expected), "loader should get only canonical regular files, once per key");
    loader.textures.clear(); // The mounted images must keep their texture alive.
    auto snapshot = rt.layout({240, 180, 1});
    check(bool(snapshot), "layout loaded images");
    check_loaded(rt, *snapshot, "photo");
    check_loaded(rt, *snapshot, "repeat");
    for (auto id : {"bad", "bad-repeat", "missing", "directory"}) check_placeholder(rt, *snapshot, id);
    check(rt.diagnostics().size() == 4, "only unavailable images should report diagnostics");

    loader.calls.clear();
    auto invalid = doc;
    invalid.resources.at("image") = "../outside.rgba";
    ui::diagnostic error;
    auto* photo = rt.widget("photo");
    check(!rt.mount(invalid, 2, bindings, &error) && error.path == "/resources/image",
        "reject parent-relative resource during mount validation");
    check(loader.calls.empty() && rt.widget("photo") == photo, "invalid resource path must not load or replace tree");

    write_image(assets / "truncated.rgba");
    check(rt.mount(doc, 3, bindings), "remount after repairing image file");
    check(std::ranges::is_permutation(loader.calls, expected), "remount must reload both successful and failed resource keys");
    snapshot = rt.layout({240, 180, 1});
    check(bool(snapshot), "layout repaired image");
    check_loaded(rt, *snapshot, "bad");
    check_loaded(rt, *snapshot, "bad-repeat");
    check(rt.diagnostics().size() == 2, "remount should clear repaired resource diagnostics");
}

void test_button_atlases() {
    temp_dir temp;
    const auto assets = temp.path / "assets";
    fs::create_directory(assets);
    constexpr int width = 256, height = 96;
    write_image(assets / "skin.rgba", true, width, height);
    write_image(assets / "odd.rgba", true, width, height - 1);
    write_image(assets / "bad.rgba", false);

    auto doc = ui::make_document();
    doc.nodes.at(doc.root).props["layout"] = "column"s;
    doc.resources = {{"skin", "assets/skin.rgba"}, {"odd", "assets/odd.rgba"}, {"bad", "assets/bad.rgba"},
        {"missing", "assets/not-present.rgba"}, {"unused", "assets/skin.rgba"}};
    const auto add_button = [&](const char* id, const char* key = nullptr) {
        auto button = ui::node{"button", "", {{"text", "Send"s}, {"action", "form.send"s}}, {}};
        if (key) button.props.emplace("atlas", std::string(key));
        doc.nodes.at(doc.root).children.push_back(id);
        doc.nodes.emplace(id, std::move(button));
    };
    add_button("skinned", "skin");
    add_image(doc, "photo", "skin");
    add_button("repeat", "skin");
    add_button("plain");
    add_button("odd", "odd");
    add_image(doc, "odd-photo", "odd"); // Still a valid image after rejection as an atlas.
    add_button("bad", "bad");
    add_button("missing", "missing");
    add_button("with-icon");
    auto& icon = doc.nodes.at("with-icon").props;
    icon.emplace("icon", "skin"s);
    icon.emplace("icon_width", 20.0);
    icon.emplace("icon_height", 16.0);
    icon.emplace("icon_position", "right"s);
    icon.emplace("icon_gap", 5.0);
    add_button("missing-icon");
    doc.nodes.at("missing-icon").props.emplace("icon", "missing"s);

    std::unique_ptr<vgui_context, decltype(&vgui_destroy)> ctx(vgui_create(nullptr, 16), &vgui_destroy);
    check(bool(ctx), "create button image context");
    image_loader loader;
    loader.sizes = {{"skin", {width, height}}, {"odd", {width, height - 1}}};
    auto bindings = loader.bindings(assets / "..");
    bindings.on_action = [](std::string_view id, std::string_view action, void* user) noexcept {
        on_action(id, action, &static_cast<image_loader*>(user)->actions);
    };
    ui::runtime rt(ctx.get());
    check(rt.mount(doc, 1, bindings), "mount button atlas fixtures");
    const std::vector<image_call> expected = {{ctx.get(), "skin", fs::canonical(assets / "skin.rgba")},
        {ctx.get(), "odd", fs::canonical(assets / "odd.rgba")}, {ctx.get(), "bad", fs::canonical(assets / "bad.rgba")}};
    check(std::ranges::is_permutation(loader.calls, expected), "button atlases, icons and images must share one canonical load per key");
    loader.textures.clear();
    const auto snapshot = rt.layout({400, 600, 1});
    check(bool(snapshot), "layout button atlas fixtures");
    check_loaded(rt, *snapshot, "photo", width, height);
    check_loaded(rt, *snapshot, "odd-photo", width, height - 1);
    check(rt.diagnostics().size() == 4, "only unavailable button atlases and icons should report diagnostics");
    for (auto id : {"odd", "bad", "missing"}) check_placeholder(rt, *snapshot, id, "atlas");
    check(std::ranges::any_of(rt.diagnostics(), [](const auto& d) { return d.path == "/nodes/missing-icon/props/icon"; }),
        "missing button icon reports its own property path");
    for (auto id : {"skinned", "repeat", "plain", "with-icon", "missing-icon"})
        check(!node(*snapshot, id).image_placeholder, std::string(id) + ": unexpected button placeholder");

    float plain_w, plain_h;
    vgui_widget_get_intrinsic_size(rt.widget("plain"), &plain_w, &plain_h);
    float icon_w, icon_h;
    vgui_widget_get_intrinsic_size(rt.widget("with-icon"), &icon_w, &icon_h);
    check(near(icon_w, plain_w + 25), "explicit icon width and gap contribute to button sizing");
    for (auto id : {"skinned", "repeat", "plain", "odd", "bad", "missing"}) {
        auto* button = rt.widget(id);
        check(button && vgui_widget_get_kind(button) == VGUI_BUTTON && std::string_view(vgui_widget_get_text(button)) == "Send",
            std::string(id) + ": atlas changed the button kind or text");
        float w, h;
        vgui_widget_get_intrinsic_size(button, &w, &h);
        const auto skinned = std::string_view(id) == "skinned" || std::string_view(id) == "repeat";
        check(near(w, skinned ? width / 2.f : plain_w) && near(h, skinned ? height / 2.f : plain_h),
            std::string(id) + ": expected one atlas frame or normal button sizing");

        const auto& rect = node(*snapshot, id).rect_px;
        const float x = rect.x + rect.width / 2, y = rect.y + rect.height / 2;
        const auto prior_calls = loader.actions.calls;
        vgui_input_mouse_move(ctx.get(), x, y);
        vgui_input_mouse_down(ctx.get(), 1, x, y, 0);
        check(loader.actions.calls == prior_calls, "atlas button fired on pointer down");
        vgui_input_mouse_up(ctx.get(), 1, x, y, 0);
        check(loader.actions.calls == prior_calls + 1 && loader.actions.id == id && loader.actions.action == "form.send",
            std::string(id) + ": atlas changed button action routing");
    }
}

void test_button_icon_sources() {
    temp_dir temp;
    constexpr int width = 96, height = 64;
    const auto file = temp.path / "icons.rgba";
    write_image(file, true, width, height);
    ui::document doc;
    ui::diagnostic error;
    check(ui::decode_json(R"({"format":"vgui-ui-design","version":1,"root":"root",
        "resources":{"icons":"icons.rgba"},"nodes":{
            "root":{"type":"panel","props":{"layout":"column"},"children":["first","second","uncropped"]},
            "first":{"type":"button","props":{"icon":"icons","icon_source_x":4,"icon_source_y":8,
                "icon_source_width":24,"icon_source_height":20}},
            "second":{"type":"button","props":{"icon":"icons","icon_source_x":80,"icon_source_y":40,
                "icon_source_height":72,"icon_width":22}},
            "uncropped":{"type":"button","props":{"icon":"icons"}}
        }})", doc, &error), "decode three buttons that share one icon texture");
    auto invalid = doc;
    invalid.nodes.at("first").props["icon_source_x"] = -1.;
    check(!ui::validate(invalid, &error) && error.path == "/nodes/first/props/icon_source_x", "reject negative source coordinates");

    std::unique_ptr<ovg_ctx_cb, decltype(&free_ctx_cb)> cb(new_ctx_cb(), &free_ctx_cb);
    std::unique_ptr<rvg_t, void (*)(rvg_t*)> vg(cb->new_rvg(cb->ac), cb->destroy_rvg);
    std::unique_ptr<vgui_context, decltype(&vgui_destroy)> ctx(vgui_create(nullptr, 16), &vgui_destroy);
    check(bool(ctx), "create icon crop context");
    image_loader loader;
    loader.sizes = {{"icons", {width, height}}};
    ui::runtime rt(ctx.get());
    check(rt.mount(doc, 1, loader.bindings(temp.path)) && rt.diagnostics().empty(), "mount icon crop fixture");
    check(loader.calls == std::vector<image_call>{{ctx.get(), "icons", fs::canonical(file)}}, "shared icon texture must load only once");
    loader.textures.clear();

    static std::vector<ovg_image_r> draws;
    static int uploads = 0;
    uploads = 0;
    cb->image_update = [](rvg_t*, vg_image_t* img, vg_image_desc_t*) { img->id = ++uploads; };
    cb->image_destroy = [](rvg_t*, vg_image_t*) {};
    cb->add_image = [](rvg_t*, ovg_image_r* img) { draws.push_back(*img); };
    const auto verify_draw = [&](float scale, glm::ivec4 first, glm::ivec2 first_size) {
        check(bool(rt.layout({240, 300, scale})), "lay out icon fixture");
        vg->width = int(240 * scale);
        vg->height = int(300 * scale);
        cb->clear(vg.get());
        draws.clear();
        vgui_draw(ctx.get(), cb.get(), vg.get());
        check(draws.size() == 3 && uploads == 1 && draws[0].img == draws[1].img && draws[0].img == draws[2].img,
            "buttons must draw different regions of the same uploaded image");
        check(draws[0].rc == first && draws[1].rc == glm::ivec4(80, 40, 16, 24) &&
            draws[2].rc == glm::ivec4(0, 0, width, height), "source pixels, edge clipping, and full-texture fallback");
        check(glm::ivec2(draws[0].dst.z, draws[0].dst.w) == first_size &&
            glm::ivec2(draws[1].dst.z, draws[1].dst.w) == glm::ivec2(22 * scale, 24 * scale) &&
            glm::ivec2(draws[2].dst.z, draws[2].dst.w) == glm::ivec2(width * scale, height * scale),
            "auto icon size follows the source while explicit width and DPI affect only the destination");
        float first_w = 0, legacy_w = 0;
        vgui_widget_get_intrinsic_size(rt.widget("first"), &first_w, nullptr);
        vgui_widget_get_intrinsic_size(rt.widget("uncropped"), &legacy_w, nullptr);
        check(near(legacy_w - first_w, width * scale - first_size.x), "button measurement uses the same cropped icon size as paint");
    };
    verify_draw(1, {4, 8, 24, 20}, {24, 20});
    verify_draw(2, {4, 8, 24, 20}, {48, 40});
    vgui_button_set_icon_source(rt.widget("first"), 1000, 1000, 32, 32);
    verify_draw(2, {95, 63, 1, 1}, {2, 2});
    vgui_button_set_icon_source(rt.widget("first"), 0, 0, 0, 0);
    verify_draw(1, {0, 0, width, height}, {width, height});
    rt.unmount();
    cb->clear(vg.get());
    vgui_draw(ctx.get(), cb.get(), vg.get());
}

void test_collection_icons_and_actions() {
    temp_dir temp;
    const auto assets = temp.path / "assets";
    fs::create_directory(assets);
    write_image(assets / "shared.rgba");
    write_image(assets / "accent.rgba", true, image_width + 1);
    write_image(assets / "bad.rgba", false);

    ui::document doc;
    ui::diagnostic error;
    const bool decoded = ui::decode_json(R"({"format":"vgui-ui-design","version":1,"root":"root",
        "resources":{"shared":"assets/shared.rgba","accent":"assets/accent.rgba",
            "missing":"assets/missing.rgba","bad":"assets/bad.rgba"},"nodes":{
        "root":{"type":"panel","children":["rows","tree","menu","bar","photo"]},
        "rows":{"type":"list","props":{"width":200,"height":125,"scrollable_y":false,"items":[
            {"text":"First","icon":"shared","buttons":[{"icon":"accent","action":"rows.left"},
                {"icon":"shared"},{},{"icon":"missing","action":"rows.right"}]},
            {"text":"Other","selected":true,"buttons":[{"action":"rows.other"}]},
            {"text":"Disabled","enabled":false,"buttons":[{"action":"rows.disabled"}]},"Plain"]}},
        "tree":{"type":"tree","props":{"x":215,"width":200,"height":125,"scrollable_x":false,"scrollable_y":false,
            "items":[{"text":"Branch","expanded":true,"icon":"accent","buttons":[{"action":"tree.root"}],"children":[
                {"text":"Leaf","selected":true,"icon":"missing","buttons":[{"icon":"shared","action":"tree.leaf.left"},
                    {"icon":"bad","action":"tree.leaf.right"}]},
                {"text":"Disabled","enabled":false,"buttons":[{"action":"tree.disabled"}]}]}]}},
        "menu":{"type":"menu","props":{"y":140,"width":150,"height":32,"close_on_activate":false,"items":[
            {"text":"Pin","icon":"shared","checked":true,"action":"tools.pin"},{"separator":true},
            {"text":"More","icon":"accent","children":[{"text":"Export","icon":"bad","action":"tools.export"}]}]}},
        "bar":{"type":"menubar","props":{"x":215,"y":140,"width":200,"height":32,"items":[
            {"text":"File","children":[{"text":"Open","icon":"accent","action":"tools.open"},
                {"text":"Print","icon":"missing"}]}]}},
        "photo":{"type":"image","props":{"y":185,"resource":"shared"}}
    }})", doc, &error);
    check(decoded, "decode decorated collections: " + error.path + " " + error.message);
    const auto original = doc;
    std::unique_ptr<ovg_ctx_cb, decltype(&free_ctx_cb)> cb(new_ctx_cb(), &free_ctx_cb);
    std::unique_ptr<rvg_t, void (*)(rvg_t*)> vg(cb->new_rvg(cb->ac), cb->destroy_rvg);
    // VGUI borrows the drawing callbacks through context destruction.
    std::unique_ptr<vgui_context, decltype(&vgui_destroy)> ctx(vgui_create(nullptr, 16), &vgui_destroy);
    check(bool(ctx), "create collection icon context");
    vgui_style style{};
    vgui_context_get_style(ctx.get(), &style);
    style.list.row_height = 24.f;
    style.tree.row_height = 26.f;
    vgui_context_set_style(ctx.get(), &style);
    image_loader loader;
    loader.sizes = {{"accent", {image_width + 1, image_height}}};
    auto bindings = loader.bindings(assets / "..");
    bindings.on_action = [](std::string_view id, std::string_view action, void* user) noexcept {
        on_action(id, action, &static_cast<image_loader*>(user)->actions);
    };
    ui::runtime rt(ctx.get());
    const bool mounted = rt.mount(doc, 1, bindings, &error);
    check(mounted, "mount decorated collections: " + error.path + " " + error.message);
    const std::vector<image_call> expected = {{ctx.get(), "shared", fs::canonical(assets / "shared.rgba")},
        {ctx.get(), "accent", fs::canonical(assets / "accent.rgba")}, {ctx.get(), "bad", fs::canonical(assets / "bad.rgba")}};
    check(std::ranges::is_permutation(loader.calls, expected), "collection icons and images share one canonical load per key");
    const auto snap = rt.layout({460, 240, 1});
    check(bool(snap), "layout decorated collections");
    check_loaded(rt, *snap, "photo");
    for (const auto* id : {"rows", "tree", "menu", "bar"})
        check(!node(*snap, id).image_placeholder, std::string(id) + ": a missing item icon is not a widget placeholder");
    const std::vector<std::string> paths = {"/nodes/rows/props/items/0/buttons/3/icon", "/nodes/tree/props/items/0/children/0/icon",
        "/nodes/tree/props/items/0/children/0/buttons/1/icon", "/nodes/menu/props/items/2/children/0/icon",
        "/nodes/bar/props/items/0/children/1/icon"};
    check(rt.diagnostics().size() == paths.size(), "one diagnostic per unavailable icon use");
    for (const auto& path : paths)
        check(std::ranges::any_of(rt.diagnostics(), [&](const auto& d) { return d.path == path && !d.message.empty(); }),
            "missing icon diagnostic at " + path);

    auto* rows = rt.widget("rows");
    const auto first = vgui_list_get_item_at(rows, 0), other = vgui_list_get_item_at(rows, 1);
    const auto disabled_row = vgui_list_get_item_at(rows, 2), plain = vgui_list_get_item_at(rows, 3);
    const auto* shared = vgui_list_get_item_icon(rows, first);
    const auto* accent = vgui_list_get_item_button_icon(rows, first, 0);
    check(shared && accent && shared != accent && vgui_list_get_count(rows) == 4 &&
        std::string_view(vgui_list_get_item_text(rows, first)) == "First" && !vgui_list_get_item_icon(rows, plain) &&
        vgui_list_get_item_buttons(rows, plain) == 0, "decorate object rows without changing text or the string shorthand");
    check(vgui_list_get_item_buttons(rows, first) == 4 && vgui_list_get_item_button_icon(rows, first, 1) == shared &&
        !vgui_list_get_item_button_icon(rows, first, 2) && !vgui_list_get_item_button_icon(rows, first, 3),
        "keep native button order/count and leave an unavailable button icon unset");

    auto* tree = rt.widget("tree");
    const auto branch = vgui_tree_get_root(tree, 0), leaf = vgui_tree_get_child(tree, branch, 0);
    const auto disabled_leaf = vgui_tree_get_child(tree, branch, 1);
    check(vgui_tree_get_node_count(tree) == 3 && vgui_tree_get_item_icon(tree, branch) == accent &&
        !vgui_tree_get_item_icon(tree, leaf) && vgui_tree_get_item_buttons(tree, leaf) == 2 &&
        vgui_tree_get_item_button_icon(tree, leaf, 0) == shared && !vgui_tree_get_item_button_icon(tree, leaf, 1),
        "project tree icons and preserve the child after failed loads");

    auto* host = rt.widget("menu");
    const auto heading = vgui_menubar_get_item_at(host, 0);
    auto* menu = vgui_menubar_get_menu(host, heading);
    const auto pin = vgui_menu_get_item_at(menu, 0), more = vgui_menu_get_item_at(menu, 2);
    auto* submenu = vgui_menu_get_submenu(menu, more);
    const auto export_item = vgui_menu_get_item_at(submenu, 0);
    auto* bar = rt.widget("bar");
    const auto file = vgui_menubar_get_item_at(bar, 0);
    auto* popup = vgui_menubar_get_menu(bar, file);
    const auto open = vgui_menu_get_item_at(popup, 0);
    check(vgui_menu_get_item_icon(menu, pin) == shared && vgui_menu_get_item_icon(menu, more) == accent &&
        !vgui_menu_get_item_icon(submenu, export_item) && vgui_menu_get_item_icon(popup, open) == accent &&
        !vgui_menu_get_item_icon(popup, vgui_menu_get_item_at(popup, 1)), "attach icons to the actual popup entries");

    // Use the native trailing button geometry (no scrollbars, 1x, LTR).
    const auto click = [&](std::string_view id, int row, int index, int count, std::string_view expected_action) {
        const auto& rect = node(*snap, id).rect_px;
        const auto padding_x = id == "tree" ? style.tree.padding_x : style.list.padding_x;
        const auto padding_y = id == "tree" ? style.tree.padding_y : style.list.padding_y;
        const auto row_h = id == "tree" ? style.tree.row_height : style.list.row_height;
        const float size = std::clamp(row_h - 8.f, 10.f, 18.f);
        const float x = rect.x + rect.width - std::max(1.f, padding_x) - 4.f - size / 2 - (count - 1 - index) * (size + 4.f);
        const float y = rect.y + std::max(1.f, padding_y) + (row + 0.5f) * row_h;
        const auto prior_calls = loader.actions.calls;
        vgui_input_mouse_move(ctx.get(), x, y);
        vgui_input_mouse_down(ctx.get(), 1, x, y, 0);
        vgui_input_mouse_up(ctx.get(), 1, x, y, 0);
        check(expected_action.empty() ? loader.actions.calls == prior_calls :
            loader.actions.calls == prior_calls + 1 && loader.actions.id == id && loader.actions.action == expected_action,
            std::string(id) + " row " + std::to_string(row) + " button " + std::to_string(index) + ": action mismatch");
    };
    click("rows", 0, 0, 4, "rows.left");
    click("rows", 0, 1, 4, ""); // Icon-only, no action.
    click("rows", 0, 2, 4, ""); // Native fallback button without an action.
    click("rows", 0, 3, 4, "rows.right"); // Missing icon, action still works.
    click("rows", 1, 0, 1, "rows.other");
    click("rows", 2, 0, 1, "");
    vgui_list_set_item_enabled(rows, disabled_row, 1);
    click("rows", 2, 0, 1, "rows.disabled"); // Same hit, now enabled.
    check(vgui_list_get_selected_count(rows) == 1 && vgui_list_get_selected(rows, 0) == other,
        "list buttons do not change selection");
    click("tree", 0, 0, 1, "tree.root");
    click("tree", 1, 0, 2, "tree.leaf.left");
    click("tree", 1, 1, 2, "tree.leaf.right");
    click("tree", 2, 0, 1, "");
    vgui_tree_set_item_enabled(tree, disabled_leaf, 1);
    click("tree", 2, 0, 1, "tree.disabled");
    check(vgui_tree_get_selected_count(tree) == 1 && vgui_tree_get_selected(tree, 0) == leaf &&
        vgui_tree_get_item_expanded(tree, branch), "tree buttons do not change selection or expansion");

    const auto press = [&](vgui_key key) { vgui_input_key(ctx.get(), key, 1, 0); vgui_input_key(ctx.get(), key, 0, 0); };
    const auto activate = [&](vgui_widget* popup_owner, vgui_item_id item, std::string_view id, std::string_view action) {
        const auto prior_calls = loader.actions.calls;
        vgui_menu_set_cursor(popup_owner, item);
        press(VGUI_KEY_ENTER);
        check(loader.actions.calls == prior_calls + 1 && loader.actions.id == id && loader.actions.action == action,
            "decorated popup action does not route to its document owner");
    };
    check(vgui_menubar_open_item(host, heading), "open decorated standalone popup");
    activate(menu, pin, "menu", "tools.pin");
    vgui_menu_set_cursor(menu, more);
    press(VGUI_KEY_RIGHT);
    check(vgui_menu_is_open(submenu), "open decorated submenu");
    activate(submenu, export_item, "menu", "tools.export");
    vgui_menubar_close(host);
    check(vgui_menubar_open_item(bar, file), "open decorated menubar popup");
    activate(popup, open, "bar", "tools.open");
    check(doc == original, "row buttons and menu icons/actions leave the document unchanged");

    // Exercise upload/retirement without a renderer. Unlike shared, accent
    // has no ordinary image widget to keep it alive after the loader drops it.
    static int retired = 0;
    retired = 0;
    cb->image_update = [](rvg_t*, vg_image_t* img, vg_image_desc_t* desc) { img->id = desc->width; };
    cb->image_destroy = [](rvg_t*, vg_image_t*) { ++retired; };
    vgui_draw(ctx.get(), cb.get(), vg.get());
    loader.textures.clear();
    cb->clear(vg.get());
    vgui_draw(ctx.get(), cb.get(), vg.get());
    check(retired == 0 && vgui_texture_get_width(accent) == image_width + 1,
        "mounted collection must retain icons after the loader releases them");
    rt.unmount();
    cb->clear(vg.get());
    vgui_draw(ctx.get(), cb.get(), vg.get());
    check(retired == 2, "unmount releases both uploaded resource textures");
}

// On Windows an ordinary account can create directory junctions without
// Developer Mode. Like directory symlinks, canonical() must follow them.
std::error_code make_directory_link(const fs::path& target, const fs::path& link) {
#ifndef _WIN32
    std::error_code ec;
    fs::create_directory_symlink(target, link, ec);
    return ec;
#else
    const auto native = fs::canonical(target).native();
    const auto substitute = native.starts_with(L"\\\\?\\") ? L"\\??\\" + native.substr(4) : L"\\??\\" + native;
    // Mount-point reparse data: offsets are relative to names, lengths are
    // in bytes without terminators, and data_size excludes the first 8 bytes.
    struct {
        DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
        WORD data_size, reserved = 0;
        WORD substitute_offset = 0, substitute_size, print_offset, print_size = 0;
        wchar_t names[4096]{};
    } data;
    check(substitute.size() + 2 <= 4096, "junction target name too long");
    data.substitute_size = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    data.print_offset = data.substitute_size + sizeof(wchar_t);
    data.data_size = 8 + data.print_offset + sizeof(wchar_t);
    substitute.copy(data.names, substitute.size());
    fs::create_directory(link);
    const HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return {static_cast<int>(GetLastError()), std::system_category()};
    DWORD bytes = 0;
    const auto ok = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &data, data.data_size + 8,
        nullptr, 0, &bytes, nullptr);
    const auto err = ok ? 0 : GetLastError();
    CloseHandle(handle);
    return {static_cast<int>(err), std::system_category()};
#endif
}

int test_link_paths() {
    temp_dir temp;
    const auto project = fs::canonical(temp.path) / "project";
    const auto inside = project / "assets";
    const auto outside = project.parent_path() / "project-outside";
    fs::create_directories(inside);
    fs::create_directory(outside);
    write_image(inside / "image.rgba");
    write_image(outside / "image.rgba");
    for (const auto& [target, link] : {std::pair{inside, project / "local"}, std::pair{outside, project / "escape"}}) {
        const auto ec = make_directory_link(target, link);
        if (ec == std::errc::operation_not_supported || ec == std::errc::function_not_supported ||
            ec == std::errc::operation_not_permitted || ec == std::errc::permission_denied
#ifdef _WIN32
            || ec == std::error_code(ERROR_INVALID_FUNCTION, std::system_category())
#endif
            ) {
            std::printf("SKIP: cannot create directory links: %s\n", ec.message().c_str());
            return 77;
        }
        if (ec) throw fs::filesystem_error("create directory link", target, link, ec);
        check(fs::canonical(link / "image.rgba") == fs::canonical(target / "image.rgba"),
            "test link must resolve to an existing image");
    }

    auto doc = ui::make_document();
    doc.resources = {{"local", "local/image.rgba"}, {"escape", "escape/image.rgba"}};
    add_image(doc, "local", "local");
    add_image(doc, "escape", "escape");
    std::unique_ptr<vgui_context, decltype(&vgui_destroy)> ctx(vgui_create(nullptr, 16), &vgui_destroy);
    check(bool(ctx), "create link test context");
    image_loader loader;
    ui::runtime rt(ctx.get());
    check(rt.mount(doc, 1, loader.bindings(project)), "mount linked image fixtures");
    check(loader.calls == std::vector<image_call>{{ctx.get(), "local", fs::canonical(inside / "image.rgba")}},
        "load internal link by canonical path but never call host for escape to similarly named sibling");
    auto snapshot = rt.layout({240, 180, 1});
    check(bool(snapshot), "layout linked images");
    check_loaded(rt, *snapshot, "local");
    check_placeholder(rt, *snapshot, "escape");
    check(rt.diagnostics().size() == 1, "only the escaped image should report a diagnostic");
    return 0;
}

void test_declarative_widgets() {
    ui::document doc;
    ui::diagnostic error;
    const bool decoded = ui::decode_json(R"({"format":"vgui-ui-design","version":1,"root":"root","nodes":{
        "root":{"type":"panel","children":["field","photo","radio","switch","slider","progress","rule",
            "rows","tree","menu","bar","split","tabs","dock"]},
        "heading":{"type":"label","props":{"text":"Active page"}},
        "submit":{"type":"button","props":{"text":"Send","button_style":"gradient","tooltip":"Send form"}},
        "field":{"type":"edit","props":{"text":"<b>literal</b>\nsecond line","width":180,"height":50,
            "multiline":true,"readonly":true,"selection_color":"#12345680"}},
        "photo":{"type":"image"},
        "check":{"type":"checkbox","props":{"text":"Choice","checked":true}},
        "radio":{"type":"radio","props":{"text":"Option","checked":true}},
        "switch":{"type":"switch","props":{"text":"Switch","checked":true}},
        "slider":{"type":"slider","props":{"min_value":10,"max_value":30,"value":17,"step":5}},
        "progress":{"type":"progress","props":{"min_value":10,"max_value":30,"value":22}},
        "rule":{"type":"separator","props":{"width":28,"thickness":3}},
        "rows":{"type":"list","props":{"selection_mode":"multiple","items":["Plain",
            {"text":"Earlier","selected":true},{"text":"Disabled","enabled":false},{"text":"Last","selected":true}]}},
        "tree":{"type":"tree","props":{"items":[{"text":"Parent","expanded":true,"selected":true,
            "children":[{"text":"Child","selected":true}]}]}},
        "menu":{"type":"menu","props":{"text":"Commands","y":60,"height":30,"menu_width":145,
            "close_on_activate":false,"items":[{"text":"Pin","checked":true,"action":"tools.pin"},
            {"separator":true},{"text":"More","children":[{"text":"Export","action":"tools.export"}]}]}},
        "bar":{"type":"menubar","props":{"items":[{"text":"File","children":[]},
            {"text":"Unavailable","enabled":false,"children":[]}]}},
        "split":{"type":"splitter","props":{"x":200,"width":180,"height":108,"orientation":"vertical",
            "ratio":0.25,"sash_size":8},"children":["submit","check"]},
        "tabs":{"type":"tabview","props":{"y":120,"width":180,"height":120,"active_index":1,"tab_height":26},
            "children":["page_a","page_b"]},
        "page_a":{"type":"panel","name":"Overview"},
        "page_b":{"type":"panel","props":{"page_title":"Details"},"children":["heading"]},
        "dock":{"type":"dock","props":{"x":210,"y":120,"width":420,"height":145,"left_width":112,
            "right_collapsed":true},"children":["dock_tools","dock_main"]},
        "dock_tools":{"type":"panel","props":{"page_title":"Tools","dock_zone":"left","dock_closeable":true}},
        "dock_main":{"type":"panel","name":"Main","props":{"dock_open":false}}
    }})", doc, &error);
    check(decoded, "decode declarative widgets: " + error.path + " " + error.message);
    const auto original = doc;
    std::unique_ptr<vgui_context, decltype(&vgui_destroy)> ctx(vgui_create(nullptr, 16), &vgui_destroy);
    check(bool(ctx), "create declarative widget context");
    action_log actions;
    ui::runtime_bindings bindings;
    bindings.on_action = on_action;
    bindings.user = &actions;
    ui::runtime rt(ctx.get());
    const bool mounted = rt.mount(doc, 1, bindings, &error);
    check(mounted, "mount declarative widgets: " + error.path + " " + error.message);
    const struct { const char* id; vgui_widget_kind kind; } kinds[] = {
        {"root", VGUI_PANEL}, {"heading", VGUI_LABEL}, {"submit", VGUI_BUTTON}, {"field", VGUI_EDIT}, {"photo", VGUI_IMAGE},
        {"check", VGUI_CHECKBOX}, {"radio", VGUI_RADIO}, {"switch", VGUI_SWITCH}, {"slider", VGUI_SLIDER},
        {"progress", VGUI_PROGRESS}, {"rule", VGUI_SEPARATOR}, {"rows", VGUI_LIST}, {"tree", VGUI_TREE},
        {"menu", VGUI_MENUBAR}, {"bar", VGUI_MENUBAR}, {"split", VGUI_SPLITTER}, {"tabs", VGUI_TABVIEW}, {"dock", VGUI_DOCK},
    };
    for (const auto& [id, kind] : kinds)
        check(rt.widget(id) && vgui_widget_get_kind(rt.widget(id)) == kind, std::string("native kind of ") + id);
    const auto snap = rt.layout({720, 360, 1});
    check(snap && snap->nodes.size() == doc.nodes.size(), "every declarative widget appears in layout");
    for (const auto* id : {"check", "radio", "switch"})
        check(vgui_widget_get_checked(rt.widget(id)), std::string(id) + " initial check state");
    check(near(vgui_widget_get_value(rt.widget("slider")), 15) && near(vgui_widget_get_value(rt.widget("progress")), 22),
        "range values use document units and slider step");
    check(vgui_button_get_style(rt.widget("submit")) == VGUI_BUTTON_STYLE_GRADIENT &&
        std::string_view(vgui_widget_get_tooltip(rt.widget("submit"))) == "Send form" &&
        near(node(*snap, "rule").rect_px.height, 3), "declarative appearance and separator thickness");
    auto* field = rt.widget("field");
    vgui_set_focus(ctx.get(), field);
    check(vgui_get_focus(ctx.get()) == field, "edit accepts keyboard focus");
    vgui_input_text(ctx.get(), "replacement");
    check(std::string_view(vgui_edit_get_text(field)) == "<b>literal</b>\nsecond line",
        "read-only edit preserves literal markup and line breaks");
    vgui_set_focus(ctx.get(), nullptr);

    auto* rows = rt.widget("rows");
    const auto row = [&](int index) { return vgui_list_get_item_at(rows, index); };
    check(vgui_list_get_count(rows) == 4 && std::string_view(vgui_list_get_item_text(rows, row(0))) == "Plain" &&
        !vgui_list_get_item_enabled(rows, row(2)), "list rows and enabled state");
    check(vgui_list_get_selection_mode(rows) == VGUI_SELECTION_MULTIPLE && vgui_list_get_selected_count(rows) == 2 &&
        vgui_list_is_selected(rows, row(1)) && vgui_list_is_selected(rows, row(3)), "list initial multiple selection");
    auto* tree = rt.widget("tree");
    const auto branch = vgui_tree_get_root(tree, 0);
    const auto leaf = vgui_tree_get_child(tree, branch, 0);
    check(vgui_tree_get_node_count(tree) == 2 && vgui_tree_get_item_expanded(tree, branch) &&
        std::string_view(vgui_tree_get_item_text(tree, leaf)) == "Child" && vgui_tree_get_parent(tree, leaf) == branch,
        "tree hierarchy and expansion");
    check(vgui_tree_get_selected_count(tree) == 1 && vgui_tree_get_selected(tree, 0) == leaf,
        "tree initial single selection uses the last selected node");

    auto* bar = rt.widget("bar");
    const auto unavailable = vgui_menubar_get_item_at(bar, 1);
    check(vgui_menubar_get_count(bar) == 2 && !vgui_menubar_get_item_enabled(bar, unavailable) &&
        !vgui_menubar_open_item(bar, unavailable), "menubar headings and disabled state");
    auto* host = rt.widget("menu");
    const auto heading = vgui_menubar_get_item_at(host, 0);
    auto* menu = vgui_menubar_get_menu(host, heading);
    check(vgui_menubar_get_count(host) == 1 && std::string_view(vgui_menubar_get_item_text(host, heading)) == "Commands" &&
        menu && vgui_widget_get_kind(menu) == VGUI_MENU && vgui_menu_get_count(menu) == 3 &&
        near(vgui_menu_get_width(menu), 145), "standalone menu mounts a native popup under a one-title bar");
    const auto pin = vgui_menu_get_item_at(menu, 0);
    const auto more = vgui_menu_get_item_at(menu, 2);
    auto* submenu = vgui_menu_get_submenu(menu, more);
    check(vgui_menu_get_item_checkable(menu, pin) && vgui_menu_get_item_checked(menu, pin) &&
        vgui_menu_get_item_type(menu, vgui_menu_get_item_at(menu, 1)) == VGUI_MENU_ITEM_SEPARATOR &&
        submenu && vgui_menu_get_count(submenu) == 1, "menu check item, separator, and submenu");
    const auto press = [&](vgui_key key) { vgui_input_key(ctx.get(), key, 1, 0); vgui_input_key(ctx.get(), key, 0, 0); };
    check(vgui_menubar_open_item(host, heading), "open declarative standalone popup");
    vgui_menu_set_cursor(menu, pin);
    press(VGUI_KEY_ENTER);
    check(actions.calls == 1 && actions.id == "menu" && actions.action == "tools.pin" && vgui_menu_is_open(menu),
        "popup action routes to its document node without closing the popup");
    vgui_menu_set_cursor(menu, more);
    press(VGUI_KEY_RIGHT);
    check(vgui_menu_is_open(submenu), "open declarative submenu");
    vgui_menu_set_cursor(submenu, vgui_menu_get_item_at(submenu, 0));
    press(VGUI_KEY_ENTER);
    check(actions.calls == 2 && actions.id == "menu" && actions.action == "tools.export", "submenu action routes separately");
    vgui_menubar_close(host);

    const auto& top = node(*snap, "submit").rect_px;
    const auto& bottom = node(*snap, "check").rect_px;
    check(vgui_splitter_get_vertical(rt.widget("split")) && near(top.height, 25) &&
        near(bottom.y - top.y - top.height, 8), "splitter uses initial pane ratio, orientation, and sash size");
    auto* tabs = rt.widget("tabs");
    const auto first = vgui_tab_get_at(tabs, 0), last = vgui_tab_get_at(tabs, 1);
    check(vgui_tab_get_count(tabs) == 2 && vgui_tab_get_active(tabs) == last && near(vgui_tab_get_tab_height(tabs), 26) &&
        vgui_tab_get_page(tabs, first) == rt.widget("page_a") && vgui_tab_get_page(tabs, last) == rt.widget("page_b") &&
        std::string_view(vgui_tab_get_title(tabs, first)) == "Overview" &&
        std::string_view(vgui_tab_get_title(tabs, last)) == "Details" && !vgui_tab_get_closeable(tabs, last) &&
        !node(*snap, "page_a").visible && node(*snap, "page_b").visible, "tab registration and zero-based initial selection");
    auto* dock = rt.widget("dock");
    const auto left = vgui_dock_find(dock, "dock_tools"), center = vgui_dock_find(dock, "dock_main");
    check(vgui_dock_get_count(dock) == 2 && vgui_dock_get_panel(dock, left) == rt.widget("dock_tools") &&
        vgui_dock_get_panel(dock, center) == rt.widget("dock_main") && vgui_dock_get_active(dock, VGUI_DOCK_LEFT) == left &&
        std::string_view(vgui_dock_get_title(dock, left)) == "Tools" && vgui_dock_get_closeable(dock, left) &&
        !vgui_dock_is_open(dock, center) && near(vgui_dock_get_zone_size(dock, VGUI_DOCK_LEFT), 112) &&
        vgui_dock_get_side_collapsed(dock, VGUI_DOCK_RIGHT), "dock registration and initial zone settings");
    check(doc == original, "projection and native interactions leave the document unchanged");
}

void test_button_gradients() {
    ui::document doc;
    check(ui::decode_json(R"({"format":"vgui-ui-design","version":1,"root":"root","nodes":{
        "root":{"type":"panel","children":["button"]},
        "button":{"type":"button","props":{"width":100,"height":42,"button_style":"gradient","gradient_preset":"blue"}}
    }})", doc), "read a JSON button with the blue gradient preset");
    std::unique_ptr<ovg_ctx_cb, decltype(&free_ctx_cb)> cb(new_ctx_cb(), &free_ctx_cb);
    std::unique_ptr<rvg_t, void (*)(rvg_t*)> vg(cb->new_rvg(cb->ac), cb->destroy_rvg);
    std::unique_ptr<vgui_context, decltype(&vgui_destroy)> ctx(vgui_create(nullptr, 16), &vgui_destroy);
    vg->width = 120;
    vg->height = 80;
    static std::vector<uint32_t> stops, solids;
    static const auto solid_source = cb->set_source_color;
    cb->set_source_color = [](rvg_t* view, uint32_t color) {
        solids.push_back(color);
        solid_source(view, color);
    };
    static const auto add_stop = cb->pattern_add_color_stop;
    cb->pattern_add_color_stop = [](vg_pattern_t* pattern, float t, float r, float g, float b, float a) {
        const auto byte = [](float value) { return static_cast<uint32_t>(std::lround(value * 255)); };
        stops.push_back((byte(a) << 24) | (byte(r) << 16) | (byte(g) << 8) | byte(b));
        return add_stop(pattern, t, r, g, b, a);
    };
    ui::runtime rt(ctx.get());
    const auto mount = [&](std::uint64_t revision) {
        check(rt.mount(doc, revision) && rt.layout({120, 80, 1}), "mount the styled button");
    };
    const auto paint = [&] {
        cb->clear(vg.get());
        stops.clear();
        solids.clear();
        vgui_draw(ctx.get(), cb.get(), vg.get());
    };
    mount(1);
    paint();
    check(stops == std::vector<uint32_t>{0xFF4A8FE7, 0xFF2D6FD9}, "normal state paints the blue gradient, not the dark default");
    vgui_input_mouse_move(ctx.get(), 40, 20);
    paint();
    check(stops == std::vector<uint32_t>{0xFF6AA8F2, 0xFF3E82E0}, "hover paints its own blue gradient");
    vgui_input_mouse_down(ctx.get(), 1, 40, 20, 1);
    paint();
    check(stops.size() == 2 && (stops[0] & 0xFFFFFF) == 0x1E4FB0 && (stops[1] & 0xFFFFFF) == 0x163D99,
        "pressed state paints its own blue gradient");
    vgui_input_cancel(ctx.get());
    vgui_input_mouse_move(ctx.get(), 110, 60);
    auto& props = doc.nodes.at("button").props;
    props["button_style"] = "solid"s;
    mount(2);
    paint();
    check(stops.empty(), "selecting a preset must not turn a solid button into a gradient");
    props["button_style"] = "gradient"s;
    props.erase("gradient_preset");
    mount(3);
    paint();
    check(stops == std::vector<uint32_t>{0xFF4A4A4A, 0xFF3A3A3A}, "old designs keep the native dark gradient");

    props["gradient_preset"] = "green"s;
    props["gradient_normal_top"] = "#ABC12380"s;
    props["gradient_normal_bottom"] = ""s;
    props["gradient_hover_top"] = "#231ABC42"s;
    props["gradient_hover_bottom"] = "#4713AA"s;
    props["gradient_pressed_top"] = "#41325980"s;
    props["gradient_pressed_bottom"] = "#00000000"s;
    props["gradient_border_light"] = "#AABBCCAA"s;
    props["gradient_border_dark"] = "#124B5C62"s;
    props["opacity"] = 0.5;
    mount(4);
    paint();
    check(stops == std::vector<uint32_t>{0x40ABC123, 0x8016A34A},
        "override one normal stop and inherit the other from the selected preset");
    check(std::ranges::find(solids, 0x55CCBBAAu) != solids.end() &&
        std::ranges::find(solids, 0x315C4B12u) != solids.end(), "paint both custom edges with the widget opacity");
    vgui_input_mouse_move(ctx.get(), 40, 20);
    paint();
    check(stops == std::vector<uint32_t>{0x21231ABC, 0x804713AA}, "paint both hover overrides");
    vgui_input_mouse_down(ctx.get(), 1, 40, 20, 1);
    paint();
    check(stops == std::vector<uint32_t>{0x33413259, 0}, "press feedback must multiply, not replace, both stop alphas");
    vgui_input_cancel(ctx.get());
    vgui_input_mouse_move(ctx.get(), 110, 60);
    props["enabled"] = false;
    props["gradient_normal_top"] = "#00000000"s;
    mount(5);
    paint();
    check(stops.size() == 2 && (stops[0] >> 24) == 0 && (stops[1] >> 24) == 0x80,
        "disabled tint must not make a transparent stop visible");
    props["gradient_normal_top"] = "#ZZ3245"s;
    ui::diagnostic error;
    check(!rt.mount(doc, 6, {}, &error) && error.path == "/nodes/button/props/gradient_normal_top",
        "malformed gradient colors must fail at the corresponding property");
}

void test_panel_gradients() {
    ui::document doc;
    check(ui::decode_json(R"({"format":"vgui-ui-design","version":1,"root":"root","nodes":{
        "root":{"type":"panel","children":["surface"]},
        "surface":{"type":"panel","props":{"x":14,"y":20,"width":100,"height":60,"background":"#792B14",
            "background_style":"gradient","background_gradient_end":"#12345600","scrollable_y":true,
            "rounding":7,"border_color":"#0099BB","border_width":2,"opacity":0.5},"children":["content"]},
        "content":{"type":"panel","props":{"width":60,"height":160,"background":"#00000000","border_width":0}}
    }})", doc), "read a scrollable panel with a native gradient");
    std::unique_ptr<ovg_ctx_cb, decltype(&free_ctx_cb)> cb(new_ctx_cb(), &free_ctx_cb);
    std::unique_ptr<rvg_t, void (*)(rvg_t*)> vg(cb->new_rvg(cb->ac), cb->destroy_rvg);
    std::unique_ptr<vgui_context, decltype(&vgui_destroy)> ctx(vgui_create(nullptr, 16), &vgui_destroy);
    vg->width = 160;
    vg->height = 120;
    using axis = std::array<float, 4>;
    static std::vector<axis> axes;
    static std::vector<uint32_t> stops, solids;
    static std::vector<float> radii;
    static const auto linear = cb->new_pattern_linear;
    cb->new_pattern_linear = [](rvg_t* view, float x0, float y0, float x1, float y1) {
        axes.push_back({x0, y0, x1, y1});
        return linear(view, x0, y0, x1, y1);
    };
    static const auto round_rect = cb->rounded_rectangle;
    cb->rounded_rectangle = [](rvg_t* view, float x, float y, float w, float h, float r) {
        radii.push_back(r);
        round_rect(view, x, y, w, h, r);
    };
    static const auto stop = cb->pattern_add_color_stop;
    cb->pattern_add_color_stop = [](vg_pattern_t* pattern, float t, float r, float g, float b, float a) {
        const auto byte = [](float value) { return static_cast<uint32_t>(std::lround(value * 255)); };
        stops.push_back((byte(a) << 24) | (byte(r) << 16) | (byte(g) << 8) | byte(b));
        return stop(pattern, t, r, g, b, a);
    };
    static const auto solid = cb->set_source_color;
    cb->set_source_color = [](rvg_t* view, uint32_t color) { solids.push_back(color); solid(view, color); };
    const auto paint = [&] {
        cb->clear(vg.get());
        axes.clear();
        stops.clear();
        solids.clear();
        radii.clear();
        vgui_draw(ctx.get(), cb.get(), vg.get());
    };
    ui::runtime rt(ctx.get());
    const auto mount = [&](std::uint64_t revision) {
        check(rt.mount(doc, revision) && rt.layout({160, 120, 1}), "mount the gradient panel");
        return rt.widget("surface");
    };
    auto* surface = mount(1);
    paint();
    check(axes == std::vector<axis>{{14, 20, 14, 80}} && stops == std::vector<uint32_t>{0x8043576B, 0x00123456},
        "vertical background spans the entire panel and preserves endpoint alpha");
    check(!radii.empty() && radii.front() == 7 && std::ranges::find(solids, 0x80BB9900u) != solids.end() &&
        std::ranges::find(solids, 0x80142B79u) == solids.end(), "gradient keeps panel rounding and border, but replaces the solid fill");
    vgui_panel_set_scroll(surface, 0, 30);
    float scroll = 0;
    vgui_panel_get_scroll(surface, nullptr, &scroll);
    paint();
    check(scroll > 0 && axes == std::vector<axis>{{14, 20, 14, 80}}, "scrolling content must not move the panel gradient");

    auto& props = doc.nodes.at("surface").props;
    props["background_gradient_direction"] = "horizontal"s;
    props["background_gradient_start"] = "#56789A"s;
    surface = mount(2);
    vgui_widget_set_recording_hint(vgui_root(ctx.get()), 1);
    paint();
    check(axes == std::vector<axis>{{14, 20, 114, 20}} && stops == std::vector<uint32_t>{0x8056789A, 0x00123456},
        "horizontal background runs from left to right");
    paint();
    check(axes.empty(), "unchanged ancestor should replay its recording");
    const auto before = vgui_context_paint_revision(ctx.get());
    vgui_panel_set_background_gradient(surface, 0xFF56789A, 0x00123456, 1);
    check(vgui_context_paint_revision(ctx.get()) == before, "same gradient should not request a new frame");
    vgui_panel_set_background_gradient(surface, 0xFF113355, 0x00123456, 1);
    paint();
    check(vgui_context_paint_revision(ctx.get()) > before && !stops.empty() && stops.front() == 0x80113355,
        "new gradient must request a frame and invalidate the ancestor recording");
    vgui_panel_clear_background_gradient(surface);
    paint();
    check(axes.empty() && std::ranges::find(solids, 0x80142B79u) != solids.end(),
        "clearing the gradient should redraw the original solid background");
    props["background_style"] = "solid"s;
    mount(3);
    paint();
    check(axes.empty() && std::ranges::find(solids, 0x80142B79u) != solids.end(),
        "solid style ignores saved gradient endpoints");
}

void test_decorative_gradients() {
    ui::document doc;
    ui::diagnostic error;
    check(ui::decode_json(R"({"format":"vgui-ui-design","version":1,"root":"root","nodes":{
        "root":{"type":"panel","children":["button","paint"]},
        "button":{"type":"button","props":{"x":14,"y":20,"width":80,"height":60,"action":"button.run"}},
        "paint":{"type":"gradient","props":{"x":14,"y":20,"width":80,"height":60,"input_enabled":true,"focusable":"yes",
            "rounding":6,"opacity":0.5,"gradient_extend":"reflect","gradient_affine_xx":2,"gradient_affine_yx":0.25,"gradient_affine_tx":0.125,
            "gradient_stops":[{"offset":0,"color":"#923050"},{"offset":0.4,"color":"#0955AE80"},{"offset":1,"color":"#FFE06400"}]}}
    }})", doc, &error), "read a resource-free gradient overlay");
    std::string json;
    ui::document reloaded;
    check(ui::encode_json(doc, json) && ui::decode_json(json, reloaded) && reloaded == doc,
        "multistop colors should survive JSON array roundtrip with alpha");
    const auto invalid = [&](const char* field, ui::value value) {
        auto broken = doc;
        broken.nodes.at("paint").props[field] = std::move(value);
        check(!ui::validate(broken), std::string("gradient validator accepted invalid ") + field);
    };
    invalid("gradient_stops", R"([{"offset":0,"color":"#123456"},{"offset":0,"color":"#654321"}])"s);
    invalid("gradient_stops", R"([{"offset":0,"color":"#123456"},{"offset":1,"color":"bad"}])"s);
    std::vector<ui::gradient_stop> excess(33);
    for (std::size_t i = 0; i < excess.size(); ++i) excess[i] = {float(i) / 32, "#123456"};
    invalid("gradient_stops", ui::encode_gradient_stops(excess));
    invalid("gradient_affine_yy", 0.0);
    invalid("gradient_y1", 0.0);

    std::unique_ptr<ovg_ctx_cb, decltype(&free_ctx_cb)> cb(new_ctx_cb(), &free_ctx_cb);
    std::unique_ptr<rvg_t, void (*)(rvg_t*)> vg(cb->new_rvg(cb->ac), cb->destroy_rvg);
    std::unique_ptr<vgui_context, decltype(&vgui_destroy)> ctx(vgui_create(nullptr, 16), &vgui_destroy);
    vg->width = 160;
    vg->height = 120;
    static std::vector<vg_gradient_t> fills;
    static const auto set_source = cb->set_source;
    cb->set_source = [](rvg_t* view, vg_pattern_t* p) {
        if (p && p->type >= VG_PATTERN_TYPE_LINEAR && p->data) fills.push_back(*static_cast<vg_gradient_t*>(p->data));
        set_source(view, p);
    };
    const auto draw = [&] {
        cb->clear(vg.get());
        fills.clear();
        vgui_draw(ctx.get(), cb.get(), vg.get());
    };
    ui::runtime rt(ctx.get());
    action_log log;
    const auto mount = [&](std::uint64_t revision) {
        ui::runtime_bindings bindings;
        bindings.on_action = on_action;
        bindings.user = &log;
        check(rt.mount(doc, revision, bindings) && rt.layout({160, 120, 1}), "mount gradient widget");
    };
    mount(1);
    draw();
    check(fills.size() == 1, "gradient overlay must emit exactly one native brush");
    const auto& linear = fills.front();
    check(linear.count == 3 && near(linear.stops[1], 0.4f) && near(linear.colors[0].a, 128.f / 255) &&
        near(linear.colors[1].a, 64.f / 255) && near(linear.colors[2].a, 0) && linear.extend == VG_EXTEND_REFLECT,
        "three OVG color stops should preserve offsets, opacity and reflect mode");
    check(linear.m.x == 512 && linear.m.z == -96 && near(linear.cp[0].x, 104) && near(linear.cp[0].y, 27.5f),
        "gradient transform must use local dimensions and preserve the canonical gradient axis");
    auto* widget = rt.widget("paint");
    vgui_widget_set_input_enabled(widget, 1);
    vgui_widget_set_focusable(widget, 1);
    vgui_set_focus(ctx.get(), widget);
    check(vgui_get_focus(ctx.get()) != widget, "gradient overlay must never take keyboard focus");
    vgui_input_mouse_down(ctx.get(), 1, 40, 40, 1);
    vgui_input_mouse_up(ctx.get(), 1, 40, 40, 1);
    check(log.calls == 1 && log.id == "button" && log.action == "button.run", "gradient overlay swallowed the click on the button below it");

    auto& p = doc.nodes.at("paint").props;
    p.erase("gradient_affine_xx"); p.erase("gradient_affine_yx"); p.erase("gradient_affine_tx");
    p["gradient_kind"] = "radial"s;
    p["gradient_ellipse"] = true;
    p["gradient_radius0"] = 0.1;
    p["gradient_radius1"] = 0.45;
    mount(2);
    draw();
    check(fills.size() == 1 && fills[0].type == VG_PATTERN_TYPE_RADIAL && fills[0].m.x == 512 &&
        near(fills[0].cp[0].x, 54) && near(fills[0].cp[0].z, 6) && near(fills[0].cp[1].z, 27),
        "radial gradients must use both radii and stretch their paint horizontally when elliptical");
    p["gradient_kind"] = "sweep"s;
    p["gradient_extend"] = "repeat"s;
    p["gradient_angle0"] = 90.0;
    p["gradient_angle1"] = 270.0;
    mount(3);
    auto* root = vgui_root(ctx.get());
    vgui_widget_set_recording_hint(root, 1);
    draw();
    check(fills.size() == 1 && fills[0].type == VG_PATTERN_TYPE_SWEEP && fills[0].extend == VG_EXTEND_REPEAT &&
        near(fills[0].cp[0].z, 0.5f) && near(fills[0].cp[0].w, 1.5f), "sweep angles must reach OVG in half-turns");
    draw();
    check(fills.empty(), "static gradient should be cached with its ancestor");
    vgui_gradient_desc paint;
    vgui_gradient_desc_default(&paint);
    const vgui_gradient_stop stops[] = {{0, 0xFFFF0012}, {1, 0x00080602}};
    widget = rt.widget("paint");
    check(vgui_gradient_set(widget, &paint, stops, 2), "could not change gradient from host code");
    const auto revision = vgui_context_paint_revision(ctx.get());
    check(vgui_gradient_set(widget, &paint, stops, 2) && vgui_context_paint_revision(ctx.get()) == revision,
        "same gradient update should not request another frame");
    draw();
    check(fills.size() == 1 && fills[0].type == VG_PATTERN_TYPE_LINEAR, "new paint must invalidate its recorded ancestor");
}

void test_gradient_animations() {
    ui::document doc;
    check(ui::decode_json(R"({"format":"vgui-ui-design","version":1,"root":"root","nodes":{
        "root":{"type":"panel","children":["shine","group"]},
        "shine":{"type":"gradient","props":{"x":10,"y":20,"width":100,"height":30,
            "gradient_animation":"shine","gradient_animation_duration":2,"gradient_animation_loop":false}},
        "group":{"type":"panel","props":{"x":10,"y":70,"width":100,"height":30},"children":["color"]},
        "color":{"type":"gradient","props":{"width":100,"height":30,
            "gradient_animation":"color","gradient_animation_color":"#0000FF80",
            "gradient_stops":[{"offset":0,"color":"#FF0000"},{"offset":0.4,"color":"#00FF0080"},{"offset":1,"color":"#00FF0000"}]}}
    }})", doc), "decode two animated gradients");
    std::string json;
    ui::document reloaded;
    check(ui::encode_json(doc, json) && ui::decode_json(json, reloaded) && reloaded == doc,
        "gradient animation settings should round-trip through JSON");
    auto invalid = doc;
    invalid.nodes.at("shine").props["gradient_animation_duration"] = 0.0;
    check(!ui::validate(invalid), "zero animation duration must be rejected");
    invalid = doc;
    invalid.nodes.at("shine").props["gradient_affine_tx"] = 16.0;
    check(!ui::validate(invalid), "translation and animated travel cannot exceed the paint bounds");

    const auto authored = doc;
    std::unique_ptr<ovg_ctx_cb, decltype(&free_ctx_cb)> cb(new_ctx_cb(), &free_ctx_cb);
    std::unique_ptr<rvg_t, void (*)(rvg_t*)> vg(cb->new_rvg(cb->ac), cb->destroy_rvg);
    std::unique_ptr<vgui_context, decltype(&vgui_destroy)> ctx(vgui_create(nullptr, 16), &vgui_destroy);
    vg->width = vg->height = 200;
    static std::vector<vg_gradient_t> layers;
    static const auto set_source = cb->set_source;
    cb->set_source = [](rvg_t* view, vg_pattern_t* p) {
        if (p && p->type == VG_PATTERN_TYPE_LINEAR && p->data) layers.push_back(*static_cast<vg_gradient_t*>(p->data));
        set_source(view, p);
    };
    const auto draw = [&](std::size_t count = 2) {
        cb->clear(vg.get());
        layers.clear();
        vgui_draw(ctx.get(), cb.get(), vg.get());
        check(layers.size() == count, "draw expected number of gradient layers");
    };
    ui::runtime rt(ctx.get());
    check(rt.mount(doc, 1) && rt.layout({200, 200, 1}), "mount animations without playing");
    auto* original = rt.widget("shine");
    draw();
    check(near(layers[0].cp[0].x, 60) && near(layers[1].colors[0].r, 1.f), "layout without a host tick stays authored");
    rt.advance_animations(0);
    draw();
    check(near(layers[0].cp[0].x, -40), "sweep starts half a travel before the authored origin");
    rt.advance_animations(.5);
    draw();
    const auto mix = layers[1].colors[0];
    check(near(layers[0].cp[0].x, 10) && near(layers[0].cp[0].y, 20), "a quarter sweep translates paint at constant speed");
    check(near(mix.r, .836f) && near(mix.b, .613f) && near(mix.a, .751f) && near(layers[1].colors[2].a, 0),
        "mid-fade uses linear-light premultiplied colors, even on transparent stops");
    rt.advance_animations(.5);
    draw();
    check(near(layers[0].cp[0].x, 60) && near(layers[1].colors[0].b, 1) && near(layers[1].colors[1].a, .252f),
        "the half-cycle reaches the target color while preserving the alpha envelope");
    rt.advance_animations(1);
    draw();
    check(near(layers[0].cp[0].x, 160) && near(layers[1].colors[0].r, 1), "single sweep holds the endpoint; loop returns to the original color");

    auto* group = rt.widget("group");
    vgui_widget_set_visible(group, 0);
    const auto paused = vgui_context_paint_revision(ctx.get());
    rt.advance_animations(7);
    check(vgui_context_paint_revision(ctx.get()) == paused, "hidden animations and finished one-shots must not request frames");
    draw(1);
    check(near(layers[0].cp[0].x, 160) && rt.widget("shine") == original && doc == authored,
        "sampling and stopping never remount widgets or edit the document");
    vgui_widget_set_visible(group, 1);
    rt.advance_animations(.5);
    draw();
    check(near(layers[1].colors[0].r, mix.r), "showing an animated child resumes from its previous phase");
    const auto frame = vgui_context_paint_revision(ctx.get());
    rt.advance_animations(-1);
    rt.advance_animations(std::numeric_limits<double>::quiet_NaN());
    rt.advance_animations(0);
    check(vgui_context_paint_revision(ctx.get()) == frame, "invalid or unchanged sample times never dirty the paint");
    check(rt.mount(doc, 2) && rt.layout({200, 200, 1}), "remount the animation document");
    draw();
    check(near(layers[0].cp[0].x, 60) && near(layers[1].colors[0].r, 1), "remount restores the authored gradient");

    // A short loop and a longer one-shot share the same absolute playhead.
    doc.nodes.at("shine").props["gradient_animation_duration"] = 3.0;
    check(rt.mount(doc, 3) && rt.layout({200, 200, 1}), "mount effects with different periods");
    const auto timing = rt.animation_timing();
    check(timing.duration == 3.0 && timing.loops, "the page span is the longest duration and reports any loop");
    rt.seek_animations(.5);
    draw();
    check(near(layers[0].cp[0].x, -40 + 100.f / 3) && near(layers[1].colors[0].r, mix.r),
        "seek uses each effect's own duration");
    rt.seek_animations(4.5);
    draw();
    check(near(layers[0].cp[0].x, 160) && near(layers[1].colors[0].r, mix.r),
        "absolute time clamps one-shots and wraps loops individually");
    rt.seek_animations(1);
    draw();
    check(near(layers[1].colors[0].b, 1), "backward seek must sample, not integrate a delta");
    rt.seek_animations(.5);
    draw();
    check(near(layers[0].cp[0].x, -40 + 100.f / 3) && near(layers[1].colors[0].r, mix.r),
        "the same time always returns the same paint, including on previously finished clips");
    const auto seek_paint_revision = vgui_context_paint_revision(ctx.get());
    rt.seek_animations(.5);
    rt.seek_animations(-1);
    rt.seek_animations(std::numeric_limits<double>::infinity());
    check(vgui_context_paint_revision(ctx.get()) == seek_paint_revision, "seeking to the same or an invalid time must not dirty paint");
    rt.unmount();
    check(rt.animation_timing().duration == 0 && !rt.animation_timing().loops, "unmount clears timing");
}

void test_gradient_keyframe_animation() {
    ui::document doc;
    check(ui::decode_json(R"({"format":"vgui-ui-design","version":1,"root":"root","nodes":{
        "root":{"type":"panel","children":["paint"]},
        "paint":{"type":"gradient","props":{"x":10,"y":20,"width":100,"height":40,
            "gradient_affine_tx":0.125,"gradient_affine_ty":-0.125,"gradient_extend":"reflect",
            "gradient_stops":[{"offset":0,"color":"#C0804080"},{"offset":0.4,"color":"#20A0F0"},{"offset":1,"color":"#00AA5500"}],
            "gradient_animation":"keyframes","gradient_animation_duration":4,"gradient_animation_loop":false,
            "gradient_animation_keyframes":[
                {"offset":0.125,"x":0.125,"y":0.25,"tint":"#FFFFFF","opacity":1,"interpolation":"linear"},
                {"offset":0.375,"x":0.375,"y":0.5,"tint":"#0080FF","opacity":0.5,"interpolation":"step"},
                {"offset":0.625,"x":-0.125,"y":-0.25,"tint":"#FF0000","opacity":0.25,"interpolation":"smooth"},
                {"offset":0.875,"x":0.875,"y":0.25,"tint":"#FFFFFF","opacity":1,"interpolation":"linear"}]}}
    }})", doc), "read keyframed gradient");
    const auto authored = doc;
    std::unique_ptr<ovg_ctx_cb, decltype(&free_ctx_cb)> cb(new_ctx_cb(), &free_ctx_cb);
    std::unique_ptr<rvg_t, void (*)(rvg_t*)> vg(cb->new_rvg(cb->ac), cb->destroy_rvg);
    std::unique_ptr<vgui_context, decltype(&vgui_destroy)> ctx(vgui_create(nullptr, 16), &vgui_destroy);
    vg->width = vg->height = 200;
    static std::vector<vg_gradient_t> fills;
    static const auto set_source = cb->set_source;
    cb->set_source = [](rvg_t* view, vg_pattern_t* p) {
        if (p && p->type == VG_PATTERN_TYPE_LINEAR && p->data) fills.push_back(*static_cast<vg_gradient_t*>(p->data));
        set_source(view, p);
    };
    const auto expect_paint = [&](float x, float y, std::array<int, 4> rgba, int middle_alpha, std::string_view message) {
        cb->clear(vg.get());
        fills.clear();
        vgui_draw(ctx.get(), cb.get(), vg.get());
        check(fills.size() == 1, "draw a keyframed native gradient");
        const auto& paint = fills.front();
        const auto& color = paint.colors[0];
        check(near(paint.cp[0].x, x) && near(paint.cp[0].y, y) &&
              color.r == rgba[0] / 255.f && color.g == rgba[1] / 255.f &&
              color.b == rgba[2] / 255.f && color.a == rgba[3] / 255.f &&
              paint.colors[1].a == middle_alpha / 255.f, message);
        check(paint.count == 3 && near(paint.stops[1], .4f) && paint.extend == VG_EXTEND_REFLECT &&
            paint.colors[2].a == 0.f, "keyframes leave stop layout and spread unchanged and never add authored alpha");
    };
    constexpr std::array<int, 4> original{192, 128, 64, 128};
    ui::runtime rt(ctx.get());
    const auto mount = [&](std::uint64_t revision) {
        check(rt.mount(doc, revision) && rt.layout({200, 200, 1}), "mount gradient keyframes");
    };
    mount(1);
    auto* widget = rt.widget("paint");
    check(rt.animation_timing().duration == 4 && !rt.animation_timing().loops, "keyframes use their configured clip timing");
    expect_paint(72.5f, 15, original, 255, "mount and layout alone keep the authored paint");
    rt.advance_animations(0);
    expect_paint(85, 25, original, 255, "first key clamps before its offset; neutral tint and opacity preserve exact colors");
    rt.advance_animations(1);
    expect_paint(97.5f, 30, {96, 96, 64, 96}, 191, "linear midpoint interpolates relative translation, tint and opacity");
    rt.seek_animations(2);
    expect_paint(110, 35, {0, 64, 64, 64}, 128, "outgoing step holds at the left key");
    rt.seek_animations(2.5);
    expect_paint(60, 5, {192, 0, 0, 32}, 64, "step switches at the exact next key");
    rt.seek_animations(2.75);
    expect_paint(75.625f, 8.125f, {192, 20, 10, 47}, 94, "smooth uses the timeline's smoothstep, not linear interpolation");
    rt.seek_animations(4);
    expect_paint(160, 25, original, 255, "non-looping clip holds its last key past the last offset");
    rt.seek_animations(1);
    expect_paint(97.5f, 30, {96, 96, 64, 96}, 191, "backward seek reproduces the paint from advancement");
    check(rt.widget("paint") == widget && doc == authored, "sampling does not remount the widget or edit the design");

    auto& props = doc.nodes.at("paint").props;
    props["gradient_animation_loop"] = true;
    mount(2);
    rt.seek_animations(3.875);
    expect_paint(160, 25, original, 255, "loop holds the last pose up to the clip end, without blending across the boundary");
    rt.advance_animations(.125);
    expect_paint(85, 25, original, 255, "advance restarts the loop on the exact cycle boundary");
    rt.seek_animations(5);
    expect_paint(97.5f, 30, {96, 96, 64, 96}, 191, "absolute seek wraps each loop's own duration");
    props["gradient_animation_duration"] = .7;
    mount(3);
    const double cycle = .7 * 3;
    rt.seek_animations(std::nextafter(cycle, 0.));
    expect_paint(160, 25, original, 255, "a representable time just before a decimal period boundary still holds the last key");
    rt.seek_animations(cycle);
    expect_paint(85, 25, original, 255, "an exact computed decimal period boundary restarts the loop");

    std::vector<ui::gradient_keyframe> keys;
    std::string error;
    check(ui::decode_gradient_keyframes(std::get<std::string>(props.at("gradient_animation_keyframes")), keys, error),
        "retrieve the authored keys");
    keys[2].interpolation = "step";
    keys.back().offset = 1;
    props["gradient_animation_keyframes"] = ui::encode_gradient_keyframes(keys);
    mount(4);
    rt.seek_animations(std::nextafter(.7, 0.));
    expect_paint(60, 5, {192, 0, 0, 32}, 64, "a step segment has not reached the terminal key before the end");
    rt.seek_animations(.7, true);
    expect_paint(160, 25, original, 255, "inspecting the terminal key samples its exact pose, even after a step");
    rt.seek_animations(.7);
    expect_paint(85, 25, original, 255, "normal seeks still wrap when arriving at the exact same boundary");

    props.erase("gradient_animation_keyframes");
    mount(5);
    check(rt.animation_timing().duration == .7 && rt.animation_timing().loops, "an empty keyframes mode still reports timeline timing");
    const auto revision = vgui_context_paint_revision(ctx.get());
    rt.seek_animations(.5);
    rt.advance_animations(1);
    check(vgui_context_paint_revision(ctx.get()) == revision, "empty keyframes do not dirty the authored paint");
    expect_paint(72.5f, 15, original, 255, "empty keyframes preserve authored paint under both clocks");
}

void test_gradient_keyframe_geometry() {
    ui::document doc;
    check(ui::decode_json(R"({"format":"vgui-ui-design","version":1,"root":"root","nodes":{
        "root":{"type":"panel","children":["linear","radial","sweep"]},
        "linear":{"type":"gradient","props":{"x":10,"y":10,"width":80,"height":40,
            "gradient_x0":0.125,"gradient_y0":0.25,"gradient_x1":0.875,"gradient_y1":0.75,
            "gradient_affine_tx":0.125,"gradient_affine_ty":-0.125,"gradient_extend":"reflect",
            "gradient_stops":[{"offset":0,"color":"#C0804080"},{"offset":0.4,"color":"#20A0F0"},{"offset":1,"color":"#00AA5500"}],
            "gradient_animation":"keyframes","gradient_animation_duration":1,"gradient_animation_loop":false,
            "gradient_animation_keyframes":[
                {"offset":0,"x":0,"y":0,"tint":"#FFFFFF","opacity":1,"interpolation":"linear",
                    "start_x":0.25,"start_y":0.375,"end_x":0.75,"end_y":0.625,"center_x":0,"outer_y":1},
                {"offset":1,"x":0.25,"y":0.5,"tint":"#0080FF","opacity":0.5,"interpolation":"linear",
                    "start_x":0.75,"start_y":0.625,"end_x":0.25,"end_y":0.375,"center_x":0,"outer_y":1}]}},
        "radial":{"type":"gradient","props":{"x":10,"y":60,"width":80,"height":40,"gradient_kind":"radial",
            "gradient_center_x":0.375,"gradient_center_y":0.375,"gradient_outer_x":0.5,"gradient_outer_y":0.625,
            "gradient_radius0":0.125,"gradient_radius1":0.875,
            "gradient_stops":[{"offset":0,"color":"#FFF1CE"},{"offset":0.375,"color":"#FC876380"},{"offset":1,"color":"#582F68"}],
            "gradient_animation":"keyframes","gradient_animation_duration":1,"gradient_animation_loop":false,
            "gradient_animation_keyframes":[
                {"offset":0,"x":0,"y":0,"tint":"#FFFFFF","opacity":1,"interpolation":"smooth"},
                {"offset":0.5,"x":0,"y":0,"tint":"#FFFFFF","opacity":1,"interpolation":"step",
                    "center_x":0.625,"center_y":0.625,"outer_x":0.625,"outer_y":0.375,"inner_radius":0.25,"outer_radius":0.625},
                {"offset":1,"x":0,"y":0,"tint":"#FFFFFF","opacity":1,"interpolation":"linear",
                    "center_y":0.5,"outer_radius":0.75}]}},
        "sweep":{"type":"gradient","props":{"x":10,"y":110,"width":80,"height":40,"gradient_kind":"sweep",
            "gradient_center_y":0.625,
            "gradient_stops":[{"offset":0,"color":"#123640"},{"offset":0.55,"color":"#44D2A880"},{"offset":1,"color":"#123640"}],
            "gradient_animation":"keyframes","gradient_animation_duration":1,"gradient_animation_loop":false,
            "gradient_animation_keyframes":[
                {"offset":0,"x":0,"y":0,"tint":"#FFFFFF","opacity":1,"interpolation":"linear",
                    "center_x":0.25,"center_y":0.375,"start_angle":350,"end_angle":360},
                {"offset":1,"x":0,"y":0,"tint":"#FFFFFF","opacity":1,"interpolation":"linear",
                    "center_x":0.75,"start_angle":10,"end_angle":20}]}}
    }})", doc), "read three gradients with optional keyframe geometry");
    std::unique_ptr<ovg_ctx_cb, decltype(&free_ctx_cb)> cb(new_ctx_cb(), &free_ctx_cb);
    std::unique_ptr<rvg_t, void (*)(rvg_t*)> vg(cb->new_rvg(cb->ac), cb->destroy_rvg);
    std::unique_ptr<vgui_context, decltype(&vgui_destroy)> ctx(vgui_create(nullptr, 16), &vgui_destroy);
    vg->width = vg->height = 200;
    static std::vector<vg_gradient_t> fills;
    static const auto set_source = cb->set_source;
    cb->set_source = [](rvg_t* view, vg_pattern_t* p) {
        if (p && p->type >= VG_PATTERN_TYPE_LINEAR && p->data) fills.push_back(*static_cast<vg_gradient_t*>(p->data));
        set_source(view, p);
    };
    ui::runtime rt(ctx.get());
    check(rt.mount(doc, 1) && rt.layout({200, 200, 1}), "mount three keyframed gradient kinds");
    const auto draw = [&] {
        cb->clear(vg.get());
        fills.clear();
        vgui_draw(ctx.get(), cb.get(), vg.get());
        check(fills.size() == 3, "draw one native brush for each keyframed gradient");
    };
    const auto seek = [&](double seconds, const ui::gradient_shape_preview* draft = nullptr) {
        rt.seek_animations(seconds, false, draft);
        draw();
    };
    const auto fill = [&](int kind) -> const vg_gradient_t& {
        const auto it = std::ranges::find(fills, kind, &vg_gradient_t::type);
        check(it != fills.end(), "draw expected native gradient kind");
        return *it;
    };
    const auto point_is = [](const glm::vec4& point, float x, float y, float z, float w = 0.f) {
        return near(point.x, x) && near(point.y, y) && near(point.z, z) && near(point.w, w);
    };
    const auto expect_fallback = [&] {
        seek(0.5);
        const auto& linear = fill(VG_PATTERN_TYPE_LINEAR);
        check(point_is(linear.cp[0], 40, 25, 100, 45),
            "coincident linear endpoints restore authored geometry with the current translation");
        check(linear.count == 3 && linear.extend == VG_EXTEND_REFLECT && near(linear.stops[1], 0.4f) &&
            linear.colors[0] == glm::vec4(96, 96, 64, 96) / 255.f &&
            linear.colors[1].a == 191.f / 255 && linear.colors[2].a == 0.f,
            "degenerate geometry preserves this sample's tint and opacity, stop offsets and spread");
    };

    seek(0);
    const auto& first_linear = fill(VG_PATTERN_TYPE_LINEAR);
    check(point_is(first_linear.cp[0], 40, 20, 80, 30) &&
        first_linear.colors[0] == glm::vec4(192, 128, 64, 128) / 255.f && first_linear.colors[2].a == 0.f,
        "linear geometry is absolute, ignores inactive channels and preserves neutral stop colors and alpha");
    check(point_is(fill(VG_PATTERN_TYPE_RADIAL).cp[0], 40, 75, 5) &&
        point_is(fill(VG_PATTERN_TYPE_RADIAL).cp[1], 50, 85, 35), "old keys without geometry use both authored radial circles");
    check(point_is(fill(VG_PATTERN_TYPE_SWEEP).cp[0], 30, 125, 350.f / 180, 2),
        "sweep native brush receives keyframed center and both angles in half-turns");

    seek(0.125);
    check(point_is(fill(VG_PATTERN_TYPE_LINEAR).cp[0], 47.5f, 23.75f, 77.5f, 31.25f),
        "linear endpoint positions and paint translation interpolate together");
    check(point_is(fill(VG_PATTERN_TYPE_RADIAL).cp[0], 43.125f, 76.5625f, 5.78125f) &&
        point_is(fill(VG_PATTERN_TYPE_RADIAL).cp[1], 51.5625f, 83.4375f, 33.4375f),
        "smooth interpolation animates both centers and radii relative to the widget's short side");
    expect_fallback();
    check(point_is(fill(VG_PATTERN_TYPE_SWEEP).cp[0], 50, 130, 1, 190.f / 180),
        "sweep angles interpolate numerically across the seam and missing center Y uses the authored value");

    seek(0.75);
    check(point_is(fill(VG_PATTERN_TYPE_LINEAR).cp[0], 85, 42.5f, 65, 37.5f),
        "a valid sample beyond the degenerate frame restores animated endpoints");
    check(point_is(fill(VG_PATTERN_TYPE_RADIAL).cp[0], 60, 85, 10) &&
        point_is(fill(VG_PATTERN_TYPE_RADIAL).cp[1], 60, 75, 25), "the outgoing step holds the keyed centers and radii");
    expect_fallback();

    seek(1);
    check(point_is(fill(VG_PATTERN_TYPE_RADIAL).cp[0], 40, 80, 5) &&
        point_is(fill(VG_PATTERN_TYPE_RADIAL).cp[1], 50, 85, 30),
        "at the next key the inner radius inherits the static value while the outer radius stays keyed");
    const auto terminal = fill(VG_PATTERN_TYPE_LINEAR);
    const auto radial = fill(VG_PATTERN_TYPE_RADIAL).cp[0];
    ui::gradient_shape_preview draft{"linear", {{.25f, .125f, .625f, .75f}, {}}};
    seek(1, &draft);
    const auto& transient = fill(VG_PATTERN_TYPE_LINEAR);
    check(point_is(transient.cp[0], 60, 30, 90, 55) && transient.colors[0] == terminal.colors[0] &&
        transient.colors[1] == terminal.colors[1] && transient.count == terminal.count &&
        transient.stops[1] == terminal.stops[1] && transient.extend == terminal.extend &&
        fill(VG_PATTERN_TYPE_RADIAL).cp[0] == radial,
        "a draft at the same time replaces only the named brush's endpoints, not its paint or other nodes");
    const auto revision = vgui_context_paint_revision(ctx.get());
    rt.seek_animations(1, false, &draft);
    check(vgui_context_paint_revision(ctx.get()) == revision, "an unchanged draft does not invalidate paint");
    draft.draft.geometry[std::size_t(ui::gradient_geometry_axis::end_x)] = .75f;
    seek(1, &draft);
    check(point_is(fill(VG_PATTERN_TYPE_LINEAR).cp[0], 60, 30, 100, 55), "a second move updates the same sampled time");
    seek(1);
    check(fill(VG_PATTERN_TYPE_LINEAR).cp[0] == terminal.cp[0], "clearing a draft restores the original same-time brush");
    seek(1, &draft);
    rt.advance_animations(0);
    draw();
    check(fill(VG_PATTERN_TYPE_LINEAR).cp[0] == terminal.cp[0], "normal playback does not retain a draft at the clip end");

    using axis = ui::gradient_geometry_axis;
    const auto radial_base = fill(VG_PATTERN_TYPE_RADIAL);
    ui::gradient_shape_preview radial_draft{"radial", {}};
    const auto override = [&](axis channel, float value) { radial_draft.draft.geometry[std::size_t(channel)] = value; };
    override(axis::center_x, .375f);
    override(axis::center_y, .25f);
    override(axis::outer_x, .625f);
    override(axis::outer_y, .5f);
    override(axis::inner_radius, .1875f);
    override(axis::outer_radius, .875f);
    override(axis::start_x, 12.f); // Aliases the same descriptor member but is inactive here.
    seek(1, &radial_draft);
    const auto& radial_live = fill(VG_PATTERN_TYPE_RADIAL);
    check(point_is(radial_live.cp[0], 40, 70, 7.5f) && point_is(radial_live.cp[1], 60, 80, 35) &&
        radial_live.count == radial_base.count && radial_live.colors[0] == radial_base.colors[0] &&
        fill(VG_PATTERN_TYPE_LINEAR).cp[0] == terminal.cp[0],
        "radial draft changes both centers and both short-side radii, without altering other paint");
    radial_draft.draft = {{}, ui::gradient_stop_offset{1, .625f}};
    seek(1, &radial_draft);
    const auto& radial_stop = fill(VG_PATTERN_TYPE_RADIAL);
    check(radial_stop.cp[0] == radial_base.cp[0] && radial_stop.cp[1] == radial_base.cp[1] &&
        radial_stop.m == radial_base.m && radial_stop.stops[1] == .625f &&
        fill(VG_PATTERN_TYPE_LINEAR).cp[0] == terminal.cp[0],
        "a radial stop draft applies at the same time without changing the radial geometry or another track");
    for (int i = 0; i < radial_base.count; ++i)
        check(radial_stop.colors[i] == radial_base.colors[i] && (i == 1 || radial_stop.stops[i] == radial_base.stops[i]),
            "radial live preview preserves every color and the other offsets");
    radial_draft.draft.stop->offset = 1.f;
    seek(1, &radial_draft);
    check(fill(VG_PATTERN_TYPE_RADIAL).stops[1] == .375f, "a radial stop draft cannot duplicate its neighbor");
    seek(1);
    check(fill(VG_PATTERN_TYPE_RADIAL).cp[0] == radial_base.cp[0] && fill(VG_PATTERN_TYPE_RADIAL).cp[1] == radial_base.cp[1] &&
        fill(VG_PATTERN_TYPE_RADIAL).stops[1] == radial_base.stops[1],
        "cancel restores both circles and the original stop offsets without moving the playhead");

    const auto sweep_base = fill(VG_PATTERN_TYPE_SWEEP);
    ui::gradient_shape_preview sweep_draft{"sweep", {}};
    sweep_draft.draft.geometry[std::size_t(axis::center_x)] = .5f;
    sweep_draft.draft.geometry[std::size_t(axis::center_y)] = .375f;
    sweep_draft.draft.geometry[std::size_t(axis::start_angle)] = 0.f;
    sweep_draft.draft.geometry[std::size_t(axis::end_angle)] = 360.f;
    seek(1, &sweep_draft);
    const auto& sweep_live = fill(VG_PATTERN_TYPE_SWEEP);
    check(point_is(sweep_live.cp[0], 50, 125, 0, 2) && sweep_live.count == sweep_base.count &&
        sweep_live.colors[0] == sweep_base.colors[0] && fill(VG_PATTERN_TYPE_RADIAL).cp[0] == radial_base.cp[0],
        "a full-turn sweep draft updates the center and native half-turn angles without modifying other paint");
    sweep_draft.draft.geometry[std::size_t(axis::start_angle)] = 340.f;
    sweep_draft.draft.geometry[std::size_t(axis::end_angle)] = 30.f;
    seek(1, &sweep_draft);
    check(point_is(fill(VG_PATTERN_TYPE_SWEEP).cp[0], 50, 125, 340.f / 180, 30.f / 180),
        "same-time sweep edits retain the authored reverse order rather than wrapping angles");
    seek(1);
    check(fill(VG_PATTERN_TYPE_SWEEP).cp[0] == sweep_base.cp[0], "cancel restores the original sweep at the same time");
    sweep_draft.draft = {{}, ui::gradient_stop_offset{1, .75f}};
    seek(1, &sweep_draft);
    const auto& sweep_stop = fill(VG_PATTERN_TYPE_SWEEP);
    check(sweep_stop.cp[0] == sweep_base.cp[0] && sweep_stop.m == sweep_base.m &&
        sweep_stop.count == sweep_base.count && sweep_stop.stops[1] == .75f &&
        fill(VG_PATTERN_TYPE_RADIAL).cp[0] == radial_base.cp[0],
        "a sweep stop draft applies at the same time without changing the sweep geometry or another track");
    for (int i = 0; i < sweep_base.count; ++i)
        check(sweep_stop.colors[i] == sweep_base.colors[i] && (i == 1 || sweep_stop.stops[i] == sweep_base.stops[i]),
            "sweep live preview preserves every color and the other offsets");
    sweep_draft.draft.stop->offset = 1.f;
    seek(1, &sweep_draft);
    check(fill(VG_PATTERN_TYPE_SWEEP).stops[1] == .55f, "a sweep stop draft cannot duplicate its neighbor");
    seek(1);
    check(fill(VG_PATTERN_TYPE_SWEEP).stops[1] == sweep_base.stops[1], "cancel restores the original sweep stop offset");
    expect_fallback();

    const auto original = fill(VG_PATTERN_TYPE_LINEAR);
    auto& props = doc.nodes.at("linear").props;
    std::vector<ui::gradient_keyframe> keys;
    std::vector<ui::gradient_stop> stops;
    std::string reason;
    check(ui::decode_gradient_keyframes(std::get<std::string>(props.at("gradient_animation_keyframes")), keys, reason) &&
        ui::decode_gradient_stops(std::get<std::string>(props.at("gradient_stops")), stops, reason), "decode capture track");
    std::array<double, ui::gradient_geometry_channels.size()> authored{.125, .25, .875, .75};
    auto captured = ui::capture_gradient_pose("linear", authored, stops, keys, .5, 1.);
    check(captured.x == .125 && captured.y == .25 && captured.opacity == .75 && captured.tint == "#80C0FF" &&
        captured.geometry[std::size_t(axis::start_x)] == .125 && captured.geometry[std::size_t(axis::end_x)] == .875 &&
        !captured.geometry[std::size_t(axis::center_x)] && captured.stops.size() == stops.size(),
        "capturing coincident interpolated endpoints uses the authored geometry while retaining the sampled pose");
    captured.offset = .5;
    keys.insert(keys.begin() + 1, captured);
    props["gradient_animation_keyframes"] = ui::encode_gradient_keyframes(keys);
    check(rt.mount(doc, 2) && rt.layout({200, 200, 1}), "mount the document with a captured fallback key");
    seek(.5);
    const auto& after = fill(VG_PATTERN_TYPE_LINEAR);
    check(after.cp[0] == original.cp[0] && after.count == original.count && after.extend == original.extend,
        "captured geometry produces the same native brush");
    for (int i = 0; i < after.count; ++i) {
        const auto diff = glm::abs(after.colors[i] - original.colors[i]);
        check(after.stops[i] == original.stops[i] && glm::all(glm::lessThanEqual(diff, glm::vec4(1.001f / 255.f))),
            "captured brush matches its sampled stops within one 8-bit tint step");
    }
}

void test_gradient_keyframe_stops() {
    ui::document doc;
    check(ui::decode_json(R"({"format":"vgui-ui-design","version":1,"root":"root","nodes":{
        "root":{"type":"panel","children":["paint"]},
        "paint":{"type":"gradient","props":{"x":10,"y":20,"width":100,"height":40,
            "gradient_stops":[{"offset":0,"color":"#0000FF"},{"offset":0.375,"color":"#FF0000"},
                {"offset":1,"color":"#12345600"}],
            "gradient_animation":"keyframes","gradient_animation_duration":4,"gradient_animation_loop":false,
            "gradient_animation_keyframes":[
                {"offset":0,"x":0,"y":0,"tint":"#FFFFFF","opacity":1,"interpolation":"smooth"},
                {"offset":0.25,"x":0.5,"y":0,"tint":"#80FFFF","opacity":0.5,"interpolation":"step",
                    "stops":[{"offset":0.125,"color":"#FF000000"},{"offset":0.5,"color":"#00FF00"},
                        {"offset":0.875,"color":"#12345600"}]},
                {"offset":0.5,"x":0,"y":0,"tint":"#FFFFFF","opacity":1,"interpolation":"linear",
                    "stops":[{"offset":0,"color":"#FF000080"},{"offset":0.625,"color":"#FFFFFF"},
                        {"offset":0.9375,"color":"#AB341200"}]},
                {"offset":0.75,"x":0,"y":0,"tint":"#FFFFFF","opacity":1,"interpolation":"linear"},
                {"offset":1,"x":0,"y":0,"tint":"#FFFFFF","opacity":1,"interpolation":"linear"}]}}
    }})", doc), "read keyframed stop palettes");

    std::unique_ptr<ovg_ctx_cb, decltype(&free_ctx_cb)> cb(new_ctx_cb(), &free_ctx_cb);
    std::unique_ptr<rvg_t, void (*)(rvg_t*)> vg(cb->new_rvg(cb->ac), cb->destroy_rvg);
    std::unique_ptr<vgui_context, decltype(&vgui_destroy)> ctx(vgui_create(nullptr, 16), &vgui_destroy);
    vg->width = vg->height = 200;
    static std::vector<vg_gradient_t> fills;
    static const auto set_source = cb->set_source;
    cb->set_source = [](rvg_t* view, vg_pattern_t* p) {
        if (p && p->type == VG_PATTERN_TYPE_LINEAR && p->data) fills.push_back(*static_cast<vg_gradient_t*>(p->data));
        set_source(view, p);
    };
    ui::runtime rt(ctx.get());
    check(rt.mount(doc, 1) && rt.layout({200, 200, 1}), "mount animated stop palettes");
    using palette = std::array<std::array<int, 4>, 3>;
    const auto expect_paint = [&](float x, const std::array<float, 3>& offsets, const palette& colors,
        std::string_view message) {
        cb->clear(vg.get());
        fills.clear();
        vgui_draw(ctx.get(), cb.get(), vg.get());
        check(fills.size() == 1 && fills.front().count == 3, "draw a native gradient with three stops");
        const auto& paint = fills.front();
        check(near(paint.cp[0].x, x), message);
        for (std::size_t i = 0; i < offsets.size(); ++i) {
            const auto& rgba = colors[i];
            const auto& color = paint.colors[i];
            check(paint.stops[i] == offsets[i] && color.r == rgba[0] / 255.f && color.g == rgba[1] / 255.f &&
                color.b == rgba[2] / 255.f && color.a == rgba[3] / 255.f, message);
        }
    };
    constexpr std::array base_offsets{0.f, .375f, 1.f};
    constexpr palette base_colors{{{0, 0, 255, 255}, {255, 0, 0, 255}, {18, 52, 86, 0}}};
    constexpr std::array step_offsets{.125f, .5f, .875f};
    constexpr palette step_colors{{{128, 0, 0, 0}, {0, 255, 0, 128}, {9, 52, 86, 0}}};
    expect_paint(60, base_offsets, base_colors, "mount without sampling keeps the authored stop palette");
    rt.seek_animations(.25);
    expect_paint(67.8125f, {.01953125f, .39453125f, .98046875f},
        {{{0, 0, 255, 198}, {219, 110, 0, 235}, {17, 52, 86, 0}}},
        "smoothly blend base to keyed stops in linear premultiplied RGBA, then apply the sampled tint and opacity");
    rt.seek_animations(1);
    expect_paint(110, step_offsets, step_colors, "an exact key preserves its authored stop bytes before tint and opacity");
    rt.seek_animations(1.5);
    expect_paint(110, step_offsets, step_colors, "step holds the left palette and pose until the next key");
    ui::gradient_shape_preview stop_draft{"paint", {{}, ui::gradient_stop_offset{1, .625f}}};
    rt.seek_animations(1.5, false, &stop_draft);
    expect_paint(110, {.125f, .625f, .875f}, step_colors,
        "a temporary stop drag changes only its offset at the same time, retaining the key's tint and opacity");
    stop_draft.draft.stop->offset = .875f;
    rt.seek_animations(1.5, false, &stop_draft);
    expect_paint(110, step_offsets, step_colors, "a temporary offset cannot duplicate or cross its neighbor");
    stop_draft.draft.stop->offset = .375f;
    rt.seek_animations(1.5, false, &stop_draft);
    expect_paint(110, {.125f, .375f, .875f}, step_colors, "a second draft still samples the same key");
    rt.seek_animations(1.5);
    expect_paint(110, step_offsets, step_colors, "cancel restores the authored stop offsets without changing time");
    ui::gradient_shape_preview color_draft{"paint", {{}, {}, ui::gradient_stop_color{1, 0x80FF00FFu}}};
    rt.seek_animations(1.5, false, &color_draft);
    expect_paint(110, step_offsets, {{{128, 0, 0, 0}, {128, 0, 255, 64}, {9, 52, 86, 0}}},
        "a color draft changes just the requested stop, before multiplying keyframe tint and opacity");
    color_draft.draft.color->color = 0x00FF00FFu;
    rt.seek_animations(1.5, false, &color_draft);
    expect_paint(110, step_offsets, {{{128, 0, 0, 0}, {128, 0, 255, 0}, {9, 52, 86, 0}}},
        "editing only alpha resamples the brush at the same time, preserving the stop's RGB bytes");
    rt.seek_animations(1.5);
    expect_paint(110, step_offsets, step_colors, "discarding the color draft restores the brush without changing time");
    rt.seek_animations(2);
    expect_paint(60, {0.f, .625f, .9375f}, {{{255, 0, 0, 128}, {255, 255, 255, 255}, {171, 52, 18, 0}}},
        "step switches at the exact next key, including transparent RGB bytes");
    rt.seek_animations(2.5);
    expect_paint(60, {0.f, .5f, .96875f}, {{{156, 0, 213, 192}, {255, 188, 188, 255}, {0, 0, 0, 0}}},
        "linear blending returns to the base palette when the right key omits stops");
    rt.seek_animations(3.5);
    expect_paint(60, base_offsets, base_colors, "omitting both palettes preserves every base stop byte");

    auto& props = doc.nodes.at("paint").props;
    std::vector<ui::gradient_keyframe> keys;
    std::vector<ui::gradient_stop> stops;
    std::string reason;
    check(ui::decode_gradient_keyframes(std::get<std::string>(props.at("gradient_animation_keyframes")), keys, reason) &&
        ui::decode_gradient_stops(std::get<std::string>(props.at("gradient_stops")), stops, reason), "decode palette capture track");
    const std::array<double, ui::gradient_geometry_channels.size()> authored{.5, 0, .5, 1};
    const auto step = ui::capture_gradient_pose("linear", authored, stops, keys, 1.5, 4.);
    check(step.interpolation == "step" && step.tint == "#80FFFF" && step.opacity == .5 &&
        ui::parse_text_color(step.stops[0].color) == 0x00ff0000u && step.stops[1].offset == .5f,
        "capture honors step and keeps untinted RGBA, even hidden RGB");
    auto capture = ui::capture_gradient_pose("linear", authored, stops, keys, .25, 4.);
    check(capture.interpolation == "smooth" && capture.x == .078125 && capture.opacity == .921875 &&
        capture.tint == "#EBFFFF" && capture.stops[1].offset == .39453125f &&
        ui::parse_text_color(capture.stops[0].color) == 0xd70000ffu,
        "capture uses the runtime smooth curve, linear-light colors and premultiplied alpha");
    capture.offset = .25 / 4.;
    keys.insert(keys.begin() + 1, capture);
    props["gradient_animation_keyframes"] = ui::encode_gradient_keyframes(keys);
    check(rt.mount(doc, 2) && rt.layout({200, 200, 1}), "mount captured palette");
    rt.seek_animations(.25);
    // The interpolated 235.15625 tint becomes 235 in the JSON key. Applied to
    // the red stop byte 237 this rounds to 218, not the original brush's 219.
    expect_paint(67.8125f, {.01953125f, .39453125f, .98046875f},
        {{{0, 0, 255, 198}, {218, 110, 0, 235}, {17, 52, 86, 0}}},
        "inserting the captured pose reproduces the brush at authored tint precision, without applying tint or opacity twice");

    const auto first = ui::capture_gradient_pose("linear", authored, stops, {}, 0., 4.);
    check(first.stops == stops && first.geometry[0] == .5 && first.geometry[3] == 1. && first.tint == "#FFFFFF" &&
        first.opacity == 1. && first.interpolation == "linear", "an empty track captures the authored paint");

    const std::vector<ui::gradient_stop> close_a{{.00119474309f, "#FF0000"}, {.00129474315f, "#0000FF"}},
        close_b{{.00519330660f, "#FF0000"}, {.00529330689f, "#0000FF"}};
    std::array<ui::gradient_keyframe, 2> close_keys{};
    close_keys[1].offset = 1.;
    close_keys[1].stops = close_b;
    const auto close = ui::capture_gradient_pose("linear", authored, close_a, close_keys, .75, 1.);
    std::vector<ui::gradient_stop> roundtrip;
    check(ui::decode_gradient_stops(ui::encode_gradient_stops(close.stops), roundtrip, reason) &&
        close.stops[0].offset == .00419366593f && close.stops[1].offset > .00429366576f &&
        close.stops[1].offset <= std::nextafter(.00429366576f, 1.f),
        "rounding a minimum-gap blend preserves stop order with the smallest legal adjustment");
}

void test_switch_appearance() {
    ui::document doc;
    check(ui::decode_json(R"({"format":"vgui-ui-design","version":1,"root":"root","nodes":{
        "root":{"type":"panel","children":["switch"]},
        "switch":{"type":"switch","props":{"x":12,"width":134,"height":28,"text":"Wi-Fi",
            "text_position":"left","thumb_color":"#F49B38CC"}}
    }})", doc), "decode optional switch text position and thumb color");
    const auto font_path = fs::path(__FILE__).parent_path() / "res/fonts/DejaVuSans.ttf";
    std::unique_ptr<font_cache_cx, decltype(&free_font_cache)> cache(new_font_cache(), free_font_cache);
    check(cache && load_font_from_path(cache.get(), font_path.string().c_str(), "SwitchTest"), "load switch label font");
    std::unique_ptr<font_familys_t, decltype(&delete_font_family)> font(new_font_family(cache.get(), "SwitchTest", nullptr), delete_font_family);
    check(font && font->count, "resolve switch label font");
    std::unique_ptr<vgui_context, decltype(&vgui_destroy)> ctx(vgui_create(font.get(), 16), &vgui_destroy);
    std::unique_ptr<ovg_ctx_cb, decltype(&free_ctx_cb)> cb(new_ctx_cb(), &free_ctx_cb);
    std::unique_ptr<rvg_t, void (*)(rvg_t*)> vg(cb->new_rvg(cb->ac), cb->destroy_rvg);
    check(bool(ctx) && bool(vg), "create switch draw context");
    vg->width = 180;
    vg->height = 60;
    struct thumb { float x, y, radius; uint32_t color; };
    static std::vector<thumb> thumbs;
    static const auto circle = cb->circle;
    static const auto source = cb->set_source_color;
    cb->circle = [](rvg_t* view, float x, float y, float radius) {
        thumbs.push_back({x, y, radius, 0});
        circle(view, x, y, radius);
    };
    cb->set_source_color = [](rvg_t* view, uint32_t color) {
        if (!thumbs.empty() && !thumbs.back().color) thumbs.back().color = color;
        source(view, color);
    };
    struct label { std::string text; int x, width; float align_x; };
    static std::vector<label> labels;
    static const auto add_text = cb->add_text;
    cb->add_text = [](rvg_t* view, text_st_t* text, text_style_t* style, text_box_rt* box) {
        labels.push_back({std::string(text->text, text->text_len), box->rc.x, box->rc.z, box->text_align.x});
        add_text(view, text, style, box);
    };
    ui::runtime rt(ctx.get());
    const auto mount = [&](std::uint64_t revision) {
        check(rt.mount(doc, revision) && rt.layout({180, 60, 1}), "mount and lay out the switch");
    };
    const auto draw = [&] {
        for (int i = 0; i < 24; ++i) vgui_update(ctx.get(), .05f);
        cb->clear(vg.get());
        thumbs.clear();
        labels.clear();
        vgui_draw(ctx.get(), cb.get(), vg.get());
        check(thumbs.size() == 1, "switch must draw one thumb");
        return thumbs.front();
    };
    mount(1);
    const auto off = draw();
    check(off.x > 100 && off.color == 0xCC389BF4u, "left text places the track on the right with the authored RGBA thumb");
    vgui_input_mouse_down(ctx.get(), 1, 24, 14, 1); // Click the text side, not the track.
    vgui_input_mouse_up(ctx.get(), 1, 24, 14, 1);
    const auto on = draw();
    check(vgui_widget_get_checked(rt.widget("switch")) && on.x > off.x && on.color == off.color,
        "clicking the text toggles the switch and both states use the authored color");
    auto& props = doc.nodes.at("switch").props;
    props.erase("text_position");
    props.erase("thumb_color");
    mount(2);
    const auto legacy = draw();
    check(legacy.x < 48 && legacy.color == 0xFFFFFFFFu, "old JSON keeps a left track and the default theme thumb in LTR");
    vgui_context_set_direction(ctx.get(), VGUI_DIRECTION_RTL);
    mount(3);
    check(draw().x > 100, "automatic text position still mirrors in RTL");
    props["text_position"] = "right"s;
    mount(4);
    check(draw().x < 48, "explicit right text is physical even in RTL");
    props["text"] = ""s;
    mount(5);
    check(draw().x > 100 && labels.empty(), "an empty label keeps the natural RTL track position");

    // New physical labels override old single-label props without depending on direction.
    props["text"] = "Old caption"s;
    props["text_left"] = "Disabled"s;
    props["text_right"] = "On"s;
    props["width"] = 0.0;
    mount(6);
    const auto dual = draw();
    const auto expect_sides = [&](float thumb_x) {
        check(labels.size() == 2 && labels[0].text == "Disabled" && labels[1].text == "On" &&
            labels[0].x + labels[0].width < thumb_x && labels[1].x > thumb_x,
            "distinct labels must be painted on their physical sides of the track");
    };
    expect_sides(dual.x);
    auto* widget = rt.widget("switch");
    float dual_width = 0;
    vgui_widget_get_rect(widget, nullptr, nullptr, &dual_width, nullptr);
    check(std::string_view(vgui_widget_get_a11y_name(widget)) == "Disabled / On", "both labels provide the accessible toggle name");
    const float right_x = labels[1].x + labels[1].width / 2;
    vgui_input_mouse_down(ctx.get(), 1, right_x, 14, 1);
    vgui_input_mouse_up(ctx.get(), 1, right_x, 14, 1);
    const auto toggled = draw();
    expect_sides(toggled.x);
    check(vgui_widget_get_checked(widget) && toggled.x < dual.x, "right text toggles the switch without relocating its labels in RTL");
    auto roundtrip = ui::document{};
    std::string json;
    check(ui::encode_json(doc, json) && ui::decode_json(json, roundtrip) && roundtrip == doc, "both switch sides survive JSON roundtrip");
    props["text_left"] = ""s;
    mount(7);
    draw();
    float single_width = 0;
    vgui_widget_get_rect(rt.widget("switch"), nullptr, nullptr, &single_width, nullptr);
    check(labels.size() == 1 && labels.front().text == "On" && single_width < dual_width,
        "an empty side omits its label and gap from intrinsic width");
    props["width"] = 30.0;
    props["text_left"] = "Disabled"s;
    mount(8);
    draw();
    check(labels.empty(), "an undersized switch must not draw text over its track");

    props["width"] = 155.0;
    props["text_align_x"] = "start"s;
    props["text_align_left"] = "center"s;
    props["text_align_right"] = "left"s;
    mount(9);
    expect_sides(draw().x);
    check(near(labels[0].align_x, .5f) && near(labels[1].align_x, 0.f),
        "per-side alignment must reach the two draw calls without RTL mirroring");
    props["text_align_left"] = "right"s;
    props.erase("text_align_right");
    vgui_context_set_direction(ctx.get(), VGUI_DIRECTION_LTR);
    mount(10);
    expect_sides(draw().x);
    check(near(labels[0].align_x, 1.f) && near(labels[1].align_x, 0.f),
        "changing the left side must not change the right side's inherited alignment");
    vgui_context_set_direction(ctx.get(), VGUI_DIRECTION_RTL);
    expect_sides(draw().x);
    check(near(labels[0].align_x, 1.f) && near(labels[1].align_x, 1.f),
        "only an inherited alignment follows the context direction");

    vgui_context_set_direction(ctx.get(), VGUI_DIRECTION_LTR);
    props["text_left"] = ""s;
    props["text_right"] = ""s;
    props["width"] = props["height"] = 0.0;
    mount(11);
    const auto themed = draw();
    const auto size = [&] {
        ui::pixel_rect r{};
        vgui_widget_get_rect(rt.widget("switch"), &r.x, &r.y, &r.width, &r.height);
        return r;
    };
    const float default_width = size().width;
    props["switch_track_width"] = 64.0;
    mount(12);
    check(near(size().width, 64) && near(draw().radius, themed.radius),
        "changing the track length must not change the inherited slider height");
    props["switch_track_height"] = 30.0;
    props.erase("switch_track_width");
    mount(13);
    check(near(size().width, default_width) && near(size().height, 30) && near(draw().radius, 13),
        "changing track height fits auto layout and enlarges the thumb without changing track length");
    props["switch_track_width"] = 64.0;
    mount(14);
    const auto size_at_one = size();
    const auto left_end = draw();
    vgui_widget_set_checked(rt.widget("switch"), 1);
    check(near(draw().x - left_end.x, 34), "thumb travels across the configured length in the checked state");
    vg->width = 270;
    vg->height = 90;
    check(rt.layout({180, 60, 1.5f}).has_value(), "scale a mounted switch without remounting");
    const auto scaled = size();
    check(near(scaled.width, size_at_one.width * 1.5f) && near(scaled.height, size_at_one.height * 1.5f) &&
        near(draw().radius, 19.5f), "logical track size must scale once with DPI");
    props["width"] = 24.0;
    props["height"] = 10.0;
    mount(15);
    const auto bounded = size();
    const auto small = draw();
    check(near(bounded.width, 24) && near(bounded.height, 10) && near(small.radius, 3) &&
        near(small.y, bounded.y + bounded.height / 2) && small.x - small.radius >= bounded.x,
        "an oversized track must remain within a fixed-height switch without clipping the thumb");
    props["width"] = props["height"] = 0.0;
    mount(16);
    auto* sized_switch = rt.widget("switch");
    const auto rev = vgui_context_paint_revision(ctx.get());
    vgui_switch_set_track_size(sized_switch, 64, 30);
    check(vgui_context_paint_revision(ctx.get()) == rev, "unchanged track size must not invalidate on-demand drawing");
    vgui_switch_set_track_size(sized_switch, 0, std::numeric_limits<float>::infinity());
    check(vgui_context_paint_revision(ctx.get()) != rev && rt.layout({180, 60, 1}).has_value() &&
        near(size().width, default_width) && near(draw().radius, themed.radius),
        "zero and invalid dimensions must restore theme sizes and invalidate layout");
}

void test_progress_colors() {
    std::unique_ptr<ovg_ctx_cb, decltype(&free_ctx_cb)> cb(new_ctx_cb(), &free_ctx_cb);
    std::unique_ptr<rvg_t, void (*)(rvg_t*)> vg(cb->new_rvg(cb->ac), cb->destroy_rvg);
    std::unique_ptr<vgui_context, decltype(&vgui_destroy)> ctx(vgui_create(nullptr, 16), &vgui_destroy);
    check(bool(ctx), "create progress color context");
    vg->width = vg->height = 120;
    static std::vector<uint32_t> colors;
    static const auto source = cb->set_source_color;
    cb->set_source_color = [](rvg_t* view, uint32_t color) {
        colors.push_back(color);
        source(view, color);
    };
    ui::runtime rt(ctx.get());
    auto doc = ui::make_document();
    doc.nodes.at(doc.root).children.push_back("progress");
    auto& progress = doc.nodes.emplace("progress", ui::node{"progress", "", {{"width", 80.0}, {"height", 60.0},
        {"value", 0.7}, {"show_text", false}}, {}}).first->second;
    const auto expect = [&](std::uint64_t revision, uint32_t fill, uint32_t track, const char* message) {
        check(rt.mount(doc, revision) && rt.layout({120, 120, 1}), "mount and lay out the progress bar");
        cb->clear(vg.get());
        colors.clear();
        vgui_draw(ctx.get(), cb.get(), vg.get());
        // OVG receives packed AABBGGRR through the same callbacks used by native preview.
        check(std::ranges::find(colors, fill) != colors.end() && std::ranges::find(colors, track) != colors.end(), message);
    };
    expect(1, 0xFFFFCB8Cu, 0xFF303030u, "default horizontal progress draws pale blue and a neutral track");
    progress.props["radial"] = true;
    progress.props["accent_color"] = ""s;
    progress.props["track_color"] = "#234567"s;
    expect(2, 0xFFFFCB8Cu, 0xFF674523u, "clearing a radial progress fill restores pale blue without swapping channels");
    progress.props.erase("radial");
    progress.props["vertical"] = true;
    progress.props["accent_color"] = "#257693"s;
    expect(3, 0xFF937625u, 0xFF674523u, "vertical progress preserves explicitly configured fill and track channels");
}

void test_radio_group_input() {
    const auto radio = [](double x, std::string group, bool checked) {
        ui::node item{"radio", "", {{"x", x}, {"width", 80.0}, {"height", 28.0}, {"checked", checked}}, {}};
        if (!group.empty()) item.props["radio_group"] = std::move(group);
        return item;
    };
    auto doc = ui::make_document();
    doc.nodes.at(doc.root).children = {"first", "second", "independent", "default", "default-other", "nested"};
    doc.nodes.emplace("first", radio(0, "主题", true));
    doc.nodes.emplace("second", radio(80, "主题", false));
    doc.nodes.emplace("independent", radio(160, "语言", true));
    doc.nodes.emplace("default", radio(240, {}, true));
    doc.nodes.emplace("default-other", radio(320, {}, false));
    doc.nodes.emplace("nested", ui::node{"panel", "", {{"y", 40.0}, {"width", 100.0}, {"height", 40.0}}, {"other-parent"}});
    doc.nodes.emplace("other-parent", radio(0, "主题", true));
    const auto original = doc;
    std::unique_ptr<vgui_context, decltype(&vgui_destroy)> ctx(vgui_create(nullptr, 16), &vgui_destroy);
    check(bool(ctx), "create radio group context");
    ui::runtime rt(ctx.get());
    check(rt.mount(doc, 1), "mount independent radio groups");
    const auto layout = rt.layout({420, 120, 1});
    check(bool(layout), "layout radio groups");
    const auto checked = [&](const char* id) { return vgui_widget_get_checked(rt.widget(id)) != 0; };
    check(checked("first") && !checked("second") && checked("independent") && checked("default") && checked("other-parent"),
        "initial choices must survive in all groups");
    const auto click = [&](const char* id) {
        const auto& bounds = node(*layout, id).rect_px;
        const float x = bounds.x + bounds.width / 2, y = bounds.y + bounds.height / 2;
        vgui_input_mouse_down(ctx.get(), 1, x, y, 1);
        vgui_input_mouse_up(ctx.get(), 1, x, y, 1);
    };
    click("second");
    check(checked("second") && !checked("first") && checked("independent") && checked("default") && checked("other-parent"),
        "mouse selection must only clear the matching group under the same parent");
    vgui_set_focus(ctx.get(), rt.widget("first"));
    check(vgui_input_key(ctx.get(), ' ', 1, 0), "focused radio did not handle Space");
    vgui_input_key(ctx.get(), ' ', 0, 0);
    check(checked("first") && !checked("second") && checked("independent") && checked("default"),
        "keyboard activation must use the same named-group scope");
    click("default-other");
    check(!checked("default") && checked("default-other") && checked("first") && checked("independent") && checked("other-parent"),
        "unnamed siblings remain mutually exclusive without affecting named groups");
    check(doc == original && rt.mount(doc, 2) && checked("first") && checked("default") && !checked("default-other"),
        "runtime input does not change saved initial selections");
}

void run() {
    std::unique_ptr<vgui_context, decltype(&vgui_destroy)> ctx(vgui_create(nullptr, 16), &vgui_destroy);
    check(bool(ctx), "create headless context");
    ui::runtime rt(ctx.get());
    check(!rt.layout({320, 200, 1}), "unmounted layout should be empty");
    auto doc = fixture();
    const auto original = doc;
    action_log actions;
    ui::runtime_bindings bindings;
    bindings.project_root = std::filesystem::temp_directory_path();
    bindings.on_action = on_action;
    bindings.user = &actions;
    ui::diagnostic error;
    check(rt.mount(doc, 42, bindings, &error), "mount valid fixture");
    check(doc == original, "mount changed document");
    check(rt.widget("root") && rt.widget("row") && rt.widget("heading") &&
        rt.widget("submit") && rt.widget("field") && rt.widget("photo"), "ID mapping incomplete");
    check(!rt.widget("unknown"), "unknown ID must not map to a widget");
    check(vgui_widget_get_kind(rt.widget("heading")) == VGUI_LABEL &&
        vgui_widget_get_kind(rt.widget("submit")) == VGUI_BUTTON &&
        vgui_widget_get_kind(rt.widget("field")) == VGUI_EDIT &&
        vgui_widget_get_kind(rt.widget("photo")) == VGUI_IMAGE, "widget kinds");
    check(!rt.diagnostics().empty(), "missing image must report a diagnostic");
    bool descriptive = false;
    for (const auto& d : rt.diagnostics()) if (!d.message.empty()) descriptive = true;
    check(descriptive, "resource diagnostic must explain failure");

    std::uint64_t serial = 0;
    float narrow_row_width = 0;
    for (auto view : {ui::viewport{240, 180, 1}, ui::viewport{400, 180, 1},
                      ui::viewport{240, 180, 2}, ui::viewport{400, 180, 2}}) {
        auto snapshot = rt.layout(view);
        check(bool(snapshot), "valid viewport layout");
        check(snapshot->revision == 42 && snapshot->serial > serial, "snapshot revision/serial");
        serial = snapshot->serial;
        check(snapshot->pixel_width == int(view.width * view.scale) &&
            snapshot->pixel_height == int(view.height * view.scale), "physical viewport dimensions");
        check(snapshot->nodes.size() == 6, "snapshot node count");
        const char* order[] = {"root", "row", "heading", "submit", "field", "photo"};
        for (std::size_t i = 0; i < snapshot->nodes.size(); ++i) {
            const auto& n = snapshot->nodes[i];
            check(n.id == order[i], "snapshot must follow preorder");
            ui::pixel_rect actual;
            vgui_widget_get_rect(rt.widget(n.id), &actual.x, &actual.y, &actual.width, &actual.height);
            same_rect(n.rect_px, actual);
            check(n.visible && n.enabled, "visible enabled fixture node");
        }
        check(node(*snapshot, "photo").image_placeholder, "missing image placeholder");
        auto root = node(*snapshot, "root").rect_px;
        check(near(root.width, view.width * view.scale) && near(root.height, view.height * view.scale),
            "zero root dimensions fill viewport");
        auto heading = node(*snapshot, "heading").rect_px;
        auto button = node(*snapshot, "submit").rect_px;
        check(near(heading.width, 60 * view.scale) && near(button.width, 80 * view.scale), "DPI-scaled sizes");
        // Older documents omit every font property; preserve the core defaults.
        for (const auto* id : {"heading", "submit", "field"}) {
            auto* widget = rt.widget(id);
            check(near(vgui_widget_get_font_size(widget), vgui_context_get_font_size(ctx.get())),
                std::string(id) + ": omitted font size should inherit the context size");
            check(vgui_widget_get_font_weight(widget) == VGUI_FONT_WEIGHT_DEFAULT &&
                vgui_widget_get_font_slant(widget) == VGUI_FONT_SLANT_DEFAULT,
                std::string(id) + ": omitted font weight and slant should preserve the defaults");
        }
        check(near(button.x - (heading.x + heading.width), 5 * view.scale), "row gap scales");
        auto row = node(*snapshot, "row").rect_px;
        if (view.scale == 1 && view.width == 240) narrow_row_width = row.width;
        if (view.scale == 1 && view.width == 400) check(row.width > narrow_row_width, "row responds to wider viewport");
    }
    check(!rt.layout({0, 180, 1}) && !rt.layout({240, 180, 0}) &&
        !rt.layout({240, 180, std::numeric_limits<float>::quiet_NaN()}), "invalid viewport rejected");

    vgui_edit_set_text(rt.widget("field"), "runtime value");
    check(doc == original, "runtime edit wrote back to document");
    check(rt.layout({240, 180, 1}).has_value(), "relayout after edit");
    check(std::string(vgui_edit_get_text(rt.widget("field"))) == "runtime value", "relayout preserves live edit");
    auto button_layout = rt.layout({240, 180, 1});
    auto b = node(*button_layout, "submit").rect_px;
    float x = b.x + b.width / 2, y = b.y + b.height / 2;
    vgui_input_mouse_move(ctx.get(), x, y);
    vgui_input_mouse_down(ctx.get(), 1, x, y, 0);
    check(actions.calls == 0, "action must not fire on pointer down");
    vgui_input_mouse_up(ctx.get(), 1, x, y, 0);
    check(actions.calls == 1 && actions.id == "submit" && actions.action == "form.send", "activation routes action");
    check(doc == original, "action wrote back to document");

    auto* old_root = rt.widget("root");
    auto invalid = doc;
    invalid.nodes.at("root").children.push_back("absent");
    error = {};
    check(!rt.mount(invalid, 99, bindings, &error) && !error.message.empty(), "invalid mount diagnosed");
    check(rt.widget("root") == old_root && std::string(vgui_edit_get_text(rt.widget("field"))) == "runtime value",
        "invalid mount preserves old tree and live state");
    auto preserved = rt.layout({240, 180, 1});
    check(preserved && preserved->revision == 42, "invalid mount preserves revision");
    check(rt.mount(doc, 43, bindings, &error), "valid remount");
    check(std::string(vgui_edit_get_text(rt.widget("field"))) == "original", "remount restores document value");
    rt.unmount();
    check(!rt.widget("root") && !rt.layout({240, 180, 1}), "unmount clears mapping and snapshot");
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--links") return test_link_paths();
        check(argc == 1, "usage: ui_design_runtime_test [--links]");
        run();
        test_declarative_widgets();
        test_button_gradients();
        test_panel_gradients();
        test_decorative_gradients();
        test_gradient_animations();
        test_gradient_keyframe_animation();
        test_gradient_keyframe_geometry();
        test_gradient_keyframe_stops();
        test_switch_appearance();
        test_progress_colors();
        test_radio_group_input();
        test_label_orientation();
        test_font_properties();
        test_font_families();
        test_image_loading();
        test_button_atlases();
        test_button_icon_sources();
        test_collection_icons_and_actions();
        std::puts("ui_design_runtime_test: ok");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ui_design_runtime_test: %s\n", e.what());
        return 1;
    }
}
