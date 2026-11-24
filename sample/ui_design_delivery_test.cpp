// U3: export, relocate, verify, and mount without a window or editor kit.
#include "delivery.h"
#include "delivery_hash.h"
#include "runtime.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace ui = vgui::ui_design;
namespace fs = std::filesystem;
using namespace std::string_literals;

namespace {
void check(bool ok, std::string_view message) {
    if (!ok) throw std::runtime_error(std::string(message));
}

struct temp_dir {
    fs::path path;

    temp_dir() {
        const auto parent = fs::temp_directory_path();
        std::random_device random;
        for (int i = 0; i < 32; ++i) {
            path = parent / ("vgui-u3-" + std::to_string(random()) + "-" + std::to_string(random()));
            if (fs::create_directory(path)) return;
        }
        throw std::runtime_error("could not reserve a U3 test directory");
    }
    temp_dir(const temp_dir&) = delete;
    temp_dir& operator=(const temp_dir&) = delete;
    ~temp_dir() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

void write_bytes(const fs::path& path, std::string_view bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    check(bool(out), "write fixture: " + path.string());
}

std::string read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    check(bool(in), "open fixture: " + path.string());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::map<fs::path, std::string> bundle_files(const fs::path& root) {
    std::map<fs::path, std::string> files;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        const auto status = entry.symlink_status();
        if (fs::is_directory(status)) continue;
        check(fs::is_regular_file(status), "delivery must contain ordinary files");
        files.emplace(entry.path().lexically_relative(root), read_bytes(entry.path()));
    }
    return files;
}

const auto rgba = "\x01\0\r\n\x80\xaa\xff\x44"s; // Two raw RGBA8 pixels, decoded by the host.

ui::document fixture() {
    return {"root", {
        {"root", {"panel", "Delivery fixture", {{"layout", "row"s}, {"gap", 5.0},
            {"pad_left", 8.0}, {"pad_top", 6.0}}, {"send", "photo"}}},
        {"send", {"button", "", {{"text", "Send"s}, {"action", "form.send"s},
            {"width", 40.0}, {"height", 24.0}}, {}}},
        {"photo", {"image", "", {{"resource", "photo"s}, {"width", 16.0}, {"height", 12.0}}, {}}},
    }, {{"photo", "art with spaces/pixels.RGBA"}, {"unused", "art with spaces/notes"}}};
}

struct image_loader {
    fs::path expected;
    int calls = 0;
    std::unique_ptr<vgui_texture, decltype(&vgui_texture_destroy)> texture{nullptr, &vgui_texture_destroy};

    static vgui_texture* load(vgui_context* ctx, std::string_view key, const fs::path& path, void* user) {
        auto& self = *static_cast<image_loader*>(user);
        ++self.calls;
        check(key == "photo" && path == self.expected, "runtime resolved the wrong asset");
        const auto bytes = read_bytes(path);
        check(bytes == rgba, "runtime received the wrong image bytes");
        const vgui_texture_desc desc{2, 1, 0, VGUI_TEXTURE_FORMAT_RGBA8, bytes.data()};
        self.texture.reset(vgui_texture_create(ctx, &desc));
        return self.texture.get();
    }
};

bool same_rect(const ui::pixel_rect& a, const ui::pixel_rect& b) {
    return std::abs(a.x - b.x) < 0.01f && std::abs(a.y - b.y) < 0.01f &&
        std::abs(a.width - b.width) < 0.01f && std::abs(a.height - b.height) < 0.01f;
}

void check_layout(const ui::document& doc, const fs::path& directory) {
    std::unique_ptr<vgui_context, decltype(&vgui_destroy)> ctx(vgui_create(nullptr, 16), &vgui_destroy);
    check(bool(ctx), "create core context");
    image_loader loader{fs::canonical(directory / doc.resources.at("photo"))};
    ui::runtime rt(ctx.get());
    const ui::runtime_bindings bindings{directory, image_loader::load, nullptr, &loader};
    ui::diagnostic error;
    if (!rt.mount(doc, 1, bindings, &error)) throw std::runtime_error("mount: " + error.message);
    check(loader.calls == 1 && loader.texture && rt.diagnostics().empty(), "load one referenced texture");

    const auto layout = rt.layout({160, 100, 2});
    constexpr std::array ids{"root", "send", "photo"};
    constexpr std::array kinds{VGUI_PANEL, VGUI_BUTTON, VGUI_IMAGE};
    constexpr std::array<ui::pixel_rect, 3> rects{{{0, 0, 320, 200}, {16, 12, 80, 48}, {106, 12, 32, 24}}};
    check(layout && layout->nodes.size() == ids.size(), "layout node count");
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const auto& node = layout->nodes[i];
        auto* widget = rt.widget(ids[i]);
        check(node.id == ids[i] && widget && vgui_widget_get_kind(widget) == kinds[i], "layout tree order or kind");
        check(node.visible && !node.image_placeholder, "layout contained a hidden node or placeholder");
        ui::pixel_rect actual;
        vgui_widget_get_rect(widget, &actual.x, &actual.y, &actual.width, &actual.height);
        check(same_rect(actual, rects[i]) && same_rect(actual, node.rect_px), "unexpected core rectangle: "s + ids[i]);
    }
}

