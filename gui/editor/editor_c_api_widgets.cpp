/* gui/editor/editor_c_api_widgets.cpp - C facade for the small editor widgets:
 * spinbox, combo box, search box, breadcrumb, toolbar family and status bar. */
#include "editor_c_api.h"
#include "editor_c_api_borrow.h"

#include <new>
#include <utility>
#include <vector>

#include <vgui/editor/breadcrumb.h>
#include <vgui/editor/combo_box.h>
#include <vgui/editor/search_box.h>
#include <vgui/editor/spinbox.h>
#include <vgui/editor/status_bar.h>
#include <vgui/editor/toolbar.h>

using namespace vgui_editor_kit;

/* -------------------------------------------------------------------------- */
/* Handle storage                                                            */
/* -------------------------------------------------------------------------- */

struct vgui_editor_spinbox {
	spinbox impl;
	std::string scratch;
	vgui_editor_value_fn change_fn = nullptr;
	void* change_user = nullptr;
	vgui_editor_value_fn commit_fn = nullptr;
	void* commit_user = nullptr;
};

struct vgui_editor_combo_box {
	combo_box impl;
	std::string scratch;
	vgui_editor_combo_fn change_fn = nullptr;
	void* change_user = nullptr;
	vgui_editor_combo_fn commit_fn = nullptr;
	void* commit_user = nullptr;
};

struct vgui_editor_search_box : vgeditor::c_object<search_box> {
	using c_object::c_object;

	vgui_editor_text_fn change_fn = nullptr;
	void* change_user = nullptr;
	vgui_editor_text_fn submit_fn = nullptr;
	void* submit_user = nullptr;
};

struct vgui_editor_breadcrumb : vgeditor::c_object<breadcrumb> {
	using c_object::c_object;

	vgui_editor_navigate_fn navigate_fn = nullptr;
	void* navigate_user = nullptr;
};

/* Editor-owned widgets either own their C++ object or borrow one from a
 * container (a toolbar button is owned by its toolbar and must not be
 * destroyed through the button handle). */
struct vgui_editor_icon_button {
	std::unique_ptr<icon_button> owned;
	icon_button* impl = nullptr;
	bool borrowed = false;
};

struct vgui_editor_toggle_button {
	std::unique_ptr<toggle_button> owned;
	toggle_button* impl = nullptr;
	bool borrowed = false;
};

struct vgui_editor_segmented_control {
	std::unique_ptr<segmented_control> owned;
	segmented_control* impl = nullptr;
	bool borrowed = false;
};

struct vgui_editor_toolbar : vgeditor::c_object<toolbar_view> {
	using c_object::c_object;

	std::vector<std::unique_ptr<vgui_editor_icon_button>> buttons;
	std::vector<std::unique_ptr<vgui_editor_toggle_button>> toggles;
	std::vector<std::unique_ptr<vgui_editor_segmented_control>> segments;
};

struct vgui_editor_status_bar : vgeditor::c_object<status_bar> {
	using c_object::c_object;
};

namespace {

	vgui_editor_icon_button* wrap(icon_button* target)
	{
		vgui_editor_icon_button* button = new (std::nothrow) vgui_editor_icon_button();
		if (!button)
			return nullptr;
		button->impl = target;
		button->borrowed = true;
		return button;
	}

	vgui_editor_toggle_button* wrap(toggle_button* target)
	{
		vgui_editor_toggle_button* button = new (std::nothrow) vgui_editor_toggle_button();
		if (!button)
			return nullptr;
		button->impl = target;
		button->borrowed = true;
		return button;
	}

	vgui_editor_segmented_control* wrap(segmented_control* target)
	{
		vgui_editor_segmented_control* seg =
			new (std::nothrow) vgui_editor_segmented_control();
		if (!seg)
			return nullptr;
		seg->impl = target;
		seg->borrowed = true;
		return seg;
	}

} /* namespace */

