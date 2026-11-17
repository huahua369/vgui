#pragma once
/*
 * file_selector.h - a vgui-native, thumbnail-enabled file/folder picker.
 *
 * The SDL3 backend's native dialogs cannot show thumbnails: SDL3 3.4 exposes
 * only title/location/filters/many/window/accept/cancel properties, with no
 * preview hook.  This component fills that gap with a picker built entirely
 * from vgui widgets, so callers that want an in-dialog thumbnail preview can
 * use it in place of (or alongside) the native dialogs.
 *
 * It reuses editor-kit building blocks rather than reinventing them:
 *   - asset_browser  : grid/list views, breadcrumb navigation, search/type
 *                      filters, selection and the async thumbnail cache;
 *   - vgui_modal_show: modal host with Esc/backdrop handling;
 *   - vgui_file_dialog_post_result: the exact same result contract as the
 *                      native dialogs, so hosts share one callback path.
 *
 * The selector always enumerates directories itself with std::filesystem and
 * asks thumbnail_cache to decode visible entries on a worker thread.  Because
 * the core never decodes images, the selector ships a deterministic procedural
 * loader (colour swatch per file type); hosts may replace it with a real image
 * decoder via set_thumbnail_loader().
 *
 * Lifetime: a selector outlives its modal.  Like the native dialog contract,
 * the result callback is delivered from vgui_update() on a later frame; the
 * selector must stay alive until it fires.  An internal weak alive token makes
 * a premature destruction report a cancellation instead of calling a dangling
 * callback.
 *
 * Created: 2026-11-17
 */
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <vgui/vgui.h>

#include "asset_browser.h"
#include "thumbnail_cache.h"

namespace vgui_editor_kit {

	/* Pure-C-file-dialog-compatible picker with a thumbnail grid. */
	class file_selector
	{
	public:
		/* Signature matches thumbnail_cache::load_handler.  The selector calls
		 * this on a worker thread for each visible file when a real decoder is
		 * installed; returning false triggers the cache's retry policy. */
		using thumbnail_loader = thumbnail_cache::load_handler;

		file_selector();
		~file_selector();
		file_selector(const file_selector&) = delete;
		file_selector& operator=(const file_selector&) = delete;

		/* Creates the detached panel + widget tree.  Call once before show(). */
		void build(vgui_context* ctx);
		vgui_context* context() const noexcept;
		vgui_widget* root() const noexcept;
		asset_browser& browser() noexcept;

		/* Optional custom thumbnail decoder.  When unset, a procedural per-type
		 * swatch is generated (headless-friendly, no external resources). */
		void set_thumbnail_loader(thumbnail_loader loader);

		/* Opens the picker as a context modal.  `desc` follows the same meaning
		 * as vgui_file_dialog_desc: default_location seeds the starting folder,
		 * filters drive the extension drop-down, allow_many enables multi-select
		 * for OPEN/FOLDER.  Returns 1 when shown, 0 when a modal is already open
		 * or build() was not called.  The result is delivered through `fn` from
		 * vgui_update() exactly like a native dialog. */
		int show(vgui_file_dialog_kind kind, const vgui_file_dialog_desc* desc,
			vgui_file_dialog_fn fn, void* user);

		/* True while the picker modal is active. */
		bool active() const noexcept;

		/* ---- Host-facing state, useful for tests and custom host UI ---- */
		vgui_file_dialog_kind kind() const noexcept;
		const std::string& current_path() const;
		std::vector<std::string> selected_paths() const;
		/* Enumerates one directory into (name, is_folder) pairs, folders first
		 * then case-insensitive by name.  `pattern` is a semicolon glob list
		 * ("*.png;*.jpg" or "png;jpg"); empty/"*" accepts every file.  Folders
		 * are always included.  Returns false when the folder cannot be read. */
		static bool enumerate(const std::string& folder, const std::string& pattern,
			std::vector<std::pair<std::string, bool>>* out);

	private:
		class impl;
		std::unique_ptr<impl> impl_;
	};

} // namespace vgui_editor_kit
