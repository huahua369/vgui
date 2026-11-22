// U0 headless document, history and persistence tests (docs/ui_editor_roadmap.md).
#include "model.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {
namespace ui = vgui::ui_design;
namespace fs = std::filesystem;
using namespace std::string_literals;

void check(bool condition, std::string_view context)
{
    if (!condition) throw std::runtime_error(std::string(context));
}

void expect_ok(bool ok, const ui::diagnostic& error, std::string_view context)
{
    if (!ok) throw std::runtime_error(std::string(context) + ": " + error.path + " " + error.message);
}

void expect_failure(bool ok, const ui::diagnostic& error, std::string_view context)
{
    check(!ok, std::string(context) + ": unexpectedly accepted");
    check(!error.message.empty(), std::string(context) + ": missing diagnostic");
}

class temp_dir {
public:
    temp_dir()
    {
        const auto parent = fs::temp_directory_path();
        std::random_device random;
        for (int attempt = 0; attempt < 32; ++attempt) {
            auto candidate = parent / ("vgui-u0-" + std::to_string(random()) + "-" + std::to_string(random()));
            std::error_code ec;
            // Claim a directory atomically; never write into an existing one.
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
    ~temp_dir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    fs::path path;
};

void write_bytes(const fs::path& path, std::string_view text)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    file.close();
    check(bool(file), "write test fixture");
}

std::string read_bytes(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    check(bool(file), "open test fixture");
    std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    check(!file.bad(), "read test fixture");
    return text;
}

std::set<fs::path> entries_under(const fs::path& directory)
{
    std::set<fs::path> entries;
    for (const auto& entry : fs::recursive_directory_iterator(directory))
        entries.insert(entry.path().lexically_relative(directory));
    return entries;
}

constexpr std::string_view minimal =
    R"({"format":"vgui-ui-design","version":1,"root":"root","nodes":{"root":{"type":"panel"}}})";

std::string json_document(std::string_view nodes, std::string_view root = "root", std::string_view resources = "{}")
{
    std::string json = R"({"format":"vgui-ui-design","version":1,"root":")";
    json.append(root).append(R"(","nodes":)").append(nodes).append(R"(,"resources":)").append(resources);
    return json + '}';
}

ui::document sentinel()
{
    return {"keep", {{"keep", {"panel", "Keep this document", {}, {}}}}, {{"keep", "assets/keep.png"}}};
}

ui::document decode(std::string_view text)
{
    ui::document out = sentinel();
    ui::diagnostic error;
    expect_ok(ui::decode_json(text, out, &error), error, "decode v1 document");
    return out;
}

std::string encode(const ui::document& doc)
{
    std::string out = "sentinel output";
    ui::diagnostic error;
    expect_ok(ui::encode_json(doc, out, &error), error, "encode v1 document");
    return out;
}

ui::diagnostic reject_json(std::string_view text, std::string_view context)
{
    const auto before = sentinel();
    auto out = before;
    ui::diagnostic error;
    expect_failure(ui::decode_json(text, out, &error), error, context);
    check(out == before, std::string(context) + ": changed decode output on failure");
    return error;
}

void canonical_round_trip()
{
    const auto bare = decode(minimal);
    check(bare == ui::document{"root", {{"root", {"panel", "", {}, {}}}}, {}},
        "omitted name/props/children/resources have empty storage");
    const auto expanded = decode(R"({"resources": {}, "nodes":{"root":{
        "children":[],"props":{},"name":"","type":"panel"}},
        "root":"root", "version":1, "format":"vgui-ui-design"})");
    check(encode(bare) == encode(expanded), "optional empty fields and member order normalize identically");
    const auto* layout = ui::find_property("panel", "layout");
    check(layout && layout->default_value == ui::value{"absolute"s}, "panel layout defaults to absolute");
    const auto absolute = decode(json_document(R"({"root":{"type":"panel","props":{"layout":"absolute"}}})"));
    const auto& explicit_props = absolute.nodes.at(absolute.root).props;
    check(!explicit_props.contains("layout") || explicit_props.at("layout") == layout->default_value,
        "explicit absolute layout has the same effective value as the default");

    const auto doc = decode(R"({
        "resources":{"z.badge":"assets/badge.png","a.logo":"assets/icons/logo.png"},
        "nodes":{
            "photo":{"props":{"resource":"a.logo"},"type":"image"},
            "empty_image":{"type":"image","props":{"resource":""}},
            "field":{"type":"edit","props":{"text":"Ada","text_key":"form.value","placeholder":"Name"}},
            "submit":{"type":"button","props":{"action":"Form.submit-1_","text_key":"form.submit","text":"Send"}},
            "heading":{"type":"label","props":{"text":"Caf\u00e9\n\"UI\"","text_key":"form.title"}},
            "box":{"children":["submit","field"],"props":{"layout":"row"},"type":"panel"},
            "root":{"type":"panel","name":"Main form","children":["photo","heading","box","empty_image"],"props":{
                "layout":"column","width":240,"height":120,"x":-5,"y":4,"visible":false,"enabled":false,
                "grow":1.5,"gap":8,"pad_left":1,"pad_top":2,"pad_right":3,"pad_bottom":4}}
        }, "root":"root", "version":1, "format":"vgui-ui-design"
    })");
    const ui::document expected{"root", {
        {"root", {"panel", "Main form", {{"layout", "column"s}, {"width", 240.0}, {"height", 120.0},
            {"x", -5.0}, {"y", 4.0}, {"visible", false}, {"enabled", false}, {"grow", 1.5}, {"gap", 8.0},
            {"pad_left", 1.0}, {"pad_top", 2.0}, {"pad_right", 3.0}, {"pad_bottom", 4.0}},
            {"photo", "heading", "box", "empty_image"}}},
        {"box", {"panel", "", {{"layout", "row"s}}, {"submit", "field"}}},
        {"heading", {"label", "", {{"text", "Caf\xC3\xA9\n\"UI\""s}, {"text_key", "form.title"s}}, {}}},
        {"submit", {"button", "", {{"text", "Send"s}, {"text_key", "form.submit"s}, {"action", "Form.submit-1_"s}}, {}}},
        {"field", {"edit", "", {{"text", "Ada"s}, {"text_key", "form.value"s}, {"placeholder", "Name"s}}, {}}},
        {"photo", {"image", "", {{"resource", "a.logo"s}}, {}}},
        {"empty_image", {"image", "", {{"resource", ""s}}, {}}}
    }, {{"a.logo", "assets/icons/logo.png"}, {"z.badge", "assets/badge.png"}}};
    check(doc == expected, "decode preserves v1 types, values, UTF-8, references and child order");
    const auto canonical = encode(doc);
    const auto reopened = decode(canonical);
    check(reopened == expected, "canonical JSON round-trips semantically");
    check(encode(reopened) == canonical, "encode/decode/encode is byte stable");
    auto reordered = doc;
    auto& children = reordered.nodes.at(doc.root).children;
    std::swap(children[0], children[1]);
    check(encode(reordered) != canonical && decode(encode(reordered)) == reordered,
        "canonical encoding must not sort or discard ordered children");
}

