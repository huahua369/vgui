// U3: export, relocate, verify, and mount without a window or editor kit.
#include "delivery.h"
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
} // namespace

int main() {
    try {
        test_delivery();
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ui_design_delivery_test: %s\n", e.what());
        return 1;
    }
}