void test_delivery() {
    temp_dir temp;
    const auto doc = fixture();
    const auto project = temp.path / "source";
    fs::create_directories(project / "art with spaces");
    write_bytes(project / doc.resources.at("photo"), rgba);
    write_bytes(project / doc.resources.at("unused"), "abc");
    check_layout(doc, project);

    ui::diagnostic error;
    const auto first = temp.path / "first";
    const auto second = temp.path / "second";
    for (const auto& destination : {first, second}) {
        if (!ui::export_delivery(doc, project, destination, &error))
            throw std::runtime_error("export: " + error.path + ": " + error.message);
    }
    const auto files = bundle_files(first);
    check(files == bundle_files(second), "repeated exports differ");
    check(!ui::export_delivery(doc, project, first, &error) && bundle_files(first) == files,
        "export must not overwrite an existing destination");

    auto expected = doc;
    expected.resources = {{"photo", "assets/1.rgba"}, {"unused", "assets/2.bin"}};
    const fs::path design(ui::delivery_document), manifest(ui::delivery_manifest);
    std::string canonical;
    check(ui::encode_json(expected, canonical), "encode expected delivery document");
    check(files.size() == 4 && files.at(design) == canonical &&
        files.at("assets/1.rgba") == rgba && files.at("assets/2.bin") == "abc", "unexpected export contents");
    check(files.at(manifest).find("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") !=
        std::string::npos, "manifest must contain the SHA-256 of the unreferenced asset");

    const auto moved = temp.path / "recipient" / "renamed bundle";
    fs::create_directory(moved.parent_path());
    fs::rename(first, moved);
    fs::remove_all(project);
    ui::document received;
    if (!ui::read_delivery(moved, received, &error))
        throw std::runtime_error("verify moved bundle: " + error.path + ": " + error.message);
    check(received == expected, "verification did not return the rewritten document");
    check_layout(received, moved);

    const auto reject_tamper = [&](const fs::path& file, std::string_view bytes) {
        write_bytes(moved / file, bytes);
        auto out = doc; // Distinct from the verified document and any tampered candidate.
        error = {};
        check(!ui::read_delivery(moved, out, &error) && !error.message.empty(),
            "tampering was not rejected with a diagnostic: " + file.string());
        check(out == doc, "failed verification changed out: " + file.string());
        write_bytes(moved / file, files.at(file));
    };
    auto changed_pixels = rgba;
    changed_pixels[0] ^= 1; // Preserve byte count: this must fail the hash check.
    reject_tamper("assets/1.rgba", changed_pixels);
    reject_tamper("assets/2.bin", "abd");
    auto changed_doc = expected;
    changed_doc.nodes.at("send").props.at("text") = "Sent"s;
    check(ui::encode_json(changed_doc, canonical), "encode valid but modified document");
    reject_tamper(design, canonical);
    reject_tamper(manifest, files.at(manifest) + '\n');
}

std::string replace_once(std::string text, std::string_view original, std::string_view replacement) {
    const auto at = text.find(original);
    check(at != std::string::npos, "expected fixture bytes to tamper: " + std::string(original));
    text.replace(at, original.size(), replacement);
    return text;
}

std::string digest(std::string_view text) {
    ui::detail::sha256 hash;
    hash.update(text);
    return hash.hex_digest();
}