void collection_json_round_trip()
{
    const auto doc = decode(json_document(R"({
        "root":{"type":"panel","children":["rows","tree","menu","bar"]},
        "rows":{"type":"list","props":{"items":["First",{"text":"Second","selected":true,"icon":"open","buttons":[
            {"icon":"open","action":"row.open"},{"action":"row.details"}]}]}},
        "tree":{"type":"tree","props":{"items":[{"text":"Parent","expanded":true,
            "children":[{"text":"Child","selected":true,"buttons":[{"icon":"open"}]}]}]}},
        "menu":{"type":"menu","props":{"items":[{"separator":true},{"text":"Run","action":"file.run","icon":"open"}]}},
        "bar":{"type":"menubar","props":{"items":[{"text":"File","children":[{"text":"Open","icon":"open"}]}]}}
    })", "root", R"({"open":"icons/open.png"})"));
    const auto encoded = encode(doc);
    std::size_t offset = 0;
    for (int collections = 0; collections < 4; ++collections) {
        offset = encoded.find("\"items\": [", offset);
        check(offset != std::string::npos, "each collection must encode items as a natural JSON array");
        ++offset;
    }
    const auto reopened = decode(encoded);
    check(reopened == doc && encode(reopened) == encoded, "collection JSON canonical round trip");

    const struct { const char* kind; const char* items; } invalid[] = {
        {"list", R"("[]")"}, // A file must not contain a string with embedded JSON.
        {"list", R"([{"text":"Row","selected":1}])"},
        {"list", R"([{"text":"Row","icon":""}])"},
        {"list", R"([{"text":"Row","buttons":[1]}])"},
        {"list", R"([{"text":"Row","buttons":[{},{},{},{},{}]}])"},
        {"tree", R"([{"text":"Row","buttons":[{"action":"bad/key"}]}])"},
        {"menu", R"([{"text":"Row","buttons":[]}])"},
        {"menubar", R"([{"text":"Title","children":[],"icon":"open"}])"},
        {"tree", R"([{"text":"Parent","children":{}}])"},
        {"menu", R"([{"separator":true,"action":"bad.extra"}])"},
        {"menu", R"([{"text":"Run","action":"file/run"}])"},
        {"menubar", R"([{"text":"Missing children"}])"},
    };
    for (const auto& [kind, items] : invalid) {
        const auto nodes = R"({"root":{"type":"panel","children":["widget"]},"widget":{"type":")"s + kind +
            R"(","props":{"items":)" + items + "}}}";
        const auto error = reject_json(json_document(nodes), std::string(kind) + " malformed items " + items);
        check(error.path == "/nodes/widget/props/items", "collection diagnostic points to the items property");
    }
    check(reject_json(json_document(R"({"root":{"type":"panel","children":["tree"]},
        "tree":{"type":"tree","props":{"items":[{"text":"Root","children":[
        {"text":"Leaf","buttons":[{"icon":"unregistered"}]}]}]}}})"), "dangling nested button icon").path ==
        "/nodes/tree/props/items/0/children/0/buttons/0/icon", "dangling icon reports the exact nested field");

    auto broken = doc;
    broken.resources.clear();
    ui::diagnostic error;
    check(!ui::validate(broken, &error) && error.path == "/nodes/bar/props/items/0/children/0/icon",
        "an icon referenced by a popup entry prevents removing its resource");

    // The deepest supported tree item can still contain a button object.
    std::string deep = R"([{"text":"Leaf","buttons":[{"icon":"open"}]}])";
    for (int i = 1; i < 12; ++i) deep = R"([{"text":"Parent","children":)" + deep + "}]";
    auto full_depth = doc;
    full_depth.nodes.at("tree").props["items"] = deep;
    check(encode(decode(encode(full_depth))) == encode(full_depth), "deepest supported row round trips with buttons");
}

std::string gradient_keyframe_page(std::string_view keys)
{
    return json_document(R"({"root":{"type":"panel","children":["paint"]},"paint":{"type":"gradient",
        "props":{"gradient_animation":"keyframes","gradient_animation_keyframes":)"s + std::string(keys) + "}}}");
}

void gradient_keyframe_round_trip()
{
    const auto* desc = ui::find_property("gradient", "gradient_animation_keyframes");
    check(desc && desc->label == "Keyframes" && desc->type == ui::value_type::gradient_keyframes &&
        std::get<std::string>(desc->default_value) == "[]", "keyframe schema uses an optional array");
    const auto doc = decode(gradient_keyframe_page(R"([
        {"offset":0,"x":16,"y":-16,"tint":"#FFFFFF","opacity":1,"interpolation":"step"},
        {"offset":0.5,"x":-0.125,"y":0.25,"tint":"#aBcDeF","opacity":0.5,"interpolation":"smooth"},
        {"offset":1,"x":0,"y":0,"tint":"#000000","opacity":0,"interpolation":"linear"}])"));
    const auto text = encode(doc);
    check(text.find("\"gradient_animation_keyframes\": [") != std::string::npos && decode(text) == doc &&
        encode(decode(text)) == text, "keyframes round trip canonically as native JSON arrays");
    const std::vector<ui::gradient_keyframe> expected{{0, 16, -16, "#FFFFFF", 1, "step"},
        {0.5, -0.125, 0.25, "#aBcDeF", 0.5, "smooth"}, {1, 0, 0, "#000000", 0, "linear"}};
    const auto& value = doc.nodes.at("paint").props.at("gradient_animation_keyframes");
    std::vector<ui::gradient_keyframe> keys;
    std::string error;
    check(ui::decode_gradient_keyframes(std::get<std::string>(value), keys, error) && keys == expected && error.empty(),
        "explicit frames decode without losing their properties");
    check(ui::encode_gradient_keyframes(expected) ==
        R"([{"interpolation":"step","offset":0.0,"opacity":1.0,"tint":"#FFFFFF","x":16.0,"y":-16.0},)"
        R"({"interpolation":"smooth","offset":0.5,"opacity":0.5,"tint":"#aBcDeF","x":-0.125,"y":0.25},)"
        R"({"interpolation":"linear","offset":1.0,"opacity":0.0,"tint":"#000000","x":0.0,"y":0.0}])",
        "old keyframes keep exactly the same canonical bytes");
    check(ui::decode_gradient_keyframes(ui::encode_gradient_keyframes(expected), keys, error) && keys == expected,
        "model helpers round trip explicit frame data");
    check(ui::decode_gradient_keyframes("[]", keys, error) && keys.empty() && ui::encode_gradient_keyframes(keys) == "[]",
        "empty keyframe sequences are valid");
    const std::string_view prop_path = "/nodes/paint/props/gradient_animation_keyframes";
    check(reject_json(gradient_keyframe_page(R"("[]")"), "embedded keyframe JSON string").path == prop_path,
        "version 1 files must use a native array for keyframes");
    constexpr std::string_view invalid[] = {
        R"([{"offset":0,"x":0,"y":0,"tint":"#FFFFFF","opacity":1}])",
        R"([{"offset":0,"x":0,"y":0,"tint":"#FFFFFF","opacity":1,"interpolation":"linear","stop":0}])",
        R"([{"offset":0,"x":false,"y":0,"tint":"#FFFFFF","opacity":1,"interpolation":"linear"}])",
        R"([{"offset":0,"x":0,"y":0,"tint":"#FFFFFF80","opacity":1,"interpolation":"linear"}])",
        R"([{"offset":0,"x":0,"y":0,"tint":"white","opacity":1,"interpolation":"linear"}])",
        R"([{"offset":0,"x":0,"y":0,"tint":"#FFFFFF","opacity":1.01,"interpolation":"linear"}])",
        R"([{"offset":0,"x":0,"y":0,"tint":"#FFFFFF","opacity":1,"interpolation":"ease"}])",
    };
    for (const auto bad : invalid) {
        keys = expected;
        check(!ui::decode_gradient_keyframes(bad, keys, error) && keys == expected && !error.empty(),
            "invalid keyframes must not replace decoder output");
        check(reject_json(gradient_keyframe_page(bad), "invalid gradient keyframe").path == prop_path,
            "invalid keyframe diagnostic must point to its property");
    }
    check(!ui::decode_gradient_keyframes(R"([{"offset":0,"offset":1,"x":0,"y":0,"tint":"#FFFFFF",
        "opacity":1,"interpolation":"linear"}])", keys, error), "direct decoder rejects duplicate fields");
    check(!ui::decode_gradient_keyframes("[]\0trailing"s, keys, error), "direct decoder rejects raw NUL");

    auto broken = doc;
    auto& props = broken.nodes.at("paint").props;
    auto frames = expected;
    const auto validate_frames = [&](bool valid) {
        props["gradient_animation_keyframes"] = ui::encode_gradient_keyframes(frames);
        ui::diagnostic issue;
        check(ui::validate(broken, &issue) == valid && (valid || issue.path == prop_path),
            valid ? "valid keyframe list was rejected" : "invalid keyframe list was accepted or poorly diagnosed");
    };
    frames[1].offset = ui::gradient_keyframe_min_gap;
    validate_frames(true);
    frames[1].offset *= 0.5;
    validate_frames(false);
    frames[1].offset = 1;
    validate_frames(false);
    frames = expected;
    frames.front().offset = -0.01;
    validate_frames(false);
    frames = expected;
    frames.back().x = -16.01;
    validate_frames(false);
    frames = expected;
    props["gradient_affine_tx"] = 0.125;
    validate_frames(false);
    props["gradient_affine_tx"] = 0.0;
    props["gradient_affine_ty"] = -0.125;
    validate_frames(false);
    props["gradient_animation"] = "none"s;
    validate_frames(true); // A dormant list is shape-validated, but its pose is not applied.
    props.erase("gradient_animation_keyframes");
    check(decode(encode(broken)) == broken && encode(broken).find("gradient_animation_keyframes") == std::string::npos,
        "legacy v1 gradients need no migration or injected property");

    frames.resize(ui::max_gradient_keyframes + 1);
    for (std::size_t i = 0; i < frames.size(); ++i) frames[i] = {double(i) / ui::max_gradient_keyframes};
    validate_frames(false);
    frames.pop_back();
    validate_frames(true);
}

