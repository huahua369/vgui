/* gui/editor/editor_c_api_thumbnails.cpp - C facade for the thumbnail cache and asset preview declared in <vgui/vgui_editor.h>.
 *
 * The load, disk-read, disk-write and preview loaders run on a worker thread,
 * so every bridge copies what crosses the boundary before returning: the C
 * strings become kit std::string and the host pixel/array buffers become
 * kit-owned vectors, because the host owns those buffers only until its
 * callback returns.  Nothing in a worker lambda may touch the handle, so each
 * bridge captures the function pointer plus one shared user owner by value.
 * set_disk_cache installs two callbacks for one user pointer, hence a single
 * owner releases it exactly once.  The C preview struct carries no texture
 * format, so image pixels always cross as RGBA8. */
#include "editor_c_api.h"
#include "editor_c_api_borrow.h"

#include <new>
#include <utility>
#include <vector>

#include <vgui/editor/thumbnail_cache.h>
#include <vgui/editor/asset_preview.h>

using namespace vgui_editor_kit;

/* -------------------------------------------------------------------------- */
/* Handle storage                                                            */
/* -------------------------------------------------------------------------- */

struct vgui_editor_thumbnails : vgeditor::c_object<thumbnail_cache> {
	using c_object::c_object;

	vgeditor::callback_slot<vgui_editor_thumbnail_load_fn> load;
	/* The read and write callbacks share one user pointer, so the read slot owns
	 * it and the write lambda captures a copy of that same owner. */
	vgeditor::callback_slot<vgui_editor_thumbnail_disk_read_fn> disk_read;
};

struct vgui_editor_asset_preview : vgeditor::c_object<asset_preview> {
	using c_object::c_object;

	vgeditor::callback_slot<vgui_editor_preview_load_fn> load;
};

namespace {

	/* The C enums mirror the kit ordering; these asserts turn a future reorder in
	 * either header into a build error rather than a silent mismatch. */
	static_assert((int)VGUI_EDITOR_THUMB_MISSING ==
		(int)thumbnail_cache::state::missing, "thumbnail state drift");
	static_assert((int)VGUI_EDITOR_THUMB_QUEUED ==
		(int)thumbnail_cache::state::queued, "thumbnail state drift");
	static_assert((int)VGUI_EDITOR_THUMB_LOADING ==
		(int)thumbnail_cache::state::loading, "thumbnail state drift");
	static_assert((int)VGUI_EDITOR_THUMB_READY ==
		(int)thumbnail_cache::state::ready, "thumbnail state drift");
	static_assert((int)VGUI_EDITOR_THUMB_FAILED ==
		(int)thumbnail_cache::state::failed, "thumbnail state drift");

	static_assert((int)VGUI_EDITOR_THUMB_LOW ==
		(int)thumbnail_cache::priority::low, "thumbnail priority drift");
	static_assert((int)VGUI_EDITOR_THUMB_HIGH ==
		(int)thumbnail_cache::priority::high, "thumbnail priority drift");

	static_assert((int)VGUI_EDITOR_PREVIEW_NONE ==
		(int)asset_preview::preview_kind::none, "preview kind drift");
	static_assert((int)VGUI_EDITOR_PREVIEW_IMAGE ==
		(int)asset_preview::preview_kind::image, "preview kind drift");
	static_assert((int)VGUI_EDITOR_PREVIEW_MODEL ==
		(int)asset_preview::preview_kind::model, "preview kind drift");
	static_assert((int)VGUI_EDITOR_PREVIEW_MATERIAL ==
		(int)asset_preview::preview_kind::material, "preview kind drift");
	static_assert((int)VGUI_EDITOR_PREVIEW_TEXT ==
		(int)asset_preview::preview_kind::text, "preview kind drift");
	static_assert((int)VGUI_EDITOR_PREVIEW_AUDIO ==
		(int)asset_preview::preview_kind::audio, "preview kind drift");
	static_assert((int)VGUI_EDITOR_PREVIEW_CUSTOM ==
		(int)asset_preview::preview_kind::custom, "preview kind drift");