extern "C" {

	/* ------------------------------------------------------------------ */
	/* Spinbox                                                            */
	/* ------------------------------------------------------------------ */

	vgui_editor_spinbox* vgui_editor_spinbox_create(vgui_widget* parent,
		const char* label)
	{
		if (!parent)
			return nullptr;
		std::unique_ptr<vgui_editor_spinbox> spin(new (std::nothrow) vgui_editor_spinbox());
		if (!spin)
			return nullptr;
		spin->impl.build(parent, vgeditor::to_text(label));
		return spin.release();
	}

	void vgui_editor_spinbox_destroy(vgui_editor_spinbox* spin)
	{
		if (!spin)
			return;
		vgui_widget* widget = spin->impl.widget();
		spin->impl.set_on_change(nullptr);
		spin->impl.set_on_commit(nullptr);
		delete spin;
		if (widget)
			vgui_widget_destroy(widget);
	}

	vgui_widget* vgui_editor_spinbox_widget(vgui_editor_spinbox* spin)
	{
		return spin ? spin->impl.widget() : nullptr;
	}

	vgui_widget* vgui_editor_spinbox_edit_widget(vgui_editor_spinbox* spin)
	{
		return spin ? spin->impl.edit_widget() : nullptr;
	}

	vgui_widget* vgui_editor_spinbox_label_widget(vgui_editor_spinbox* spin)
	{
		return spin ? spin->impl.label_widget() : nullptr;
	}

	vgui_widget* vgui_editor_spinbox_decrement_widget(vgui_editor_spinbox* spin)
	{
		return spin ? spin->impl.decrement_widget() : nullptr;
	}

	vgui_widget* vgui_editor_spinbox_increment_widget(vgui_editor_spinbox* spin)
	{
		return spin ? spin->impl.increment_widget() : nullptr;
	}

	void vgui_editor_spinbox_set_id(vgui_editor_spinbox* spin, const char* id)
	{
		if (spin)
			spin->impl.set_id(vgeditor::to_text(id));
	}

	const char* vgui_editor_spinbox_id(vgui_editor_spinbox* spin)
	{
		return spin ? spin->impl.id().c_str() : "";
	}

	void vgui_editor_spinbox_set_label(vgui_editor_spinbox* spin, const char* label)
	{
		if (spin)
			spin->impl.set_label(vgeditor::to_text(label));
	}

	const char* vgui_editor_spinbox_label(vgui_editor_spinbox* spin)
	{
		return spin ? spin->impl.label().c_str() : "";
	}

	void vgui_editor_spinbox_set_label_width(vgui_editor_spinbox* spin, float width)
	{
		if (spin)
			spin->impl.set_label_width(width);
	}

	float vgui_editor_spinbox_label_width(vgui_editor_spinbox* spin)
	{
		return spin ? spin->impl.label_width() : 0.0f;
	}

	void vgui_editor_spinbox_set_font_size(vgui_editor_spinbox* spin, float size)
	{
		if (spin)
			spin->impl.set_font_size(size);
	}

	float vgui_editor_spinbox_font_size(vgui_editor_spinbox* spin)
	{
		return spin ? spin->impl.font_size() : 0.0f;
	}

	void vgui_editor_spinbox_set_edit_min_width(vgui_editor_spinbox* spin, float width)
	{
		if (spin)
			spin->impl.set_edit_min_width(width);
	}

	void vgui_editor_spinbox_set_auto_width(vgui_editor_spinbox* spin, int enabled)
	{
		if (spin)
			spin->impl.set_auto_width(enabled != 0);
	}

	int vgui_editor_spinbox_auto_width(vgui_editor_spinbox* spin)
	{
		return spin && spin->impl.auto_width() ? 1 : 0;
	}

	void vgui_editor_spinbox_set_buttons_visible(vgui_editor_spinbox* spin, int visible)
	{
		if (spin)
			spin->impl.set_buttons_visible(visible != 0);
	}

	int vgui_editor_spinbox_buttons_visible(vgui_editor_spinbox* spin)
	{
		return spin && spin->impl.buttons_visible() ? 1 : 0;
	}

	void vgui_editor_spinbox_set_value_type(vgui_editor_spinbox* spin,
		vgui_editor_value_type type)
	{
		if (!spin)
			return;
		spin->impl.set_value_type(type == VGUI_EDITOR_VALUE_INTEGER ?
			spinbox::value_type::integer : spinbox::value_type::number);
	}

	vgui_editor_value_type vgui_editor_spinbox_value_type(vgui_editor_spinbox* spin)
	{
		if (!spin)
			return VGUI_EDITOR_VALUE_NUMBER;
		return spin->impl.type() == spinbox::value_type::integer ?
			VGUI_EDITOR_VALUE_INTEGER : VGUI_EDITOR_VALUE_NUMBER;
	}

	void vgui_editor_spinbox_set_range(vgui_editor_spinbox* spin, double min_value,
		double max_value)
	{
		if (spin)
			spin->impl.set_range(min_value, max_value);
	}

	void vgui_editor_spinbox_clear_range(vgui_editor_spinbox* spin)
	{
		if (spin)
			spin->impl.clear_range();
	}

	int vgui_editor_spinbox_has_range(vgui_editor_spinbox* spin)
	{
		return spin && spin->impl.has_range() ? 1 : 0;
	}

	double vgui_editor_spinbox_min_value(vgui_editor_spinbox* spin)
	{
		return spin ? spin->impl.min_value() : 0.0;
	}

	double vgui_editor_spinbox_max_value(vgui_editor_spinbox* spin)
	{
		return spin ? spin->impl.max_value() : 0.0;
	}

	void vgui_editor_spinbox_set_step(vgui_editor_spinbox* spin, double step)
	{
		if (spin)
			spin->impl.set_step(step);
	}

	double vgui_editor_spinbox_step(vgui_editor_spinbox* spin)
	{
		return spin ? spin->impl.step() : 0.0;
	}

	void vgui_editor_spinbox_set_value(vgui_editor_spinbox* spin, double value)
	{
		if (spin)
			spin->impl.set_value(value);
	}

	double vgui_editor_spinbox_value(vgui_editor_spinbox* spin)
	{
		return spin ? spin->impl.value() : 0.0;
	}

	const char* vgui_editor_spinbox_value_text(vgui_editor_spinbox* spin)
	{
		if (!spin)
			return "";
		spin->scratch = spin->impl.value_text();
		return spin->scratch.c_str();
	}

	int vgui_editor_spinbox_commit_text(vgui_editor_spinbox* spin, const char* text)
	{
		return spin && spin->impl.commit_text(vgeditor::to_text(text)) ? 1 : 0;
	}

	int vgui_editor_spinbox_commit_value(vgui_editor_spinbox* spin, double value)
	{
		return spin && spin->impl.commit_value(value) ? 1 : 0;
	}

	void vgui_editor_spinbox_step_by(vgui_editor_spinbox* spin, int steps)
	{
		if (spin)
			spin->impl.step_by(steps);
	}

	void vgui_editor_spinbox_set_read_only(vgui_editor_spinbox* spin, int read_only)
	{
		if (spin)
			spin->impl.set_read_only(read_only != 0);
	}

	int vgui_editor_spinbox_read_only(vgui_editor_spinbox* spin)
	{
		return spin && spin->impl.read_only() ? 1 : 0;
	}

	void vgui_editor_spinbox_set_enabled(vgui_editor_spinbox* spin, int enabled)
	{
		if (spin)
			spin->impl.set_enabled(enabled != 0);
	}

	int vgui_editor_spinbox_enabled(vgui_editor_spinbox* spin)
	{
		return spin && spin->impl.enabled() ? 1 : 0;
	}

	void vgui_editor_spinbox_set_error(vgui_editor_spinbox* spin, const char* error)
	{
		if (spin)
			spin->impl.set_error(vgeditor::to_text(error));
	}

	const char* vgui_editor_spinbox_error(vgui_editor_spinbox* spin)
	{
		return spin ? spin->impl.error().c_str() : "";
	}

	void vgui_editor_spinbox_focus(vgui_editor_spinbox* spin)
	{
		if (spin)
			spin->impl.focus();
	}

	void vgui_editor_spinbox_set_on_change(vgui_editor_spinbox* spin,
		vgui_editor_value_fn fn, void* user)
	{
		if (!spin)
			return;
		spin->change_fn = fn;
		spin->change_user = user;
		spin->impl.set_on_change([spin](double value) {
			if (spin->change_fn)
				spin->change_fn(spin->change_user, value);
		});
	}

	void vgui_editor_spinbox_set_on_commit(vgui_editor_spinbox* spin,
		vgui_editor_value_fn fn, void* user)
	{
		if (!spin)
			return;
		spin->commit_fn = fn;
		spin->commit_user = user;
		spin->impl.set_on_commit([spin](double value) {
			if (spin->commit_fn)
				spin->commit_fn(spin->commit_user, value);
		});
	}

	/* ------------------------------------------------------------------ */
	/* Combo box                                                          */
	/* ------------------------------------------------------------------ */

	vgui_editor_combo_box* vgui_editor_combo_box_create(vgui_widget* parent)
	{
		if (!parent)
			return nullptr;
		std::unique_ptr<vgui_editor_combo_box> combo(
			new (std::nothrow) vgui_editor_combo_box());
		if (!combo)
			return nullptr;
		combo->impl.build(parent);
		return combo.release();
	}

	void vgui_editor_combo_box_destroy(vgui_editor_combo_box* combo)
	{
		if (!combo)
			return;
		vgui_widget* widget = combo->impl.widget();
		combo->impl.set_on_change(nullptr);
		combo->impl.set_on_commit(nullptr);
		/* The destructor tears the detached menu down, so it has to run while
		 * the widgets are still alive. */
		delete combo;
		if (widget)
			vgui_widget_destroy(widget);
	}

	vgui_widget* vgui_editor_combo_box_widget(vgui_editor_combo_box* combo)
	{
		return combo ? combo->impl.widget() : nullptr;
	}

	void vgui_editor_combo_box_set_id(vgui_editor_combo_box* combo, const char* id)
	{
		if (combo)
			combo->impl.set_id(vgeditor::to_text(id));
	}

	const char* vgui_editor_combo_box_id(vgui_editor_combo_box* combo)
	{
		return combo ? combo->impl.id().c_str() : "";
	}

	void vgui_editor_combo_box_set_items(vgui_editor_combo_box* combo,
		const vgui_editor_combo_item* items, size_t count)
	{
		if (!combo)
			return;
		std::vector<combo_box::item> value;
		if (items)
		{
			value.reserve(count);
			for (size_t i = 0; i < count; ++i)
			{
				const vgui_editor_combo_item item = vgeditor::take_desc(&items[i]);
				combo_box::item entry;
				entry.label = vgeditor::to_text(item.label);
				entry.value = vgeditor::to_text(item.value);
				entry.enabled = item.enabled != 0;
				value.push_back(std::move(entry));
			}
		}
		combo->impl.set_items(std::move(value));
	}

	size_t vgui_editor_combo_box_item_count(vgui_editor_combo_box* combo)
	{
		return combo ? combo->impl.item_count() : 0;
	}

	int vgui_editor_combo_box_item_at(vgui_editor_combo_box* combo, int index,
		vgui_editor_combo_item* out)
	{
		if (!combo || !out)
			return 0;
		const combo_box::item* item = combo->impl.item_at(index);
		std::memset(out, 0, sizeof(*out));
		out->struct_size = (uint32_t)sizeof(*out);
		if (!item)
			return 0;
		out->label = item->label.c_str();
		out->value = item->value.c_str();
		out->enabled = item->enabled ? 1 : 0;
		return 1;
	}

	int vgui_editor_combo_box_index_of_value(vgui_editor_combo_box* combo,
		const char* value)
	{
		return combo ? combo->impl.index_of_value(vgeditor::to_text(value)) : -1;
	}

	int vgui_editor_combo_box_set_item_enabled(vgui_editor_combo_box* combo, int index,
		int enabled)
	{
		return combo && combo->impl.set_item_enabled(index, enabled != 0) ? 1 : 0;
	}

	void vgui_editor_combo_box_set_placeholder(vgui_editor_combo_box* combo,
		const char* text)
	{
		if (combo)
			combo->impl.set_placeholder(vgeditor::to_text(text));
	}

	const char* vgui_editor_combo_box_placeholder(vgui_editor_combo_box* combo)
	{
		return combo ? combo->impl.placeholder().c_str() : "";
	}

	const char* vgui_editor_combo_box_display_text(vgui_editor_combo_box* combo)
	{
		if (!combo)
			return "";
		combo->scratch = combo->impl.display_text();
		return combo->scratch.c_str();
	}

	int vgui_editor_combo_box_set_selected_index(vgui_editor_combo_box* combo, int index)
	{
		return combo && combo->impl.set_selected_index(index) ? 1 : 0;
	}

	int vgui_editor_combo_box_set_value(vgui_editor_combo_box* combo, const char* value)
	{
		return combo && combo->impl.set_value(vgeditor::to_text(value)) ? 1 : 0;
	}

	void vgui_editor_combo_box_clear_selection(vgui_editor_combo_box* combo)
	{
		if (combo)
			combo->impl.clear_selection();
	}

	int vgui_editor_combo_box_select_index(vgui_editor_combo_box* combo, int index)
	{
		return combo && combo->impl.select_index(index) ? 1 : 0;
	}

	int vgui_editor_combo_box_select_value(vgui_editor_combo_box* combo,
		const char* value)
	{
		return combo && combo->impl.select_value(vgeditor::to_text(value)) ? 1 : 0;
	}

	int vgui_editor_combo_box_select_none(vgui_editor_combo_box* combo)
	{
		return combo && combo->impl.select_none() ? 1 : 0;
	}

	int vgui_editor_combo_box_selected_index(vgui_editor_combo_box* combo)
	{
		return combo ? combo->impl.selected_index() : -1;
	}

	const char* vgui_editor_combo_box_value(vgui_editor_combo_box* combo)
	{
		return combo ? combo->impl.value().c_str() : "";
	}

	int vgui_editor_combo_box_open(vgui_editor_combo_box* combo)
	{
		return combo && combo->impl.open() ? 1 : 0;
	}

	void vgui_editor_combo_box_close(vgui_editor_combo_box* combo)
	{
		if (combo)
			combo->impl.close();
	}

	int vgui_editor_combo_box_is_open(vgui_editor_combo_box* combo)
	{
		return combo && combo->impl.is_open() ? 1 : 0;
	}

	void vgui_editor_combo_box_toggle(vgui_editor_combo_box* combo)
	{
		if (combo)
			combo->impl.toggle();
	}

	void vgui_editor_combo_box_set_enabled(vgui_editor_combo_box* combo, int enabled)
	{
		if (combo)
			combo->impl.set_enabled(enabled != 0);
	}

	int vgui_editor_combo_box_enabled(vgui_editor_combo_box* combo)
	{
		return combo && combo->impl.enabled() ? 1 : 0;
	}

	void vgui_editor_combo_box_set_read_only(vgui_editor_combo_box* combo, int read_only)
	{
		if (combo)
			combo->impl.set_read_only(read_only != 0);
	}

	int vgui_editor_combo_box_read_only(vgui_editor_combo_box* combo)
	{
		return combo && combo->impl.read_only() ? 1 : 0;
	}

	void vgui_editor_combo_box_focus(vgui_editor_combo_box* combo)
	{
		if (combo)
			combo->impl.focus();
	}

	void vgui_editor_combo_box_set_on_change(vgui_editor_combo_box* combo,
		vgui_editor_combo_fn fn, void* user)
	{
		if (!combo)
			return;
		combo->change_fn = fn;
		combo->change_user = user;
		combo->impl.set_on_change([combo](int index, const std::string& value) {
			if (combo->change_fn)
				combo->change_fn(combo->change_user, index, value.c_str());
		});
	}

	void vgui_editor_combo_box_set_on_commit(vgui_editor_combo_box* combo,
		vgui_editor_combo_fn fn, void* user)
	{
		if (!combo)
			return;
		combo->commit_fn = fn;
		combo->commit_user = user;
		combo->impl.set_on_commit([combo](int index, const std::string& value) {
			if (combo->commit_fn)
				combo->commit_fn(combo->commit_user, index, value.c_str());
		});
	}

	/* ------------------------------------------------------------------ */
	/* Search box                                                         */
	/* ------------------------------------------------------------------ */

	vgui_editor_search_box* vgui_editor_search_box_create(vgui_widget* parent,
		const char* placeholder)
	{
		if (!parent)
			return nullptr;
		std::unique_ptr<vgui_editor_search_box> search(
			vgeditor::own_object<vgui_editor_search_box>(
				new (std::nothrow) search_box()));
		if (!search)
			return nullptr;
		search->impl.build(parent, placeholder ? std::string(placeholder) :
			std::string("Search"));
		return search.release();
	}

	void vgui_editor_search_box_destroy(vgui_editor_search_box* search)
	{
		if (!search || !search->owns_object())
			return;
		vgui_widget* widget = search->impl.widget();
		search->impl.set_on_change(nullptr);
		search->impl.set_on_submit(nullptr);
		delete search;
		if (widget)
			vgui_widget_destroy(widget);
	}

	vgui_widget* vgui_editor_search_box_widget(vgui_editor_search_box* search)
	{
		return search ? search->impl.widget() : nullptr;
	}

	vgui_widget* vgui_editor_search_box_edit_widget(vgui_editor_search_box* search)
	{
		return search ? search->impl.edit_widget() : nullptr;
	}

	vgui_widget* vgui_editor_search_box_clear_widget(vgui_editor_search_box* search)
	{
		return search ? search->impl.clear_widget() : nullptr;
	}

	void vgui_editor_search_box_set_placeholder(vgui_editor_search_box* search,
		const char* text)
	{
		if (search)
			search->impl.set_placeholder(vgeditor::to_text(text));
	}

	void vgui_editor_search_box_set_text(vgui_editor_search_box* search,
		const char* text)
	{
		if (search)
			search->impl.set_text(vgeditor::to_text(text));
	}

	const char* vgui_editor_search_box_text(vgui_editor_search_box* search)
	{
		return search ? search->impl.text().c_str() : "";
	}

	void vgui_editor_search_box_clear(vgui_editor_search_box* search)
	{
		if (search)
			search->impl.clear();
	}

	void vgui_editor_search_box_submit(vgui_editor_search_box* search)
	{
		if (search)
			search->impl.submit();
	}

	void vgui_editor_search_box_focus(vgui_editor_search_box* search)
	{
		if (search)
			search->impl.focus();
	}

	void vgui_editor_search_box_set_on_change(vgui_editor_search_box* search,
		vgui_editor_text_fn fn, void* user)
	{
		if (!search)
			return;
		search->change_fn = fn;
		search->change_user = user;
		search->impl.set_on_change([search](const std::string& text) {
			if (search->change_fn)
				search->change_fn(search->change_user, text.c_str());
		});
	}

	void vgui_editor_search_box_set_on_submit(vgui_editor_search_box* search,
		vgui_editor_text_fn fn, void* user)
	{
		if (!search)
			return;
		search->submit_fn = fn;
		search->submit_user = user;
		search->impl.set_on_submit([search](const std::string& text) {
			if (search->submit_fn)
				search->submit_fn(search->submit_user, text.c_str());
		});
	}

	/* ------------------------------------------------------------------ */
	/* Breadcrumb                                                         */
	/* ------------------------------------------------------------------ */

	vgui_editor_breadcrumb* vgui_editor_breadcrumb_create(vgui_widget* parent)
	{
		if (!parent)
			return nullptr;
		std::unique_ptr<vgui_editor_breadcrumb> path(
			vgeditor::own_object<vgui_editor_breadcrumb>(
			new (std::nothrow) breadcrumb()));
		if (!path)
			return nullptr;
		path->impl.build(parent);
		return path.release();
	}

	void vgui_editor_breadcrumb_destroy(vgui_editor_breadcrumb* path)
	{
		if (!path || !path->owns_object())
			return;
		vgui_widget* widget = path->impl.widget();
		path->impl.set_on_navigate(nullptr);
		delete path;
		if (widget)
			vgui_widget_destroy(widget);
	}

	vgui_widget* vgui_editor_breadcrumb_widget(vgui_editor_breadcrumb* path)
	{
		return path ? path->impl.widget() : nullptr;
	}

	void vgui_editor_breadcrumb_add_item(vgui_editor_breadcrumb* path,
		const char* label, const char* id)
	{
		if (path)
			path->impl.add_item(vgeditor::to_text(label), vgeditor::to_text(id));
	}

	void vgui_editor_breadcrumb_set_items(vgui_editor_breadcrumb* path,
		const vgui_editor_breadcrumb_item* items, size_t count)
	{
		if (!path)
			return;
		std::vector<vgui_editor_kit::breadcrumb_item> value;
		if (items)
		{
			value.reserve(count);
			for (size_t i = 0; i < count; ++i)
			{
				const vgui_editor_breadcrumb_item item = vgeditor::take_desc(&items[i]);
				vgui_editor_kit::breadcrumb_item entry;
				entry.label = vgeditor::to_text(item.label);
				entry.id = vgeditor::to_text(item.id);
				value.push_back(std::move(entry));
			}
		}
		path->impl.set_items(std::move(value));
	}

	void vgui_editor_breadcrumb_clear(vgui_editor_breadcrumb* path)
	{
		if (path)
			path->impl.clear();
	}

	size_t vgui_editor_breadcrumb_count(vgui_editor_breadcrumb* path)
	{
		return path ? path->impl.count() : 0;
	}

	int vgui_editor_breadcrumb_item_at(vgui_editor_breadcrumb* path, size_t index,
		vgui_editor_breadcrumb_item* out)
	{
		if (!path || !out)
			return 0;
		std::memset(out, 0, sizeof(*out));
		out->struct_size = (uint32_t)sizeof(*out);
		if (index >= path->impl.count())
			return 0;
		const vgui_editor_kit::breadcrumb_item& item = path->impl.item(index);
		out->label = item.label.c_str();
		out->id = item.id.c_str();
		return 1;
	}

	vgui_widget* vgui_editor_breadcrumb_item_widget(vgui_editor_breadcrumb* path,
		size_t index)
	{
		return path ? path->impl.item_widget(index) : nullptr;
	}

	int vgui_editor_breadcrumb_navigate(vgui_editor_breadcrumb* path, size_t index)
	{
		return path && path->impl.navigate(index) ? 1 : 0;
	}

	void vgui_editor_breadcrumb_set_active(vgui_editor_breadcrumb* path, size_t index)
	{
		if (path)
			path->impl.set_active(index);
	}

	size_t vgui_editor_breadcrumb_active(vgui_editor_breadcrumb* path)
	{
		return path ? path->impl.active() : 0;
	}

	void vgui_editor_breadcrumb_set_on_navigate(vgui_editor_breadcrumb* path,
		vgui_editor_navigate_fn fn, void* user)
	{
		if (!path)
			return;
		path->navigate_fn = fn;
		path->navigate_user = user;
		path->impl.set_on_navigate([path](std::size_t index, const std::string& id) {
			if (path->navigate_fn)
				path->navigate_fn(path->navigate_user, index, id.c_str());
		});
	}

	/* ------------------------------------------------------------------ */
	/* Icon / toggle buttons and segmented controls                       */
	/* ------------------------------------------------------------------ */

	vgui_editor_icon_button* vgui_editor_icon_button_create(vgui_widget* parent,
		const char* text)
	{
		if (!parent)
			return nullptr;
		std::unique_ptr<vgui_editor_icon_button> button(
			new (std::nothrow) vgui_editor_icon_button());
		if (!button)
			return nullptr;
		button->owned = std::make_unique<icon_button>();
		button->owned->build(parent, vgeditor::to_text(text));
		button->impl = button->owned.get();
		return button.release();
	}

	void vgui_editor_icon_button_destroy(vgui_editor_icon_button* button)
	{
		if (!button || button->borrowed)
			return;
		vgui_widget* widget = button->impl ? button->impl->widget() : nullptr;
		button->impl->set_on_click(nullptr);
		button->owned.reset();
		delete button;
		if (widget)
			vgui_widget_destroy(widget);
	}

	vgui_widget* vgui_editor_icon_button_widget(vgui_editor_icon_button* button)
	{
		return button && button->impl ? button->impl->widget() : nullptr;
	}

	void vgui_editor_icon_button_set_icon(vgui_editor_icon_button* button,
		vgui_texture* texture, int width, int height)
	{
		if (button && button->impl)
			button->impl->set_icon(texture, width, height);
	}

	void vgui_editor_icon_button_set_text(vgui_editor_icon_button* button,
		const char* text)
	{
		if (button && button->impl)
			button->impl->set_text(vgeditor::to_text(text));
	}

	void vgui_editor_icon_button_set_tooltip(vgui_editor_icon_button* button,
		const char* text)
	{
		if (button && button->impl)
			button->impl->set_tooltip(vgeditor::to_text(text));
	}

	void vgui_editor_icon_button_set_enabled(vgui_editor_icon_button* button,
		int enabled)
	{
		if (button && button->impl)
			button->impl->set_enabled(enabled != 0);
	}

	int vgui_editor_icon_button_enabled(vgui_editor_icon_button* button)
	{
		return button && button->impl && button->impl->enabled() ? 1 : 0;
	}

	void vgui_editor_icon_button_set_checked(vgui_editor_icon_button* button,
		int checked)
	{
		if (button && button->impl)
			button->impl->set_checked(checked != 0);
	}

	int vgui_editor_icon_button_checked(vgui_editor_icon_button* button)
	{
		return button && button->impl && button->impl->checked() ? 1 : 0;
	}

	void vgui_editor_icon_button_set_on_click(vgui_editor_icon_button* button,
		vgui_editor_click_fn fn, void* user)
	{
		if (!button || !button->impl)
			return;
		icon_button* target = button->impl;
		target->set_on_click([fn, user]() {
			if (fn)
				fn(user);
		});
	}

	void vgui_editor_icon_button_click(vgui_editor_icon_button* button)
	{
		if (button && button->impl)
			button->impl->click();
	}

	vgui_editor_toggle_button* vgui_editor_toggle_button_create(vgui_widget* parent,
		const char* text)
	{
		if (!parent)
			return nullptr;
		std::unique_ptr<vgui_editor_toggle_button> button(
			new (std::nothrow) vgui_editor_toggle_button());
		if (!button)
			return nullptr;
		button->owned = std::make_unique<toggle_button>();
		button->owned->build(parent, vgeditor::to_text(text));
		button->impl = button->owned.get();
		return button.release();
	}

	void vgui_editor_toggle_button_destroy(vgui_editor_toggle_button* button)
	{
		if (!button || button->borrowed)
			return;
		vgui_widget* widget = button->impl ? button->impl->widget() : nullptr;
		button->impl->set_on_change(nullptr);
		button->owned.reset();
		delete button;
		if (widget)
			vgui_widget_destroy(widget);
	}

	vgui_widget* vgui_editor_toggle_button_widget(vgui_editor_toggle_button* button)
	{
		return button && button->impl ? button->impl->widget() : nullptr;
	}

	void vgui_editor_toggle_button_set_icon(vgui_editor_toggle_button* button,
		vgui_texture* texture, int width, int height)
	{
		if (button && button->impl)
			button->impl->set_icon(texture, width, height);
	}

	void vgui_editor_toggle_button_set_text(vgui_editor_toggle_button* button,
		const char* text)
	{
		if (button && button->impl)
			button->impl->set_text(vgeditor::to_text(text));
	}

	void vgui_editor_toggle_button_set_tooltip(vgui_editor_toggle_button* button,
		const char* text)
	{
		if (button && button->impl)
			button->impl->set_tooltip(vgeditor::to_text(text));
	}

	void vgui_editor_toggle_button_set_enabled(vgui_editor_toggle_button* button,
		int enabled)
	{
		if (button && button->impl)
			button->impl->set_enabled(enabled != 0);
	}

	int vgui_editor_toggle_button_enabled(vgui_editor_toggle_button* button)
	{
		return button && button->impl && button->impl->enabled() ? 1 : 0;
	}

	void vgui_editor_toggle_button_set_checked(vgui_editor_toggle_button* button,
		int checked)
	{
		if (button && button->impl)
			button->impl->set_checked(checked != 0);
	}

	int vgui_editor_toggle_button_checked(vgui_editor_toggle_button* button)
	{
		return button && button->impl && button->impl->checked() ? 1 : 0;
	}

	void vgui_editor_toggle_button_toggle(vgui_editor_toggle_button* button)
	{
		if (button && button->impl)
			button->impl->toggle();
	}

	void vgui_editor_toggle_button_set_on_change(vgui_editor_toggle_button* button,
		vgui_editor_toggle_fn fn, void* user)
	{
		if (!button || !button->impl)
			return;
		button->impl->set_on_change([fn, user](bool checked) {
			if (fn)
				fn(user, checked ? 1 : 0);
		});
	}

	vgui_editor_segmented_control* vgui_editor_segmented_control_create(
		vgui_widget* parent)
	{
		if (!parent)
			return nullptr;
		std::unique_ptr<vgui_editor_segmented_control> seg(
			new (std::nothrow) vgui_editor_segmented_control());
		if (!seg)
			return nullptr;
		seg->owned = std::make_unique<segmented_control>();
		seg->owned->build(parent);
		seg->impl = seg->owned.get();
		return seg.release();
	}

	void vgui_editor_segmented_control_destroy(vgui_editor_segmented_control* seg)
	{
		if (!seg || seg->borrowed)
			return;
		vgui_widget* widget = seg->impl ? seg->impl->widget() : nullptr;
		seg->impl->set_on_change(nullptr);
		seg->owned.reset();
		delete seg;
		if (widget)
			vgui_widget_destroy(widget);
	}

	vgui_widget* vgui_editor_segmented_control_widget(
		vgui_editor_segmented_control* seg)
	{
		return seg && seg->impl ? seg->impl->widget() : nullptr;
	}

	size_t vgui_editor_segmented_control_add_segment(vgui_editor_segmented_control* seg,
		const char* text)
	{
		return seg && seg->impl ? seg->impl->add_segment(vgeditor::to_text(text)) : 0;
	}

	void vgui_editor_segmented_control_clear(vgui_editor_segmented_control* seg)
	{
		if (seg && seg->impl)
			seg->impl->clear();
	}

	size_t vgui_editor_segmented_control_count(vgui_editor_segmented_control* seg)
	{
		return seg && seg->impl ? seg->impl->count() : 0;
	}

	int vgui_editor_segmented_control_select(vgui_editor_segmented_control* seg,
		size_t index)
	{
		return seg && seg->impl && seg->impl->select(index) ? 1 : 0;
	}

	void vgui_editor_segmented_control_set_selected(vgui_editor_segmented_control* seg,
		size_t index)
	{
		if (seg && seg->impl)
			seg->impl->set_selected(index);
	}

	size_t vgui_editor_segmented_control_selected(vgui_editor_segmented_control* seg)
	{
		return seg && seg->impl ? seg->impl->selected() : 0;
	}

	int vgui_editor_segmented_control_has_selection(vgui_editor_segmented_control* seg)
	{
		return seg && seg->impl && seg->impl->has_selection() ? 1 : 0;
	}

	void vgui_editor_segmented_control_set_segment_enabled(
		vgui_editor_segmented_control* seg, size_t index, int enabled)
	{
		if (seg && seg->impl)
			seg->impl->set_segment_enabled(index, enabled != 0);
	}

	int vgui_editor_segmented_control_segment_enabled(
		vgui_editor_segmented_control* seg, size_t index)
	{
		return seg && seg->impl && seg->impl->segment_enabled(index) ? 1 : 0;
	}

	void vgui_editor_segmented_control_set_on_change(
		vgui_editor_segmented_control* seg, vgui_editor_segment_fn fn, void* user)
	{
		if (!seg || !seg->impl)
			return;
		seg->impl->set_on_change([fn, user](std::size_t index) {
			if (fn)
				fn(user, index);
		});
	}

	vgui_widget* vgui_editor_segmented_control_segment_widget(
		vgui_editor_segmented_control* seg, size_t index)
	{
		return seg && seg->impl ? seg->impl->segment_widget(index) : nullptr;
	}

	/* ------------------------------------------------------------------ */
	/* Toolbar                                                            */
	/* ------------------------------------------------------------------ */

	vgui_editor_toolbar* vgui_editor_toolbar_create(vgui_widget* parent)
	{
		if (!parent)
			return nullptr;
		std::unique_ptr<vgui_editor_toolbar> toolbar(
			vgeditor::own_object<vgui_editor_toolbar>(
			new (std::nothrow) toolbar_view()));
		if (!toolbar)
			return nullptr;
		toolbar->impl.build(parent);
		return toolbar.release();
	}

	void vgui_editor_toolbar_destroy(vgui_editor_toolbar* toolbar)
	{
		if (!toolbar || !toolbar->owns_object())
			return;
		vgui_widget* widget = toolbar->impl.widget();
		/* The C++ object owns the buttons; the wrapper handles only alias them,
		 * so releasing the wrappers before the toolbar dies keeps every
		 * borrowed pointer from outliving its owner. */
		toolbar->buttons.clear();
		toolbar->toggles.clear();
		toolbar->segments.clear();
		delete toolbar;
		if (widget)
			vgui_widget_destroy(widget);
	}

	vgui_widget* vgui_editor_toolbar_widget(vgui_editor_toolbar* toolbar)
	{
		return toolbar ? toolbar->impl.widget() : nullptr;
	}

	vgui_editor_icon_button* vgui_editor_toolbar_add_icon_button(
		vgui_editor_toolbar* toolbar, const char* text, vgui_texture* icon,
		int icon_width, int icon_height)
	{
		if (!toolbar)
			return nullptr;
		vgui_editor_icon_button* button = wrap(&toolbar->impl.add_icon_button(
			vgeditor::to_text(text), icon, icon_width, icon_height));
		if (button)
			toolbar->buttons.push_back(std::unique_ptr<vgui_editor_icon_button>(button));
		return button;
	}

	vgui_editor_toggle_button* vgui_editor_toolbar_add_toggle_button(
		vgui_editor_toolbar* toolbar, const char* text, int checked)
	{
		if (!toolbar)
			return nullptr;
		vgui_editor_toggle_button* button = wrap(&toolbar->impl.add_toggle_button(
			vgeditor::to_text(text), checked != 0));
		if (button)
			toolbar->toggles.push_back(
				std::unique_ptr<vgui_editor_toggle_button>(button));
		return button;
	}

	vgui_editor_segmented_control* vgui_editor_toolbar_add_segmented_control(
		vgui_editor_toolbar* toolbar)
	{
		if (!toolbar)
			return nullptr;
		vgui_editor_segmented_control* seg = wrap(&toolbar->impl.add_segmented_control());
		if (seg)
			toolbar->segments.push_back(
				std::unique_ptr<vgui_editor_segmented_control>(seg));
		return seg;
	}

	vgui_editor_icon_button* vgui_editor_toolbar_add_command_button(
		vgui_editor_toolbar* toolbar, vgui_editor_command_registry* registry,
		const char* id, vgui_texture* icon, int icon_width, int icon_height)
	{
		if (!toolbar || !registry)
			return nullptr;
		icon_button* button = toolbar->impl.add_command_button(
			*vgeditor::registry_impl(registry), id, icon, icon_width, icon_height);
		if (!button)
			return nullptr;
		vgui_editor_icon_button* wrapper = wrap(button);
		if (wrapper)
			toolbar->buttons.push_back(std::unique_ptr<vgui_editor_icon_button>(wrapper));
		return wrapper;
	}

	vgui_widget* vgui_editor_toolbar_add_label(vgui_editor_toolbar* toolbar,
		const char* text, int grow)
	{
		return toolbar ? toolbar->impl.add_label(vgeditor::to_text(text), grow != 0) :
			nullptr;
	}

	vgui_widget* vgui_editor_toolbar_add_spacer(vgui_editor_toolbar* toolbar, float grow)
	{
		return toolbar ? toolbar->impl.add_spacer(grow) : nullptr;
	}

	vgui_widget* vgui_editor_toolbar_add_separator(vgui_editor_toolbar* toolbar)
	{
		return toolbar ? toolbar->impl.add_separator() : nullptr;
	}

	void vgui_editor_toolbar_refresh_commands(vgui_editor_toolbar* toolbar)
	{
		if (toolbar)
			toolbar->impl.refresh_commands();
	}

	/* ------------------------------------------------------------------ */
	/* Status bar                                                         */
	/* ------------------------------------------------------------------ */

	vgui_editor_status_bar* vgui_editor_status_bar_create(vgui_widget* parent)
	{
		if (!parent)
			return nullptr;
		std::unique_ptr<vgui_editor_status_bar> status(
			vgeditor::own_object<vgui_editor_status_bar>(
			new (std::nothrow) status_bar()));
		if (!status)
			return nullptr;
		status->impl.build(parent);
		return status.release();
	}

	void vgui_editor_status_bar_destroy(vgui_editor_status_bar* status)
	{
		if (!status || !status->owns_object())
			return;
		vgui_widget* widget = status->impl.widget();
		delete status;
		if (widget)
			vgui_widget_destroy(widget);
	}

	vgui_widget* vgui_editor_status_bar_widget(vgui_editor_status_bar* status)
	{
		return status ? status->impl.widget() : nullptr;
	}

	vgui_widget* vgui_editor_status_bar_message_widget(vgui_editor_status_bar* status)
	{
		return status ? status->impl.message_widget() : nullptr;
	}

	vgui_widget* vgui_editor_status_bar_selection_widget(vgui_editor_status_bar* status)
	{
		return status ? status->impl.selection_widget() : nullptr;
	}

	vgui_widget* vgui_editor_status_bar_coordinate_widget(vgui_editor_status_bar* status)
	{
		return status ? status->impl.coordinate_widget() : nullptr;
	}

	vgui_widget* vgui_editor_status_bar_error_widget(vgui_editor_status_bar* status)
	{
		return status ? status->impl.error_widget() : nullptr;
	}

	void vgui_editor_status_bar_set_message(vgui_editor_status_bar* status,
		const char* text)
	{
		if (status)
			status->impl.set_message(vgeditor::to_text(text));
	}

	const char* vgui_editor_status_bar_message(vgui_editor_status_bar* status)
	{
		return status ? status->impl.message().c_str() : "";
	}

	void vgui_editor_status_bar_set_selected_object(vgui_editor_status_bar* status,
		vgui_editor_object_id id)
	{
		if (status)
			status->impl.set_selected_object(id);
	}

	void vgui_editor_status_bar_set_selection_text(vgui_editor_status_bar* status,
		const char* text)
	{
		if (status)
			status->impl.set_selection_text(vgeditor::to_text(text));
	}

	const char* vgui_editor_status_bar_selection_text(vgui_editor_status_bar* status)
	{
		return status ? status->impl.selection_text().c_str() : "";
	}

	void vgui_editor_status_bar_set_coordinates(vgui_editor_status_bar* status, float x,
		float y)
	{
		if (status)
			status->impl.set_coordinates(x, y);
	}

	void vgui_editor_status_bar_set_coordinate_text(vgui_editor_status_bar* status,
		const char* text)
	{
		if (status)
			status->impl.set_coordinate_text(vgeditor::to_text(text));
	}

	const char* vgui_editor_status_bar_coordinate_text(vgui_editor_status_bar* status)
	{
		return status ? status->impl.coordinate_text().c_str() : "";
	}

	void vgui_editor_status_bar_set_error(vgui_editor_status_bar* status,
		const char* text)
	{
		if (status)
			status->impl.set_error(vgeditor::to_text(text));
	}

	void vgui_editor_status_bar_clear_error(vgui_editor_status_bar* status)
	{
		if (status)
			status->impl.clear_error();
	}

	const char* vgui_editor_status_bar_error(vgui_editor_status_bar* status)
	{
		return status ? status->impl.error().c_str() : "";
	}

} /* extern "C" */