void gradient_keyframe_geometry_round_trip()
{
    const std::vector<ui::gradient_keyframe> expected{{0, 0, 0, "#FFFFFF", 1, "linear",
        {-16, -1.5, 16, 2.5, 0, 1.25, 2, 3, 0, 360, 0, 16}}};
    const auto encoded = ui::encode_gradient_keyframes(expected);
    check(encoded ==
        R"([{"center_x":0.0,"center_y":1.25,"end_angle":360.0,"end_x":16.0,"end_y":2.5,"inner_radius":0.0,)"
        R"("interpolation":"linear","offset":0.0,"opacity":1.0,"outer_radius":16.0,"outer_x":2.0,"outer_y":3.0,)"
        R"("start_angle":0.0,"start_x":-16.0,"start_y":-1.5,"tint":"#FFFFFF","x":0.0,"y":0.0}])", "geometry uses flat optional fields in canonical order");
    const auto doc = decode(gradient_keyframe_page(encoded));
    check(decode(encode(doc)) == doc, "page round trips all active and dormant geometry fields");
    std::vector<ui::gradient_keyframe> keys;
    std::string error;
    const auto& props = doc.nodes.at("paint").props;
    check(ui::decode_gradient_keyframes(std::get<std::string>(props.at("gradient_animation_keyframes")), keys, error) &&
        keys == expected && error.empty(), "all geometry values (including zero) survive page decoding");

    constexpr std::string_view invalid[] = {
        R"("start_x":0,"start_\u0078":1)", R"("outer_x":null)", R"("end_y":true)", R"("center_x":"0")",
        R"("inner_x":0)", R"("start_x":-16.001)", R"("center_y":16.001)",
        R"("start_angle":-0.001)", R"("end_angle":360.001)",
        R"("inner_radius":-0.001)", R"("outer_radius":16.001)", R"("outer_radius":"1")",
    };
    for (const auto fields : invalid) {
        const auto bad = R"([{"offset":0,"x":0,"y":0,"tint":"#FFFFFF","opacity":1,"interpolation":"linear",)"s +
            std::string(fields) + "}]";
        check(!ui::decode_gradient_keyframes(bad, keys, error) && keys == expected && !error.empty(),
            "invalid or duplicate geometry must not replace decoder output");
        check(reject_json(gradient_keyframe_page(bad), "invalid geometry field").path.starts_with(
            "/nodes/paint/props/gradient_animation_keyframes"), "invalid geometry is rejected even when its kind is dormant");
    }

    std::vector full(ui::max_gradient_keyframes, expected.front());
    for (std::size_t i = 0; i < full.size(); ++i) {
        auto& key = full[i];
        key.offset = double(i) / ui::max_gradient_keyframes;
        key.geometry = {-15.123456789012345, -10.223456789012345, 14.123456789012345, 11.223456789012345,
            -9.323456789012345, 8.423456789012345, 7.523456789012345, -6.623456789012345,
            9.723456789012345, 358.823456789012345, 0.12345678901234567, 0.9876543210987654};
    }
    const auto max_encoded = ui::encode_gradient_keyframes(full);
    check(max_encoded.size() > 32 * 1024, "full-frame coverage exceeds the legacy byte cap");
    check(ui::decode_gradient_keyframes(max_encoded, keys, error) && keys == full,
        "all 128 full-precision frames fit the internal byte cap");
    const auto max_doc = decode(gradient_keyframe_page(max_encoded));
    check(decode(encode(max_doc)) == max_doc, "128 full frames round trip through a page file");
    full.push_back(full.back());
    full.back().offset = 1;
    check(!ui::decode_gradient_keyframes(ui::encode_gradient_keyframes(full), keys, error) &&
        keys.size() == ui::max_gradient_keyframes, "the 129th fully populated frame is rejected");
}

void gradient_keyframe_geometry_validation()
{
    auto doc = decode(gradient_keyframe_page("[]"));
    auto& props = doc.nodes.at("paint").props;
    std::vector<ui::gradient_keyframe> keys(2);
    keys.back().offset = 1;
    using axis = ui::gradient_geometry_axis;
    const auto channel = [&](axis id) -> auto& { return keys.back().geometry[static_cast<std::size_t>(id)]; };
    const auto validate_keys = [&](bool valid, std::string_view context) {
        props["gradient_animation_keyframes"] = ui::encode_gradient_keyframes(keys);
        ui::diagnostic issue;
        if (valid) expect_ok(ui::validate(doc, &issue), issue, context);
        else {
            expect_failure(ui::validate(doc, &issue), issue, context);
            check(issue.path == "/nodes/paint/props/gradient_animation_keyframes/1", "reports the degenerate frame");
        }
    };

    channel(axis::end_y) = 0; // Falls onto the default start point (0.5, 0).
    channel(axis::center_x) = -16;
    channel(axis::outer_x) = 16;
    props["gradient_kind"] = "radial"s;
    validate_keys(true, "linear fields are dormant on radial gradients");
    check(decode(encode(doc)) == doc, "dormant linear overrides survive a radial page round trip");
    props["gradient_kind"] = "linear"s;
    validate_keys(false, "switching to linear activates a previously dormant field");
    props["gradient_animation"] = "none"s;
    validate_keys(true, "disabled keyframe animation does not apply geometry poses");
    props["gradient_animation"] = "keyframes"s;
    props["gradient_x0"] = 1.0;
    validate_keys(true, "missing frame channels use authored endpoints, not just schema defaults");
    props.erase("gradient_x0");
    channel(axis::end_y) = 0.00005;
    validate_keys(false, "linear distance below the renderer threshold is rejected");
    channel(axis::end_y) = 0.0001;
    validate_keys(true, "linear distance at the renderer threshold is accepted");
    channel(axis::end_y) = -1;
    channel(axis::start_angle) = 360;
    validate_keys(true, "angular fields stay dormant on linear gradients and crossing segments are allowed");

    props["gradient_kind"] = "sweep"s;
    validate_keys(false, "switching to sweep activates the start angle and falls back to the default end angle");
    props["gradient_angle1"] = 270.0;
    validate_keys(true, "missing sweep angle falls back to the authored end angle; reversed sweeps are valid");
    channel(axis::start_angle) = 0;
    channel(axis::end_angle) = 0.0005;
    validate_keys(false, "sweep angle gap below the renderer threshold is rejected");
    channel(axis::end_angle) = 0.001;
    validate_keys(true, "sweep angle gap at the renderer threshold is accepted");

    channel(axis::inner_radius) = .75;
    validate_keys(true, "radius channels are dormant on a sweep gradient");
    props["gradient_kind"] = "radial"s;
    validate_keys(false, "a keyed inner radius cannot exceed the inherited outer radius");
    props["gradient_radius1"] = 1.0;
    validate_keys(true, "a missing radius uses the authored radius, not the default");
    channel(axis::outer_radius) = .75;
    validate_keys(false, "equal keyed radial radii are invalid");
    channel(axis::outer_radius) = .75000000001;
    validate_keys(false, "keyed radii that round to the same renderer value are invalid");
    channel(axis::outer_radius) = .875;
    validate_keys(true, "two distinct keyed radii are accepted");
    channel(axis::inner_radius).reset();
    props["gradient_radius0"] = .875;
    validate_keys(false, "a keyed outer radius must exceed the inherited inner radius");
}

void named_radio_groups()
{
    const auto doc = decode(json_document(R"({
        "root":{"type":"panel","children":["light","dark","language","default","unnamed","nested"]},
        "light":{"type":"radio","props":{"radio_group":"主题","checked":true}},
        "dark":{"type":"radio","props":{"radio_group":"主题"}},
        "language":{"type":"radio","props":{"radio_group":"语言","checked":true}},
        "default":{"type":"radio","props":{"radio_group":"","checked":true}},
        "unnamed":{"type":"radio"},
        "nested":{"type":"panel","children":["other"]},
        "other":{"type":"radio","props":{"radio_group":"主题","checked":true}}
    })"));
    check(decode(encode(doc)) == doc, "named and default groups round trip without changing the tree");
    ui::diagnostic error;
    auto invalid = doc;
    invalid.nodes.at("dark").props["checked"] = true;
    check(!ui::validate(invalid, &error) && error.path == "/nodes/dark/props/checked",
        "two checked radios cannot share a named group under one parent");
    invalid = doc;
    invalid.nodes.at("unnamed").props["checked"] = true;
    check(!ui::validate(invalid, &error) && error.path == "/nodes/unnamed/props/checked",
        "an omitted name and an explicit empty name are the same group");
    invalid = doc;
    for (const ui::value& name : {ui::value{true}, ui::value{"multi\nline"s}, ui::value{std::string(257, 'a')}}) {
        invalid.nodes.at("light").props["radio_group"] = name;
        check(!ui::validate(invalid, &error) && error.path == "/nodes/light/props/radio_group",
            "a group name is a bounded single-line string");
    }
}

void label_text_orientation()
{
    const auto* descriptor = ui::find_property("label", "text_orientation");
    check(descriptor && descriptor->default_value == ui::value{"horizontal"s} && descriptor->choices.size() == 2,
        "label text orientation must default to horizontal");
    const auto doc = decode(json_document(R"({"root":{"type":"panel","children":["caption","legacy"]},
        "caption":{"type":"label","props":{"text":"联络我们","text_orientation":"vertical"}},
        "legacy":{"type":"label"}})"));
    check(doc.nodes.at("caption").props.at("text_orientation") == ui::value{"vertical"s} &&
        !doc.nodes.at("legacy").props.contains("text_orientation") && decode(encode(doc)) == doc,
        "vertical labels must round trip without serializing an omitted default");
    for (const std::string_view node : {
        R"({"type":"label","props":{"text_orientation":"rotated"}})",
        R"({"type":"label","props":{"text_orientation":true}})",
        R"({"type":"button","props":{"text_orientation":"vertical"}})"}) {
        const auto invalid = std::string(R"({"root":{"type":"panel","children":["caption"]},"caption":)") +
            std::string(node) + '}';
        check(reject_json(json_document(invalid), "invalid label orientation").path == "/nodes/caption/props/text_orientation",
            "label orientation validation must report the invalid property");
    }
}

void font_properties_round_trip()
{
    const ui::document doc{"root", {
        {"root", {"panel", "", {}, {"heading", "submit", "field", "caption", "inherited"}}},
        {"heading", {"label", "", {{"font_size", 4.0}, {"font_weight", "regular"s},
            {"font_slant", "normal"s}, {"text_color", "#aB01fE"s}}, {}}},
        {"submit", {"button", "", {{"font_size", 256.0}, {"font_weight", "medium"s},
            {"font_slant", "italic"s}, {"text_color", "#123456aB"s}}, {}}},
        {"field", {"edit", "", {{"font_size", 17.5}, {"font_weight", "semi_bold"s},
            {"font_slant", "oblique"s}, {"text_color", "#ABCDEf00"s}}, {}}},
        {"caption", {"label", "", {{"font_weight", "bold"s}}, {}}},
        {"inherited", {"edit", "", {{"font_size", 0.0}, {"font_weight", "default"s},
            {"font_slant", "default"s}, {"text_color", ""s}}, {}}},
    }, {}};
    const auto canonical = encode(doc);
    const auto reopened = decode(canonical);
    check(reopened == doc, "font overrides and explicit defaults survive a v1 round trip");
    check(encode(reopened) == canonical, "font property encoding is byte stable");
    check(ui::parse_text_color("#12AbEF") == 0xff12abefu, "RGB text color uses opaque alpha and preserves channel order");
    check(ui::parse_text_color("#12AbEF80") == 0x8012abefu, "RGBA suffix is moved to the runtime alpha byte");
    check(ui::parse_text_color("#00000000") == 0u && !ui::parse_text_color(""),
        "an explicit transparent color is different from theme inheritance");

    for (const auto* type : {"label", "button", "edit"}) {
        const struct { const char* key; ui::value value; } defaults[] = {
            {"font_size", 0.0}, {"font_weight", "default"s}, {"font_slant", "default"s}, {"text_color", ""s},
        };
        for (const auto& [key, value] : defaults) {
            const auto* property = ui::find_property(type, key);
            check(property && property->default_value == value, std::string(type) + ": default " + key);
        }
    }
}

void reject_bad_font_properties()
{
    const auto reject = [](const char* type, const char* key, const char* json_value) {
        const auto nodes = R"({"root":{"type":"panel","children":["sample"]},"sample":{"type":")"s + type +
            R"(","props":{")" + key + "\":" + json_value + "}}}";
        const auto context = std::string(type) + ": " + key + " = " + json_value;
        const auto error = reject_json(json_document(nodes), context);
        check(error.path == "/nodes/sample/props/"s + key, context + ": diagnostic identifies the property");
    };
    const struct { const char* key; const char* json_value; } invalid[] = {
        {"font_size", "-1"}, {"font_size", "0.5"}, {"font_size", "3.99"}, {"font_size", "256.01"},
        {"font_size", R"("16")"}, {"font_size", "true"},
        {"font_weight", "700"}, {"font_weight", R"("black")"}, {"font_weight", R"("semi-bold")"},
        {"font_slant", "2"}, {"font_slant", R"("Italic")"}, {"font_slant", R"("slanted")"},
        {"text_color", "123456"}, {"text_color", R"("123456")"}, {"text_color", R"("#123")"},
        {"text_color", R"("#1234567")"}, {"text_color", R"("#123456789")"}, {"text_color", R"("#12345g")"},
        {"text_color", R"("#123456z0")"}, {"text_color", R"("#123456 ")"},
    };
    for (const auto* type : {"label", "button", "edit"})
        for (const auto& [key, json_value] : invalid) reject(type, key, json_value);

    for (const auto* type : {"panel", "image"}) {
        reject(type, "font_size", "18");
        reject(type, "font_weight", R"("bold")");
        reject(type, "font_slant", R"("italic")");
        reject(type, "text_color", R"("#aB01fE")");
    }
}

void font_family_validation()
{
    constexpr std::string_view resources = R"({
        "project.body":"assets/fonts/Body.TtF",
        "project.heading":"assets/fonts/Heading.oTf",
        "not-a-font.ttf":"assets/preview.png",
        "font-preview":"assets/fonts/Body.ttf.png"
    })";
    const auto font_document = [&](const char* type, std::string_view json_value) {
        const auto nodes = R"({"root":{"type":"panel","children":["sample"]},"sample":{"type":")"s + type +
            R"(","props":{"font_family":)" + std::string(json_value) + "}}}";
        return json_document(nodes, "root", resources);
    };
    const auto reject = [&](const char* type, const char* json_value) {
        const auto context = std::string(type) + ": font_family = " + json_value;
        const auto error = reject_json(font_document(type, json_value), context);
        check(error.path == "/nodes/sample/props/font_family", context + ": diagnostic identifies the property");
    };

    for (const auto* type : {"label", "button", "edit"}) {
        const auto* property = ui::find_property(type, "font_family");
        check(property && property->default_value == ui::value{""s}, std::string(type) + ": font family defaults to empty");
        for (const auto* family : {"", "builtin.sans", "builtin.sans_sc", "project.body", "project.heading"}) {
            const auto doc = decode(font_document(type, '"' + std::string(family) + '"'));
            check(doc.nodes.at("sample").props.at("font_family") == ui::value{std::string(family)} &&
                decode(encode(doc)) == doc, std::string(type) + ": font family does not round trip: " + family);
        }
        for (const auto* invalid : {"16", R"("missing-font")", R"("builtin.serif")", R"("assets/fonts/Body.TtF")",
                R"("not-a-font.ttf")", R"("font-preview")"})
            reject(type, invalid);
    }
    for (const auto* type : {"panel", "image"}) reject(type, R"("builtin.sans")");
}

