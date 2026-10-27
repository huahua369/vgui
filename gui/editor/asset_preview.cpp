#include "asset_preview.h"

#include "pch.h"
#include "tools/tools.hpp"
#include "ui_scale.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>

namespace vgui_editor_kit {

	namespace {

		constexpr vgui_color kFallbackPanel = 0xFF1E232B;
		constexpr vgui_color kFallbackBorder = 0xFF2E3440;
		constexpr vgui_color kFallbackText = 0xFFD8DEE9;
		constexpr vgui_color kFallbackMuted = 0xFF7F8B9C;
		constexpr vgui_color kFallbackAccent = 0xFF409EFF;
		constexpr vgui_color kFallbackDanger = 0xFFE06C75;

		vgui_theme theme_for(vgui_widget* widget)
		{
			vgui_theme theme = {};
			vgui_context* ctx = widget ? vgui_widget_get_context(widget) : nullptr;
			if (ctx)
				vgui_context_get_theme(ctx, &theme);
			if (!theme.panel_bg) theme.panel_bg = kFallbackPanel;
			if (!theme.panel_border) theme.panel_border = kFallbackBorder;
			if (!theme.text) theme.text = kFallbackText;
			if (!theme.disabled) theme.disabled = kFallbackMuted;
			if (!theme.accent) theme.accent = kFallbackAccent;
			if (!theme.modal_danger) theme.modal_danger = kFallbackDanger;
			if (!theme.edit_bg) theme.edit_bg = theme.panel_bg;
			return theme;
		}

		uint32_t to_ovg(vgui_color color)
		{
			return ((color & 0x00FF0000u) >> 16) |
				(color & 0x0000FF00u) |
				((color & 0x000000FFu) << 16) |
				(color & 0xFF000000u);
		}

		float clampf(float value, float lo, float hi)
		{
			return value < lo ? lo : (value > hi ? hi : value);
		}

		uint32_t with_alpha(uint32_t color, float alpha)
		{
			const uint32_t a = (uint32_t)(clampf(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
			return (color & 0x00FFFFFFu) | (a << 24);
		}

		uint32_t mix_color(uint32_t a, uint32_t b, float t)
		{
			t = clampf(t, 0.0f, 1.0f);
			uint32_t out = 0;
			for (int shift = 0; shift < 32; shift += 8)
			{
				const float ca = (float)((a >> shift) & 0xFF);
				const float cb = (float)((b >> shift) & 0xFF);
				out |= ((uint32_t)(ca + (cb - ca) * t + 0.5f) & 0xFF) << shift;
			}
			return out;
		}

	} /* namespace */

	struct asset_preview::async_state
	{
		struct job
		{
			std::uint64_t generation = 0;
			request req;
			load_handler loader;
		};

		struct completed_job
		{
			std::uint64_t generation = 0;
			data value;
			bool success = false;
		};

		async_state()
		{
			worker = std::thread([this]() { run(); });
		}

		~async_state()
		{
			{
				std::lock_guard<std::mutex> lock(mutex);
				quit = true;
				has_job = false;
			}
			cond.notify_all();
			if (worker.joinable())
				worker.join();
		}

		void run()
		{
			std::unique_lock<std::mutex> lock(mutex);
			for (;;)
			{
				cond.wait(lock, [this]() { return quit || has_job; });
				if (quit)
					break;

				const job current = std::move(pending);
				has_job = false;
				lock.unlock();

				data value;
				bool success = false;
				if (current.loader)
				{
					try
					{
						success = current.loader(current.req, &value);
					}
					catch (...)
					{
						success = false;
						value.error = "Preview loader raised an exception";
					}
				}
				else
				{
					value.error = "No preview loader configured";
				}

				lock.lock();
				if (quit)
					break;
				result.generation = current.generation;
				result.value = std::move(value);
				result.success = success;
				has_result = true;
			}
		}

		/* Invalidates every in-flight result.  Returns the new active
		 * generation so callers can schedule a job if needed. */
		std::uint64_t invalidate()
		{
			std::lock_guard<std::mutex> lock(mutex);
			active_generation = next_generation++;
			has_job = false;
			pending = job{};
			has_result = false;
			return active_generation;
		}

		std::uint64_t schedule(const request& req, const load_handler& loader)
		{
			std::lock_guard<std::mutex> lock(mutex);
			const std::uint64_t generation = next_generation++;
			pending.generation = generation;
			pending.req = req;
			pending.loader = loader;
			has_job = true;
			active_generation = generation;
			has_result = false;
			return generation;
		}

		load_handler current_loader() const
		{
			std::lock_guard<std::mutex> lock(mutex);
			return handler;
		}

		void set_loader(load_handler loader)
		{
			std::lock_guard<std::mutex> lock(mutex);
			handler = std::move(loader);
		}

		bool take_result(completed_job* out, std::uint64_t* active)
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (!has_result)
				return false;
			*out = std::move(result);
			*active = active_generation;
			has_result = false;
			return true;
		}

		mutable std::mutex mutex;
		std::condition_variable cond;
		std::thread worker;
		bool quit = false;
		bool has_job = false;
		job pending;
		std::uint64_t next_generation = 1;
		std::uint64_t active_generation = 0;
		bool has_result = false;
		completed_job result;
		load_handler handler;
	};

