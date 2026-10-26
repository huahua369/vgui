#pragma once
/*
 * asset_preview.h - preview panel for the selected asset.
 *
 * The host supplies a loader through set_load_handler().  The loader runs on a
 * background worker, so decoding a large texture or mesh never stalls the UI.
 * Loaders produce a CPU-only data value; the component uploads image
 * pixels on the main thread and owns the resulting texture.  The preview kind
 * selects a built-in renderer (image/model/material/text/audio); hosts can use
 * kind::custom for their own data and provide an image or text fallback.
 *
 * Selection wiring is intentionally explicit: connect asset_browser's
 * selection listener to set_item().  The built-in editor_shell does this for
 * its preview panel and lets the host replace or extend the loader.
 */
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "vgui.h"

namespace vgui_editor_kit {

	class asset_preview
	{
	public:
		enum class preview_kind
		{
			none,
			image,
			model,
			material,
			text,
			audio,
			custom
		};

		/* CPU image data.  stride is bytes per row; 0 means width * 4.  The
		 * component copies the pixels into a vgui texture on the main thread. */
		struct image_data
		{
			int width = 0;
			int height = 0;
			int stride = 0;
			vgui_texture_format format = VGUI_TEXTURE_FORMAT_RGBA8;
			std::vector<unsigned char> pixels;
		};

		struct field
		{
			std::string label;
			std::string value;
		};

		/* Loader result.  name/type fall back to the request when left empty.
		 * error marks a failed load; the preview keeps the item and shows the
		 * error text over a placeholder. */
		struct data
		{
			preview_kind kind = preview_kind::none;
			std::string name;
			std::string type;
			std::string detail;
			std::string error;
			std::string text;
			std::vector<field> fields;
			image_data image;
			vgui_color accent = 0xFF409EFF;
			vgui_color secondary = 0xFF2E3440;
			std::vector<vgui_color> swatches;
			std::vector<float> waveform;
		};

		struct request
		{
			vgui_item_id id = 0;
			std::string name;
			std::string type;
		};

		/* Runs on the worker thread.  Only touch host-side data here; never call
		 * vgui or this component from the loader.  Return false (optionally
		 * filling data::error) to show the failure placeholder. */
		using load_handler = std::function<bool(const request&, data*)>;

		asset_preview();
		/* The panel must be destroyed before the vgui_context it was built on. */
		~asset_preview();

		asset_preview(const asset_preview&) = delete;
		asset_preview& operator=(const asset_preview&) = delete;

		void build(vgui_widget* parent);

		void set_load_handler(load_handler handler);
		void set_item(vgui_item_id item, std::string name = {}, std::string type = {});
		void clear();
		/* Re-runs the loader for the current item without changing the item. */
		void reload();

		vgui_item_id item() const noexcept;
		bool loading() const noexcept;
		bool has_error() const noexcept;
		const std::string& error() const noexcept;
		const data& current_data() const noexcept;
		const std::string& status_text() const noexcept;
		vgui_texture* texture() const noexcept;

		/* Defaults: "Select an asset to preview", "Loading preview...",
		 * "Preview failed".  Empty arguments restore the defaults. */
		void set_texts(std::string empty, std::string loading, std::string failed);

		/* Advances the loading spinner and applies a completed background load.
		 * The component drives this from a 2x2 ticker; headless hosts/tests can
		 * call tick() or poll() directly.  poll() returns true when new data was
		 * applied. */
		void tick(float delta_time);
		bool poll();

		vgui_widget* widget() const noexcept;
		vgui_widget* stage() const noexcept;

	private:
		struct async_state;

		static void on_stage_draw(vgui_widget* widget, ovg_ctx_cb* cb, rvg_t* vg,
			float abs_x, float abs_y, float abs_w, float abs_h, float delta_time,
			void* user);
		static void on_info_draw(vgui_widget* widget, ovg_ctx_cb* cb, rvg_t* vg,
			float abs_x, float abs_y, float abs_w, float abs_h, float delta_time,
			void* user);
		static void on_tick_draw(vgui_widget* widget, ovg_ctx_cb* cb, rvg_t* vg,
			float abs_x, float abs_y, float abs_w, float abs_h, float delta_time,
			void* user);

		void start_load();
		void apply_data(data value, bool success);
		void rebuild_texture();
		void destroy_texture();
		void update_widgets();
		void compute_status();

		void draw_stage(ovg_ctx_cb* cb, rvg_t* vg, float x, float y,
			float width, float height);
		void draw_info(ovg_ctx_cb* cb, rvg_t* vg, float x, float y,
			float width, float height);
		void draw_image(ovg_ctx_cb* cb, rvg_t* vg, float x, float y,
			float width, float height);
		void draw_model(ovg_ctx_cb* cb, rvg_t* vg, float x, float y,
			float width, float height);
		void draw_material(ovg_ctx_cb* cb, rvg_t* vg, float x, float y,
			float width, float height);
		void draw_text(ovg_ctx_cb* cb, rvg_t* vg, float x, float y,
			float width, float height);
		void draw_audio(ovg_ctx_cb* cb, rvg_t* vg, float x, float y,
			float width, float height);
		void draw_placeholder(ovg_ctx_cb* cb, rvg_t* vg, float x, float y,
			float width, float height);

		std::unique_ptr<async_state> async_;

		vgui_widget* root_ = nullptr;
		vgui_widget* header_ = nullptr;
		vgui_widget* name_label_ = nullptr;
		vgui_widget* type_label_ = nullptr;
		vgui_widget* stage_ = nullptr;
		vgui_widget* ticker_ = nullptr;
		vgui_widget* info_ = nullptr;
		vgui_texture* texture_ = nullptr;

		vgui_item_id item_ = 0;
		std::string request_name_;
		std::string request_type_;
		data data_;
		std::string error_;
		std::string status_;
		std::string empty_text_ = "Select an asset to preview";
		std::string loading_text_ = "Loading preview...";
		std::string failed_text_ = "Preview failed";
		bool loading_ = false;
		bool loaded_ = false;
		float anim_time_ = 0.0f;
	};

} /* namespace vgui_editor_kit */
