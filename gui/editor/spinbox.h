#pragma once
/*
 * spinbox.h - numeric editor with step buttons and a drag-to-scrub label.
 *
 * A spinbox combines a single-line edit, a label that can be dragged
 * horizontally to change the value, and decrement/increment buttons.  Values
 * are normalized to the configured type and range: integer values are rounded,
 * floating point values are clamped, and dragging snaps to the configured step.
 *
 * Programmatic set_value() is silent.  User gestures (Enter, step buttons,
 * keyboard Up/Down and label drags) validate the input, update the edit text,
 * call the change listener for every value change and call the commit listener
 * once when the gesture finishes.  Invalid text keeps the typed text visible
 * and reports through error().
 *
 * Like the other editor-kit components the spinbox is non-copyable because its
 * widget callbacks point back at the C++ object.  Keep it alive for at least as
 * long as its widget tree.
 */
#include <functional>
#include <string>

#include "toolbar.h"
#include "vgui.h"

namespace vgui_editor_kit {

	class spinbox
	{
	public:
		enum class value_type
		{
			integer = 0,
			number
		};

		using value_listener = std::function<void(double)>;

		spinbox() = default;
		~spinbox() = default;
		spinbox(const spinbox&) = delete;
		spinbox& operator=(const spinbox&) = delete;

		void build(vgui_widget* parent, std::string label = {});

		/* Widget id prefix.  Call before build() to name the panel and its
		 * children; applying it after build() renames the existing widgets. */
		void set_id(std::string id);
		const std::string& id() const noexcept;

		void set_label(std::string label);
		const std::string& label() const noexcept;

		/* Label column width.  The default 72px fits names such as "Radius";
		 * compact editors (for example vector components) can shrink it.
		 * An empty label keeps its own narrow width. */
		void set_label_width(float width);
		float label_width() const noexcept;

		/* Chrome font size in logical units.  0 (default) follows the context
		 * font; the row height derives from it (font + 10, at least 24).
		 * Popup editors keep the context size, while property-grid and table
		 * embeddings pin it to stay inside their fixed 24px rows. */
		void set_font_size(float size);
		float font_size() const noexcept;

		/* Overrides the edit box's intrinsic width floor.  The context style
		 * edit.min_width (hosts may set it large) otherwise becomes the
		 * spinbox's measured width even for compact T/V editors.  0 (default)
		 * follows the style; the edit keeps growing inside its row either way. */
		void set_edit_min_width(float width);
		float edit_min_width() const noexcept;

		/* Step buttons are shown by default.  Compact editors can hide them
		 * and rely on text input plus label dragging instead. */
		void set_buttons_visible(bool visible);
		bool buttons_visible() const noexcept;

		void set_value_type(value_type type);
		value_type type() const noexcept;

		/* The range is inclusive.  Setting it clamps the current value without
		 * notifying listeners. */
		void set_range(double min_value, double max_value);
		void clear_range();
		bool has_range() const noexcept;
		double min_value() const noexcept;
		double max_value() const noexcept;

		/* step() is the effective step: the configured value, or 1 for
		 * integers / 0.1 for floating point when set_step(0) selects the
		 * default. */
		void set_step(double step);
		double step() const noexcept;

		/* Silent programmatic update: clamps, rounds integers, refreshes the
		 * edit text and clears any error. */
		void set_value(double value);
		double value() const noexcept;
		std::string value_text() const;

		/* User-style edits.  They reject read-only/disabled controls, validate
		 * the input and clamp to the range.  change_listener() fires whenever
		 * the value changes; commit_listener() fires once for a completed edit
		 * that actually changed the value. */
		bool commit_text(std::string text);
		bool commit_value(double value);
		void step_by(int steps);

		void set_read_only(bool read_only);
		bool read_only() const noexcept;
		void set_enabled(bool enabled);
		bool enabled() const noexcept;

		void set_error(std::string error);
		const std::string& error() const noexcept;

		void focus();

		void set_on_change(value_listener listener);
		void set_on_commit(value_listener listener);

		vgui_widget* widget() const noexcept;
		vgui_widget* edit_widget() const noexcept;
		vgui_widget* label_widget() const noexcept;
		vgui_widget* decrement_widget() const noexcept;
		vgui_widget* increment_widget() const noexcept;

	private:
		static void on_label_event(vgui_widget* widget, const vgui_event* event,
			void* user);
		static void on_edit_event(vgui_widget* widget, const vgui_event* event,
			void* user);
		static void on_edit_text(vgui_widget* widget, const char* text, void* user);

		void apply_ids();
		void sync_text();
		void update_control_state();
		void update_error_ui();
		void apply_font();
		void apply_chrome();
		float chrome_row_height() const;
		double effective_step() const noexcept;
		double normalize_value(double value, bool snap_to_step) const noexcept;
		double canonicalize(double value) const;
		void notify_change();
		void notify_commit();
		void nudge(int direction, int mods);
		void begin_drag(const vgui_event& event);
		void update_drag(const vgui_event& event);
		void end_drag();
		void cancel_drag();

		vgui_widget* panel_ = nullptr;
		vgui_widget* label_widget_ = nullptr;
		vgui_widget* edit_ = nullptr;
		icon_button decrement_;
		icon_button increment_;

		std::string id_ = "spinbox";
		std::string label_;
		std::string error_;
		float label_width_ = 72.0f;
		float font_size_ = 0.0f;	/* 0 = follow the context font */
		float edit_min_width_ = 0.0f;	/* 0 = follow style.edit.min_width */
		bool buttons_visible_ = true;
		value_type type_ = value_type::number;
		double value_ = 0.0;
		bool has_range_ = false;
		double min_value_ = 0.0;
		double max_value_ = 0.0;
		double step_ = 0.0;
		bool read_only_ = false;
		bool enabled_ = true;

		bool dragging_ = false;
		float drag_start_x_ = 0.0f;
		double drag_start_value_ = 0.0;
		bool drag_changed_ = false;

		value_listener change_listener_;
		value_listener commit_listener_;
	};

} /* namespace vgui_editor_kit */
