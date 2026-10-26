#pragma once
/*
 * curve_editor.h - keyframed curve editor for vgui_editor_kit.
 *
 * The editor shows a normalized curve over t=0..1: keyframes are draggable
 * points, each segment uses linear, step or cubic interpolation, and cubic
 * segments expose in/out tangent handles.  Double-clicking the graph adds a
 * key; the Add/Delete buttons and the numeric fields below the graph provide
 * keyboard-free alternatives.  Time, value and tangent fields reuse spinbox so
 * exact values can be typed or scrubbed.
 *
 * Values are plain strings so a PropertyGrid row can own them without the grid
 * knowing the curve layout:
 *
 *   cubic;0,0,0,0;0.5,1,0,0;1,0,0,0
 *   linear;0,0;1,1
 *
 * The token before the first semicolon is the interpolation mode (linear,
 * step, cubic).  Each following entry is t,value[,in_tangent,out_tangent].
 * Times and values are clamped to 0..1; tangents are slopes in value/time
 * units.  Keys are sorted and duplicate/shorter-than-two-key curves are
 * rejected by parse_value().
 *
 * User gestures call the press listener once when they start, the change
 * listener for every update and the commit listener once when they finish.
 * Programmatic set_value() is silent.  Like the other editor-kit components
 * this class is non-copyable because widget callbacks point back at the object.
 */
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "vgui.h"

namespace vgui_editor_kit {

	enum class curve_interpolation
	{
		linear = 0,
		step,
		cubic
	};

	struct curve_key
	{
		float t = 0.0f;		/* 0..1 */
		float value = 0.0f;	/* 0..1 */
		float in_tangent = 0.0f;
		float out_tangent = 0.0f;
	};

	struct curve_value
	{
		curve_interpolation interpolation = curve_interpolation::cubic;
		std::vector<curve_key> keys;
	};

	class curve_editor
	{
	public:
		using value_listener = std::function<void(const curve_value&)>;

		curve_editor() = default;
		~curve_editor();
		curve_editor(const curve_editor&) = delete;
		curve_editor& operator=(const curve_editor&) = delete;

		void build(vgui_widget* parent);
		/* Builds a detached editor suitable for vgui_popup_open(). */
		void build(vgui_context* ctx);

		/* Widget id prefix.  Call before build() to name the panel and its
		 * children; applying it after build() renames the existing widgets. */
		void set_id(std::string id);
		const std::string& id() const noexcept;
		vgui_widget* widget() const noexcept;

		/* Silent programmatic updates.  Invalid values leave the editor
		 * unchanged and return false. */
		bool set_value(const curve_value& value);
		bool set_value_text(const std::string& text, std::string* error = nullptr);
		const curve_value& value() const noexcept;
		std::string value_text() const;

		void set_read_only(bool read_only);
		bool read_only() const noexcept;
		void set_enabled(bool enabled);
		bool enabled() const noexcept;

		/* Selection is by index into the sorted key list.  Returns false when
		 * the index is out of range. */
		bool select_key(int index);
		int selected_key() const noexcept;
		/* User-style key operations; they notify listeners and commit once. */
		bool add_key(float t, float value);
		bool remove_selected_key();
		/* Interpolation changes are user-style commits. */
		bool commit_interpolation(curve_interpolation mode);
		curve_interpolation interpolation() const noexcept;

		void set_on_press(value_listener listener);
		void set_on_change(value_listener listener);
		void set_on_commit(value_listener listener);

		vgui_widget* graph_widget() const noexcept;
		vgui_widget* mode_widget(curve_interpolation mode) const noexcept;
		vgui_widget* add_widget() const noexcept;
		vgui_widget* remove_widget() const noexcept;
		vgui_widget* time_widget() const noexcept;
		vgui_widget* value_widget() const noexcept;
		vgui_widget* in_tangent_widget() const noexcept;
		vgui_widget* out_tangent_widget() const noexcept;

		/* PropertyGrid row preview: compact curve without controls or
		 * handles.  mixed draws a neutral placeholder. */
		static void draw_preview(ovg_ctx_cb* cb, rvg_t* vg, float x, float y,
			float w, float h, const curve_value& value, bool mixed);

		/* Canonical text form shared with PropertyGrid. */
		static bool parse_value(const std::string& text, curve_value& out,
			std::string* error = nullptr);
		static std::string format_value(const curve_value& value);
		static const char* interpolation_name(curve_interpolation mode);
		static const char* interpolation_label(curve_interpolation mode);

	private:
		enum class drag_kind
		{
			none = 0,
			key,
			in_tangent,
			out_tangent
		};

		struct graph_rect
		{
			float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
		};

		void apply_ids();
		void sync_controls();
		void sync_mode_buttons();
		void select_clamped(int index);
		graph_rect graph_area(vgui_widget* widget) const;
		graph_rect graph_area_from_size(float w, float h) const;
		void key_to_local(const graph_rect& area, const curve_key& key,
			float* out_x, float* out_y) const;
		void local_to_key(const graph_rect& area, float x, float y, float* t,
			float* value) const;
		int key_at(const graph_rect& area, float x, float y) const;
		int tangent_at(const graph_rect& area, float x, float y,
			drag_kind* out_kind) const;
		void tangent_points(const graph_rect& area, int index, float* in_x,
			float* in_y, float* out_x, float* out_y) const;
		void begin_gesture();
		void end_gesture();
		void notify_change();

		static int on_graph_event(vgui_widget* widget, const vgui_event* event,
			void* user);
		static void draw_graph(vgui_widget* widget, ovg_ctx_cb* cb, rvg_t* vg,
			float x, float y, float w, float h, float dt, void* user);
		static void on_mode_click(vgui_widget* widget, const vgui_event* event,
			void* user);
		static void on_add_click(vgui_widget* widget, const vgui_event* event,
			void* user);
		static void on_remove_click(vgui_widget* widget, const vgui_event* event,
			void* user);
		void on_time_change(double value);
		void on_time_commit();
		void on_value_change(double value);
		void on_value_commit();
		void on_in_change(double value);
		void on_in_commit();
		void on_out_change(double value);
		void on_out_commit();

		vgui_widget* root_ = nullptr;
		vgui_widget* graph_ = nullptr;
		vgui_widget* mode_row_ = nullptr;
		vgui_widget* key_row_ = nullptr;
		vgui_widget* tangent_row_ = nullptr;
		vgui_widget* add_ = nullptr;
		vgui_widget* remove_ = nullptr;
		vgui_widget* mode_buttons_[3] = {};
		std::unique_ptr<class spinbox> time_spin_;
		std::unique_ptr<class spinbox> value_spin_;
		std::unique_ptr<class spinbox> in_spin_;
		std::unique_ptr<class spinbox> out_spin_;

		std::string id_ = "curve_editor";
		curve_value value_;
		int selected_ = 0;
		bool read_only_ = false;
		bool enabled_ = true;
		bool syncing_ = false;
		bool gesture_active_ = false;

		drag_kind drag_ = drag_kind::none;

		value_listener press_listener_;
		value_listener change_listener_;
		value_listener commit_listener_;
	};

} /* namespace vgui_editor_kit */