	/* ---- thumbnail cache ------------------------------------------------ */

	thumbnail_cache::priority to_priority(vgui_editor_thumbnail_priority prio)
	{
		if ((int)prio < (int)VGUI_EDITOR_THUMB_LOW ||
			(int)prio > (int)VGUI_EDITOR_THUMB_HIGH)
			return thumbnail_cache::priority::high;
		return (thumbnail_cache::priority)(int)prio;
	}

	vgui_editor_thumbnail_state from_state(thumbnail_cache::state state)
	{
		if ((int)state < (int)thumbnail_cache::state::missing ||
			(int)state > (int)thumbnail_cache::state::failed)
			return VGUI_EDITOR_THUMB_MISSING;
		return (vgui_editor_thumbnail_state)(int)state;
	}

	thumbnail_cache::request to_request(const vgui_editor_thumbnail_request* src)
	{
		thumbnail_cache::request out;
		if (!src)
			return out;
		const vgui_editor_thumbnail_request d = vgeditor::take_desc(src);
		out.key = vgeditor::to_text(d.key);
		out.name = vgeditor::to_text(d.name);
		out.type = vgeditor::to_text(d.type);
		return out;
	}

	thumbnail_cache::options to_options(const vgui_editor_thumbnail_options* src)
	{
		thumbnail_cache::options out;
		if (!src)
			return out;
		const vgui_editor_thumbnail_options d = vgeditor::take_desc(src);
		out.memory_limit = d.memory_limit;
		out.max_attempts = d.max_attempts;
		out.retry_delay = d.retry_delay;
		out.disk_cache = d.disk_cache != 0;
		return out;
	}

	void init_image(vgui_editor_thumbnail_image* image)
	{
		std::memset(image, 0, sizeof(*image));
		image->struct_size = (uint32_t)sizeof(*image);
		image->format = VGUI_TEXTURE_FORMAT_RGBA8;
	}

	/* The kit request strings are stable for the duration of the worker call, so
	 * the C view borrows them; the host must not keep the pointers. */
	void fill_c_request(vgui_editor_thumbnail_request* out,
		const thumbnail_cache::request& req)
	{
		std::memset(out, 0, sizeof(*out));
		out->struct_size = (uint32_t)sizeof(*out);
		out->key = req.key.c_str();
		out->name = req.name.c_str();
		out->type = req.type.c_str();
	}

	/* Read-only view of a kit image for the disk-write callback. */
	void fill_c_image(vgui_editor_thumbnail_image* out,
		const thumbnail_cache::image& value)
	{
		init_image(out);
		out->width = value.width;
		out->height = value.height;
		out->stride = value.stride;
		out->format = value.format;
		out->pixels = value.pixels.empty() ? nullptr : value.pixels.data();
		out->pixel_count = value.pixels.size();
		out->error = value.error.c_str();
	}

	/* Takes ownership of the host's bytes: the buffer and the error text are only
	 * the host's until its callback returns, so both are copied here. */
	void copy_c_image(const vgui_editor_thumbnail_image* src,
		thumbnail_cache::image* out)
	{
		if (!out)
			return;
		const vgui_editor_thumbnail_image d = vgeditor::take_desc(src);
		out->width = d.width;
		out->height = d.height;
		out->stride = d.stride;
		out->format = d.format;
		out->error = vgeditor::to_text(d.error);
		out->pixels.clear();
		if (d.pixels && d.pixel_count > 0)
			out->pixels.assign(d.pixels, d.pixels + d.pixel_count);
	}

	/* ---- asset preview -------------------------------------------------- */

