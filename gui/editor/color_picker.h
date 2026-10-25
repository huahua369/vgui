#pragma once
/*
 * color_picker.h - HSV color editor with alpha, hosted in a context popup.
 *
 * Layout (top to bottom): a preview row (new/current swatches + hex edit),
 * the saturation/value box, the hue bar and the alpha bar (all custom-drawn
 * through the ovg tools picker primitives), and a row of preset/recent
 * swatches.  Dragging inside the SV box or the bars fires the change listener
 * for every update; releasing, pressing a swatch or committing the hex edit
 * fires the commit listener once.  Programmatic set_value() is silent.
 *
 * Colors travel as vgui hex strings ("#RRGGBB" / "#RRGGBBAA", value domain).
 * set_color_transform() lets the host map between the stored (value) domain
 * and the domain the picker edits - e.g. sRGB <-> linear conversion policies.
 * Like the other editor-kit components this class is non-copyable because its
 * widget callbacks point back at the C++ object.
 */
#include <functional>
#include <string>
#include <vector>

#include "vgui.h"

namespace vgui_editor_kit {

	/* 线性 0~1 RGBA(编辑域)。 */
	struct picker_rgb
	{
		float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
	};

	class color_picker
	{
	public:
		/* Receives the canonical value-domain hex after every change/commit. */
		using color_listener = std::function<void(const std::string& hex)>;
		/* value domain -> edit domain and back.  Defaults to identity. */
		using transform_fn = std::function<std::string(const std::string&)>;

		color_picker() = default;
		~color_picker();
		color_picker(const color_picker&) = delete;
		color_picker& operator=(const color_picker&) = delete;

		void build(vgui_context* ctx);
		void set_id(std::string id);
		vgui_widget* widget() const noexcept;

		/* Canonical value-domain hex ("#RRGGBB" / "#RRGGBBAA").  Silent. */
		void set_value(const std::string& hex);
		const std::string& value() const noexcept;
		/* Session start color shown as the "current" swatch (value domain). */
		void set_current(const std::string& hex);

		void set_on_change(color_listener listener);
		/* Fired on the mouse-down that starts an SV/hue/alpha gesture, before
		 * any value update - the grid snapshots its current value here. */
		void set_on_press(color_listener listener);
		void set_on_commit(color_listener listener);

		void set_presets(std::vector<std::string> presets);
		void set_recent(std::vector<std::string> recent);

		/* Conversion between the stored value domain and the picker's edit
		 * domain.  Both directions must return parseable hex. */
		void set_color_transform(transform_fn to_edit, transform_fn from_edit);

		/* True when the current edit-domain color parses. */
		bool valid() const noexcept;

	private:
		void build_swatch_row();
		void sync_ui();
		void apply_color(const picker_rgb& rgb, bool commit);
		void commit_edit_text();
		static void set_swatch_background(vgui_widget* swatch, const picker_rgb& rgb);

		static void on_swatch_click(vgui_widget* widget, const vgui_event* event,
			void* user);
		static void on_hex_event(vgui_widget* widget, const vgui_event* event,
			void* user);
		static int on_sv_event(vgui_widget* widget, const vgui_event* event,
			void* user);
		static int on_hue_event(vgui_widget* widget, const vgui_event* event,
			void* user);
		static int on_alpha_event(vgui_widget* widget, const vgui_event* event,
			void* user);
		static void draw_sv(vgui_widget* widget, ovg_ctx_cb* cb, rvg_t* vg,
			float x, float y, float w, float h, float dt, void* user);
		static void draw_hue(vgui_widget* widget, ovg_ctx_cb* cb, rvg_t* vg,
			float x, float y, float w, float h, float dt, void* user);
		static void draw_alpha(vgui_widget* widget, ovg_ctx_cb* cb, rvg_t* vg,
			float x, float y, float w, float h, float dt, void* user);

		vgui_context* ctx_ = nullptr;
		vgui_widget* root_ = nullptr;
		vgui_widget* new_swatch_ = nullptr;
		vgui_widget* current_swatch_ = nullptr;
		vgui_widget* hex_ = nullptr;
		vgui_widget* sv_ = nullptr;
		vgui_widget* hue_ = nullptr;
		vgui_widget* alpha_ = nullptr;
		vgui_widget* swatch_row_ = nullptr;
		std::string id_;

		/* Edit-domain state。h 单位度,s/v/a 0~1。 */
		float h_deg_ = 0.0f, s_ = 1.0f, v_ = 1.0f, a_ = 1.0f;
		std::string value_;			/* value domain, canonical */
		std::string current_;		/* session start, value domain */
		picker_rgb edit_rgb_;

		std::vector<std::string> presets_;
		std::vector<std::string> recent_;
		std::vector<std::string> swatch_hexes_;
		transform_fn to_edit_;
		transform_fn from_edit_;

		color_listener change_listener_;
		color_listener press_listener_;
		color_listener commit_listener_;
		bool dragging_ = false;		/* suppresses commit-on-hex-blur noise */
		bool inside_edit_ = false;
	};

} /* namespace vgui_editor_kit */