void reject_bad_schema_and_tree()
{
    struct example { std::string_view name, json; };
    for (const auto& [name, json] : {
            example{"malformed JSON", R"({"format":)"},
            example{"missing format", R"({"version":1,"root":"root","nodes":{"root":{"type":"panel"}}})"},
            example{"missing version", R"({"format":"vgui-ui-design","root":"root","nodes":{"root":{"type":"panel"}}})"},
            example{"missing root", R"({"format":"vgui-ui-design","version":1,"nodes":{"root":{"type":"panel"}}})"},
            example{"missing nodes", R"({"format":"vgui-ui-design","version":1,"root":"root"})"},
            example{"legacy format", R"({"format":"vgui-ui-editor","version":1,"root":"root","nodes":{"root":{"type":"panel"}}})"},
            example{"future version", R"({"format":"vgui-ui-design","version":2,"root":"root","nodes":{"root":{"type":"panel"}}})"},
            example{"wrong version type", R"({"format":"vgui-ui-design","version":"1","root":"root","nodes":{"root":{"type":"panel"}}})"},
            example{"duplicate document key", R"({"format":"vgui-ui-design","version":1,"root":"root","root":"root","nodes":{"root":{"type":"panel"}}})"}
        }) reject_json(json, name);

    for (const auto& [name, nodes] : {
            example{"missing node type", R"({"root":{}})"},
            example{"duplicate node ID", R"({"root":{"type":"panel"},"root":{"type":"panel"}})"},
            example{"duplicate node ID after JSON unescaping", R"({"root":{"type":"panel"},"\u0072oot":{"type":"panel"}})"},
            example{"duplicate property key", R"({"root":{"type":"panel","props":{"gap":1,"gap":1}}})"},
            example{"missing root node", R"({})"},
            example{"root must be a panel", R"({"root":{"type":"label"}})"},
            example{"missing child", R"({"root":{"type":"panel","children":["gone"]}})"},
            example{"orphan node", R"({"root":{"type":"panel"},"orphan":{"type":"label"}})"},
            example{"duplicate child ID", R"({"root":{"type":"panel","children":["a","a"]},"a":{"type":"label"}})"},
            example{"two parents", R"({"root":{"type":"panel","children":["a","b"]},"a":{"type":"panel","children":["c"]},"b":{"type":"panel","children":["c"]},"c":{"type":"label"}})"},
            example{"child of a leaf", R"({"root":{"type":"panel","children":["a"]},"a":{"type":"button","children":["b"]},"b":{"type":"label"}})"},
            example{"cycle through the root", R"({"root":{"type":"panel","children":["a"]},"a":{"type":"panel","children":["root"]}})"},
            example{"detached cycle with no parentless node", R"({"root":{"type":"panel"},"a":{"type":"panel","children":["b"]},"b":{"type":"panel","children":["a"]}})"},
            example{"invalid layout", R"({"root":{"type":"panel","props":{"layout":"grid"}}})"},
            example{"invalid value type", R"({"root":{"type":"panel","props":{"visible":1}}})"},
            example{"invalid geometry", R"({"root":{"type":"panel","props":{"width":-1}}})"},
            example{"property on the wrong node type", R"({"root":{"type":"panel","children":["a"]},"a":{"type":"label","props":{"gap":2}}})"},
            example{"image resource must exist", R"({"root":{"type":"panel","children":["a"]},"a":{"type":"image","props":{"resource":"absent"}}})"},
            example{"invalid action key", R"({"root":{"type":"panel","children":["a"]},"a":{"type":"button","props":{"action":".."}}})"},
            example{"invalid localization key", R"({"root":{"type":"panel","children":["a"]},"a":{"type":"edit","props":{"text_key":"form/name"}}})"}
        }) reject_json(json_document(nodes), name);

    auto error = reject_json(json_document(R"({"root":{"type":"panel","children":["a"]},"a":{"type":"custom"}})"),
        "unknown node type");
    check(error.path == "/nodes/a/type", "type diagnostic identifies the node and field");
    error = reject_json(json_document(R"({"root":{"type":"panel","props":{"future/value":true}}})"),
        "unknown property");
    check(error.path == "/nodes/root/props/future~1value", "property diagnostic is a JSON Pointer");

    auto invalid = sentinel();
    invalid.nodes.at(invalid.root).children = {invalid.root};
    std::string out = "preserve output bytes";
    expect_failure(ui::encode_json(invalid, out, &error), error, "encode invalid graph");
    check(out == "preserve output bytes", "failed encode leaves output unchanged");
}

