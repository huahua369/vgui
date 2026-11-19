// U1 runtime contract tests: core-only, no window, GPU or font required.
#include "runtime.h"

#include <algorithm>
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

    std::unique_ptr<vgui_context, decltype(&vgui_destroy)> ctx(vgui_create(nullptr, 16), &vgui_destroy);
    check(bool(ctx), "create button atlas context");
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
    check(std::ranges::is_permutation(loader.calls, expected), "buttons and images must share one canonical load per key");
    loader.textures.clear();
    const auto snapshot = rt.layout({400, 600, 1});
    check(bool(snapshot), "layout button atlas fixtures");
    check_loaded(rt, *snapshot, "photo", width, height);
    check_loaded(rt, *snapshot, "odd-photo", width, height - 1);
    check(rt.diagnostics().size() == 3, "only unavailable button atlases should report diagnostics");
    for (auto id : {"odd", "bad", "missing"}) check_placeholder(rt, *snapshot, id, "atlas");
    for (auto id : {"skinned", "repeat", "plain"})
        check(!node(*snapshot, id).image_placeholder, std::string(id) + ": unexpected button placeholder");

    float plain_w, plain_h;
    vgui_widget_get_intrinsic_size(rt.widget("plain"), &plain_w, &plain_h);
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
        test_image_loading();
        test_button_atlases();
        std::puts("ui_design_runtime_test: ok");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ui_design_runtime_test: %s\n", e.what());
        return 1;
    }
}
