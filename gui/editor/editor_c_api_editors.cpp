/* gui/editor/editor_c_api_editors.cpp - C facade for the value editors declared
 * in <vgui/vgui_editor.h>: color picker, vector editor, curve editor and
 * gradient editor.
 *
 * Curve and gradient values cross the C boundary either as the canonical text
 * form the kit already uses, or as key/stop arrays whose elements carry an
 * inline color buffer.  Colors travel as hex strings in every case. */
#include "editor_c_api.h"

#include <new>
#include <utility>
#include <vector>

#include <vgui/editor/color_picker.h>
#include <vgui/editor/curve_editor.h>
#include <vgui/editor/gradient_editor.h>
#include <vgui/editor/spinbox.h>
#include <vgui/editor/vector_editor.h>

using namespace vgui_editor_kit;

/* -------------------------------------------------------------------------- */
/* Handle storage                                                            */
/* -------------------------------------------------------------------------- */

struct vgui_editor_color_picker {
	color_picker impl;
	vgui_editor_color_fn press_fn = nullptr;
	void* press_user = nullptr;
	vgui_editor_color_fn change_fn = nullptr;
	void* change_user = nullptr;
	vgui_editor_color_fn commit_fn = nullptr;
	void* commit_user = nullptr;
};

struct vgui_editor_vector_editor {
	vector_editor impl;
	std::string text_scratch;
	std::string id_scratch;
	std::vector<double> emit;
	vgui_editor_vector_fn change_fn = nullptr;
	void* change_user = nullptr;
	vgui_editor_vector_fn commit_fn = nullptr;
	void* commit_user = nullptr;
	vgui_editor_vector_source_fn source_fn = nullptr;
	void* source_user = nullptr;
};

struct vgui_editor_curve_editor {
	curve_editor impl;
	std::string text_scratch;
	std::vector<vgui_editor_curve_key> emit;
	vgui_editor_curve_fn press_fn = nullptr;
	void* press_user = nullptr;
	vgui_editor_curve_fn change_fn = nullptr;
	void* change_user = nullptr;
	vgui_editor_curve_fn commit_fn = nullptr;
	void* commit_user = nullptr;
};

struct vgui_editor_gradient_editor {
	gradient_editor impl;
	std::string text_scratch;
	std::vector<vgui_editor_gradient_stop> emit;
	vgui_editor_gradient_fn press_fn = nullptr;
	void* press_user = nullptr;
	vgui_editor_gradient_fn change_fn = nullptr;
	void* change_user = nullptr;
	vgui_editor_gradient_fn commit_fn = nullptr;
	void* commit_user = nullptr;
};

namespace {

	/* ---- conversions ---------------------------------------------------- */

	/* The C enums intentionally mirror the kit's ordering; the asserts make a
	 * future reorder in either header a build error rather than a silent mix. */
	static_assert((int)VGUI_EDITOR_CURVE_LINEAR ==
		(int)curve_interpolation::linear, "curve interpolation drift");
	static_assert((int)VGUI_EDITOR_CURVE_STEP ==
		(int)curve_interpolation::step, "curve interpolation drift");
	static_assert((int)VGUI_EDITOR_CURVE_CUBIC ==
		(int)curve_interpolation::cubic, "curve interpolation drift");

	vector_semantic to_semantic(vgui_editor_vector_semantic semantic)
	{
		switch (semantic) {
		case VGUI_EDITOR_VECTOR_POSITION:
			return vector_semantic::position;
		case VGUI_EDITOR_VECTOR_COLOR:
			return vector_semantic::color;
		default:
			return vector_semantic::generic;
		}
	}

	vgui_editor_vector_semantic from_semantic(vector_semantic semantic)
	{
		switch (semantic) {
		case vector_semantic::position:
			return VGUI_EDITOR_VECTOR_POSITION;
		case vector_semantic::color:
			return VGUI_EDITOR_VECTOR_COLOR;
		default:
			return VGUI_EDITOR_VECTOR_GENERIC;
		}
	}

	/* Runs a host string callback with a grow-and-retry pass so long values are
	 * never truncated. */
	std::string read_color(vgui_editor_color_transform_fn fn, void* user,
		const char* hex)
	{
		if (!fn)
			return vgeditor::to_text(hex);
		return vgeditor::read_text([fn, user, hex](char* out, size_t cap) {
			return fn(hex, out, cap, user);
		}).value;
	}

	curve_value to_curve_value(vgui_editor_curve_interpolation interpolation,
		const vgui_editor_curve_key* keys, size_t count)
	{
		curve_value out;
		out.interpolation = static_cast<curve_interpolation>(interpolation);
		if (!keys)
			return out;
		out.keys.reserve(count);
		for (size_t i = 0; i < count; ++i) {
			const vgui_editor_curve_key key = vgeditor::take_desc(keys + i);
			curve_key value;
			value.t = key.t;
			value.value = key.value;
			value.in_tangent = key.in_tangent;
			value.out_tangent = key.out_tangent;
			out.keys.push_back(value);
		}
		return out;
	}

	void fill_curve_keys(std::vector<vgui_editor_curve_key>& scratch,
		const std::vector<curve_key>& keys)
	{
		scratch.clear();
		scratch.reserve(keys.size());
		for (const curve_key& key : keys) {
			vgui_editor_curve_key out = {};
			out.struct_size = sizeof(out);
			out.t = key.t;
			out.value = key.value;
			out.in_tangent = key.in_tangent;
			out.out_tangent = key.out_tangent;
			scratch.push_back(out);
		}
	}