void keys_and_paths()
{
    const auto max_key = "A0._-"s + std::string(59, 'a');
    const auto doc = decode(json_document("{\"" + max_key + R"(":{"type":"panel","children":["i"]},)"
        R"("i":{"type":"image","props":{"resource":")" + max_key + R"("}}})", max_key,
        "{\"" + max_key + R"(":"assets/logo.png"})"));
    check(doc.root == max_key && doc.nodes.contains("i") && doc.resources.contains(max_key),
        "one-byte and 64-byte keys are accepted, including ASCII punctuation");

    for (const auto& key : {""s, "."s, ".."s, "a/b"s, "a b"s, R"(\u00e9)"s, std::string(65, 'a')}) {
        // Keep the graph otherwise valid so only the ID/key can cause rejection.
        reject_json(json_document("{\"" + key + R"(":{"type":"panel"}})", key), "invalid node ID: " + key);
        reject_json(json_document(R"({"root":{"type":"panel"}})", "root",
            "{\"" + key + R"(":"assets/logo.png"})"), "invalid resource key: " + key);
    }
    for (std::string_view path : {"../logo.png", "assets/../../logo.png", "/logo.png", "C:/logo.png",
            R"(assets\\..\\..\\logo.png)", R"(\\\\server\\share\\logo.png)"}) {
        reject_json(json_document(R"({"root":{"type":"panel"}})", "root",
            R"({"logo":")" + std::string(path) + R"("})"), "non-project resource path: " + std::string(path));
    }
}

