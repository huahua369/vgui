#pragma once
/*
 * vgui C++ convenience wrapper for the file dialog API.
 *
 * The C API in <vgui/vgui.h> is callback- and string-based on purpose: it must
 * stay usable from plain C and must not leak SDL3 or any other platform type.
 * This header wraps it with std::string / std::function / std::vector so C++
 * hosts can write:
 *
 *     vgui::file_dialog_options options;
 *     options.title = "Export scene";
 *     options.filters = { { "Scene", "*.json" }, { "All files", "*" } };
 *     options.default_name = "scene.json";
 *     vgui::save_file(options, [](const vgui::file_dialog_result& result) {
 *         if (result.ok()) export_scene(result.path());
 *     });
 *
 * Callbacks are delivered from vgui_update(), so keep calling it every frame.
 *
 * Created: 2026-11-03
 */
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <vgui/vgui.h>

namespace vgui
{
	/* One type drop-down entry.  `pattern` accepts "*.png;*.jpg" as well as
	 * "png;jpg"; "*" matches every file. */
	struct file_filter
	{
		std::string name;
		std::string pattern;
	};

	struct file_dialog_options
	{
		std::string title;
		std::string default_location;  // starting folder (save too: file path)
		std::string default_name;      // suggested file name, SAVE only
		std::string accept_label;      // e.g. "Export"
		std::string cancel_label;
		std::vector<file_filter> filters;
		bool allow_many = false;       // OPEN / FOLDER only
	};

	/* Result of one dialog.  `paths` is empty both when the user cancelled and
	 * when the dialog failed, so test `error` first. */
	struct file_dialog_result
	{
		vgui_file_dialog_kind kind = VGUI_FILE_DIALOG_OPEN;
		std::vector<std::string> paths;
		std::string error;

		bool failed() const { return !error.empty(); }
		bool cancelled() const { return error.empty() && paths.empty(); }
		bool ok() const { return error.empty() && !paths.empty(); }
		/* First selected path, or an empty string when nothing was selected. */
		std::string path() const { return paths.empty() ? std::string() : paths.front(); }
	};

	using file_dialog_callback = std::function<void(const file_dialog_result&)>;

	namespace detail
	{
		/* The C bridge only carries a void*, so the std::function travels on the
		 * heap and is freed by the trampoline after the single invocation. */
		struct file_dialog_state
		{
			file_dialog_callback callback;
		};

		inline void file_dialog_trampoline(void* user, vgui_file_dialog_kind kind,
			const char* const* paths, int count, const char* error)
		{
			std::unique_ptr<file_dialog_state> state(static_cast<file_dialog_state*>(user));
			file_dialog_result result;
			result.kind = kind;
			if (error) result.error = error;
			if (paths)
				for (int i = 0; i < count; ++i)
					if (paths[i]) result.paths.emplace_back(paths[i]);
			if (state && state->callback)
				state->callback(result);
		}

		inline bool show_file_dialog(vgui_file_dialog_kind kind,
			const file_dialog_options& options, file_dialog_callback callback)
		{
			if (!callback) return false;

			// The C descriptor borrows these strings only for the duration of the
			// show call; the backend copies whatever it keeps.
			std::vector<vgui_file_filter> filters;
			filters.reserve(options.filters.size());
			for (const file_filter& entry : options.filters)
				filters.push_back({ entry.name.c_str(), entry.pattern.c_str() });

			vgui_file_dialog_desc desc;
			vgui_file_dialog_desc_init(&desc);
			desc.title = options.title.empty() ? nullptr : options.title.c_str();
			desc.default_location =
				options.default_location.empty() ? nullptr : options.default_location.c_str();
			desc.default_name =
				options.default_name.empty() ? nullptr : options.default_name.c_str();
			desc.accept_label =
				options.accept_label.empty() ? nullptr : options.accept_label.c_str();
			desc.cancel_label =
				options.cancel_label.empty() ? nullptr : options.cancel_label.c_str();
			desc.filters = filters.empty() ? nullptr : filters.data();
			desc.filter_count = (int)filters.size();
			desc.allow_many = options.allow_many ? 1 : 0;

			auto* state = new file_dialog_state{ std::move(callback) };
			if (!vgui_file_dialog_show(kind, &desc, &file_dialog_trampoline, state))
			{
				delete state;
				return false;
			}
			return true;
		}
	} // namespace detail

	/* Non-zero when a backend provider is installed (the SDL3 backend always
	 * installs one).  When it is 0 the show helpers return false and the
	 * callback never runs, so hosts can fall back to a manual path field. */
	inline bool file_dialog_available()
	{
		return vgui_file_dialog_available() != 0;
	}

	inline bool open_file(const file_dialog_options& options, file_dialog_callback callback)
	{
		return detail::show_file_dialog(VGUI_FILE_DIALOG_OPEN, options, std::move(callback));
	}

	inline bool save_file(const file_dialog_options& options, file_dialog_callback callback)
	{
		return detail::show_file_dialog(VGUI_FILE_DIALOG_SAVE, options, std::move(callback));
	}

	inline bool select_folder(const file_dialog_options& options, file_dialog_callback callback)
	{
		return detail::show_file_dialog(VGUI_FILE_DIALOG_FOLDER, options, std::move(callback));
	}

	/* Escape hatch for hosts that want to keep the C-level callback. */
	inline bool show_file_dialog(vgui_file_dialog_kind kind,
		const file_dialog_options& options, file_dialog_callback callback)
	{
		return detail::show_file_dialog(kind, options, std::move(callback));
	}
} // namespace vgui