	gradient_value to_gradient_value(const vgui_editor_gradient_stop* stops,
		size_t count)
	{
		gradient_value out;
		if (!stops)
			return out;
		out.stops.reserve(count);
		for (size_t i = 0; i < count; ++i) {
			const vgui_editor_gradient_stop stop = vgeditor::take_desc(stops + i);
			gradient_stop value;
			value.t = stop.t;
			value.color = vgeditor::to_text(stop.color);
			if (value.color.empty())
				value.color = "#FFFFFFFF";
			out.stops.push_back(value);
		}
		return out;
	}

	void fill_gradient_stops(std::vector<vgui_editor_gradient_stop>& scratch,
		const std::vector<gradient_stop>& stops)
	{
		scratch.clear();
		scratch.reserve(stops.size());
		for (const gradient_stop& stop : stops) {
			vgui_editor_gradient_stop out = {};
			out.struct_size = sizeof(out);
			out.t = stop.t;
			vgeditor::copy_string(out.color, sizeof(out.color),
				stop.color.c_str());
			scratch.push_back(out);
		}
	}

	void copy_keys(vgui_editor_curve_key* out, size_t cap,
		const std::vector<curve_key>& keys)
	{
		size_t written = 0;
		for (const curve_key& key : keys) {
			if (written >= cap)
				break;
			out[written].struct_size = sizeof(out[written]);
			out[written].t = key.t;
			out[written].value = key.value;
			out[written].in_tangent = key.in_tangent;
			out[written].out_tangent = key.out_tangent;
			++written;
		}
	}

	void copy_stops(vgui_editor_gradient_stop* out, size_t cap,
		const std::vector<gradient_stop>& stops)
	{
		size_t written = 0;
		for (const gradient_stop& stop : stops) {
			if (written >= cap)
				break;
			out[written].struct_size = sizeof(out[written]);
			out[written].t = stop.t;
			vgeditor::copy_string(out[written].color,
				sizeof(out[written].color), stop.color.c_str());
			++written;
		}
	}

	vector_editor::vector_value to_array(const double* values)
	{
		vector_editor::vector_value out{};
		if (!values)
			return out;
		for (int i = 0; i < vector_editor::kMaxComponents; ++i)
			out[(std::size_t)i] = values[i];
		return out;
	}

	void from_array(const vector_editor::vector_value& values, double* out)
	{
		if (!out)
			return;
		for (int i = 0; i < vector_editor::kMaxComponents; ++i)
			out[i] = values[(std::size_t)i];
	}

} /* namespace */

/* -------------------------------------------------------------------------- */
/* Color picker                                                              */
/* -------------------------------------------------------------------------- */