	asset_preview::preview_kind to_kind(vgui_editor_preview_kind kind)
	{
		if ((int)kind < (int)VGUI_EDITOR_PREVIEW_NONE ||
			(int)kind > (int)VGUI_EDITOR_PREVIEW_CUSTOM)
			return asset_preview::preview_kind::none;
		return (asset_preview::preview_kind)(int)kind;
	}

	void init_preview_data(vgui_editor_preview_data* data)
	{
		std::memset(data, 0, sizeof(*data));
		data->struct_size = (uint32_t)sizeof(*data);
		data->kind = VGUI_EDITOR_PREVIEW_NONE;
		/* The same colors the kit's data value defaults to. */
		data->accent = 0xFF409EFF;
		data->secondary = 0xFF2E3440;
	}

	void fill_c_preview_request(vgui_editor_preview_request* out,
		const asset_preview::request& req)
	{
		std::memset(out, 0, sizeof(*out));
		out->struct_size = (uint32_t)sizeof(*out);
		out->id = req.id;
		out->name = req.name.c_str();
		out->type = req.type.c_str();
	}

	/* Copies every string, byte and array the host produced into the kit value;
	 * the C buffer has no format field, so pixels are RGBA8. */
	void copy_c_preview_data(const vgui_editor_preview_data* src,
		asset_preview::data* out)
	{
		if (!out)
			return;
		const vgui_editor_preview_data d = vgeditor::take_desc(src);
		out->kind = to_kind(d.kind);
		out->name = vgeditor::to_text(d.name);
		out->type = vgeditor::to_text(d.type);
		out->detail = vgeditor::to_text(d.detail);
		out->error = vgeditor::to_text(d.error);
		out->text = vgeditor::to_text(d.text);
		out->accent = d.accent;
		out->secondary = d.secondary;

		out->fields.clear();
		if (d.fields) {
			out->fields.reserve(d.field_count);
			for (size_t i = 0; i < d.field_count; ++i) {
				const vgui_editor_preview_field field = vgeditor::take_desc(
					d.fields + i);
				asset_preview::field value;
				value.label = vgeditor::to_text(field.label);
				value.value = vgeditor::to_text(field.value);
				out->fields.push_back(std::move(value));
			}
		}

		out->image.width = d.image_width;
		out->image.height = d.image_height;
		out->image.stride = d.image_stride;
		out->image.format = VGUI_TEXTURE_FORMAT_RGBA8;
		out->image.pixels.clear();
		if (d.image_pixels && d.image_pixel_count > 0) {
			out->image.pixels.assign(d.image_pixels,
				d.image_pixels + d.image_pixel_count);
		}

		out->swatches.clear();
		if (d.swatches && d.swatch_count > 0)
			out->swatches.assign(d.swatches, d.swatches + d.swatch_count);

		out->waveform.clear();
		if (d.waveform && d.waveform_count > 0)
			out->waveform.assign(d.waveform, d.waveform + d.waveform_count);
	}

} /* namespace */

