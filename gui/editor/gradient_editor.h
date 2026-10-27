#pragma once
/*
 * gradient_editor.h - multi-stop color gradient editor for vgui_editor_kit.
 *
 * The editor combines a gradient bar (with draggable stop handles), a small
 * numeric row for the selected stop time and a full HSV color picker bound to
 * the selected stop.  Clicking a handle selects it, dragging moves it in time;
 * double-clicking the bar inserts a stop with the interpolated color.  The
 * preset row applies common gradients as one undoable gesture.
 *
 * Values are plain strings so a PropertyGrid row can own them without the grid
 * knowing the gradient layout:
 *
 *   0,#FF0000FF;0.5,#00FF00FF;1,#0000FFFF
 *
 * Each entry is t,color.  Colors use "#RRGGBB" / "#RRGGBBAA" (the parser also
 * accepts 3/4 digit short forms and a 0x prefix).  Times are clamped to 0..1;
 * stops are sorted; duplicates and fewer than two stops are rejected by
 * parse_value().
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

	class color_picker;
	class spinbox;

	struct gradient_stop
	{
		float t = 0.0f;			/* 0..1 */
		std::string color;		/* canonical #RRGGBB / #RRGGBBAA */
	};

	struct gradient_value
	{
		std::vector<gradient_stop> stops;
	};

	class gradient_editor
	{
	public:
		using value_listener = std::function<void(const gradient_value&)>;

		gradient_editor();
		~gradient_editor();
		gradient_editor(const gradient_editor&) = delete;
		gradient_editor& operator=(const gradient_editor&) = delete;

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
		bool set_value(const gradient_value& value);
		bool set_value_text(const std::string& text, std::string* error = nullptr);
		const gradient_value& value() const noexcept;
		std::string value_text() const;

		void set_read_only(bool read_only);
		bool read_only() const noexcept;
		void set_enabled(bool enabled);
		bool enabled() const noexcept;

		/* Selection is by index into the sorted stop list.  Returns false
		 * when the index is out of range. */
		bool select_stop(int index);
		int selected_stop() const noexcept;
		/* User-style stop operations; they notify listeners and commit once.
		 * add_stop() samples the interpolated color at t. */
		bool add_stop(float t);
		bool remove_selected_stop();
		bool set_selected_color(const std::string& color);

		/* Preset helpers.  commit_preset() is a user-style whole-gradient
		 * commit; names are stable and suitable for buttons/tests. */
		std::size_t preset_count() const noexcept;
		const char* preset_name(std::size_t index) const noexcept;
		bool commit_preset(std::size_t index);

		void set_on_press(value_listener listener);
		void set_on_change(value_listener listener);
		void set_on_commit(value_listener listener);

		vgui_widget* bar_widget() const noexcept;
		vgui_widget* add_widget() const noexcept;
		vgui_widget* remove_widget() const noexcept;
		vgui_widget* time_widget() const noexcept;
		vgui_widget* preset_widget(std::size_t index) const noexcept;
		/* Embedded picker, exposed for tests and host integration. */
		color_picker* picker() const noexcept;

		/* PropertyGrid row preview: compact gradient without controls or
		 * handles.  mixed draws a neutral placeholder. */
		static void draw_preview(ovg_ctx_cb* cb, rvg_t* vg, float x, float y,
			float w, float h, const gradient_value& value, bool mixed,
			float ui_scale = 1.0f);

		/* Canonical text form shared with PropertyGrid. */
		static bool parse_value(const std::string& text, gradient_value& out,
			std::string* error = nullptr);
		static std::string format_value(const gradient_value& value);

	private:
		enum class drag_kind
		{
			none = 0,
			stop
		};

		void apply_ids();
		void sync_controls();
		void begin_gesture();
		void end_gesture();
		void notify_change();
		void select_clamped(int index);
		int stop_at(float x) const;
		float bar_t(float x) const;
		bool set_selected_color_internal(const std::string& color);
		bool commit_value(gradient_value next);

		static int on_bar_event(vgui_widget* widget, const vgui_event* event,
			void* user);
		static void draw_bar(vgui_widget* widget, ovg_ctx_cb* cb, rvg_t* vg,
			float x, float y, float w, float h, float dt, void* user);
		static void on_add_click(vgui_widget* widget, const vgui_event* event,
			void* user);
		static void on_remove_click(vgui_widget* widget, const vgui_event* event,
			void* user);
		static void on_preset_click(vgui_widget* widget, const vgui_event* event,
			void* user);
		void on_time_change(double value);
		void on_time_commit();
		void on_picker_press(const std::string& color);
		void on_picker_change(const std::string& color);
		void on_picker_commit(const std::string& color);

		vgui_widget* root_ = nullptr;
		vgui_widget* bar_ = nullptr;
		vgui_widget* stop_row_ = nullptr;
		vgui_widget* preset_row_ = nullptr;
		vgui_widget* add_ = nullptr;
		vgui_widget* remove_ = nullptr;
		vgui_widget* presets_[4] = {};
		std::unique_ptr<spinbox> time_spin_;
		std::unique_ptr<color_picker> picker_;

		std::string id_ = "gradient_editor";
		gradient_value value_;
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