std::string flat_nodes_json(std::size_t count)
{
    std::string nodes = R"({"root":{"type":"panel","children":[)";
    std::string leaves;
    for (std::size_t n = 1; n < count; ++n) {
        const auto id = "n" + std::to_string(n);
        if (n != 1) nodes += ',';
        nodes += '"' + id + '"';
        leaves += ",\"" + id + R"(":{"type":"label"})";
    }
    return json_document(nodes + "]}" + leaves + '}');
}

void input_limits()
{
    std::string padded(minimal);
    padded.resize(ui::max_json_bytes, ' ');
    check(decode(padded) == decode(minimal), "valid document exactly at the byte cap");
    padded += ' ';
    reject_json(padded, "one byte over the JSON cap");

    const temp_dir tmp;
    const auto too_large = tmp.path / "oversize.vgui.json";
    write_bytes(too_large, padded);
    auto out = sentinel();
    ui::diagnostic error;
    expect_failure(ui::read_document(too_large, out, &error), error, "file one byte over the JSON cap");
    check(out == sentinel(), "over-limit file leaves read output unchanged");

    // Nested arrays are syntactically valid JSON but not a v1 property value.
    // Distinguish parser depth rejection from ordinary schema rejection.
    const auto deep_prop = [](std::size_t depth) {
        return json_document(R"({"root":{"type":"panel","props":{"x":)"s + std::string(depth, '[') +
            '0' + std::string(depth, ']') + "}}}");
    };
    // Four enclosing objects precede x: document, nodes, root node and props.
    // At the cap parsing succeeds, then v1 rejects the array as a property value.
    constexpr std::size_t enclosing_objects = 4;
    const auto at_limit = reject_json(deep_prop(ui::max_json_depth - enclosing_objects), "schema error at JSON depth cap");
    check(at_limit.path == "/nodes/root/props/x", "parser reaches property validation at the JSON depth cap");
    auto deep = reject_json(deep_prop(ui::max_json_depth - enclosing_objects + 1), "one level over the JSON depth cap");
    for (char& ch : deep.message) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    check(deep.message.find("depth") != std::string::npos || deep.message.find("nest") != std::string::npos,
        "over-depth JSON is diagnosed as a nesting limit, not a schema error");

    check(decode(flat_nodes_json(ui::max_nodes)).nodes.size() == ui::max_nodes, "valid tree exactly at the node cap");
    reject_json(flat_nodes_json(ui::max_nodes + 1), "valid flat tree with one node over the cap");

    // The node tree is a flat JSON object, so this limit is independent of JSON nesting.
    const auto chain = [](std::size_t count) {
        std::string nodes = "{";
        for (std::size_t i = 0; i < count; ++i) {
            if (i != 0) nodes += ',';
            nodes += "\"n" + std::to_string(i) + R"(":{"type":"panel")";
            if (i + 1 < count) nodes += R"(,"children":["n)" + std::to_string(i + 1) + "\"]";
            nodes += '}';
        }
        return json_document(nodes + '}', "n0");
    };
    check(decode(chain(ui::max_tree_depth)).nodes.size() == ui::max_tree_depth, "valid tree exactly at the tree depth cap");
    reject_json(chain(ui::max_tree_depth + 1), "tree one level over the tree depth cap");
}

struct session_state {
    ui::document doc, committed;
    std::uint64_t revision;
    bool dirty, undo, redo;
    bool operator==(const session_state&) const = default;
};

session_state snapshot(const ui::session& s)
{
    return {s.current(), s.committed(), s.revision(), s.dirty(), s.can_undo(), s.can_redo()};
}

void apply(ui::session& s, std::initializer_list<ui::edit> edits)
{
    ui::diagnostic error;
    expect_ok(s.apply({edits.begin(), edits.size()}, &error), error, "apply transaction");
}

void apply_live(ui::session& s, std::initializer_list<ui::edit> edits)
{
    ui::diagnostic error;
    expect_ok(s.apply_live({edits.begin(), edits.size()}, &error), error, "apply live transaction");
}

ui::diagnostic reject_edit(ui::session& s, std::initializer_list<ui::edit> edits, std::string_view context)
{
    const auto before = snapshot(s);
    ui::diagnostic error;
    expect_failure(s.apply({edits.begin(), edits.size()}, &error), error, context);
    check(snapshot(s) == before, std::string(context) + ": mutated session on failure");
    return error;
}

void declarative_container_constraints()
{
    ui::session s;
    const auto& root = s.current().root;
    apply(s, {ui::insert_node{"split", root, {"splitter", "", {}, {}}},
        ui::insert_node{"first", "split", {"panel", "", {}, {}}},
        ui::insert_node{"label", "split", {"label", "", {}, {}}},
        ui::insert_node{"tabs", root, {"tabview", "", {}, {}}},
        ui::insert_node{"tab_page", "tabs", {"panel", "", {}, {}}},
        ui::insert_node{"dock", root, {"dock", "", {}, {}}},
        ui::insert_node{"dock_page", "dock", {"panel", "", {}, {}}},
        ui::insert_node{"rows", root, {"list", "", {{"items", R"(["Row"])"s}}, {}}}});
    check(s.current().nodes.at("split").children == std::vector<std::string>{"first", "label"},
        "a splitter accepts two children in order, including a non-panel");
    check(reject_edit(s, {ui::insert_node{"third", "split", {"panel", "", {}, {}}}},
        "third splitter child").path == "/nodes/split/children/2", "splitter diagnostic points to the extra pane");
    check(reject_edit(s, {ui::move_node{"label", "tabs"}}, "tab view requires a panel page").path ==
        "/nodes/tabs/children/1", "tab view diagnostic points to the non-panel page");
    check(reject_edit(s, {ui::copy_subtree{"label", "dock", {{"label", "copied"}}}},
        "dock requires a panel page").path == "/nodes/dock/children/1", "dock diagnostic points to the non-panel page");
    check(reject_edit(s, {ui::set_property{"rows", "items", ui::value{"["s}}},
        "malformed inspector item array").path == "/nodes/rows/props/items", "malformed item text reports its property");
}

void atomic_transactions()
{
    ui::session s;
    const auto initial = snapshot(s);
    const auto root = initial.doc.root;
    apply(s, {ui::insert_node{"photo", root, {"image", "Photo", {{"resource", "logo"s}}, {}}},
        ui::set_resource{"logo", "assets/logo.png"}}); // Reference is valid only at the end of the batch.
    const auto before = snapshot(s);
    check(before.dirty && before.undo && before.revision > initial.revision &&
        before.doc.nodes.at(root).children == std::vector<std::string>{"photo"} &&
        before.doc.nodes.at("photo").props.at("resource") == ui::value{"logo"s} &&
        before.doc.resources.at("logo") == "assets/logo.png", "whole initial batch is committed");

    apply(s, {ui::rename_node{"photo", "Redo name"}, ui::set_resource{"logo", "assets/new-logo.png"}});
    auto redo_doc = before.doc;
    redo_doc.nodes.at("photo").name = "Redo name";
    redo_doc.resources.at("logo") = "assets/new-logo.png";
    check(s.current() == redo_doc, "successful batch commits both the node and resource changes");
    check(s.undo() && s.current() == before.doc && s.can_undo() && s.can_redo(), "one undo restores both edits and leaves redo");
    const auto mid = reject_edit(s, {ui::rename_node{root, "Must roll back"}, ui::set_resource{"logo", "assets/mid.png"},
        ui::insert_node{"photo", root, {"label", "Duplicate", {}, {}}}, ui::rename_node{"photo", "Never applied"}},
        "mid-batch duplicate ID after valid edits");
    check(mid.path == "/nodes/photo", "mid-batch failure identifies the duplicate insertion");
    const auto final = reject_edit(s, {ui::set_resource{"logo", std::nullopt}, ui::rename_node{"photo", "Also rollback"}},
        "final validation rejects dangling image resource after both edits");
    check(final.path == "/nodes/photo/props/resource", "final failure identifies the dangling resource reference");
    check(s.redo() && s.current() == redo_doc && !s.can_redo(), "redo still replays both original changes");
    check(s.undo() && s.current() == before.doc, "undo still restores pre-redo document");
    check(s.undo() && s.current() == initial.doc && !s.dirty() && !s.can_undo(),
        "initial two-edit transaction undoes in one step to a clean document");
}

