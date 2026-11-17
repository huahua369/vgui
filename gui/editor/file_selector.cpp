/*
 * file_selector.cpp - vgui-native thumbnail file/folder picker.
 *
 * See include/vgui/editor/file_selector.h for the contract.  The picker is a
 * thin modal host around asset_browser plus a filesystem navigate handler;
 * results are reported through vgui_file_dialog_post_result so hosts share a
 * single callback path with the native (SDL3) dialogs.
 */
#include "pch.h"
#include <vgui/editor/file_selector.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace vgui_editor_kit {
namespace {

	namespace fs = std::filesystem;

	/* Lower-case copy for case-insensitive sorting / matching. */
	std::string ascii_lower(const std::string& s)
	{
		std::string out = s;
		for (char& c : out)
			c = (char)std::tolower((unsigned char)c);
		return out;
	}

	/* Split a semicolon glob list into lower-case extensions without leading
	 * "*" or "."; an empty list or "*" means "match everything". */
	std::vector<std::string> parse_pattern(const std::string& pattern)
	{
		std::vector<std::string> exts;
		std::size_t start = 0;
		while (start <= pattern.size())
		{
			std::size_t end = pattern.find(';', start);
			if (end == std::string::npos) end = pattern.size();
			std::string token = pattern.substr(start, end - start);
			start = end + 1;
			// trim spaces
			while (!token.empty() && std::isspace((unsigned char)token.front()))
				token.erase(token.begin());
			while (!token.empty() && std::isspace((unsigned char)token.back()))
				token.pop_back();
			while (!token.empty() && token.front() == '*') token.erase(token.begin());
			while (!token.empty() && token.front() == '.') token.erase(token.begin());
			token = ascii_lower(token);
			if (!token.empty() && token != "*" && token != "*.*")
				exts.push_back(token);
		}
		return exts;
	}

	bool matches_pattern(const std::string& filename,
		const std::vector<std::string>& exts)
	{
		if (exts.empty()) return true;  // "*" / empty
		const fs::path ext = fs::path(filename).extension();
		std::string e = ascii_lower(ext.string());
		while (!e.empty() && e.front() == '.') e.erase(e.begin());
		for (const std::string& want : exts)
			if (e == want) return true;
		return false;
	}

	/* A stable numeric id for every (folder, name, is_folder) triple. */
	vgui_item_id item_id_for(const std::string& path_id, vgui_item_id index)
	{
		std::uint64_t h = 1469598103934665603ull;  // FNV-1a
		for (unsigned char c : path_id)
		{
			h ^= c;
			h *= 1099511628211ull;
		}
		h ^= (std::uint64_t)index + 1u;
		h *= 1099511628211ull;
		vgui_item_id id = (vgui_item_id)(h & 0x7FFFFFFFu);
		return id ? id : (vgui_item_id)(index + 1);
	}

	/* Bucket a file name into a coarse type label used for the type filter and
	 * the procedural thumbnail colour. */
	std::string type_for(const std::string& name, bool is_folder)
	{
		if (is_folder) return "Folder";
		const std::string e = ascii_lower(fs::path(name).extension().string());
		if (e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".bmp" ||
			e == ".gif" || e == ".webp" || e == ".tga")
			return "Image";
		if (e == ".json" || e == ".xml" || e == ".yaml" || e == ".yml" ||
			e == ".txt" || e == ".md" || e == ".ini")
			return "Text";
		if (e == ".wav" || e == ".mp3" || e == ".ogg" || e == ".flac")
			return "Audio";
		if (e == ".mp4" || e == ".mov" || e == ".avi" || e == ".webm")
			return "Video";
		return "File";
	}

	std::uint32_t colour_for(const std::string& type)
	{
		if (type == "Folder") return 0xFFF2C14E;
		if (type == "Image") return 0xFF5FB0E8;
		if (type == "Text") return 0xFF9CD67A;
		if (type == "Audio") return 0xFFC78BE8;
		if (type == "Video") return 0xFFF08A7A;
		return 0xFF9AA4B2;
	}

