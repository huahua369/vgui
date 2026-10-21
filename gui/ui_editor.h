#pragma once
/*
 * ui_editor.h - simple retained-mode UI editor built on the vgui C API.
 *
 * The editor keeps a JSON-friendly document tree whose nodes mirror the
 * vgui.h widget properties.  A live preview is rebuilt from the document with
 * vgui_widget_add()/vgui_widget_set_*(), the document can be saved to and
 * loaded from JSON (nlohmann::json) and exported as compilable C++ that only
 * uses the public vgui.h functions.
 *
 * The implementation lives in gui/ui_editor.cpp; the SDL3 host program is
 * sample/ui_editor_main.cpp.
 */
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "vgui.h"

namespace vgui_editor {

struct ui_node;

/* Default JSON/CPP locations live in the gui/ directory of the checkout. */
std::string default_json_path();
std::string default_cpp_path();
const char* default_cpp_file_name();

/* ------------------------------------------------------------------------- */
/* Document model                                                            */
/* ------------------------------------------------------------------------- */

struct ui_node
{
	vgui_widget_kind kind = VGUI_PANEL;
	std::string id;
	std::string text;

	/* layout */
	int layout = VGUI_LAYOUT_ABSOLUTE;
	float x = 0.0f, y = 0.0f;
	float w = 0.0f, h = 0.0f;
	float min_w = 0.0f, min_h = 0.0f;
	float max_w = 0.0f, max_h = 0.0f;
	float margin[4] = { 0, 0, 0, 0 };	/* left, top, right, bottom */
	float padding[4] = { 0, 0, 0, 0 };
	float gap = 0.0f;
	float grow = 0.0f;
	int align_items = VGUI_ALIGN_START;
	int align_self = VGUI_ALIGN_AUTO;
	int justify = VGUI_JUSTIFY_START;

	/* visual */
	uint32_t bg = 0;
	uint32_t border_color = 0;
	uint32_t text_color = 0;
	float border_width = 0.0f;
	float rounding = 0.0f;
	float font_size = 0.0f;
	float opacity = 1.0f;
	float text_align_x = 0.5f;
	float text_align_y = 0.5f;

	/* state */
	bool visible = true;
	bool enabled = true;
	bool focusable = false;

	/* value controls */
	float value = 0.0f;
	float range_min = 0.0f;
	float range_max = 1.0f;
	float step = 0.0f;
	bool checked = false;
	bool vertical = false;

	/* edit */
	std::string edit_text;
	std::string placeholder;
	bool multiline = false;
	bool password = false;
	bool readonly = false;

	ui_node* parent = nullptr;
	std::vector<std::unique_ptr<ui_node>> children;

	ui_node* add_child(vgui_widget_kind child_kind);
	void remove_child(ui_node* child);
};

struct ui_document
{
	std::string name = "untitled";
	int canvas_width = 800;
	int canvas_height = 600;
	ui_node root;

	ui_document();
	/* Replaces the tree with a small default page. */
	void reset();

	std::string to_json_string(int indent = 2) const;
	bool from_json_string(const std::string& text, std::string* error);
};

/* Creates a node with kind-specific defaults (size, colours, values). */
std::unique_ptr<ui_node> make_default_node(vgui_widget_kind kind);

const char* widget_kind_name(vgui_widget_kind kind);
bool widget_kind_from_name(const std::string& name, vgui_widget_kind* kind);

/* ------------------------------------------------------------------------- */
/* C++ code generation                                                       */
/* ------------------------------------------------------------------------- */

struct ui_export_options
{
	std::string function_name = "build_ui";
	bool include_comments = true;
};

std::string ui_generate_cpp(const ui_document& doc, const ui_export_options& options = {});

/* ------------------------------------------------------------------------- */
/* Editor                                                                    */
/* ------------------------------------------------------------------------- */

class ui_editor
{
public:
	ui_editor();
	~ui_editor();
	ui_editor(const ui_editor&) = delete;
	ui_editor& operator=(const ui_editor&) = delete;

	/* Builds the editor UI inside an existing vgui context. */
	void build(vgui_context* ctx);

	/* Per-frame hook: keeps the page size and selection frame in sync. */
	void tick();

	ui_document& document() { return doc_; }
	const ui_document& document() const { return doc_; }

	ui_node* selected() const;
	void select(ui_node* node);

	void add_widget(vgui_widget_kind kind);
	void delete_selected();

	bool save_json(const std::string& path, std::string* error = nullptr);
	bool load_json(const std::string& path, std::string* error = nullptr);
	bool export_cpp(const std::string& path, std::string* error = nullptr);

	void set_status(const std::string& text);
	std::string status() const;

	/* Path edits shown in the toolbar; also used by Save/Load/Export. */
	std::string json_path() const;
	std::string cpp_path() const;
	void set_json_path(const std::string& path);
	void set_cpp_path(const std::string& path);

	/* Builds the whole editor UI again (after programmatic document changes). */
	void refresh_all();

	/* Implementation type (defined in ui_editor.cpp). Public only so the
	 * C-style vgui callbacks can name it. */
	struct impl;

private:
	std::unique_ptr<impl> p_;
	ui_document doc_;
};

} /* namespace vgui_editor */