void test_json_delivery() {
    temp_dir temp;
    const auto project = temp.path / "source";
    fs::create_directories(project / "art with spaces");
    auto doc = fixture();
    doc.resources.insert({{"atlas", "button.PNG"}, {"icon", "mark.png"}, {"row", "row.rgba"},
        {"custom_font", "family.OTF"}});
    auto& send = doc.nodes.at("send");
    send.props.insert({{"atlas", "atlas"s}, {"icon", "icon"s}, {"font_family", "builtin.sans_sc"s}});
    doc.nodes.at(doc.root).children.push_back("rows");
    doc.nodes.emplace("rows", ui::node{"tree", "", {{"items",
        R"([{"children":[{"buttons":[{"icon":"row"}],"text":"Child"}],"text":"Parent"}])"s}}, {}});
    doc.nodes.at(doc.root).children.push_back("paint");
    const std::array<ui::gradient_keyframe, 1> keys{{{0.25, 0.5, -0.125, "#ABCDEF", 0.75, "smooth"}}};
    doc.nodes.emplace("paint", ui::node{"gradient", "", {{"gradient_animation", "keyframes"s},
        {"gradient_animation_keyframes", ui::encode_gradient_keyframes(keys)}}, {}});
    const std::map<std::string, std::string, std::less<>> input{
        {"atlas", "x"}, {"custom_font", "font"}, {"icon", "\0\r\n"s}, {"photo", rgba}, {"row", ""}, {"unused", "abc"}};
    for (const auto& [key, bytes] : input) write_bytes(project / doc.resources.at(key), bytes);
    const auto original = doc;
    const auto source_files = bundle_files(project);
    const auto first = temp.path / "first.vgui.package.json";
    const auto second = temp.path / "second.vgui.package.json";
    const auto rejected = temp.path / "rejected.vgui.package.json";
    ui::diagnostic error;
    ui::delivery_summary summary;
    check(ui::summarize_delivery(doc, project, false, summary, &error) && summary.resource_count == 6 &&
        summary.resource_bytes == 19 && summary.base64_bytes == 0, "directory summary must count every registered asset");
    check(ui::summarize_delivery(doc, project, true, summary, &error) && summary.resource_count == 5 &&
        summary.resource_bytes == 15 && summary.base64_bytes == 24, "standalone summary must count all non-font assets");

    auto font_design = doc;
    font_design.nodes.at("send").props.at("font_family") = "custom_font"s;
    check(!ui::summarize_delivery(font_design, project, true, summary, &error) && summary.resource_count == 5 &&
        summary.resource_bytes == 15 && summary.base64_bytes == 24, "failed summary must leave output unchanged");
    check(!ui::export_json_delivery(font_design, project, rejected, &error) && !fs::exists(rejected) &&
        error.path == "/nodes/send/props/font_family" && error.message.find("use directory delivery") != std::string::npos,
        "project fonts must report the referring property and suggest directory delivery");
    const auto directory = temp.path / "font-directory";
    ui::document directory_doc;
    check(ui::export_delivery(font_design, project, directory, &error) && ui::read_delivery(directory, directory_doc, &error) &&
        directory_doc.nodes == font_design.nodes && read_bytes(directory / directory_doc.resources.at("custom_font")) == "font",
        "directory delivery must still carry project fonts without changing their references");
    auto missing_image = doc;
    missing_image.resources.at("atlas") = "missing.png";
    check(!ui::export_json_delivery(missing_image, project, rejected, &error) && !fs::exists(rejected) &&
        error.path == "/resources/atlas", "a missing referenced atlas must not yield a partial export");
    missing_image.resources.at("atlas") = "../outside.png";
    check(!ui::summarize_delivery(missing_image, project, true, summary, &error) && error.path == "/resources/atlas",
        "a traversal resource must fail the standalone preflight");
    const auto outside = temp.path / "outside.png";
    write_bytes(outside, "x");
    std::error_code link_error;
    fs::create_symlink(outside, project / "escape.png", link_error);
    if (!link_error) {
        missing_image.resources.at("atlas") = "escape.png";
        check(!ui::summarize_delivery(missing_image, project, true, summary, &error) &&
            !ui::export_json_delivery(missing_image, project, rejected, &error) && !fs::exists(rejected),
            "standalone export must not follow an asset link outside the project");
        fs::remove(project / "escape.png");
    }
    check(!ui::export_json_delivery(doc, project, temp.path / "not-a-package.json", &error), "require the standalone export suffix");
    for (const auto& path : {first, second}) {
        if (!ui::export_json_delivery(doc, project, path, &error))
            throw std::runtime_error("standalone export: " + error.path + ": " + error.message);
    }
    const auto content = read_bytes(first);
    check(content == read_bytes(second) && content.find("AQANCoCq/0Q=") != std::string::npos,
        "standalone bytes are not deterministic canonical Base64");
    check(!ui::export_json_delivery(doc, project, first, &error) && content == read_bytes(first),
        "standalone export must not overwrite an existing destination");
    check(doc == original && bundle_files(project) == source_files, "delivery changed the source document or project files");

    auto expected = doc;
    expected.resources = {{"atlas", "assets/1.png"}, {"icon", "assets/2.png"}, {"photo", "assets/3.rgba"},
        {"row", "assets/4.rgba"}, {"unused", "assets/5.bin"}};
    const auto recipient = temp.path / "recipient";
    fs::create_directory(recipient);
    const auto moved = recipient / "renamed package.vgui.package.json";
    fs::rename(first, moved);
    fs::remove_all(project);
    ui::embedded_delivery received;
    if (!ui::read_json_delivery(moved, received, &error))
        throw std::runtime_error("verify moved standalone: " + error.path + ": " + error.message);
    check(received.design == expected && received.images.size() == expected.resources.size(),
        "standalone verification must preserve the non-font registry and omit the unused project font");
    for (const auto& [key, name] : expected.resources) {
        const auto& bytes = input.at(key);
        check(received.images.at(key) == std::vector<std::uint8_t>(bytes.begin(), bytes.end()), "image bytes changed: " + key);
    }
    check(bundle_files(recipient).size() == 1, "standalone verification extracted assets on disk");

    const ui::embedded_delivery sentinel{doc, {{"unrelated", {42}}}};
    const auto reject_tamper = [&](std::string_view tamper, std::string_view diagnostic_text) {
        write_bytes(second, tamper);
        auto out = sentinel;
        check(!ui::read_json_delivery(second, out, &error) && error.message.find(diagnostic_text) != std::string::npos,
            "standalone verification did not reject tampering as expected: " + std::string(diagnostic_text) + ": " + error.message);
        check(out.design == sentinel.design && out.images == sentinel.images, "failed standalone verification changed out");
    };
    reject_tamper(content + '\n', "canonical JSON");
    auto raw_nul = content;
    raw_nul.push_back('\0');
    reject_tamper(raw_nul, "raw NUL");
    reject_tamper("{\"version\":1," + content.substr(1), "canonical JSON");
    reject_tamper(replace_once(content, "\"version\":1}", "\"version\":2}"), "format, version or fields");
    reject_tamper(replace_once(content, "AQANCoCq/0Q=", "BQANCoCq/0Q="), "image SHA-256");
    reject_tamper(replace_once(content, "AQANCoCq/0Q=", "A_ANCoCq/0Q="), "Base64 character");
    reject_tamper(replace_once(content, "AQANCoCq/0Q=", "AQANCoCq/0=="), "Base64 character");
    reject_tamper(replace_once(content, "AQANCoCq/0Q=", "AQANCoCq/0R="), "Base64 padding");
    reject_tamper(replace_once(content, "eA==", "eB=="), "Base64 padding");
    reject_tamper(replace_once(content, "eA==", "eA"), "Base64 length");
    reject_tamper(replace_once(content, "\"bytes\":8", "\"bytes\":67108865"), "size limit");
    reject_tamper(replace_once(content, "assets/3.rgba", "assetx/3.rgba"), "design SHA-256");
    reject_tamper(replace_once(content, "#ABCDEF", "#ABCDFE"), "design SHA-256");
    // An internally consistent design hash is not authorization to introduce a nonportable asset path.
    std::string design_text;
    check(ui::encode_json(expected, design_text), "encode standalone design for an unsafe path mutation");
    const auto wrong_path_design = replace_once(design_text, "assets/3.rgba", "outside.rgba");
    auto wrong_path_package = replace_once(content, "assets/3.rgba", "outside.rgba");
    reject_tamper(replace_once(wrong_path_package, digest(design_text), digest(wrong_path_design)), "non-canonical delivery asset path");
    const auto font_path_design = replace_once(design_text, "assets/5.bin", "assets/5.otf");
    auto font_path_package = replace_once(content, "assets/5.bin", "assets/5.otf");
    reject_tamper(replace_once(font_path_package, digest(design_text), digest(font_path_design)), "does not accept project font assets");

    fs::resize_file(second, 96 * 1024 * 1024 + 1);
    auto unchanged = sentinel;
    check(!ui::read_json_delivery(second, unchanged, &error) && error.message.find("size limit") != std::string::npos &&
        unchanged.design == sentinel.design && unchanged.images == sentinel.images, "an oversized package must be rejected before parsing");
    fs::create_symlink(moved, recipient / "linked.vgui.package.json", link_error);
    if (!link_error)
        check(!ui::read_json_delivery(recipient / "linked.vgui.package.json", unchanged, &error), "delivery file link must not verify");
}