	/* Default thumbnail loader: deterministic 16x16 two-tone swatch derived
	 * from the file name, coloured by type.  Runs on the cache worker thread. */
	bool procedural_thumbnail(const thumbnail_cache::request& req,
		thumbnail_cache::image* out)
	{
		if (!out) return false;
		const int size = 16;
		out->width = size;
		out->height = size;
		out->stride = size * 4;
		out->format = VGUI_TEXTURE_FORMAT_RGBA8;
		out->pixels.assign((std::size_t)size * size * 4u, 0);

		std::uint32_t hash = 2166136261u;
		for (unsigned char c : req.name)
		{
			hash ^= c;
			hash *= 16777619u;
		}
		const std::uint32_t base = colour_for(req.type);
		const std::uint32_t dark = (base & 0xFF000000u) |
			(((base >> 16) & 0xFF) / 2 << 16) |
			(((base >> 8) & 0xFF) / 2 << 8) |
			((base & 0xFF) / 2);
		const int cell = 2 + (int)(hash % 4);
		for (int y = 0; y < size; ++y)
		{
			for (int x = 0; x < size; ++x)
			{
				const bool checker = ((x / cell) + (y / cell)) % 2 == 0;
				const std::uint32_t c = checker ? base : dark;
				unsigned char* p = out->pixels.data() + ((std::size_t)y * size + x) * 4u;
				p[0] = (unsigned char)((c >> 16) & 0xFF);
				p[1] = (unsigned char)((c >> 8) & 0xFF);
				p[2] = (unsigned char)(c & 0xFF);
				p[3] = (unsigned char)((c >> 24) & 0xFF);
			}
		}
		return true;
	}

} // namespace

/* ------------------------------------------------------------------------- */
/* Public static: directory enumeration                                       */
/* ------------------------------------------------------------------------- */

bool file_selector::enumerate(const std::string& folder, const std::string& pattern,
	std::vector<std::pair<std::string, bool>>* out)
{
	if (!out) return false;
	out->clear();
	std::error_code ec;
	fs::path root(folder);
	if (folder.empty()) root = fs::current_path(ec);
	if (ec || !fs::is_directory(root, ec)) return false;

	const std::vector<std::string> exts = parse_pattern(pattern);
	struct entry { std::string name; bool folder; };
	std::vector<entry> entries;
	for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
	{
		fs::directory_entry de = *it;
		std::error_code sub;
		const bool is_dir = de.is_directory(sub);
		if (sub) continue;
		const std::string name = de.path().filename().string();
		if (!is_dir && !matches_pattern(name, exts)) continue;
		entries.push_back({ name, is_dir });
	}
	if (ec) return false;

	std::stable_sort(entries.begin(), entries.end(),
		[](const entry& a, const entry& b) {
			if (a.folder != b.folder) return a.folder;  // folders first
			return ascii_lower(a.name) < ascii_lower(b.name);
		});
	out->reserve(entries.size());
	for (const entry& e : entries)
		out->emplace_back(e.name, e.folder);
	return true;
}

/* ------------------------------------------------------------------------- */
/* impl                                                                       */
/* ------------------------------------------------------------------------- */

class file_selector::impl
{
public:
	vgui_context* ctx = nullptr;
	vgui_widget* panel = nullptr;          // detached modal panel (owned here)
	vgui_widget* button_row = nullptr;
	vgui_widget* ok_button = nullptr;
	vgui_widget* cancel_button = nullptr;
	asset_browser browser;
	thumbnail_loader custom_loader;

	vgui_file_dialog_kind kind = VGUI_FILE_DIALOG_OPEN;
	vgui_file_dialog_fn callback = nullptr;
	void* callback_user = nullptr;
	std::string root_folder;               // folder the browse session starts at
	std::string pattern;                   // active extension glob list
	bool show_folders_only = false;
	bool allow_many = false;
	bool modal_open = false;

	/* Maintain id -> absolute path for both files and folders in the current
	 * directory.  Rebuilt on every navigate. */
	std::unordered_map<vgui_item_id, std::string> id_to_path;