extern "C" {

	vgui_editor_color_picker* vgui_editor_color_picker_create(vgui_context* ui)
	{
		if (!ui)
			return nullptr;
		std::unique_ptr<vgui_editor_color_picker> picker(
			new (std::nothrow) vgui_editor_color_picker());
		if (!picker)
			return nullptr;
		picker->impl.build(ui);
		if (!picker->impl.widget())
			return nullptr;
		return picker.release();
	}

	void vgui_editor_color_picker_destroy(vgui_editor_color_picker* picker)
	{
		if (!picker)
			return;
		vgui_widget* widget = picker->impl.widget();
		picker->impl.set_on_press(nullptr);
		picker->impl.set_on_change(nullptr);
		picker->impl.set_on_commit(nullptr);
		delete picker;
		if (widget)
			vgui_widget_destroy(widget);
	}

	vgui_widget* vgui_editor_color_picker_widget(vgui_editor_color_picker* picker)
	{
		return picker ? picker->impl.widget() : nullptr;
	}

	void vgui_editor_color_picker_set_id(vgui_editor_color_picker* picker,
		const char* id)
	{
		if (!picker)
			return;
		picker->impl.set_id(vgeditor::to_text(id));
	}

	void vgui_editor_color_picker_set_value(vgui_editor_color_picker* picker,
		const char* hex)
	{
		if (!picker)
			return;
		picker->impl.set_value(vgeditor::to_text(hex));
	}

	const char* vgui_editor_color_picker_value(vgui_editor_color_picker* picker)
	{
		return picker ? picker->impl.value().c_str() : "";
	}

	void vgui_editor_color_picker_set_current(vgui_editor_color_picker* picker,
		const char* hex)
	{
		if (!picker)
			return;
		picker->impl.set_current(vgeditor::to_text(hex));
	}

	void vgui_editor_color_picker_set_on_press(vgui_editor_color_picker* picker,
		vgui_editor_color_fn fn, void* user)
	{
		if (!picker)
			return;
		picker->press_fn = fn;
		picker->press_user = user;
		picker->impl.set_on_press([picker](const std::string& hex) {
			if (picker->press_fn)
				picker->press_fn(picker->press_user, hex.c_str());
		});
	}

	void vgui_editor_color_picker_set_on_change(vgui_editor_color_picker* picker,
		vgui_editor_color_fn fn, void* user)
	{
		if (!picker)
			return;
		picker->change_fn = fn;
		picker->change_user = user;
		picker->impl.set_on_change([picker](const std::string& hex) {
			if (picker->change_fn)
				picker->change_fn(picker->change_user, hex.c_str());
		});
	}

	void vgui_editor_color_picker_set_on_commit(vgui_editor_color_picker* picker,
		vgui_editor_color_fn fn, void* user)
	{
		if (!picker)
			return;
		picker->commit_fn = fn;
		picker->commit_user = user;
		picker->impl.set_on_commit([picker](const std::string& hex) {
			if (picker->commit_fn)
				picker->commit_fn(picker->commit_user, hex.c_str());
		});
	}

	void vgui_editor_color_picker_set_presets(vgui_editor_color_picker* picker,
		const char* const* hexes, size_t count)
	{
		if (!picker)
			return;
		picker->impl.set_presets(vgeditor::to_text_list(hexes, count));
	}

	void vgui_editor_color_picker_set_recent(vgui_editor_color_picker* picker,
		const char* const* hexes, size_t count)
	{
		if (!picker)
			return;
		picker->impl.set_recent(vgeditor::to_text_list(hexes, count));
	}

	void vgui_editor_color_picker_set_color_transform(
		vgui_editor_color_picker* picker, vgui_editor_color_transform_fn to_edit,
		void* to_edit_user, vgui_editor_color_transform_fn from_edit,
		void* from_edit_user)
	{
		if (!picker)
			return;
		picker->impl.set_color_transform(
			[to_edit, to_edit_user](const std::string& hex) {
				return read_color(to_edit, to_edit_user, hex.c_str());
			},
			[from_edit, from_edit_user](const std::string& hex) {
				return read_color(from_edit, from_edit_user, hex.c_str());
			});
	}

	int vgui_editor_color_picker_valid(vgui_editor_color_picker* picker)
	{
		return picker && picker->impl.valid() ? 1 : 0;
	}

	float vgui_editor_color_picker_hue_degrees(vgui_editor_color_picker* picker)
	{
		return picker ? picker->impl.hue_degrees() : 0.0f;
	}

	/* ---------------------------------------------------------------------- */
	/* Vector editor                                                          */
	/* ---------------------------------------------------------------------- */

	vgui_editor_vector_editor* vgui_editor_vector_editor_create(vgui_widget* parent,
		int dimension)
	{
		if (!parent)
			return nullptr;
		std::unique_ptr<vgui_editor_vector_editor> editor(
			new (std::nothrow) vgui_editor_vector_editor());
		if (!editor)
			return nullptr;
		editor->impl.build(parent, dimension);
		if (!editor->impl.widget())
			return nullptr;
		editor->emit.assign(vector_editor::kMaxComponents, 0.0);
		return editor.release();
	}

	void vgui_editor_vector_editor_destroy(vgui_editor_vector_editor* editor)
	{
		if (!editor)
			return;
		vgui_widget* widget = editor->impl.widget();
		editor->impl.set_on_change(nullptr);
		editor->impl.set_on_commit(nullptr);
		editor->impl.set_value_source(nullptr);
		delete editor;
		if (widget)
			vgui_widget_destroy(widget);
	}

	vgui_widget* vgui_editor_vector_editor_widget(vgui_editor_vector_editor* editor)
	{
		return editor ? editor->impl.widget() : nullptr;
	}

	vgui_widget* vgui_editor_vector_editor_component_widget(
		vgui_editor_vector_editor* editor, int index)
	{
		return editor ? editor->impl.component_widget(index) : nullptr;
	}

	vgui_widget* vgui_editor_vector_editor_component_edit_widget(
		vgui_editor_vector_editor* editor, int index)
	{
		return editor ? editor->impl.component_edit_widget(index) : nullptr;
	}

	vgui_widget* vgui_editor_vector_editor_component_label_widget(
		vgui_editor_vector_editor* editor, int index)
	{
		return editor ? editor->impl.component_label_widget(index) : nullptr;
	}

	void vgui_editor_vector_editor_set_id(vgui_editor_vector_editor* editor,
		const char* id)
	{
		if (!editor)
			return;
		editor->impl.set_id(vgeditor::to_text(id));
	}

	const char* vgui_editor_vector_editor_id(vgui_editor_vector_editor* editor)
	{
		return editor ? editor->impl.id().c_str() : "";
	}

	int vgui_editor_vector_editor_set_dimension(vgui_editor_vector_editor* editor,
		int dimension)
	{
		return editor && editor->impl.set_dimension(dimension) ? 1 : 0;
	}

	int vgui_editor_vector_editor_dimension(vgui_editor_vector_editor* editor)
	{
		return editor ? editor->impl.dimension() : 0;
	}

	void vgui_editor_vector_editor_set_semantic(vgui_editor_vector_editor* editor,
		vgui_editor_vector_semantic semantic)
	{
		if (!editor)
			return;
		editor->impl.set_semantic(to_semantic(semantic));
	}

	vgui_editor_vector_semantic vgui_editor_vector_editor_semantic(
		vgui_editor_vector_editor* editor)
	{
		if (!editor)
			return VGUI_EDITOR_VECTOR_GENERIC;
		return from_semantic(editor->impl.semantic());
	}

	void vgui_editor_vector_editor_set_axis_labels(
		vgui_editor_vector_editor* editor, const char* const* labels, int count)
	{
		if (!editor || count < 0)
			return;
		editor->impl.set_axis_labels(vgeditor::to_text_list(labels,
			(size_t)count));
	}

	void vgui_editor_vector_editor_set_axis_label(vgui_editor_vector_editor* editor,
		int index, const char* label)
	{
		if (!editor)
			return;
		editor->impl.set_axis_label(index, vgeditor::to_text(label));
	}

	const char* vgui_editor_vector_editor_axis_label(
		vgui_editor_vector_editor* editor, int index)
	{
		if (!editor || index < 0)
			return "";
		const std::vector<std::string>& labels = editor->impl.axis_labels();
		return (size_t)index < labels.size() ? labels[(size_t)index].c_str() : "";
	}

	void vgui_editor_vector_editor_set_axis_colors(
		vgui_editor_vector_editor* editor, const vgui_color* colors, int count)
	{
		if (!editor || count < 0)
			return;
		std::vector<vgui_color> values;
		if (colors)
			values.assign(colors, colors + count);
		editor->impl.set_axis_colors(std::move(values));
	}

	void vgui_editor_vector_editor_set_axis_color(vgui_editor_vector_editor* editor,
		int index, vgui_color color)
	{
		if (!editor)
			return;
		editor->impl.set_axis_color(index, color);
	}

	vgui_color vgui_editor_vector_editor_axis_color(
		vgui_editor_vector_editor* editor, int index)
	{
		if (!editor || index < 0)
			return 0;
		const std::vector<vgui_color>& colors = editor->impl.axis_colors();
		return (size_t)index < colors.size() ? colors[(size_t)index] : 0;
	}

	void vgui_editor_vector_editor_set_value_type(
		vgui_editor_vector_editor* editor, vgui_editor_value_type type)
	{
		if (!editor)
			return;
		editor->impl.set_value_type(type == VGUI_EDITOR_VALUE_INTEGER ?
			spinbox::value_type::integer : spinbox::value_type::number);
	}

	vgui_editor_value_type vgui_editor_vector_editor_value_type(
		vgui_editor_vector_editor* editor)
	{
		if (!editor)
			return VGUI_EDITOR_VALUE_NUMBER;
		return editor->impl.value_type() == spinbox::value_type::integer ?
			VGUI_EDITOR_VALUE_INTEGER : VGUI_EDITOR_VALUE_NUMBER;
	}

	void vgui_editor_vector_editor_set_range(vgui_editor_vector_editor* editor,
		double min_value, double max_value)
	{
		if (!editor)
			return;
		editor->impl.set_range(min_value, max_value);
	}

	void vgui_editor_vector_editor_clear_range(vgui_editor_vector_editor* editor)
	{
		if (!editor)
			return;
		editor->impl.clear_range();
	}

	int vgui_editor_vector_editor_has_range(vgui_editor_vector_editor* editor)
	{
		return editor && editor->impl.has_range() ? 1 : 0;
	}

	double vgui_editor_vector_editor_min_value(vgui_editor_vector_editor* editor)
	{
		return editor ? editor->impl.min_value() : 0.0;
	}

	double vgui_editor_vector_editor_max_value(vgui_editor_vector_editor* editor)
	{
		return editor ? editor->impl.max_value() : 0.0;
	}

	void vgui_editor_vector_editor_set_step(vgui_editor_vector_editor* editor,
		double step)
	{
		if (!editor)
			return;
		editor->impl.set_step(step);
	}

	double vgui_editor_vector_editor_step(vgui_editor_vector_editor* editor)
	{
		return editor ? editor->impl.step() : 0.0;
	}

	void vgui_editor_vector_editor_set_value(vgui_editor_vector_editor* editor,
		const double* values)
	{
		if (!editor)
			return;
		editor->impl.set_value(to_array(values));
	}

	void vgui_editor_vector_editor_set_component_value(
		vgui_editor_vector_editor* editor, int index, double value)
	{
		if (!editor)
			return;
		editor->impl.set_component_value(index, value);
	}

	void vgui_editor_vector_editor_value(vgui_editor_vector_editor* editor,
		double* values)
	{
		if (!editor)
			return;
		from_array(editor->impl.value(), values);
	}

	const char* vgui_editor_vector_editor_value_text(
		vgui_editor_vector_editor* editor)
	{
		if (!editor)
			return "";
		editor->text_scratch = editor->impl.value_text();
		return editor->text_scratch.c_str();
	}

	int vgui_editor_vector_editor_commit_component_text(
		vgui_editor_vector_editor* editor, int index, const char* text)
	{
		if (!editor)
			return 0;
		return editor->impl.commit_component_text(index,
			vgeditor::to_text(text)) ? 1 : 0;
	}

	int vgui_editor_vector_editor_commit_component_value(
		vgui_editor_vector_editor* editor, int index, double value)
	{
		if (!editor)
			return 0;
		return editor->impl.commit_component_value(index, value) ? 1 : 0;
	}

	void vgui_editor_vector_editor_set_read_only(vgui_editor_vector_editor* editor,
		int read_only)
	{
		if (!editor)
			return;
		editor->impl.set_read_only(read_only != 0);
	}

	int vgui_editor_vector_editor_read_only(vgui_editor_vector_editor* editor)
	{
		return editor && editor->impl.read_only() ? 1 : 0;
	}

	void vgui_editor_vector_editor_set_enabled(vgui_editor_vector_editor* editor,
		int enabled)
	{
		if (!editor)
			return;
		editor->impl.set_enabled(enabled != 0);
	}

	int vgui_editor_vector_editor_enabled(vgui_editor_vector_editor* editor)
	{
		return editor && editor->impl.enabled() ? 1 : 0;
	}

	void vgui_editor_vector_editor_set_component_error(
		vgui_editor_vector_editor* editor, int index, const char* error)
	{
		if (!editor)
			return;
		editor->impl.set_component_error(index, vgeditor::to_text(error));
	}

	void vgui_editor_vector_editor_clear_component_errors(
		vgui_editor_vector_editor* editor)
	{
		if (!editor)
			return;
		editor->impl.clear_component_errors();
	}

	const char* vgui_editor_vector_editor_component_error(
		vgui_editor_vector_editor* editor, int index)
	{
		if (!editor)
			return "";
		return editor->impl.component_error(index).c_str();
	}

	void vgui_editor_vector_editor_focus_component(vgui_editor_vector_editor* editor,
		int index)
	{
		if (!editor)
			return;
		editor->impl.focus_component(index);
	}

	void vgui_editor_vector_editor_set_on_change(vgui_editor_vector_editor* editor,
		vgui_editor_vector_fn fn, void* user)
	{
		if (!editor)
			return;
		editor->change_fn = fn;
		editor->change_user = user;
		editor->impl.set_on_change([editor](const vector_editor::vector_value& v) {
			if (!editor->change_fn)
				return;
			from_array(v, editor->emit.data());
			editor->change_fn(editor->change_user, editor->emit.data(),
				editor->impl.dimension());
		});
	}

	void vgui_editor_vector_editor_set_on_commit(vgui_editor_vector_editor* editor,
		vgui_editor_vector_fn fn, void* user)
	{
		if (!editor)
			return;
		editor->commit_fn = fn;
		editor->commit_user = user;
		editor->impl.set_on_commit([editor](const vector_editor::vector_value& v) {
			if (!editor->commit_fn)
				return;
			from_array(v, editor->emit.data());
			editor->commit_fn(editor->commit_user, editor->emit.data(),
				editor->impl.dimension());
		});
	}

	void vgui_editor_vector_editor_set_value_source(
		vgui_editor_vector_editor* editor, vgui_editor_vector_source_fn fn,
		void* user)
	{
		if (!editor)
			return;
		editor->source_fn = fn;
		editor->source_user = user;
		editor->impl.set_value_source([editor](vector_editor::vector_value& out) {
			if (!editor->source_fn)
				return false;
			double values[vector_editor::kMaxComponents] = { 0.0, 0.0, 0.0, 0.0 };
			if (!editor->source_fn(editor->source_user, values,
					editor->impl.dimension()))
				return false;
			for (int i = 0; i < vector_editor::kMaxComponents; ++i)
				out[(std::size_t)i] = values[i];
			return true;
		});
	}

	void vgui_editor_vector_editor_refresh(vgui_editor_vector_editor* editor)
	{
		if (!editor)
			return;
		editor->impl.refresh();
	}

	int vgui_editor_vector_parse(const char* text, int dimension, double* values)
	{
		vector_editor::vector_value out{};
		std::string error;
		if (!vector_editor::parse_value(vgeditor::to_text(text), dimension, out,
				&error)) {
			vgeditor::set_error(error);
			return 0;
		}
		vgeditor::set_error(std::string());
		from_array(out, values);
		return 1;
	}

	size_t vgui_editor_vector_format(const double* values, int dimension,
		char* out, size_t cap)
	{
		const std::string text = vector_editor::format_value(to_array(values),
			dimension);
		return vgeditor::copy_string(out, cap, text.c_str());
	}

	const char* vgui_editor_vector_default_axis_label(int index,
		vgui_editor_vector_semantic semantic)
	{
		return vector_editor::default_axis_label(index, to_semantic(semantic));
	}

	vgui_color vgui_editor_vector_default_axis_color(int index,
		vgui_editor_vector_semantic semantic)
	{
		return vector_editor::default_axis_color(index, to_semantic(semantic));
	}

	/* ---------------------------------------------------------------------- */
	/* Curve editor                                                           */
	/* ---------------------------------------------------------------------- */

	vgui_editor_curve_editor* vgui_editor_curve_editor_create(vgui_widget* parent)
	{
		if (!parent)
			return nullptr;
		std::unique_ptr<vgui_editor_curve_editor> editor(
			new (std::nothrow) vgui_editor_curve_editor());
		if (!editor)
			return nullptr;
		editor->impl.build(parent);
		if (!editor->impl.widget())
			return nullptr;
		return editor.release();
	}

	vgui_editor_curve_editor* vgui_editor_curve_editor_create_popup(vgui_context* ui)
	{
		if (!ui)
			return nullptr;
		std::unique_ptr<vgui_editor_curve_editor> editor(
			new (std::nothrow) vgui_editor_curve_editor());
		if (!editor)
			return nullptr;
		editor->impl.build(ui);
		if (!editor->impl.widget())
			return nullptr;
		return editor.release();
	}

	void vgui_editor_curve_editor_destroy(vgui_editor_curve_editor* editor)
	{
		if (!editor)
			return;
		vgui_widget* widget = editor->impl.widget();
		editor->impl.set_on_press(nullptr);
		editor->impl.set_on_change(nullptr);
		editor->impl.set_on_commit(nullptr);
		delete editor;
		if (widget)
			vgui_widget_destroy(widget);
	}

	vgui_widget* vgui_editor_curve_editor_widget(vgui_editor_curve_editor* editor)
	{
		return editor ? editor->impl.widget() : nullptr;
	}

	vgui_widget* vgui_editor_curve_editor_graph_widget(
		vgui_editor_curve_editor* editor)
	{
		return editor ? editor->impl.graph_widget() : nullptr;
	}

	vgui_widget* vgui_editor_curve_editor_mode_widget(
		vgui_editor_curve_editor* editor, vgui_editor_curve_interpolation mode)
	{
		return editor ? editor->impl.mode_widget(
			static_cast<curve_interpolation>(mode)) : nullptr;
	}

	vgui_widget* vgui_editor_curve_editor_add_widget(
		vgui_editor_curve_editor* editor)
	{
		return editor ? editor->impl.add_widget() : nullptr;
	}

	vgui_widget* vgui_editor_curve_editor_remove_widget(
		vgui_editor_curve_editor* editor)
	{
		return editor ? editor->impl.remove_widget() : nullptr;
	}

	vgui_widget* vgui_editor_curve_editor_time_widget(
		vgui_editor_curve_editor* editor)
	{
		return editor ? editor->impl.time_widget() : nullptr;
	}

	vgui_widget* vgui_editor_curve_editor_value_widget(
		vgui_editor_curve_editor* editor)
	{
		return editor ? editor->impl.value_widget() : nullptr;
	}

	vgui_widget* vgui_editor_curve_editor_in_tangent_widget(
		vgui_editor_curve_editor* editor)
	{
		return editor ? editor->impl.in_tangent_widget() : nullptr;
	}

	vgui_widget* vgui_editor_curve_editor_out_tangent_widget(
		vgui_editor_curve_editor* editor)
	{
		return editor ? editor->impl.out_tangent_widget() : nullptr;
	}

	void vgui_editor_curve_editor_set_id(vgui_editor_curve_editor* editor,
		const char* id)
	{
		if (!editor)
			return;
		editor->impl.set_id(vgeditor::to_text(id));
	}

	const char* vgui_editor_curve_editor_id(vgui_editor_curve_editor* editor)
	{
		return editor ? editor->impl.id().c_str() : "";
	}

	int vgui_editor_curve_editor_set_value(vgui_editor_curve_editor* editor,
		vgui_editor_curve_interpolation interpolation,
		const vgui_editor_curve_key* keys, size_t key_count)
	{
		if (!editor)
			return 0;
		return editor->impl.set_value(to_curve_value(interpolation, keys,
			key_count)) ? 1 : 0;
	}

	int vgui_editor_curve_editor_set_value_text(vgui_editor_curve_editor* editor,
		const char* text)
	{
		if (!editor)
			return 0;
		std::string error;
		if (!editor->impl.set_value_text(vgeditor::to_text(text), &error)) {
			vgeditor::set_error(error);
			return 0;
		}
		vgeditor::set_error(std::string());
		return 1;
	}

	const char* vgui_editor_curve_editor_value_text(
		vgui_editor_curve_editor* editor)
	{
		if (!editor)
			return "";
		editor->text_scratch = editor->impl.value_text();
		return editor->text_scratch.c_str();
	}

	size_t vgui_editor_curve_editor_keys(vgui_editor_curve_editor* editor,
		vgui_editor_curve_key* out, size_t cap)
	{
		if (!editor)
			return 0;
		const curve_value& value = editor->impl.value();
		copy_keys(out, cap, value.keys);
		return value.keys.size();
	}

	size_t vgui_editor_curve_editor_key_count(vgui_editor_curve_editor* editor)
	{
		return editor ? editor->impl.value().keys.size() : 0;
	}

	void vgui_editor_curve_editor_set_read_only(vgui_editor_curve_editor* editor,
		int read_only)
	{
		if (!editor)
			return;
		editor->impl.set_read_only(read_only != 0);
	}

	int vgui_editor_curve_editor_read_only(vgui_editor_curve_editor* editor)
	{
		return editor && editor->impl.read_only() ? 1 : 0;
	}

	void vgui_editor_curve_editor_set_enabled(vgui_editor_curve_editor* editor,
		int enabled)
	{
		if (!editor)
			return;
		editor->impl.set_enabled(enabled != 0);
	}

	int vgui_editor_curve_editor_enabled(vgui_editor_curve_editor* editor)
	{
		return editor && editor->impl.enabled() ? 1 : 0;
	}

	int vgui_editor_curve_editor_select_key(vgui_editor_curve_editor* editor,
		int index)
	{
		if (!editor)
			return 0;
		return editor->impl.select_key(index) ? 1 : 0;
	}

	int vgui_editor_curve_editor_selected_key(vgui_editor_curve_editor* editor)
	{
		return editor ? editor->impl.selected_key() : -1;
	}

	int vgui_editor_curve_editor_add_key(vgui_editor_curve_editor* editor, float t,
		float value)
	{
		if (!editor)
			return 0;
		return editor->impl.add_key(t, value) ? 1 : 0;
	}

	int vgui_editor_curve_editor_remove_selected_key(vgui_editor_curve_editor* editor)
	{
		if (!editor)
			return 0;
		return editor->impl.remove_selected_key() ? 1 : 0;
	}

	int vgui_editor_curve_editor_commit_interpolation(
		vgui_editor_curve_editor* editor, vgui_editor_curve_interpolation mode)
	{
		if (!editor)
			return 0;
		return editor->impl.commit_interpolation(
			static_cast<curve_interpolation>(mode)) ? 1 : 0;
	}

	vgui_editor_curve_interpolation vgui_editor_curve_editor_interpolation(
		vgui_editor_curve_editor* editor)
	{
		if (!editor)
			return VGUI_EDITOR_CURVE_LINEAR;
		return static_cast<vgui_editor_curve_interpolation>(
			editor->impl.interpolation());
	}

	void vgui_editor_curve_editor_set_on_press(vgui_editor_curve_editor* editor,
		vgui_editor_curve_fn fn, void* user)
	{
		if (!editor)
			return;
		editor->press_fn = fn;
		editor->press_user = user;
		editor->impl.set_on_press([editor](const curve_value& value) {
			if (!editor->press_fn)
				return;
			fill_curve_keys(editor->emit, value.keys);
			editor->press_fn(editor->press_user,
				static_cast<vgui_editor_curve_interpolation>(value.interpolation),
				editor->emit.data(), editor->emit.size());
		});
	}

	void vgui_editor_curve_editor_set_on_change(vgui_editor_curve_editor* editor,
		vgui_editor_curve_fn fn, void* user)
	{
		if (!editor)
			return;
		editor->change_fn = fn;
		editor->change_user = user;
		editor->impl.set_on_change([editor](const curve_value& value) {
			if (!editor->change_fn)
				return;
			fill_curve_keys(editor->emit, value.keys);
			editor->change_fn(editor->change_user,
				static_cast<vgui_editor_curve_interpolation>(value.interpolation),
				editor->emit.data(), editor->emit.size());
		});
	}

	void vgui_editor_curve_editor_set_on_commit(vgui_editor_curve_editor* editor,
		vgui_editor_curve_fn fn, void* user)
	{
		if (!editor)
			return;
		editor->commit_fn = fn;
		editor->commit_user = user;
		editor->impl.set_on_commit([editor](const curve_value& value) {
			if (!editor->commit_fn)
				return;
			fill_curve_keys(editor->emit, value.keys);
			editor->commit_fn(editor->commit_user,
				static_cast<vgui_editor_curve_interpolation>(value.interpolation),
				editor->emit.data(), editor->emit.size());
		});
	}

	int vgui_editor_curve_parse(const char* text,
		vgui_editor_curve_interpolation* interpolation,
		vgui_editor_curve_key* keys, size_t key_cap, size_t* key_count)
	{
		curve_value value;
		std::string error;
		if (!curve_editor::parse_value(vgeditor::to_text(text), value, &error)) {
			vgeditor::set_error(error);
			return 0;
		}
		if (keys && interpolation)
			*interpolation = static_cast<vgui_editor_curve_interpolation>(
				value.interpolation);
		if (key_count)
			*key_count = value.keys.size();
		if (value.keys.size() > key_cap) {
			vgeditor::set_error("curve: key buffer too small");
			return 0;
		}
		vgeditor::set_error(std::string());
		copy_keys(keys, key_cap, value.keys);
		return 1;
	}

	size_t vgui_editor_curve_format(vgui_editor_curve_interpolation interpolation,
		const vgui_editor_curve_key* keys, size_t key_count, char* out, size_t cap)
	{
		const std::string text = curve_editor::format_value(
			to_curve_value(interpolation, keys, key_count));
		return vgeditor::copy_string(out, cap, text.c_str());
	}

	const char* vgui_editor_curve_interpolation_name(
		vgui_editor_curve_interpolation mode)
	{
		return curve_editor::interpolation_name(
			static_cast<curve_interpolation>(mode));
	}

	const char* vgui_editor_curve_interpolation_label(
		vgui_editor_curve_interpolation mode)
	{
		return curve_editor::interpolation_label(
			static_cast<curve_interpolation>(mode));
	}

	/* ---------------------------------------------------------------------- */
	/* Gradient editor                                                        */
	/* ---------------------------------------------------------------------- */

	size_t vgui_editor_gradient_preset_count(void)
	{
		gradient_editor probe;
		return probe.preset_count();
	}

	const char* vgui_editor_gradient_preset_name(size_t index)
	{
		gradient_editor probe;
		return probe.preset_name(index);
	}

	vgui_editor_gradient_editor* vgui_editor_gradient_editor_create(
		vgui_widget* parent)
	{
		if (!parent)
			return nullptr;
		std::unique_ptr<vgui_editor_gradient_editor> editor(
			new (std::nothrow) vgui_editor_gradient_editor());
		if (!editor)
			return nullptr;
		editor->impl.build(parent);
		if (!editor->impl.widget())
			return nullptr;
		return editor.release();
	}

	vgui_editor_gradient_editor* vgui_editor_gradient_editor_create_popup(
		vgui_context* ui)
	{
		if (!ui)
			return nullptr;
		std::unique_ptr<vgui_editor_gradient_editor> editor(
			new (std::nothrow) vgui_editor_gradient_editor());
		if (!editor)
			return nullptr;
		editor->impl.build(ui);
		if (!editor->impl.widget())
			return nullptr;
		return editor.release();
	}

	void vgui_editor_gradient_editor_destroy(vgui_editor_gradient_editor* editor)
	{
		if (!editor)
			return;
		vgui_widget* widget = editor->impl.widget();
		editor->impl.set_on_press(nullptr);
		editor->impl.set_on_change(nullptr);
		editor->impl.set_on_commit(nullptr);
		delete editor;
		if (widget)
			vgui_widget_destroy(widget);
	}

	vgui_widget* vgui_editor_gradient_editor_widget(
		vgui_editor_gradient_editor* editor)
	{
		return editor ? editor->impl.widget() : nullptr;
	}

	vgui_widget* vgui_editor_gradient_editor_bar_widget(
		vgui_editor_gradient_editor* editor)
	{
		return editor ? editor->impl.bar_widget() : nullptr;
	}

	vgui_widget* vgui_editor_gradient_editor_add_widget(
		vgui_editor_gradient_editor* editor)
	{
		return editor ? editor->impl.add_widget() : nullptr;
	}

	vgui_widget* vgui_editor_gradient_editor_remove_widget(
		vgui_editor_gradient_editor* editor)
	{
		return editor ? editor->impl.remove_widget() : nullptr;
	}

	vgui_widget* vgui_editor_gradient_editor_time_widget(
		vgui_editor_gradient_editor* editor)
	{
		return editor ? editor->impl.time_widget() : nullptr;
	}

	vgui_widget* vgui_editor_gradient_editor_preset_widget(
		vgui_editor_gradient_editor* editor, size_t index)
	{
		return editor ? editor->impl.preset_widget(index) : nullptr;
	}

	vgui_widget* vgui_editor_gradient_editor_picker_widget(
		vgui_editor_gradient_editor* editor)
	{
		if (!editor)
			return nullptr;
		color_picker* picker = editor->impl.picker();
		return picker ? picker->widget() : nullptr;
	}

	void vgui_editor_gradient_editor_set_id(vgui_editor_gradient_editor* editor,
		const char* id)
	{
		if (!editor)
			return;
		editor->impl.set_id(vgeditor::to_text(id));
	}

	const char* vgui_editor_gradient_editor_id(vgui_editor_gradient_editor* editor)
	{
		return editor ? editor->impl.id().c_str() : "";
	}

	int vgui_editor_gradient_editor_set_value(vgui_editor_gradient_editor* editor,
		const vgui_editor_gradient_stop* stops, size_t stop_count)
	{
		if (!editor)
			return 0;
		return editor->impl.set_value(to_gradient_value(stops, stop_count)) ?
			1 : 0;
	}

	int vgui_editor_gradient_editor_set_value_text(
		vgui_editor_gradient_editor* editor, const char* text)
	{
		if (!editor)
			return 0;
		std::string error;
		if (!editor->impl.set_value_text(vgeditor::to_text(text), &error)) {
			vgeditor::set_error(error);
			return 0;
		}
		vgeditor::set_error(std::string());
		return 1;
	}

	const char* vgui_editor_gradient_editor_value_text(
		vgui_editor_gradient_editor* editor)
	{
		if (!editor)
			return "";
		editor->text_scratch = editor->impl.value_text();
		return editor->text_scratch.c_str();
	}

	size_t vgui_editor_gradient_editor_stops(vgui_editor_gradient_editor* editor,
		vgui_editor_gradient_stop* out, size_t cap)
	{
		if (!editor)
			return 0;
		const std::vector<gradient_stop>& stops = editor->impl.value().stops;
		copy_stops(out, cap, stops);
		return stops.size();
	}

	size_t vgui_editor_gradient_editor_stop_count(vgui_editor_gradient_editor* editor)
	{
		return editor ? editor->impl.value().stops.size() : 0;
	}

	void vgui_editor_gradient_editor_set_read_only(
		vgui_editor_gradient_editor* editor, int read_only)
	{
		if (!editor)
			return;
		editor->impl.set_read_only(read_only != 0);
	}

	int vgui_editor_gradient_editor_read_only(vgui_editor_gradient_editor* editor)
	{
		return editor && editor->impl.read_only() ? 1 : 0;
	}

	void vgui_editor_gradient_editor_set_enabled(vgui_editor_gradient_editor* editor,
		int enabled)
	{
		if (!editor)
			return;
		editor->impl.set_enabled(enabled != 0);
	}

	int vgui_editor_gradient_editor_enabled(vgui_editor_gradient_editor* editor)
	{
		return editor && editor->impl.enabled() ? 1 : 0;
	}

	int vgui_editor_gradient_editor_select_stop(vgui_editor_gradient_editor* editor,
		int index)
	{
		if (!editor)
			return 0;
		return editor->impl.select_stop(index) ? 1 : 0;
	}

	int vgui_editor_gradient_editor_selected_stop(
		vgui_editor_gradient_editor* editor)
	{
		return editor ? editor->impl.selected_stop() : -1;
	}

	int vgui_editor_gradient_editor_add_stop(vgui_editor_gradient_editor* editor,
		float t)
	{
		if (!editor)
			return 0;
		return editor->impl.add_stop(t) ? 1 : 0;
	}

	int vgui_editor_gradient_editor_remove_selected_stop(
		vgui_editor_gradient_editor* editor)
	{
		if (!editor)
			return 0;
		return editor->impl.remove_selected_stop() ? 1 : 0;
	}

	int vgui_editor_gradient_editor_set_selected_color(
		vgui_editor_gradient_editor* editor, const char* hex)
	{
		if (!editor)
			return 0;
		return editor->impl.set_selected_color(vgeditor::to_text(hex)) ? 1 : 0;
	}

	int vgui_editor_gradient_editor_commit_preset(
		vgui_editor_gradient_editor* editor, size_t index)
	{
		if (!editor)
			return 0;
		return editor->impl.commit_preset(index) ? 1 : 0;
	}

	void vgui_editor_gradient_editor_set_on_press(
		vgui_editor_gradient_editor* editor, vgui_editor_gradient_fn fn, void* user)
	{
		if (!editor)
			return;
		editor->press_fn = fn;
		editor->press_user = user;
		editor->impl.set_on_press([editor](const gradient_value& value) {
			if (!editor->press_fn)
				return;
			fill_gradient_stops(editor->emit, value.stops);
			editor->press_fn(editor->press_user, editor->emit.data(),
				editor->emit.size());
		});
	}

	void vgui_editor_gradient_editor_set_on_change(
		vgui_editor_gradient_editor* editor, vgui_editor_gradient_fn fn, void* user)
	{
		if (!editor)
			return;
		editor->change_fn = fn;
		editor->change_user = user;
		editor->impl.set_on_change([editor](const gradient_value& value) {
			if (!editor->change_fn)
				return;
			fill_gradient_stops(editor->emit, value.stops);
			editor->change_fn(editor->change_user, editor->emit.data(),
				editor->emit.size());
		});
	}

	void vgui_editor_gradient_editor_set_on_commit(
		vgui_editor_gradient_editor* editor, vgui_editor_gradient_fn fn, void* user)
	{
		if (!editor)
			return;
		editor->commit_fn = fn;
		editor->commit_user = user;
		editor->impl.set_on_commit([editor](const gradient_value& value) {
			if (!editor->commit_fn)
				return;
			fill_gradient_stops(editor->emit, value.stops);
			editor->commit_fn(editor->commit_user, editor->emit.data(),
				editor->emit.size());
		});
	}

	int vgui_editor_gradient_parse(const char* text,
		vgui_editor_gradient_stop* stops, size_t stop_cap, size_t* stop_count)
	{
		gradient_value value;
		std::string error;
		if (!gradient_editor::parse_value(vgeditor::to_text(text), value, &error)) {
			vgeditor::set_error(error);
			return 0;
		}
		if (stop_count)
			*stop_count = value.stops.size();
		if (value.stops.size() > stop_cap) {
			vgeditor::set_error("gradient: stop buffer too small");
			return 0;
		}
		vgeditor::set_error(std::string());
		copy_stops(stops, stop_cap, value.stops);
		return 1;
	}

	size_t vgui_editor_gradient_format(const vgui_editor_gradient_stop* stops,
		size_t stop_count, char* out, size_t cap)
	{
		const std::string text = gradient_editor::format_value(
			to_gradient_value(stops, stop_count));
		return vgeditor::copy_string(out, cap, text.c_str());
	}

} /* extern "C" */