	asset_preview::asset_preview()
		: async_(std::make_unique<async_state>())
	{
	}

	asset_preview::~asset_preview()
	{
		/* Stop the worker before the host data or the vgui context disappears.
		 * The loader must not touch this component, but it may capture host
		 * state that is destroyed together with the panel. */
		async_.reset();
		destroy_texture();
	}

	void asset_preview::build(vgui_widget* parent)
	{
		if (root_ || !parent)
			return;

		vgui_theme theme = theme_for(parent);

		root_ = vgui_widget_add(parent, VGUI_PANEL, nullptr);
		vgui_widget_set_layout(root_, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_padding(root_, 6, 6, 6, 6);
		vgui_widget_set_gap(root_, 4);
		vgui_widget_set_background(root_, 0);
		vgui_widget_set_border(root_, 0, 0);
		vgui_widget_set_size(root_, 0, 224);
		vgui_widget_set_min_size(root_, 0, 176);
		vgui_widget_set_align_self(root_, VGUI_ALIGN_STRETCH);

		vgui_widget* separator = vgui_widget_add(root_, VGUI_PANEL, nullptr);
		vgui_widget_set_size(separator, 0, 1);
		vgui_widget_set_min_size(separator, 1, 1);
		vgui_widget_set_align_self(separator, VGUI_ALIGN_STRETCH);
		vgui_widget_set_background(separator, theme.panel_border);
		vgui_widget_set_border(separator, 0, 0);

		header_ = vgui_widget_add(root_, VGUI_PANEL, nullptr);
		vgui_widget_set_size(header_, 0, 26);
		vgui_widget_set_min_size(header_, 0, 22);
		vgui_widget_set_align_self(header_, VGUI_ALIGN_STRETCH);
		vgui_widget_set_layout(header_, VGUI_LAYOUT_ROW);
		vgui_widget_set_align_items(header_, VGUI_ALIGN_CENTER);
		vgui_widget_set_padding(header_, 0, 0, 0, 0);
		vgui_widget_set_gap(header_, 6);
		vgui_widget_set_background(header_, 0);
		vgui_widget_set_border(header_, 0, 0);

		name_label_ = vgui_widget_add(header_, VGUI_LABEL, "PREVIEW");
		vgui_widget_set_grow(name_label_, 1.0f);
		vgui_widget_set_font_size(name_label_, 13);
		vgui_widget_set_text_color(name_label_, theme.text);
		vgui_widget_set_padding(name_label_, 0, 0, 0, 0);

		type_label_ = vgui_widget_add(header_, VGUI_LABEL, "");
		vgui_widget_set_font_size(type_label_, 10);
		vgui_widget_set_text_color(type_label_, theme.disabled);
		vgui_widget_set_padding(type_label_, 0, 0, 0, 0);

		/* A zero-size custom widget would be culled before draw, so the
		 * spinner/async driver is a 2x2 transparent ticker. */
		ticker_ = vgui_widget_add(header_, VGUI_CUSTOM, nullptr);
		vgui_widget_set_size(ticker_, 2, 2);
		vgui_widget_set_min_size(ticker_, 1, 1);
		vgui_widget_set_background(ticker_, 0);
		vgui_widget_set_border(ticker_, 0, 0);
		vgui_custom_set_draw(ticker_, &asset_preview::on_tick_draw, this);

		stage_ = vgui_widget_add(root_, VGUI_CUSTOM, nullptr);
		vgui_widget_set_grow(stage_, 1.0f);
		vgui_widget_set_min_size(stage_, 0, 88);
		vgui_widget_set_align_self(stage_, VGUI_ALIGN_STRETCH);
		vgui_widget_set_background(stage_, 0);
		vgui_widget_set_border(stage_, 0, 0);
		vgui_widget_set_font_size(stage_, 12);
		vgui_custom_set_draw(stage_, &asset_preview::on_stage_draw, this);
		vgui_custom_set_clip_content(stage_, 1);

		info_ = vgui_widget_add(root_, VGUI_CUSTOM, nullptr);
		vgui_widget_set_size(info_, 0, 60);
		vgui_widget_set_min_size(info_, 0, 40);
		vgui_widget_set_align_self(info_, VGUI_ALIGN_STRETCH);
		vgui_widget_set_background(info_, 0);
		vgui_widget_set_border(info_, 0, 0);
		vgui_widget_set_font_size(info_, 11);
		vgui_custom_set_draw(info_, &asset_preview::on_info_draw, this);
		vgui_custom_set_clip_content(info_, 1);

		compute_status();
		update_widgets();
	}

	void asset_preview::set_load_handler(load_handler handler)
	{
		if (!async_)
			return;
		async_->set_loader(std::move(handler));
		if (item_ != 0)
			start_load();
	}

	void asset_preview::set_item(vgui_item_id item, std::string name, std::string type)
	{
		if (item == 0)
		{
			clear();
			return;
		}
		if (item == item_ && name == request_name_ && type == request_type_ &&
			(loading_ || loaded_))
			return;

		item_ = item;
		request_name_ = std::move(name);
		request_type_ = std::move(type);
		data_ = data{};
		data_.name = request_name_;
		data_.type = request_type_;
		error_.clear();
		loading_ = false;
		loaded_ = false;
		anim_time_ = 0.0f;
		destroy_texture();
		start_load();
	}

	void asset_preview::clear()
	{
		if (async_)
			(void)async_->invalidate();
		item_ = 0;
		request_name_.clear();
		request_type_.clear();
		data_ = data{};
		error_.clear();
		loading_ = false;
		loaded_ = false;
		anim_time_ = 0.0f;
		status_ = empty_text_;
		destroy_texture();
		update_widgets();
	}

	void asset_preview::reload()
	{
		if (item_ == 0)
			return;
		loaded_ = false;
		start_load();
	}

	vgui_item_id asset_preview::item() const noexcept
	{
		return item_;
	}

	bool asset_preview::loading() const noexcept
	{
		return loading_;
	}

	bool asset_preview::has_error() const noexcept
	{
		return !error_.empty();
	}

	const std::string& asset_preview::error() const noexcept
	{
		return error_;
	}

	const asset_preview::data& asset_preview::current_data() const noexcept
	{
		return data_;
	}

	const std::string& asset_preview::status_text() const noexcept
	{
		return status_;
	}

	vgui_texture* asset_preview::texture() const noexcept
	{
		return texture_;
	}

	void asset_preview::set_texts(std::string empty, std::string loading, std::string failed)
	{
		empty_text_ = empty.empty() ? "Select an asset to preview" : std::move(empty);
		loading_text_ = loading.empty() ? "Loading preview..." : std::move(loading);
		failed_text_ = failed.empty() ? "Preview failed" : std::move(failed);
		compute_status();
		update_widgets();
	}

	void asset_preview::tick(float delta_time)
	{
		(void)poll();
		if (loading_)
		{
			anim_time_ += std::max(0.0f, delta_time);
			if (anim_time_ > 1000.0f)
				anim_time_ = std::fmod(anim_time_, 1000.0f);
		}
	}

	bool asset_preview::poll()
	{
		if (!async_)
			return false;
		async_state::completed_job completed;
		std::uint64_t active = 0;
		if (!async_->take_result(&completed, &active))
			return false;
		if (completed.generation != active)
			return false;	/* stale result from an older selection */
		apply_data(std::move(completed.value), completed.success);
		return true;
	}

	vgui_widget* asset_preview::widget() const noexcept
	{
		return root_;
	}

	vgui_widget* asset_preview::stage() const noexcept
	{
		return stage_;
	}

	void asset_preview::start_load()
	{
		if (item_ == 0)
		{
			clear();
			return;
		}
		if (!async_)
			return;

		const load_handler loader = async_->current_loader();
		request req;
		req.id = item_;
		req.name = request_name_;
		req.type = request_type_;

		if (!loader)
		{
			(void)async_->invalidate();
			error_.clear();
			loading_ = false;
			loaded_ = true;
			anim_time_ = 0.0f;
			data_ = data{};
			data_.name = request_name_;
			data_.type = request_type_;
			data_.detail = "No preview available";
			status_ = data_.detail;
			destroy_texture();
			update_widgets();
			return;
		}

		data_ = data{};
		data_.name = request_name_;
		data_.type = request_type_;
		error_.clear();
		loading_ = true;
		loaded_ = false;
		anim_time_ = 0.0f;
		status_ = loading_text_;
		destroy_texture();
		async_->schedule(req, loader);
		async_->cond.notify_one();
		update_widgets();
	}

	void asset_preview::apply_data(data value, bool success)
	{
		loading_ = false;
		loaded_ = true;
		anim_time_ = 0.0f;

		if (value.name.empty())
			value.name = request_name_;
		if (value.type.empty())
			value.type = request_type_;
		data_ = std::move(value);

		const bool failed = !success || !data_.error.empty();
		if (failed)
		{
			error_ = data_.error.empty() ? failed_text_ : data_.error;
			if (data_.error.empty())
				data_.error = error_;
		}
		else
		{
			error_.clear();
			data_.error.clear();
		}

		rebuild_texture();
		compute_status();
		update_widgets();
	}

	void asset_preview::rebuild_texture()
	{
		destroy_texture();
		if (!error_.empty() || !root_)
			return;

		const image_data& image = data_.image;
		if (image.width <= 0 || image.height <= 0 || image.pixels.empty())
			return;

		int stride = image.stride > 0 ? image.stride : image.width * 4;
		if (stride < image.width * 4)
			return;
		const std::size_t required = (std::size_t)stride * (std::size_t)image.height;
		if (image.pixels.size() < required)
			return;

		vgui_context* ctx = vgui_widget_get_context(root_);
		if (!ctx)
			return;

		vgui_texture_desc desc = {};
		desc.width = image.width;
		desc.height = image.height;
		desc.stride = stride;
		desc.format = image.format;
		desc.pixels = image.pixels.data();
		texture_ = vgui_texture_create(ctx, &desc);
	}

	void asset_preview::destroy_texture()
	{
		if (!texture_)
			return;
		vgui_texture_destroy(texture_);
		texture_ = nullptr;
	}

	void asset_preview::update_widgets()
	{
		if (!root_ || !name_label_)
			return;

		vgui_theme theme = theme_for(root_);
		std::string name = data_.name.empty() ? request_name_ : data_.name;
		if (item_ == 0)
			name = "PREVIEW";
		else if (name.empty())
			name = "Asset " + std::to_string(item_);

		std::string type = data_.type.empty() ? request_type_ : data_.type;
		vgui_widget_set_text(name_label_, name.c_str());
		vgui_widget_set_text(type_label_, type.c_str());
		vgui_widget_set_text_color(name_label_, theme.text);
		vgui_widget_set_text_color(type_label_,
			error_.empty() ? theme.disabled : theme.modal_danger);
		vgui_widget_set_tooltip(name_label_, status_.c_str());
	}

	void asset_preview::compute_status()
	{
		if (loading_)
		{
			status_ = loading_text_;
			return;
		}
		if (item_ == 0)
		{
			status_ = empty_text_;
			return;
		}
		if (!error_.empty())
		{
			status_ = failed_text_;
			if (error_ != failed_text_)
			{
				status_ += ": ";
				status_ += error_;
			}
			return;
		}
		if (!data_.detail.empty())
		{
			status_ = data_.detail;
			return;
		}
		if (!data_.type.empty())
		{
			status_ = data_.type;
			return;
		}
		status_ = loaded_ ? std::string("Ready") : empty_text_;
	}

	void asset_preview::on_stage_draw(vgui_widget* widget, ovg_ctx_cb* cb, rvg_t* vg,
		float abs_x, float abs_y, float abs_w, float abs_h, float delta_time,
		void* user)
	{
		(void)widget;
		(void)delta_time;
		if (!user)
			return;
		static_cast<asset_preview*>(user)->draw_stage(cb, vg, abs_x, abs_y, abs_w, abs_h);
	}

	void asset_preview::on_info_draw(vgui_widget* widget, ovg_ctx_cb* cb, rvg_t* vg,
		float abs_x, float abs_y, float abs_w, float abs_h, float delta_time,
		void* user)
	{
		(void)widget;
		(void)delta_time;
		if (!user)
			return;
		static_cast<asset_preview*>(user)->draw_info(cb, vg, abs_x, abs_y, abs_w, abs_h);
	}

	void asset_preview::on_tick_draw(vgui_widget* widget, ovg_ctx_cb* cb, rvg_t* vg,
		float abs_x, float abs_y, float abs_w, float abs_h, float delta_time,
		void* user)
	{
		(void)widget;
		(void)cb;
		(void)vg;
		(void)abs_x;
		(void)abs_y;
		(void)abs_w;
		(void)abs_h;
		if (user)
			static_cast<asset_preview*>(user)->tick(delta_time);
	}

	float asset_preview::ui_scale() const
	{
		return ui_scale_of(stage_);
	}

	void asset_preview::draw_stage(ovg_ctx_cb* cb, rvg_t* vg, float x, float y,
		float width, float height)
	{
		if (!cb || !vg || width <= 1.0f || height <= 1.0f)
			return;

		const float scale = ui_scale();
		const vgui_theme theme = theme_for(stage_);
		const uint32_t bg = mix_color(to_ovg(theme.edit_bg ? theme.edit_bg : kFallbackPanel),
			0xFF080A0D, 0.32f);
		const uint32_t border = to_ovg(theme.panel_border);
		ovg_tools::fill_round_rect(cb, vg, x, y, width, height,
			4.0f * scale, bg);

		if (item_ == 0)
		{
			draw_placeholder(cb, vg, x, y, width, height);
		}
		else if (loading_)
		{
			draw_placeholder(cb, vg, x, y, width, height);
		}
		else if (!error_.empty())
		{
			const uint32_t danger = to_ovg(theme.modal_danger);
			ovg_tools::fill_round_rect(cb, vg, x, y, width, height,
				4.0f * scale, with_alpha(danger, 0.12f));
			draw_placeholder(cb, vg, x, y, width, height);
		}
		else
		{
			switch (data_.kind)
			{
			case preview_kind::image:
				if (texture_)
					draw_image(cb, vg, x, y, width, height);
				else
					draw_placeholder(cb, vg, x, y, width, height);
				break;
			case preview_kind::model:
				draw_model(cb, vg, x, y, width, height);
				break;
			case preview_kind::material:
				draw_material(cb, vg, x, y, width, height);
				break;
			case preview_kind::text:
				draw_text(cb, vg, x, y, width, height);
				break;
			case preview_kind::audio:
				draw_audio(cb, vg, x, y, width, height);
				break;
			case preview_kind::custom:
				if (texture_)
					draw_image(cb, vg, x, y, width, height);
				else if (!data_.text.empty())
					draw_text(cb, vg, x, y, width, height);
				else
					draw_placeholder(cb, vg, x, y, width, height);
				break;
			case preview_kind::none:
			default:
				draw_placeholder(cb, vg, x, y, width, height);
				break;
			}
		}

		ovg_tools::stroke_round_rect(cb, vg, x + 0.5f * scale,
			y + 0.5f * scale, std::max(1.0f, width - scale),
			std::max(1.0f, height - scale), 4.0f * scale, border, scale);
	}

	void asset_preview::draw_info(ovg_ctx_cb* cb, rvg_t* vg, float x, float y,
		float width, float height)
	{
		if (!cb || !vg || width <= 1.0f || height <= 1.0f)
			return;

		const vgui_theme theme = theme_for(info_);
		const float scale = ui_scale();
		const float line_h = 14.0f * scale;
		float cursor_y = y + 2.0f * scale;

		if (!error_.empty())
		{
			vgui_custom_draw_text(info_, cb, vg, failed_text_.c_str(), x, cursor_y,
				width, line_h, theme.modal_danger, 0.0f, 0.5f);
			cursor_y += line_h;
			vgui_custom_draw_text(info_, cb, vg, error_.c_str(), x, cursor_y,
				width, line_h, theme.disabled, 0.0f, 0.5f);
			return;
		}

		if (loading_)
		{
			vgui_custom_draw_text(info_, cb, vg, loading_text_.c_str(), x, cursor_y,
				width, line_h, theme.disabled, 0.0f, 0.5f);
			return;
		}

		const int rows = std::max(1,
			(int)((height - 2.0f * scale) / line_h));
		const float label_width = 72.0f * scale;
		const float value_x = 74.0f * scale;
		int used = 0;
		for (const field& item : data_.fields)
		{
			if (used >= rows)
				break;
			vgui_custom_draw_text(info_, cb, vg, item.label.c_str(), x, cursor_y,
				label_width, line_h, theme.disabled, 0.0f, 0.5f);
			vgui_custom_draw_text(info_, cb, vg, item.value.c_str(),
				x + value_x, cursor_y, std::max(1.0f, width - value_x),
				line_h, theme.text, 0.0f, 0.5f);
			cursor_y += line_h;
			++used;
		}

		if (used == 0)
		{
			const std::string text = status_.empty() ? empty_text_ : status_;
			vgui_custom_draw_text(info_, cb, vg, text.c_str(), x, cursor_y,
				width, line_h, theme.disabled, 0.0f, 0.5f);
		}
	}

	void asset_preview::draw_image(ovg_ctx_cb* cb, rvg_t* vg, float x, float y,
		float width, float height)
	{
		const float scale = ui_scale();
		const float inset = 5.0f * scale;
		const float ix = x + inset;
		const float iy = y + inset;
		const float iw = std::max(1.0f, width - inset * 2.0f);
		const float ih = std::max(1.0f, height - inset * 2.0f);
		ovg_tools::checkerboard(cb, vg, ix, iy, iw, ih, 8.0f * scale,
			0xFF3A3F46u, 0xFF4A5058u);
		vgui_custom_draw_texture(stage_, cb, vg, texture_, ix, iy, iw, ih, 0xFFFFFFFF);
	}

	void asset_preview::draw_model(ovg_ctx_cb* cb, rvg_t* vg, float x, float y,
		float width, float height)
	{
		const vgui_theme theme = theme_for(stage_);
		const uint32_t accent = to_ovg(data_.accent ? data_.accent : theme.accent);
		const uint32_t grid_minor = with_alpha(to_ovg(theme.panel_border), 0.45f);
		const uint32_t grid_major = with_alpha(to_ovg(theme.panel_border), 0.8f);
		const float scale = ui_scale();
		const float spacing = std::max(14.0f * scale, height / 6.0f);
		const float grid_inset = 4.0f * scale;
		ovg_tools::canvas_grid(cb, vg, x + grid_inset, y + grid_inset,
			std::max(1.0f, width - grid_inset * 2.0f),
			std::max(1.0f, height - grid_inset * 2.0f),
			spacing, grid_minor, 4, grid_major);

		const float cx = x + width * 0.5f;
		const float cy = y + height * 0.58f;
		const float s = std::min(width, height) * 0.23f;
		if (s <= 1.0f)
			return;

		const uint32_t top = mix_color(accent, 0xFFFFFFFF, 0.30f);
		const uint32_t left = mix_color(accent, 0xFF000000, 0.18f);
		const uint32_t right = mix_color(accent, 0xFF000000, 0.48f);
		const uint32_t edge = with_alpha(accent, 0.95f);

		ovg_tools::fill_triangle(cb, vg, cx, cy - s, cx + s, cy - s * 0.5f,
			cx, cy, top);
		ovg_tools::fill_triangle(cb, vg, cx, cy - s, cx - s, cy - s * 0.5f,
			cx, cy, top);
		ovg_tools::fill_triangle(cb, vg, cx - s, cy - s * 0.5f, cx, cy,
			cx, cy + s, left);
		ovg_tools::fill_triangle(cb, vg, cx - s, cy - s * 0.5f, cx - s,
			cy + s * 0.5f, cx, cy + s, left);
		ovg_tools::fill_triangle(cb, vg, cx + s, cy - s * 0.5f, cx, cy,
			cx, cy + s, right);
		ovg_tools::fill_triangle(cb, vg, cx + s, cy - s * 0.5f, cx + s,
			cy + s * 0.5f, cx, cy + s, right);

		ovg_tools::line(cb, vg, cx, cy - s, cx, cy, edge, 1.2f * scale);
		ovg_tools::line(cb, vg, cx - s, cy - s * 0.5f, cx, cy, edge, 1.2f * scale);
		ovg_tools::line(cb, vg, cx + s, cy - s * 0.5f, cx, cy, edge, 1.2f * scale);
		ovg_tools::line(cb, vg, cx - s, cy - s * 0.5f, cx - s, cy + s * 0.5f, edge, 1.2f * scale);
		ovg_tools::line(cb, vg, cx + s, cy - s * 0.5f, cx + s, cy + s * 0.5f, edge, 1.2f * scale);
		ovg_tools::line(cb, vg, cx - s, cy + s * 0.5f, cx, cy + s, edge, 1.2f * scale);
		ovg_tools::line(cb, vg, cx + s, cy + s * 0.5f, cx, cy + s, edge, 1.2f * scale);
		ovg_tools::line(cb, vg, cx, cy + s, cx, cy, edge, 1.2f * scale);
	}

	void asset_preview::draw_material(ovg_ctx_cb* cb, rvg_t* vg, float x, float y,
		float width, float height)
	{
		const float scale = ui_scale();
		const float inset = 5.0f * scale;
		const float ix = x + inset;
		const float iy = y + inset;
		const float iw = std::max(1.0f, width - inset * 2.0f);
		const float ih = std::max(1.0f, height - inset * 2.0f);
		ovg_tools::checkerboard(cb, vg, ix, iy, iw, ih, 8.0f * scale,
			0xFF3A3F46u, 0xFF4A5058u);

		const vgui_theme theme = theme_for(stage_);
		const uint32_t accent = to_ovg(data_.accent ? data_.accent : theme.accent);
		const uint32_t secondary = to_ovg(data_.secondary ? data_.secondary : theme.panel_bg);

		const float cx = ix + iw * 0.34f;
		const float cy = iy + ih * 0.50f;
		const float r = std::min(iw * 0.26f, ih * 0.36f);
		if (r > 1.0f)
		{
			ovg_tools::fill_circle(cb, vg, cx, cy, r, secondary);
			ovg_tools::fill_circle(cb, vg, cx, cy, r * 0.92f, accent);
			ovg_tools::fill_ellipse(cb, vg, cx - r * 0.28f, cy - r * 0.30f,
				r * 0.30f, r * 0.22f, with_alpha(0xFFFFFFFFu, 0.60f));
			ovg_tools::fill_ellipse(cb, vg, cx + r * 0.22f, cy + r * 0.30f,
				r * 0.38f, r * 0.22f, with_alpha(secondary, 0.35f));
			ovg_tools::stroke_circle(cb, vg, cx, cy, r,
				with_alpha(0xFFFFFFFFu, 0.35f), 1.0f * scale);
		}

		float sx = ix + iw * 0.64f;
		const float sy = iy + ih * 0.25f;
		const float sw = std::min(20.0f * scale, (iw * 0.30f) / 4.0f);
		const float sh = std::min(20.0f * scale, ih * 0.24f);
		std::vector<vgui_color> fallback;
		const std::vector<vgui_color>* swatches = &data_.swatches;
		if (swatches->empty())
		{
			fallback.push_back(data_.accent ? data_.accent : theme.accent);
			fallback.push_back(data_.secondary ? data_.secondary : theme.panel_bg);
			swatches = &fallback;
		}
		int shown = 0;
		for (vgui_color color : *swatches)
		{
			if (shown >= 4 || sx + sw > ix + iw)
				break;
			ovg_tools::fill_round_rect(cb, vg, sx, sy, sw, sh,
				2.0f * scale, to_ovg(color));
			ovg_tools::stroke_round_rect(cb, vg, sx, sy, sw, sh,
				2.0f * scale, with_alpha(0xFFFFFFFFu, 0.35f), scale);
			sx += sw + 4.0f * scale;
			++shown;
		}
	}

	void asset_preview::draw_text(ovg_ctx_cb* cb, rvg_t* vg, float x, float y,
		float width, float height)
	{
		const vgui_theme theme = theme_for(stage_);
		const float scale = ui_scale();
		const float inset = 6.0f * scale;
		const float ix = x + inset;
		const float iy = y + inset;
		const float iw = std::max(1.0f, width - inset * 2.0f);
		const float ih = std::max(1.0f, height - inset * 2.0f);
		ovg_tools::fill_round_rect(cb, vg, ix, iy, iw, ih, 3.0f * scale,
			with_alpha(to_ovg(theme.edit_bg), 0.85f));

		if (data_.text.empty())
		{
			draw_placeholder(cb, vg, x, y, width, height);
			return;
		}
		const float padding_x = 6.0f * scale;
		const float padding_y = 5.0f * scale;
		vgui_custom_draw_text(stage_, cb, vg, data_.text.c_str(),
			ix + padding_x, iy + padding_y,
			std::max(1.0f, iw - padding_x * 2.0f),
			std::max(1.0f, ih - padding_y * 2.0f),
			theme.text, 0.0f, 0.0f);
	}

	void asset_preview::draw_audio(ovg_ctx_cb* cb, rvg_t* vg, float x, float y,
		float width, float height)
	{
		const vgui_theme theme = theme_for(stage_);
		const uint32_t accent = to_ovg(data_.accent ? data_.accent : theme.accent);
		const uint32_t muted = with_alpha(to_ovg(theme.panel_border), 0.65f);

		const float scale = ui_scale();
		const float inset = 7.0f * scale;
		const float iw = std::max(1.0f, width - inset * 2.0f);
		const float ih = std::max(1.0f, height - inset * 2.0f);
		const float cy = y + height * 0.5f;
		ovg_tools::line(cb, vg, x + inset, cy, x + inset + iw, cy, muted,
			scale);

		const int bars = std::max(16, (int)(iw / (5.0f * scale)));
		const float step = iw / (float)bars;
		const float half = ih * 0.42f;
		for (int i = 0; i < bars; ++i)
		{
			float sample = 0.0f;
			if (!data_.waveform.empty())
			{
				const std::size_t index = (std::size_t)((float)i / (float)bars *
					(float)data_.waveform.size());
				sample = std::fabs(data_.waveform[std::min(index,
					data_.waveform.size() - 1)]);
			}
			else
			{
				sample = 0.22f + 0.78f * std::fabs(std::sin((float)i * 0.37f) *
					std::cos((float)i * 0.11f));
			}
			sample = clampf(sample, 0.0f, 1.0f);
			const float bar_h = std::max(1.5f * scale, sample * half * 2.0f);
			ovg_tools::fill_round_rect(cb, vg, x + inset + (float)i * step,
				cy - bar_h * 0.5f, std::max(1.0f, step * 0.62f), bar_h,
				scale, with_alpha(accent, 0.75f));
		}

		const float radius = 9.0f * scale;
		const float cx = x + width - inset - radius;
		ovg_tools::fill_circle(cb, vg, cx, y + inset + radius, radius,
			with_alpha(accent, 0.85f));
		ovg_tools::fill_triangle(cb, vg, cx - 2.5f * scale,
			y + inset + 4.5f * scale, cx + 4.5f * scale,
			y + inset + 9.0f * scale, cx - 2.5f * scale,
			y + inset + 13.5f * scale, with_alpha(0xFFFFFFFFu, 0.9f));
	}

	void asset_preview::draw_placeholder(ovg_ctx_cb* cb, rvg_t* vg, float x, float y,
		float width, float height)
	{
		const vgui_theme theme = theme_for(stage_);
		const uint32_t muted = to_ovg(theme.disabled);
		const uint32_t border = with_alpha(to_ovg(theme.panel_border), 0.9f);
		const float scale = ui_scale();
		const float cx = x + width * 0.5f;
		const float icon_cy = y + height * 0.38f;
		const float size = std::min(32.0f * scale,
			std::min(width, height) * 0.28f);

		const float text_inset = 8.0f * scale;
		const float text_width = std::max(1.0f, width - text_inset * 2.0f);
		const float line_h = 18.0f * scale;

		if (loading_)
		{
			for (int i = 0; i < 3; ++i)
			{
				const float phase = (float)i * 2.0944f;
				const float alpha = 0.35f + 0.65f *
					(0.5f + 0.5f * std::sin(anim_time_ * 6.0f + phase));
				ovg_tools::fill_circle(cb, vg,
					cx + (float)(i - 1) * 12.0f * scale, icon_cy,
					3.2f * scale, with_alpha(to_ovg(theme.accent), alpha));
			}
			vgui_custom_draw_text(stage_, cb, vg, loading_text_.c_str(),
				x + text_inset, y + height * 0.58f, text_width, line_h,
				theme.disabled, 0.5f, 0.5f);
			return;
		}

		if (!error_.empty())
		{
			const float h = size * 0.9f;
			ovg_tools::fill_triangle(cb, vg, cx, icon_cy - h * 0.5f,
				cx + h * 0.58f, icon_cy + h * 0.5f,
				cx - h * 0.58f, icon_cy + h * 0.5f,
				with_alpha(to_ovg(theme.modal_danger), 0.22f));
			ovg_tools::line(cb, vg, cx, icon_cy - h * 0.20f, cx, icon_cy + h * 0.16f,
				to_ovg(theme.modal_danger), 2.0f * scale);
			ovg_tools::fill_circle(cb, vg, cx, icon_cy + h * 0.32f,
				1.6f * scale, to_ovg(theme.modal_danger));
			vgui_custom_draw_text(stage_, cb, vg, failed_text_.c_str(),
				x + text_inset, y + height * 0.52f, text_width, line_h,
				theme.modal_danger, 0.5f, 0.5f);
			vgui_custom_draw_text(stage_, cb, vg, error_.c_str(),
				x + text_inset, y + height * 0.52f + line_h, text_width,
				line_h, theme.disabled, 0.5f, 0.5f);
			return;
		}

		/* Dashed "empty" tile plus a small diamond marker. */
		const float tile = std::min(44.0f * scale,
			std::min(width, height) * 0.34f);
		ovg_tools::dashed_rect(cb, vg, cx - tile * 0.5f, icon_cy - tile * 0.4f,
			tile, tile * 0.8f, border, 1.2f * scale, 5.0f * scale,
			4.0f * scale, 0.0f);
		const float diamond = 8.0f * scale;
		ovg_tools::fill_triangle(cb, vg, cx, icon_cy - diamond, cx + diamond,
			icon_cy, cx, icon_cy + diamond, with_alpha(muted, 0.65f));
		ovg_tools::fill_triangle(cb, vg, cx, icon_cy - diamond, cx - diamond,
			icon_cy, cx, icon_cy + diamond, with_alpha(muted, 0.45f));

		const std::string text = status_.empty() ? empty_text_ : status_;
		vgui_custom_draw_text(stage_, cb, vg, text.c_str(),
			x + text_inset, y + height * 0.55f, text_width, line_h,
			theme.disabled, 0.5f, 0.5f);
		if (!data_.type.empty())
			vgui_custom_draw_text(stage_, cb, vg, data_.type.c_str(),
				x + text_inset, y + height * 0.55f + 17.0f * scale,
				text_width, 16.0f * scale, with_alpha(muted, 0.75f),
				0.5f, 0.5f);
	}

} /* namespace vgui_editor_kit */