void test_row_icon_delivery() {
    temp_dir temp;
    const auto project = temp.path / "source";
    fs::create_directory(project);
    write_bytes(project / "row.rgba", rgba);
    auto doc = ui::make_document();
    doc.resources.emplace("photo", "row.rgba");
    doc.nodes.at(doc.root).children.push_back("rows");
    doc.nodes.emplace("rows", ui::node{"tree", "", {{"items",
        R"([{"children":[{"buttons":[{"action":"row.open","icon":"photo"}],"text":"Child"}],"expanded":true,"text":"Parent"}])"s}}, {}});

    ui::diagnostic error;
    const auto package = temp.path / "package";
    check(ui::export_delivery(doc, project, package, &error), "export a resource referenced only by a nested row button");
    ui::document restored;
    check(ui::read_delivery(package, restored, &error) && restored.nodes == doc.nodes &&
        restored.resources.at("photo") == "assets/1.rgba", "verify a relocated row icon and preserve its reference");

    std::unique_ptr<vgui_context, decltype(&vgui_destroy)> ctx(vgui_create(nullptr, 16), &vgui_destroy);
    check(bool(ctx), "create context for delivery row icon");
    image_loader loader{fs::canonical(package / restored.resources.at("photo"))};
    ui::runtime rt(ctx.get());
    check(rt.mount(restored, 1, {package, image_loader::load, nullptr, &loader}, &error) && rt.diagnostics().empty(),
        "mount row icon from the exported resource root");
    auto* tree = rt.widget("rows");
    const auto child = vgui_tree_get_child(tree, vgui_tree_get_root(tree, 0), 0);
    check(child && loader.calls == 1 && loader.texture && vgui_tree_get_item_button_icon(tree, child, 0) == loader.texture.get(),
        "the exported asset must reach the nested row button as a texture");
}