extern "C" {

	void vgui_editor_thumbnail_options_default(
		vgui_editor_thumbnail_options* options)
	{
		if (!options)
			return;
		std::memset(options, 0, sizeof(*options));
		options->struct_size = (uint32_t)sizeof(*options);
		/* The kit's own defaults, so an untouched struct behaves like a
		 * default-constructed options value. */
		options->memory_limit = 256;
		options->max_attempts = 2;
		options->retry_delay = 0.5f;
		options->disk_cache = 1;
	}

	void vgui_editor_thumbnail_request_default(
		vgui_editor_thumbnail_request* request)
	{
		if (!request)
			return;
		std::memset(request, 0, sizeof(*request));
		request->struct_size = (uint32_t)sizeof(*request);
	}

	void vgui_editor_thumbnail_image_default(vgui_editor_thumbnail_image* image)
	{
		if (!image)
			return;
		/* Empty: the loader decides size, format and pixels. */
		init_image(image);
	}

	/* -------------------------------------------------------------------- */
	/* Thumbnail cache                                                      */
	/* -------------------------------------------------------------------- */

	vgui_editor_thumbnails* vgui_editor_thumbnails_create(vgui_context* ui)
	{
		/* The kit accepts a NULL context; upload then waits for
		 * vgui_editor_thumbnails_set_context(). */
		return vgeditor::own_object<vgui_editor_thumbnails>(
			new (std::nothrow) thumbnail_cache(ui));
	}

	void vgui_editor_thumbnails_destroy(vgui_editor_thumbnails* cache)
	{
		/* A browser-borrowed handle must not tear down the browser's cache. */
		if (!cache || !cache->owns_object())
			return;
		/* Dropping the handlers first stops the worker from calling into memory
		 * the host may free right after this returns. */
		cache->impl.set_load_handler(nullptr);
		cache->impl.clear_disk_cache();
		cache->load.clear();
		cache->disk_read.clear();
		delete cache;
	}

	void vgui_editor_thumbnails_set_context(vgui_editor_thumbnails* cache,
		vgui_context* ui)
	{
		if (!cache)
			return;
		cache->impl.set_context(ui);
	}

	vgui_context* vgui_editor_thumbnails_context(vgui_editor_thumbnails* cache)
	{
		return cache ? cache->impl.context() : nullptr;
	}

	void vgui_editor_thumbnails_set_options(vgui_editor_thumbnails* cache,
		const vgui_editor_thumbnail_options* options)
	{
		if (!cache || !options)
			return;
		cache->impl.set_options(to_options(options));
	}

	void vgui_editor_thumbnails_get_options(vgui_editor_thumbnails* cache,
		vgui_editor_thumbnail_options* out)
	{
		if (!cache || !out)
			return;
		const thumbnail_cache::options& opts = cache->impl.settings();
		std::memset(out, 0, sizeof(*out));
		out->struct_size = (uint32_t)sizeof(*out);
		out->memory_limit = opts.memory_limit;
		out->max_attempts = opts.max_attempts;
		out->retry_delay = opts.retry_delay;
		out->disk_cache = opts.disk_cache ? 1 : 0;
	}

	void vgui_editor_thumbnails_set_load_handler(vgui_editor_thumbnails* cache,
		vgui_editor_thumbnail_load_fn fn, void* user,
		vgui_editor_release_fn release_user)
	{
		if (!cache)
			return;
		cache->load.set(fn, user, release_user);
		if (!fn) {
			cache->impl.set_load_handler(nullptr);
			return;
		}
		vgeditor::user_owner_ptr owner = cache->load.owner;
		cache->impl.set_load_handler([fn, owner](
			const thumbnail_cache::request& req, thumbnail_cache::image* out) {
			/* Worker thread: only the copied result survives this call. */
			vgui_editor_thumbnail_request view;
			fill_c_request(&view, req);
			vgui_editor_thumbnail_image result;
			init_image(&result);
			const bool ok = fn(owner->user, &view, &result) != 0;
			copy_c_image(&result, out);
			return ok;
		});
	}

	void vgui_editor_thumbnails_set_disk_cache(vgui_editor_thumbnails* cache,
		vgui_editor_thumbnail_disk_read_fn read,
		vgui_editor_thumbnail_disk_write_fn write, void* user,
		vgui_editor_release_fn release_user)
	{
		if (!cache)
			return;
		/* One owner for both callbacks so the shared user pointer is released
		 * exactly once, even when only one direction is installed. */
		cache->disk_read.set(read, user, release_user);
		if (!read && !write) {
			cache->impl.set_disk_cache(nullptr, nullptr);
			return;
		}
		vgeditor::user_owner_ptr owner = cache->disk_read.owner;
		thumbnail_cache::disk_read_handler read_handler;
		thumbnail_cache::disk_write_handler write_handler;
		if (read) {
			read_handler = [read, owner](const thumbnail_cache::request& req,
				thumbnail_cache::image* out) {
				vgui_editor_thumbnail_request view;
				fill_c_request(&view, req);
				vgui_editor_thumbnail_image result;
				init_image(&result);
				const bool ok = read(owner->user, &view, &result) != 0;
				copy_c_image(&result, out);
				return ok;
			};
		}
		if (write) {
			write_handler = [write, owner](const thumbnail_cache::request& req,
				const thumbnail_cache::image& value) {
				vgui_editor_thumbnail_request view;
				fill_c_request(&view, req);
				/* The kit owns these bytes for the duration of the call. */
				vgui_editor_thumbnail_image image;
				fill_c_image(&image, value);
				return write(owner->user, &view, &image) != 0;
			};
		}
		cache->impl.set_disk_cache(std::move(read_handler),
			std::move(write_handler));
	}

	void vgui_editor_thumbnails_clear_disk_cache(vgui_editor_thumbnails* cache)
	{
		if (!cache)
			return;
		cache->impl.clear_disk_cache();
		cache->disk_read.clear();
	}

	int vgui_editor_thumbnails_configured(vgui_editor_thumbnails* cache)
	{
		return cache && cache->impl.configured() ? 1 : 0;
	}

	int vgui_editor_thumbnails_tick(vgui_editor_thumbnails* cache,
		double delta_time)
	{
		if (!cache)
			return 0;
		return cache->impl.tick((float)delta_time) ? 1 : 0;
	}

	int vgui_editor_thumbnails_poll(vgui_editor_thumbnails* cache)
	{
		return cache && cache->impl.poll() ? 1 : 0;
	}

	int vgui_editor_thumbnails_idle(vgui_editor_thumbnails* cache)
	{
		return cache && cache->impl.idle() ? 1 : 0;
	}

	void vgui_editor_thumbnails_enqueue(vgui_editor_thumbnails* cache,
		const vgui_editor_thumbnail_request* request,
		vgui_editor_thumbnail_priority priority)
	{
		if (!cache || !request)
			return;
		/* An empty key is invalid in the kit as well. */
		cache->impl.enqueue(to_request(request), to_priority(priority));
	}

	void vgui_editor_thumbnails_cancel(vgui_editor_thumbnails* cache,
		const char* key)
	{
		if (!cache)
			return;
		cache->impl.cancel(vgeditor::to_text(key));
	}

	void vgui_editor_thumbnails_cancel_all(vgui_editor_thumbnails* cache)
	{
		if (!cache)
			return;
		cache->impl.cancel_all();
	}

	void vgui_editor_thumbnails_retry(vgui_editor_thumbnails* cache,
		const char* key)
	{
		if (!cache)
			return;
		cache->impl.retry(vgeditor::to_text(key));
	}

	void vgui_editor_thumbnails_erase(vgui_editor_thumbnails* cache,
		const char* key)
	{
		if (!cache)
			return;
		cache->impl.erase(vgeditor::to_text(key));
	}

	vgui_texture* vgui_editor_thumbnails_lookup(vgui_editor_thumbnails* cache,
		const char* key)
	{
		if (!cache || !key)
			return nullptr;
		return cache->impl.lookup(vgeditor::to_text(key));
	}

	vgui_texture* vgui_editor_thumbnails_texture(vgui_editor_thumbnails* cache,
		const char* key)
	{
		if (!cache || !key)
			return nullptr;
		return cache->impl.texture(vgeditor::to_text(key));
	}

	vgui_editor_thumbnail_state vgui_editor_thumbnails_state(
		vgui_editor_thumbnails* cache, const char* key)
	{
		if (!cache || !key)
			return VGUI_EDITOR_THUMB_MISSING;
		return from_state(cache->impl.state_of(vgeditor::to_text(key)));
	}

	size_t vgui_editor_thumbnails_error(vgui_editor_thumbnails* cache,
		const char* key, char* out, size_t cap)
	{
		if (!cache || !key)
			return 0;
		const std::string& text = cache->impl.error(vgeditor::to_text(key));
		return vgeditor::copy_string(out, cap, text.c_str());
	}

	void vgui_editor_thumbnails_get_stats(vgui_editor_thumbnails* cache,
		vgui_editor_thumbnail_stats* out)
	{
		if (!cache || !out)
			return;
		const thumbnail_cache::statistics value = cache->impl.stats();
		std::memset(out, 0, sizeof(*out));
		out->struct_size = (uint32_t)sizeof(*out);
		out->memory_entries = value.memory_entries;
		out->memory_limit = value.memory_limit;
		out->pending = value.pending;
		out->memory_hits = (unsigned long long)value.memory_hits;
		out->disk_hits = (unsigned long long)value.disk_hits;
		out->loads = (unsigned long long)value.loads;
		out->uploads = (unsigned long long)value.uploads;
		out->failures = (unsigned long long)value.failures;
		out->retries = (unsigned long long)value.retries;
		out->cancellations = (unsigned long long)value.cancellations;
	}

	void vgui_editor_thumbnails_clear(vgui_editor_thumbnails* cache)
	{
		if (!cache)
			return;
		cache->impl.clear();
	}

	/* -------------------------------------------------------------------- */
	/* Asset preview                                                        */
	/* -------------------------------------------------------------------- */

	void vgui_editor_preview_field_default(vgui_editor_preview_field* field)
	{
		if (!field)
			return;
		std::memset(field, 0, sizeof(*field));
		field->struct_size = (uint32_t)sizeof(*field);
	}

	void vgui_editor_preview_data_default(vgui_editor_preview_data* data)
	{
		if (!data)
			return;
		/* Empty value: only kind, colors and the arrays carry the kit defaults. */
		init_preview_data(data);
	}

	vgui_editor_asset_preview* vgui_editor_asset_preview_create(
		vgui_widget* parent)
	{
		if (!parent)
			return nullptr;
		std::unique_ptr<vgui_editor_asset_preview> preview(
			vgeditor::own_object<vgui_editor_asset_preview>(
				new (std::nothrow) asset_preview()));
		if (!preview)
			return nullptr;
		preview->impl.build(parent);
		if (!preview->impl.widget())
			return nullptr;
		return preview.release();
	}

	void vgui_editor_asset_preview_destroy(vgui_editor_asset_preview* preview)
	{
		/* A shell-borrowed handle must not tear down the shell's preview. */
		if (!preview || !preview->owns_object())
			return;
		vgui_widget* widget = preview->impl.widget();
		preview->impl.set_load_handler(nullptr);
		preview->load.clear();
		delete preview;
		if (widget)
			vgui_widget_destroy(widget);
	}

	vgui_widget* vgui_editor_asset_preview_widget(
		vgui_editor_asset_preview* preview)
	{
		return preview ? preview->impl.widget() : nullptr;
	}

	vgui_widget* vgui_editor_asset_preview_stage(
		vgui_editor_asset_preview* preview)
	{
		return preview ? preview->impl.stage() : nullptr;
	}

	void vgui_editor_asset_preview_set_load_handler(
		vgui_editor_asset_preview* preview, vgui_editor_preview_load_fn fn,
		void* user, vgui_editor_release_fn release_user)
	{
		if (!preview)
			return;
		preview->load.set(fn, user, release_user);
		if (!fn) {
			/* An empty handler makes the kit show "No preview available". */
			preview->impl.set_load_handler(nullptr);
			return;
		}
		vgeditor::user_owner_ptr owner = preview->load.owner;
		preview->impl.set_load_handler([fn, owner](
			const asset_preview::request& req, asset_preview::data* out) {
			/* Worker thread: the whole result is copied below. */
			vgui_editor_preview_request view;
			fill_c_preview_request(&view, req);
			vgui_editor_preview_data result;
			init_preview_data(&result);
			const bool ok = fn(owner->user, &view, &result) != 0;
			copy_c_preview_data(&result, out);
			return ok;
		});
	}

	void vgui_editor_asset_preview_set_item(vgui_editor_asset_preview* preview,
		vgui_item_id item, const char* name, const char* type)
	{
		if (!preview)
			return;
		/* NULL keeps the values the current request carries, which is what the
		 * browser handed the preview when it selected this item. */
		const asset_preview::data& current = preview->impl.current_data();
		std::string next_name = name ? vgeditor::to_text(name) : current.name;
		std::string next_type = type ? vgeditor::to_text(type) : current.type;
		preview->impl.set_item(item, std::move(next_name), std::move(next_type));
	}

	void vgui_editor_asset_preview_clear(vgui_editor_asset_preview* preview)
	{
		if (!preview)
			return;
		preview->impl.clear();
	}

	void vgui_editor_asset_preview_reload(vgui_editor_asset_preview* preview)
	{
		if (!preview)
			return;
		preview->impl.reload();
	}

	vgui_item_id vgui_editor_asset_preview_item(
		vgui_editor_asset_preview* preview)
	{
		return preview ? preview->impl.item() : 0;
	}

	int vgui_editor_asset_preview_loading(vgui_editor_asset_preview* preview)
	{
		return preview && preview->impl.loading() ? 1 : 0;
	}

	int vgui_editor_asset_preview_has_error(vgui_editor_asset_preview* preview)
	{
		return preview && preview->impl.has_error() ? 1 : 0;
	}

	size_t vgui_editor_asset_preview_error(vgui_editor_asset_preview* preview,
		char* out, size_t cap)
	{
		if (!preview)
			return 0;
		return vgeditor::copy_string(out, cap, preview->impl.error().c_str());
	}

	size_t vgui_editor_asset_preview_status_text(
		vgui_editor_asset_preview* preview, char* out, size_t cap)
	{
		if (!preview)
			return 0;
		return vgeditor::copy_string(out, cap,
			preview->impl.status_text().c_str());
	}

	vgui_texture* vgui_editor_asset_preview_texture(
		vgui_editor_asset_preview* preview)
	{
		return preview ? preview->impl.texture() : nullptr;
	}

	void vgui_editor_asset_preview_set_texts(vgui_editor_asset_preview* preview,
		const char* empty, const char* loading, const char* failed)
	{
		if (!preview)
			return;
		/* NULL becomes the kit's empty string, which restores that default. */
		preview->impl.set_texts(vgeditor::to_text(empty),
			vgeditor::to_text(loading), vgeditor::to_text(failed));
	}

	void vgui_editor_asset_preview_tick(vgui_editor_asset_preview* preview,
		float delta_time)
	{
		if (!preview)
			return;
		preview->impl.tick(delta_time);
	}

	int vgui_editor_asset_preview_poll(vgui_editor_asset_preview* preview)
	{
		return preview && preview->impl.poll() ? 1 : 0;
	}

} /* extern "C" */

namespace vgeditor {

	vgui_editor_thumbnails* borrow_thumbnails(vek::thumbnail_cache& cache)
	{
		return borrow_object<vgui_editor_thumbnails>(cache);
	}

	vgui_editor_asset_preview* borrow_asset_preview(vek::asset_preview& preview)
	{
		return borrow_object<vgui_editor_asset_preview>(preview);
	}

	void release_borrowed_thumbnails(vgui_editor_thumbnails* handle)
	{
		/* Wrapper only: a borrowed handle's C++ object belongs to its container. */
		if (handle && !handle->owns_object())
			delete handle;
	}


	void release_borrowed_asset_preview(vgui_editor_asset_preview* handle)
	{
		/* Wrapper only: a borrowed handle's C++ object belongs to its container. */
		if (handle && !handle->owns_object())
			delete handle;
	}


} /* namespace vgeditor */