namespace vgeditor {

	vgui_editor_search_box* borrow_search_box(search_box& search)
	{
		return borrow_object<vgui_editor_search_box>(search);
	}

	vgui_editor_breadcrumb* borrow_breadcrumb(breadcrumb& path)
	{
		return borrow_object<vgui_editor_breadcrumb>(path);
	}

	vgui_editor_toolbar* borrow_toolbar(toolbar_view& toolbar)
	{
		return borrow_object<vgui_editor_toolbar>(toolbar);
	}

	vgui_editor_status_bar* borrow_status_bar(status_bar& bar)
	{
		return borrow_object<vgui_editor_status_bar>(bar);
	}

	void release_borrowed_search_box(vgui_editor_search_box* handle)
	{
		/* Wrapper only: a borrowed handle's C++ object belongs to its container. */
		if (handle && !handle->owns_object())
			delete handle;
	}


	void release_borrowed_breadcrumb(vgui_editor_breadcrumb* handle)
	{
		/* Wrapper only: a borrowed handle's C++ object belongs to its container. */
		if (handle && !handle->owns_object())
			delete handle;
	}


	void release_borrowed_toolbar(vgui_editor_toolbar* handle)
	{
		/* Wrapper only: a borrowed handle's C++ object belongs to its container. */
		if (handle && !handle->owns_object())
			delete handle;
	}


	void release_borrowed_status_bar(vgui_editor_status_bar* handle)
	{
		/* Wrapper only: a borrowed handle's C++ object belongs to its container. */
		if (handle && !handle->owns_object())
			delete handle;
	}


} /* namespace vgeditor */