void live_transactions()
{
    ui::session s;
    const auto initial = snapshot(s);
    const auto& root = initial.doc.root;
    apply_live(s, {ui::set_property{root, "width", ui::value{20.0}},
        ui::set_resource{"logo", "assets/logo.png"}});
    const auto first = snapshot(s);
    check(first.doc.nodes.at(root).props.at("width") == ui::value{20.0} && first.doc.resources.contains("logo") &&
        first.committed == initial.doc && first.revision == initial.revision + 1 && first.dirty && first.undo && !first.redo,
        "first live batch is visible and dirty without modifying the committed document");

    const ui::edit invalid[] = {ui::rename_node{root, "Must not leak"}, ui::set_property{root, "width", ui::value{-1.0}}};
    ui::diagnostic error;
    expect_failure(s.apply_live(invalid, &error), error, "invalid live width");
    check(error.path == "/nodes/" + root + "/props/width" && snapshot(s) == first,
        "invalid live batch keeps the last valid preview and revision");

    const ui::edit next = ui::set_property{root, "width", ui::value{30.0}};
    expect_ok(s.apply_live({&next, 1}, &error), error, "replace live batch");
    const auto latest = snapshot(s);
    check(latest.doc.nodes.at(root).props.at("width") == ui::value{30.0} && latest.doc.resources.empty() &&
        latest.committed == initial.doc && latest.revision == first.revision + 1 && error.message.empty(),
        "every live batch starts from the committed baseline");
    check(s.apply_live({&next, 1}) && snapshot(s) == latest, "repeating a preview does not change revision");

    s.finish_live();
    check(s.current() == latest.doc && s.committed() == latest.doc && s.revision() == latest.revision,
        "finishing records the preview without a visible change");
    check(s.undo() && s.current() == initial.doc && !s.dirty() && !s.can_undo() && s.can_redo(),
        "one undo restores the clean baseline after multiple live changes");
    check(s.redo() && s.current() == latest.doc && !s.can_redo(), "redo restores only the final live result");
}

void live_cancellation()
{
    ui::session s;
    const auto base = s.committed();
    apply(s, {ui::rename_node{base.root, "Redo branch"}});
    const auto redo = s.current();
    check(s.undo() && s.can_redo() && !s.dirty(), "prepare a clean baseline with redo");

    const ui::edit preview = ui::rename_node{base.root, "Preview"};
    apply_live(s, {preview});
    const auto first = snapshot(s);
    check(!s.can_redo() && !s.redo() && snapshot(s) == first, "redo is disabled during preview");
    s.finish_live(false);
    check(s.current() == base && s.revision() == first.revision + 1 && !s.dirty() && !s.can_undo() && s.can_redo(),
        "cancelling restores the baseline and preserves redo");

    apply_live(s, {preview});
    const auto preview_revision = s.revision();
    apply_live(s, {ui::rename_node{base.root, base.nodes.at(base.root).name}});
    const auto returned = snapshot(s);
    check(returned.doc == base && returned.revision == preview_revision + 1 && !returned.dirty && !returned.undo && returned.redo,
        "a batch equal to the baseline removes the preview without recording it");
    s.finish_live();
    check(snapshot(s) == returned, "finishing with no preview is a no-op");
    apply_live(s, {preview});
    apply_live(s, {});
    check(s.current() == base && s.redo() && s.current() == redo, "empty live batch returns to baseline and leaves redo intact");

    check(s.undo(), "return to baseline before undoing an active preview");
    apply_live(s, {preview});
    const auto active = snapshot(s);
    check(s.undo() && s.current() == base && s.revision() == active.revision + 1 && !s.dirty() && s.can_redo(),
        "undo while live returns directly to the baseline");
    check(s.redo() && s.current() == active.doc && !s.can_redo(), "redo restores the live edit, not the discarded branch");
}

void live_ordinary_operations()
{
    const temp_dir tmp;
    const auto file = tmp.path / "live.vgui.json";
    ui::session s;
    ui::diagnostic error;
    const auto base = s.committed();
    apply_live(s, {ui::insert_node{"label", base.root, {"label", "Preview", {}, {}}}});
    const auto preview = snapshot(s);
    reject_edit(s, {ui::erase_node{base.root}}, "failed ordinary apply during a preview");
    apply(s, {ui::rename_node{"label", "Applied"}});
    check(s.current() == s.committed() && s.current().nodes.at("label").name == "Applied" &&
        s.revision() == preview.revision + 1, "ordinary apply commits the preview before recording a new edit");
    check(s.undo() && s.current() == preview.doc && s.undo() && s.current() == base && !s.dirty(),
        "ordinary apply and the live gesture are separate undo steps");

    apply_live(s, {ui::rename_node{base.root, "Save preview"}});
    const auto saved = snapshot(s);
    expect_failure(s.load(file, &error), error, "load missing file during live edit");
    expect_failure(s.save(tmp.path / "missing-dir" / file.filename(), &error), error, "save to missing parent during live edit");
    check(snapshot(s) == saved, "failed load and save both preserve the active preview");
    expect_ok(s.save(file, &error), error, "save live preview");
    check(s.committed() == saved.doc && s.current() == saved.doc && s.revision() == saved.revision && !s.dirty() &&
        read_bytes(file) == encode(saved.doc), "successful save commits the visible preview and establishes a clean savepoint");
    check(s.undo() && s.current() == base && s.dirty() && s.redo() && !s.dirty(), "undo and redo honor the live savepoint");

    apply_live(s, {ui::rename_node{base.root, "Discard on load"}});
    const auto before_load = s.revision();
    expect_ok(s.load(file, &error), error, "load over a live preview");
    check(s.current() == saved.doc && s.committed() == saved.doc && s.revision() == before_load + 1 &&
        !s.dirty() && !s.can_undo() && !s.can_redo(), "successful load discards the preview and history");
}