void test_indicator_atlas_delivery() {
    temp_dir temp;
    const auto project = temp.path / "source";
    fs::create_directories(project / "skins");
    const std::string pixels(12 * 8 * 4, '\x55'); // 12x8 raw RGBA8 atlas.
    write_bytes(project / "skins" / "choice.rgba", pixels);
    auto doc = ui::make_document();
    doc.resources.emplace("choice", "skins/choice.rgba");
    doc.nodes.at(doc.root).children = {"check", "option"};
    doc.nodes.emplace("check", ui::node{"checkbox", "", {{"indicator_atlas", "choice"s},
        {"indicator_source_x", 2.}, {"indicator_source_y", 1.}, {"indicator_source_width", 4.}, {"indicator_source_height", 4.}}, {}});
    doc.nodes.emplace("option", ui::node{"radio", "", {{"indicator_atlas", "choice"s}, {"checked", true},
        {"indicator_source_x", 6.}, {"indicator_source_y", 2.}, {"indicator_source_width", 6.}, {"indicator_source_height", 4.}}, {}});

    ui::diagnostic error;
    const auto package = temp.path / "package";
    const auto standalone = temp.path / "choice.vgui.package.json";
    check(ui::export_delivery(doc, project, package, &error) &&
        ui::export_json_delivery(doc, project, standalone, &error), "export indicator atlases through both delivery formats");

    const auto copied = temp.path / "recipient" / "package";
    fs::create_directory(copied.parent_path());
    fs::copy(package, copied, fs::copy_options::recursive);
    fs::remove_all(project);
    fs::remove_all(package);
    auto expected = doc;
    expected.resources.at("choice") = "assets/1.rgba";
    ui::document restored;
    check(ui::read_delivery(copied, restored, &error) && restored == expected &&
        bundle_files(copied / "assets") == std::map<fs::path, std::string>{{"1.rgba", pixels}},
        "copied delivery must preserve both indicator regions and one shared asset");
    ui::embedded_delivery embedded;
    check(ui::read_json_delivery(standalone, embedded, &error) && embedded.design == expected &&
        embedded.images.size() == 1 && embedded.images.at("choice") == std::vector<std::uint8_t>(pixels.begin(), pixels.end()),
        "standalone delivery must preserve both indicator regions and embed the shared asset once");
}