	void build(vgui_context* context)
	{
		if (panel || !context) return;
		ctx = context;
		panel = vgui_widget_create(ctx, VGUI_PANEL, nullptr);
		vgui_widget_set_layout(panel, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_align_items(panel, VGUI_ALIGN_STRETCH);
		vgui_widget_set_size(panel, 720, 520);
		vgui_widget_set_min_size(panel, 420, 320);
		vgui_widget_set_padding(panel, 12, 12, 12, 12);
		vgui_widget_set_gap(panel, 8);

		browser.build(panel);
		vgui_widget_set_grow(browser.root(), 1.0f);
		browser.set_view(asset_browser::view_mode::grid);
		browser.set_navigation_visible(true);
		browser.set_type_filter_visible(true);

		// Default procedural loader; a host may replace it before show().
		browser.thumbnails().set_load_handler(&procedural_thumbnail);

		button_row = vgui_widget_add(panel, VGUI_PANEL, nullptr);
		vgui_widget_set_layout(button_row, VGUI_LAYOUT_ROW);
		vgui_widget_set_align_items(button_row, VGUI_ALIGN_CENTER);
		vgui_widget_set_gap(button_row, 8);
		cancel_button = vgui_widget_add(button_row, VGUI_BUTTON, "Cancel");
		ok_button = vgui_widget_add(button_row, VGUI_BUTTON, "Open");
		vgui_widget_set_size(cancel_button, 96, 0);
		vgui_widget_set_size(ok_button, 96, 0);
		vgui_widget_on_event(cancel_button, &on_button_event, this);
		vgui_widget_on_event(ok_button, &on_button_event, this);

		browser.set_navigate_handler([this](const std::string& path_id,
			asset_browser::asset_directory* out) {
			return navigate(path_id, out);
		});
		// Double-click / Enter: folders drill down, files confirm immediately.
		browser.set_activate_listener([this](vgui_item_id item) {
			auto found = id_to_path.find(item);
			if (found == id_to_path.end()) return;
			std::error_code ec;
			if (fs::is_directory(fs::path(found->second), ec))
				browser.navigate_to(found->second);
			else if (kind != VGUI_FILE_DIALOG_FOLDER)
				confirm();
		});
	}

	bool navigate(const std::string& path_id, asset_browser::asset_directory* out)
	{
		if (!out) return false;
		std::string folder = path_id;
		if (folder.empty()) folder = root_folder.empty()
			? fs::current_path().string() : root_folder;

		std::vector<std::pair<std::string, bool>> entries;
		if (!file_selector::enumerate(folder, pattern, &entries))
			return false;

		// A FOLDER dialog lists only directories.
		entries.erase(std::remove_if(entries.begin(), entries.end(),
			[this](const std::pair<std::string, bool>& e) {
				return show_folders_only && !e.second;
			}), entries.end());

		// Breadcrumb: root label is the folder path itself; a single segment is
		// enough for a picker (the breadcrumb still works for back/forward).
		out->path.clear();
		out->path.push_back({ folder, folder });
		out->items.clear();
		id_to_path.clear();
		std::vector<std::string> types;
		for (std::size_t i = 0; i < entries.size(); ++i)
		{
			asset_info info;
			info.name = entries[i].first;
			info.type = type_for(info.name, entries[i].second);
			info.id = item_id_for(folder, (vgui_item_id)i);
			const fs::path full = fs::path(folder) / fs::path(info.name);
			std::string full_s = full.string();
			if (entries[i].second)
				while (!full_s.empty() && (full_s.back() == '/' || full_s.back() == '\\'))
					full_s.pop_back();
			id_to_path[info.id] = full_s;
			out->items.push_back(info);
			if (std::find(types.begin(), types.end(), info.type) == types.end())
				types.push_back(info.type);
		}
		browser.set_type_options(types);
		out->empty_text = show_folders_only ? "No folders" : "No matching files";
		return true;
	}

	std::vector<std::string> selected_paths()
	{
		std::vector<std::string> paths;
		for (vgui_item_id id : browser.selection())
		{
			auto found = id_to_path.find(id);
			if (found != id_to_path.end()) paths.push_back(found->second);
		}
		return paths;
	}

	struct modal_payload
	{
		std::weak_ptr<int> alive;
		impl* owner = nullptr;
		vgui_file_dialog_kind kind = VGUI_FILE_DIALOG_OPEN;
		vgui_file_dialog_fn fn = nullptr;
		void* user = nullptr;
		std::vector<std::string> paths;
	};

	std::shared_ptr<int> alive_token = std::make_shared<int>(0);
	std::unique_ptr<modal_payload> payload;

	void confirm()
	{
		if (!modal_open || !ctx || !payload) return;
		std::vector<std::string> paths = selected_paths();
		if (paths.empty()) return;  // require a selection before confirming
		if (!allow_many && !paths.empty()) paths.resize(1);
		payload->paths = std::move(paths);
		vgui_modal_close(ctx, VGUI_MODAL_RESULT_CONFIRM);
	}

	static void on_button_event(vgui_widget* widget, const vgui_event* event, void* user)
	{
		auto* self = static_cast<impl*>(user);
		if (!self || !event || event->type != VGUI_EVENT_CLICK || event->button != 1)
			return;
		if (widget == self->ok_button) self->confirm();
		else if (widget == self->cancel_button && self->modal_open && self->ctx)
			vgui_modal_close(self->ctx, VGUI_MODAL_RESULT_CANCEL);
	}

	static void on_modal_close(vgui_context* ctx, vgui_widget* panel,
		int result, void* user)
	{
		(void)ctx; (void)panel;
		auto* p = static_cast<modal_payload*>(user);
		if (!p || p->alive.expired()) return;  // selector destroyed; no callback
		if (p->owner) p->owner->modal_open = false;
		const bool accepted = result == VGUI_MODAL_RESULT_CONFIRM &&
			!p->paths.empty();
		if (accepted)
		{
			std::vector<const char*> argv;
			argv.reserve(p->paths.size() + 1);
			for (const std::string& path : p->paths) argv.push_back(path.c_str());
			argv.push_back(nullptr);
			vgui_file_dialog_post_result(p->kind, p->fn, p->user,
				argv.data(), (int)p->paths.size(), nullptr);
		}
		else if (p->fn)
		{
			vgui_file_dialog_post_result(p->kind, p->fn, p->user,
				nullptr, 0, nullptr);  // cancel
		}
	}

	void close_if_open()
	{
		if (modal_open && ctx)
		{
			// A destruction-time close must not invoke the host callback: the
			// selector is going away, so report no result (matches "cancelled
			// before a dialog completed", which the native dialogs also drop).
			if (payload) payload->fn = nullptr;
			modal_open = false;
			vgui_modal_close(ctx, VGUI_MODAL_RESULT_CANCEL);
		}
	}

	~impl()
	{
		close_if_open();
		// Detach callbacks before the widget tree dies; the cache is owned by
		// the browser and must be destroyed before its context.
		browser.thumbnails().set_load_handler(thumbnail_loader{});
	}
};

/* ------------------------------------------------------------------------- */
/* file_selector                                                              */
/* ------------------------------------------------------------------------- */

file_selector::file_selector() : impl_(std::make_unique<impl>()) {}
file_selector::~file_selector() = default;

void file_selector::build(vgui_context* ctx) { impl_->build(ctx); }
vgui_context* file_selector::context() const noexcept { return impl_->ctx; }
vgui_widget* file_selector::root() const noexcept { return impl_->panel; }
asset_browser& file_selector::browser() noexcept { return impl_->browser; }

void file_selector::set_thumbnail_loader(thumbnail_loader loader)
{
	if (loader)
		impl_->custom_loader = std::move(loader);
	else
		impl_->custom_loader = thumbnail_loader{};
	impl_->browser.thumbnails().set_load_handler(
		impl_->custom_loader ? impl_->custom_loader : thumbnail_loader(&procedural_thumbnail));
}

int file_selector::show(vgui_file_dialog_kind kind,
	const vgui_file_dialog_desc* desc, vgui_file_dialog_fn fn, void* user)
{
	impl& s = *impl_;
	if (!s.panel || !s.ctx || s.modal_open || !fn) return 0;
	if (kind != VGUI_FILE_DIALOG_OPEN && kind != VGUI_FILE_DIALOG_SAVE &&
		kind != VGUI_FILE_DIALOG_FOLDER)
		return 0;

	vgui_modal_options opts;
	vgui_modal_options_default(&opts);
	if (vgui_modal_active(s.ctx)) return 0;

	s.kind = kind;
	s.allow_many = desc && desc->allow_many && kind != VGUI_FILE_DIALOG_SAVE;
	s.show_folders_only = kind == VGUI_FILE_DIALOG_FOLDER;
	s.root_folder = (desc && desc->default_location && *desc->default_location)
		? desc->default_location : std::string();
	if (s.root_folder.empty())
	{
		const char* recent = vgui_recent_dirs_at(0);
		if (recent) s.root_folder = recent;
	}
	if (s.root_folder.empty())
		s.root_folder = fs::current_path().string();

	// Build the extension pattern from the descriptor filters.
	s.pattern.clear();
	if (desc && desc->filters && desc->filter_count > 0)
	{
		for (int i = 0; i < desc->filter_count; ++i)
		{
			const char* p = desc->filters[i].pattern;
			if (!p || !*p) continue;
			if (!s.pattern.empty()) s.pattern += ';';
			s.pattern += p;
		}
	}
	if (s.pattern.empty()) s.pattern = "*";

	// Reset transient state and seed the first directory.
	s.browser.clear_selection();
	s.id_to_path.clear();
	const std::string start = s.root_folder;
	if (!s.browser.navigate_to(start))
	{
		// Fall back to the current working directory if the seed is invalid.
		s.root_folder = fs::current_path().string();
		if (!s.browser.navigate_to(s.root_folder)) return 0;
	}

	// A fresh payload per show; the alive token invalidates it on destruction.
	s.payload = std::make_unique<impl::modal_payload>();
	s.payload->alive = s.alive_token;
	s.payload->owner = &s;
	s.payload->kind = kind;
	s.payload->fn = fn;
	s.payload->user = user;

	opts.close_on_escape = 1;
	opts.close_on_backdrop = 0;
	opts.dim_background = 1;
	opts.destroy_on_close = 0;
	opts.escape_result = VGUI_MODAL_RESULT_CANCEL;
	opts.on_close = &impl::on_modal_close;
	opts.user = s.payload.get();

	s.modal_open = true;
	if (!vgui_modal_show(s.ctx, s.panel, &opts))
	{
		s.modal_open = false;
		s.payload.reset();
		return 0;
	}
	return 1;
}

bool file_selector::active() const noexcept { return impl_->modal_open; }
vgui_file_dialog_kind file_selector::kind() const noexcept { return impl_->kind; }
const std::string& file_selector::current_path() const { return impl_->browser.current_path(); }
std::vector<std::string> file_selector::selected_paths() const { return impl_->selected_paths(); }

} // namespace vgui_editor_kit