void subtree_transactions()
{
    ui::session s;
    const auto root = s.current().root;
    apply(s, {ui::insert_node{"group", root, {"panel", "Group", {{"layout", "row"s}}, {}}},
        ui::insert_node{"dock", root, {"panel", "Dock", {}, {}}},
        ui::insert_node{"tail", root, {"button", "Tail", {}, {}}},
        ui::insert_node{"title", "group", {"label", "Title", {{"text", "Header"s}}, {}}},
        ui::insert_node{"inner", "group", {"panel", "Inner", {}, {}}},
        ui::insert_node{"field", "inner", {"edit", "Field", {{"placeholder", "Name"s}}, {}}}});
    std::vector<ui::document> states{s.current()};
    const auto step = [&](ui::edit edit, ui::document expected, std::string_view context) {
        apply(s, {edit});
        check(s.current() == expected, context);
        states.push_back(std::move(expected));
    };

    reject_edit(s, {ui::copy_subtree{"group", "dock", {{"group", "copy"}}}}, "copy requires every subtree ID");
    reject_edit(s, {ui::copy_subtree{"group", "dock", {{"group", "copy"}, {"title", "same"},
        {"inner", "copy_inner"}, {"field", "same"}}}}, "copy rejects duplicate new IDs");
    reject_edit(s, {ui::move_node{"group", "inner"}}, "cannot reparent to a descendant");
    reject_edit(s, {ui::move_node{"group", "tail"}}, "cannot reparent to a leaf");
    reject_edit(s, {ui::move_node{"group", "dock", 1}}, "bad index cannot detach the source");
    reject_edit(s, {ui::erase_node{root}}, "cannot erase the root");

    ui::copy_subtree copy{"group", "dock", {{"group", "copy"}, {"title", "copy_title"},
        {"inner", "copy_inner"}, {"field", "copy_field"}}, 0};
    auto expected = states.back();
    for (const auto& [old_id, new_id] : copy.ids) expected.nodes.emplace(new_id, expected.nodes.at(old_id));
    expected.nodes.at("copy").children = {"copy_title", "copy_inner"};
    expected.nodes.at("copy_inner").children = {"copy_field"};
    expected.nodes.at("dock").children = {"copy"};
    step(copy, expected, "copy preserves properties and names and remaps every descendant");

    expected.nodes.at("group").children = {"inner"};
    expected.nodes.at("dock").children = {"title", "copy"};
    step(ui::move_node{"title", "dock", 0}, expected, "reparent at index zero without changing the ID");

    expected.nodes.at(root).children = {"dock", "tail", "group"};
    step(ui::move_node{"group", root, 2}, expected, "same-parent index is interpreted after removal");

    expected.nodes.at(root).children = {"dock", "tail"};
    for (const auto* id : {"group", "inner", "field"}) expected.nodes.erase(id);
    step(ui::erase_node{"group"}, expected, "erase removes exactly the remaining original subtree");

    for (std::size_t i = states.size() - 1; i > 0; --i)
        check(s.undo() && s.current() == states[i - 1], "undo restores exact subtree structure and order");
    for (std::size_t i = 1; i < states.size(); ++i)
        check(s.redo() && s.current() == states[i], "redo restores exact subtree structure and order");
    check(!s.can_redo(), "all structural edits have been replayed");
}

void savepoint_history()
{
    const temp_dir tmp;
    const auto file = tmp.path / "design.vgui.json";
    ui::session s;
    ui::diagnostic error;
    const auto fresh = s.current();
    const auto root = fresh.root;
    check(!s.dirty() && !s.can_undo() && !s.can_redo(), "new session is clean with empty history");

    apply(s, {ui::rename_node{root, "Saved"}});
    const auto saved = s.current();
    expect_ok(s.save(file, &error), error, "save current checkpoint");
    check(!s.dirty() && s.can_undo() && read_bytes(file) == encode(saved), "save writes canonical bytes and preserves undo");
    apply(s, {ui::set_property{root, "layout", ui::value{"row"s}}});
    const auto draft = s.current();
    check(s.dirty() && draft != saved, "editing past the savepoint is dirty");
    check(s.undo() && s.current() == saved && !s.dirty(), "undo to the saved history point is clean");
    check(s.redo() && s.current() == draft && s.dirty(), "redo away from the savepoint is dirty");
    check(s.undo() && s.undo() && s.current() == fresh && s.dirty() && s.can_redo(), "undo before the savepoint is dirty");
    check(s.redo() && s.current() == saved && !s.dirty(), "redo to the savepoint is clean");
    check(s.undo() && s.can_redo(), "prepare to discard the saved history point");
    apply(s, {ui::rename_node{root, "Saved"}});
    check(s.current() == saved && s.dirty() && !s.can_redo(),
        "branch at the same cursor and bytes stays dirty when the saved state was discarded");

    apply(s, {ui::rename_node{root, "Replacement"}});
    const auto replacement = s.current();
    expect_ok(s.save(file, &error), error, "replace existing file");
    check(!s.dirty() && read_bytes(file) == encode(replacement), "successful replacement updates bytes and savepoint");
    check(entries_under(tmp.path) == std::set<fs::path>{file.filename()}, "successful replacement leaves no sibling temp");
    apply(s, {ui::rename_node{root, "Unsaved"}});
    expect_ok(s.load(file, &error), error, "reload saved document");
    check(s.current() == replacement && !s.dirty() && !s.can_undo() && !s.can_redo(), "load resets history and establishes a clean state");
}

void failed_file_operations()
{
    const temp_dir tmp;
    const auto good = tmp.path / "existing.vgui.json";
    const auto bad = tmp.path / "bad.vgui.json";
    const auto blocked = tmp.path / "blocked.vgui.json";
    const auto marker = blocked / "keep.txt";
    ui::session s;
    ui::diagnostic error;
    const auto saved = s.current();
    expect_ok(s.save(good, &error), error, "create old save file");
    const auto original = read_bytes(good);
    write_bytes(bad, R"({"format":"vgui-ui-design","version":2,"root":"root","nodes":{"root":{"type":"panel"}}})");
    // A non-empty directory cannot be replaced by a regular file, regardless of permissions.
    check(fs::create_directory(blocked), "create directory at the blocked save target");
    write_bytes(marker, "leave this target alone");
    const auto entries = entries_under(tmp.path);

    apply(s, {ui::rename_node{saved.root, "Draft"}});
    apply(s, {ui::rename_node{saved.root, "Redo draft"}});
    const auto redo_doc = s.current();
    check(s.undo() && s.can_redo() && s.dirty(), "start I/O failures with draft and redo history");
    const auto draft = snapshot(s);
    auto out = sentinel();
    expect_failure(ui::read_document(bad, out, &error), error, "read unsupported file");
    check(out == sentinel(), "failed read does not replace output");
    expect_failure(s.load(bad, &error), error, "load unsupported file");
    check(snapshot(s) == draft, "failed load preserves session, revision and history");

    auto invalid = draft.doc;
    invalid.nodes.at(invalid.root).children = {"missing"};
    expect_failure(ui::write_document(good, invalid, &error), error, "invalid save to an existing regular file");
    check(read_bytes(good) == original && entries_under(tmp.path) == entries,
        "failed validation preserves the old file and creates no leftover temp");

    expect_failure(s.save(blocked, &error), error, "save blocked by non-empty directory at destination");
    check(snapshot(s) == draft, "failed I/O save does not change session or mark draft clean");
    check(fs::is_directory(blocked) && read_bytes(marker) == "leave this target alone" && read_bytes(good) == original,
        "failed replacement preserves target directory, contents and previous save");
    check(entries_under(tmp.path) == entries, "failed save leaves no sibling temp file");
    check(s.redo() && s.current() == redo_doc && s.dirty(), "redo still works after failed I/O");
    check(s.undo() && s.current() == draft.doc && s.dirty(), "failed save did not move the savepoint to the draft");
    check(s.undo() && s.current() == saved && !s.dirty(), "undo still returns to the original clean savepoint");
}
} // namespace

int main()
{
    const struct { const char* name; void (*run)(); } tests[] = {
        {"canonical v1 JSON", canonical_round_trip},
        {"label text orientation", label_text_orientation},
        {"optional font properties round trip", font_properties_round_trip},
        {"font property validation", reject_bad_font_properties},
        {"font family resource validation and round trip", font_family_validation},
        {"schema, duplicates and hierarchy", reject_bad_schema_and_tree},
        {"collection JSON arrays", collection_json_round_trip},
        {"gradient keyframe JSON arrays", gradient_keyframe_round_trip},
        {"gradient keyframe geometry round trip", gradient_keyframe_geometry_round_trip},
        {"gradient keyframe geometry validation", gradient_keyframe_geometry_validation},
        {"named radio groups", named_radio_groups},
        {"declarative container constraints", declarative_container_constraints},
        {"IDs, keys and resource paths", keys_and_paths},
        {"input limits", input_limits},
        {"atomic transactions", atomic_transactions},
        {"live transactions", live_transactions},
        {"live cancellation and redo", live_cancellation},
        {"ordinary operations during live edits", live_ordinary_operations},
        {"subtree transactions", subtree_transactions},
        {"savepoint history", savepoint_history},
        {"failed file operations", failed_file_operations},
    };
    int failures = 0;
    for (const auto& [name, run] : tests) {
        try {
            run();
            std::printf("PASS: %s\n", name);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "FAIL: %s: %s\n", name, error.what());
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