void test_label_orientation_delivery() {
    const auto source = fs::path(__FILE__).parent_path() / "res/label-orientation.vgui.json";
    ui::diagnostic error;
    ui::document design;
    if (!ui::read_document(source, design, &error))
        throw std::runtime_error("read label orientation example: " + error.message);
    temp_dir temp;
    const auto folder = temp.path / "labels";
    const auto file = temp.path / "labels.vgui.package.json";
    check(ui::export_delivery(design, source.parent_path(), folder, &error), "export labels as a directory");
    check(ui::export_json_delivery(design, source.parent_path(), file, &error), "export labels as a single JSON file");
    ui::document directory;
    ui::embedded_delivery single;
    check(ui::read_delivery(folder, directory, &error), "verify label directory delivery");
    check(ui::read_json_delivery(file, single, &error), "verify label standalone delivery");
    check(directory == design && single.design == design && single.images.empty(), "delivery changed the label example");

    std::unique_ptr<vgui_context, decltype(&vgui_destroy)> ctx(vgui_create(nullptr, 16), &vgui_destroy);
    check(bool(ctx), "create label delivery context");
    ui::runtime rt(ctx.get());
    const auto mounted_labels = [&](const ui::document& doc) {
        check(rt.mount(doc, 1, {}, &error) && rt.layout({460, 300, 1}), "mount and lay out the label example");
        std::array<ui::pixel_rect, 2> bounds;
        auto* horizontal = rt.widget("horizontal");
        auto* vertical = rt.widget("vertical");
        check(horizontal && vertical && vgui_label_get_text_orientation(horizontal) == VGUI_LABEL_TEXT_HORIZONTAL &&
            vgui_label_get_text_orientation(vertical) == VGUI_LABEL_TEXT_VERTICAL, "delivered labels have the wrong text orientation");
        auto& [h, v] = bounds;
        vgui_widget_get_rect(horizontal, &h.x, &h.y, &h.width, &h.height);
        vgui_widget_get_rect(vertical, &v.x, &v.y, &v.width, &v.height);
        check(h.height > 0 && v.height > 3 * h.height, "the four-character label is not laid out as a vertical column");
        return bounds;
    };
    const auto expected = mounted_labels(design);
    for (const auto* restored : {&directory, &single.design}) {
        const auto actual = mounted_labels(*restored);
        check(same_rect(actual[0], expected[0]) && same_rect(actual[1], expected[1]),
            "delivered label layout does not match the source design");
    }
}
void test_hud_delivery() {
    const auto source = fs::path(__FILE__).parent_path() / "res/game-hud.vgui.json";
    const auto source_bytes = read_bytes(source);
    ui::diagnostic error;
    ui::document design;
    check(ui::read_document(source, design, &error), "read anchored HUD");
    std::unique_ptr<vgui_context, decltype(&vgui_destroy)> ctx(vgui_create(nullptr, 16), &vgui_destroy);
    check(bool(ctx), "create HUD delivery context");
    vgui_context_set_safe_area(ctx.get(), 44, 3, 16, 30);
    ui::runtime runtime(ctx.get());
    const auto mount_live_hud = [&](const ui::document& doc, bool smooth) {
        check(runtime.mount(doc, 1, {}, &error) && runtime.layout({1720, 720, 1.5f}), "mount delivered HUD");
        check(runtime.focus_default() && vgui_get_focus(ctx.get()) == runtime.widget("skill1"),
            "delivered HUD must keep its initial skill focus");
        check(vgui_focus_navigate(ctx.get(), VGUI_NAV_LEFT) && vgui_get_focus(ctx.get()) == runtime.widget("skill6") &&
            vgui_focus_navigate(ctx.get(), VGUI_NAV_RIGHT) && vgui_get_focus(ctx.get()) == runtime.widget("skill1"),
            "delivered HUD must round-trip focus links");
        check(runtime.has_modals() && vgui_focus_navigate(ctx.get(), VGUI_NAV_UP) &&
            vgui_get_focus(ctx.get()) == runtime.widget("pause_button"), "delivered HUD must keep the pause opener reachable");
        vgui_input_key(ctx.get(), VGUI_KEY_ENTER, 1, 0);
        runtime.service_modals();
        check(vgui_modal_panel(ctx.get()) == runtime.widget("pause_dialog") &&
            vgui_get_focus(ctx.get()) == runtime.widget("pause_close"), "delivered pause dialog must open and take focus");
        vgui_input_key(ctx.get(), VGUI_KEY_ESCAPE, 1, 0);
        check(!vgui_modal_active(ctx.get()) && vgui_get_focus(ctx.get()) == runtime.widget("skill1"),
            "delivered pause dialog must restore authored focus on return");
        ui::pixel_rect radar{}, reticle{};
        vgui_widget_get_rect(runtime.widget("minimap"), &radar.x, &radar.y, &radar.width, &radar.height);
        vgui_widget_get_rect(runtime.widget("crosshair"), &reticle.x, &reticle.y, &reticle.width, &reticle.height);
        check(radar.x >= 44 * 1.5f && radar.y + radar.height <= (720 - 30) * 1.5f &&
            reticle.x + reticle.width * .5f == 1720 * .75f && reticle.y + reticle.height * .5f == 720 * .75f,
            "delivered HUD must respect safe-area insets without displacing the reticle");
        auto* hp = runtime.widget("hp");
        auto* shield = runtime.widget("shield");
        check(hp && shield && vgui_widget_get_value(hp) == 1.0f && vgui_widget_get_value(shield) == 0.5f,
            "mount must use authored vitals, not previous live values");
        check(runtime.set_data("player.hp_ratio", ui::value{0.25}) && runtime.set_data("player.shield_ratio", ui::value{1.0}),
            "delivered HUD must preserve working vitals bindings");
        check(vgui_widget_get_value(hp) == (smooth ? 1.f : .25f) && vgui_widget_get_value(shield) == (smooth ? .5f : 1.f),
            "only explicitly configured vitals should transition");
        if (smooth) {
            runtime.advance_animations(0.125); // Shield is done; HP has half of its time remaining.
            check(vgui_widget_get_value(hp) > .25f && vgui_widget_get_value(hp) < 1.f && vgui_widget_get_value(shield) == 1.f,
                "delivered HP and shield must keep independent transition durations");
            runtime.advance_animations(0.125);
            check(vgui_widget_get_value(hp) == .25f, "delivered HP should reach its target");
        }
    };
    const auto original = design;
    mount_live_hud(design, false);
    check(design == original, "runtime HUD values must not mutate the document");
    design.nodes.at("hp").props["bind_value_transition_ms"] = 250.;
    design.nodes.at("shield").props["bind_value_transition_ms"] = 125.;
    const auto authored = design;
    mount_live_hud(design, true);
    check(design == authored, "runtime must not mutate the configured HUD");

    temp_dir temp;
    const auto folder = temp.path / "hud";
    const auto file = temp.path / "hud.vgui.package.json";
    check(ui::export_delivery(design, source.parent_path(), folder, &error) &&
        ui::export_json_delivery(design, source.parent_path(), file, &error), "export both HUD packages with a live override");
    ui::document directory;
    ui::embedded_delivery single;
    check(ui::read_delivery(folder, directory, &error) && ui::read_json_delivery(file, single, &error) &&
        directory == authored && single.design == authored, "deliver authored bindings and defaults, not live HUD values");
    mount_live_hud(directory, true);
    mount_live_hud(single.design, true);
    check(read_bytes(source) == source_bytes, "running and delivering the HUD must not rewrite its source");
}
} // namespace

int main() {
    try {
        test_delivery();
        test_json_delivery();
        test_row_icon_delivery();
        test_indicator_atlas_delivery();
        test_label_orientation_delivery();
        test_hud_delivery();
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ui_design_delivery_test: %s\n", e.what());
        return 1;
    }
}
