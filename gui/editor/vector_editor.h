#pragma once
/*
 * vector_editor.h - compact Vector2/Vector3/Vector4 numeric editor.
 *
 * The editor is a row of spinbox components sharing one value.  Each component
 * has a draggable axis label and an independent edit box, so a single component
 * can be typed or scrubbed without touching the others.  Every component change
 * reports the complete vector through the change listener; a completed gesture
 * reports it once through the commit listener.  Programmatic set_value() and
 * set_component_value() are silent.
 *
 * Axis labels and colors can be customized.  The semantic presets provide the
 * usual editor conventions: position uses X/Y/Z/W with axis colors, while color
 * uses R/G/B/A.  A value source + refresh() makes host-to-editor binding
 * explicit, complementing the change/commit listeners for editor-to-host
 * binding (for example a Gizmo driving the same vector).
 *
 * Like the other editor-kit components the vector editor is non-copyable
 * because its widget callbacks point back at the C++ object.  Keep it alive for
 * at least as long as its widget tree and destroy it before the vgui context.
 */
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "spinbox.h"
#include "vgui.h"

namespace vgui_editor_kit {

	enum class vector_semantic
	{
		generic = 0,
		position,
		color
	};

	class vector_editor
	{
	public:
		static constexpr int kMaxComponents = 4;

		using vector_value = std::array<double, kMaxComponents>;
		using value_listener = std::function<void(const vector_value&)>;
		using value_source = std::function<bool(vector_value&)>;

		vector_editor() = default;
		~vector_editor() = default;
		vector_editor(const vector_editor&) = delete;
		vector_editor& operator=(const vector_editor&) = delete;

		/* `dimension` is 2, 3 or 4; 0 keeps the configured dimension (3 by
		 * default).  Dimensions cannot change after build(). */
		void build(vgui_widget* parent, int dimension = 0);

		bool set_dimension(int dimension);
		int dimension() const noexcept;

		/* Widget id prefix.  Component widgets use id_0, id_1, ...  Call
		 * before build() to name the panel and its children; applying it
		 * after build() renames the existing widgets. */
		void set_id(std::string id);
		const std::string& id() const noexcept;

		/* Semantic presets reset axis labels/colors to their defaults.
		 * Custom labels/colors set afterwards take precedence. */
		void set_semantic(vector_semantic semantic);
		vector_semantic semantic() const noexcept;

		void set_axis_labels(std::vector<std::string> labels);
		void set_axis_label(int index, std::string label);
		const std::vector<std::string>& axis_labels() const noexcept;

		void set_axis_colors(std::vector<vgui_color> colors);
		void set_axis_color(int index, vgui_color color);
		const std::vector<vgui_color>& axis_colors() const noexcept;

		void set_value_type(spinbox::value_type type);
		spinbox::value_type value_type() const noexcept;

		/* The range is inclusive and applies to every component.  Setting it
		 * clamps the current values without notifying listeners. */
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

		/* Silent programmatic updates.  Extra components beyond dimension()
		 * are ignored. */
		void set_value(const vector_value& values);
		void set_component_value(int index, double value);
		vector_value value() const noexcept;
		std::string value_text() const;

		/* User-style component edits.  They reject read-only/disabled
		 * controls, validate the input and clamp to the range.  The change
		 * listener fires for every changed component; the commit listener
		 * fires once for a completed edit or drag. */
		bool commit_component_text(int index, std::string text);
		bool commit_component_value(int index, double value);

		void set_read_only(bool read_only);
		bool read_only() const noexcept;
		void set_enabled(bool enabled);
		bool enabled() const noexcept;

		void set_component_error(int index, std::string error);
		void clear_component_errors();
		const std::string& component_error(int index) const;

		void focus_component(int index);

		void set_on_change(value_listener listener);
		void set_on_commit(value_listener listener);

		/* Optional host-to-editor binding.  refresh() reads the source and
		 * applies it silently; change/commit listeners provide the other
		 * direction. */
		void set_value_source(value_source source);
		void refresh();

		vgui_widget* widget() const noexcept;
		vgui_widget* component_widget(int index) const noexcept;
		vgui_widget* component_edit_widget(int index) const noexcept;
		vgui_widget* component_label_widget(int index) const noexcept;

		/* Vector text helpers shared with PropertyGrid.  Values use the
		 * canonical "x, y, z" form; parsing also accepts spaces, semicolons
		 * and surrounding brackets. */
		static bool parse_value(const std::string& text, int dimension,
			vector_value& out, std::string* error = nullptr);
		static std::string format_value(const vector_value& values, int dimension);
		static const char* default_axis_label(int index,
			vector_semantic semantic);
		static vgui_color default_axis_color(int index,
			vector_semantic semantic);

	private:
		void apply_ids();
		void apply_semantic_defaults();
		void apply_axis_ui();
		void apply_options();
		void sync_values();
		void handle_change(int index);
		void handle_commit(int index);
		void notify_change();
		void notify_commit();
		double effective_step() const noexcept;

		vgui_widget* panel_ = nullptr;
		std::vector<std::unique_ptr<spinbox>> components_;
		std::vector<std::string> labels_;
		std::vector<vgui_color> colors_;

		std::string id_ = "vector_editor";
		int dimension_ = 3;
		vector_semantic semantic_ = vector_semantic::generic;
		bool custom_labels_ = false;
		bool custom_colors_ = false;
		spinbox::value_type value_type_ = spinbox::value_type::number;
		bool has_range_ = false;
		double min_value_ = 0.0;
		double max_value_ = 0.0;
		double step_ = 0.0;
		bool read_only_ = false;
		bool enabled_ = true;
		vector_value values_{};

		value_listener change_listener_;
		value_listener commit_listener_;
		value_source value_source_;
	};

} /* namespace vgui_editor_kit */
