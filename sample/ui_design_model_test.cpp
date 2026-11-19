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
        {"schema, duplicates and hierarchy", reject_bad_schema_and_tree},
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
