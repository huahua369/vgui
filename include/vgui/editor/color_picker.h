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

#include <vgui/vgui.h>

namespace vgui_editor_kit {

	/* Linear 0..1 RGBA in the edit domain. */
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
		/* Current hue in degrees (0~360).  Kept at the position set by the hue
		 * bar even at the wrap-around end, where the RGB is identical to 0 deg. */
		float hue_degrees() const noexcept;

	private:
		void build_swatch_row();
		void sync_ui();
		/* sync_hsv=false keeps the h/s/v values already maintained by a drag;
		 * otherwise they are recomputed from rgb (hex/swatch paths). */
		void apply_color(const picker_rgb& rgb, bool commit, bool sync_hsv = true);
		void commit_edit_text();
		/* Drag ownership for the SV/hue/alpha bars: mouse moves do not carry a
		 * button in vgui 0.3, so the press widget must be tracked explicitly.
		 * Returns true when this event should drive the given bar. */
		bool accept_drag_event(vgui_widget* widget, const vgui_event* event);
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

		/* Edit-domain state.  h is in degrees; s/v/a are 0..1. */
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
		vgui_widget* drag_widget_ = nullptr;	/* SV box / hue bar / alpha bar */
		bool inside_edit_ = false;
	};

} /* namespace vgui_editor_kit */
