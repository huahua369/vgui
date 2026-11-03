/*
 * vgui SDL3 backend: native file dialogs.
 *
 * Bridges the platform-free dialog API from vgui.h onto SDL3's dialog support
 * (SDL_ShowFileDialogWithProperties, i.e. the native Explorer / Finder / XDG
 * portal pickers).  vgui.h never mentions SDL: this translation unit installs
 * a provider into the core bridge from app_mgr's constructor.
 *
 * The SDL dialog is asynchronous.  SDL keeps reading the filter array until it
 * invokes the callback, and our own descriptor strings only live for the show
 * call, so every request owns copies of them and frees them from the callback.
 *
 * Created: 2026-11-03
 */
#include <SDL3/SDL.h>

#include <vgui/vgui.h>

#include "vgui_sdl3_dialog.h"

#include <memory>
#include <string>
#include <vector>

namespace {

/* Everything that must outlive the SDL call: the filter array plus the strings
 * it points to, and the property list carrying title / labels / location. */
struct sdl3_dialog_request
{
	~sdl3_dialog_request()
	{
		if (props) SDL_DestroyProperties(props);
	}

	vgui_file_dialog_kind kind = VGUI_FILE_DIALOG_OPEN;
	vgui_file_dialog_fn fn = nullptr;
	void* user = nullptr;
	SDL_PropertiesID props = 0;

	std::string location;
	std::vector<std::string> filter_names;
	std::vector<std::string> filter_patterns;
	std::vector<SDL_DialogFileFilter> filters;
};

/* SDL wants bare extensions ("png;jpg") while vgui accepts globs ("*.png;*.jpg")
 * as well; "*" and "*.*" both mean "everything". */
std::string sdl3_pattern(const char* pattern)
{
	std::string out;
	const char* p = pattern;
	if (!p) return "*";
	while (*p)
	{
		while (*p == ';' || *p == ' ' || *p == '\t') ++p;
		if (!*p) break;
		const char* start = p;
		while (*p && *p != ';') ++p;
		std::string token(start, (size_t)(p - start));
		while (!token.empty() && (token.back() == ' ' || token.back() == '\t'))
			token.pop_back();
		if (token.empty()) continue;
		if (token == "*" || token == "*.*" || token == "*.")
			token = "*";
		else if (token.size() > 2 && token[0] == '*' && token[1] == '.')
			token.erase(0, 2);
		else if (token[0] == '.')
			token.erase(0, 1);
		if (!out.empty()) out += ';';
		out += token;
	}
	return out.empty() ? std::string("*") : out;
}

/* The window the dialog should be modal for.  SDL tolerates NULL (the platform
 * then picks a window), so a missing focus window is not an error. */
SDL_Window* sdl3_dialog_parent()
{
	SDL_Window* window = SDL_GetKeyboardFocus();
	if (!window) window = SDL_GetMouseFocus();
	return window;
}

void SDLCALL sdl3_dialog_done(void* userdata, const char* const* filelist, int filter)
{
	(void)filter;
	std::unique_ptr<sdl3_dialog_request> req(static_cast<sdl3_dialog_request*>(userdata));
	if (!req) return;

	if (!filelist)
	{
		const char* err = SDL_GetError();
		vgui_file_dialog_post_result(req->kind, req->fn, req->user, nullptr, 0,
			(err && *err) ? err : "file dialog failed");
		return;
	}

	int count = 0;
	while (filelist[count]) ++count;
	// A NULL first entry means the user cancelled; vgui reports that as zero
	// paths with no error.
	vgui_file_dialog_post_result(req->kind, req->fn, req->user, filelist, count, nullptr);
}

int sdl3_show_dialog(void* provider_user, vgui_file_dialog_kind kind,
	const vgui_file_dialog_desc* desc, vgui_file_dialog_fn fn, void* user)
{
	(void)provider_user;
	if (!fn) return 0;

	const bool is_save = kind == VGUI_FILE_DIALOG_SAVE;
	const bool is_folder = kind == VGUI_FILE_DIALOG_FOLDER;

	auto req = std::make_unique<sdl3_dialog_request>();
	req->kind = kind;
	req->fn = fn;
	req->user = user;

	req->props = SDL_CreateProperties();
	if (!req->props) return 0;

	if (desc && desc->title && *desc->title)
		SDL_SetStringProperty(req->props, SDL_PROP_FILE_DIALOG_TITLE_STRING, desc->title);
	if (desc && desc->accept_label && *desc->accept_label)
		SDL_SetStringProperty(req->props, SDL_PROP_FILE_DIALOG_ACCEPT_STRING, desc->accept_label);
	if (desc && desc->cancel_label && *desc->cancel_label)
		SDL_SetStringProperty(req->props, SDL_PROP_FILE_DIALOG_CANCEL_STRING, desc->cancel_label);

	if (desc && desc->default_location && *desc->default_location)
		req->location = desc->default_location;
	if (is_save && desc && desc->default_name && *desc->default_name)
	{
		// The save dialog takes a single "location" that may include the file
		// name; append the suggestion when the host did not already do so.
		const char* name = desc->default_name;
		const size_t name_len = SDL_strlen(name);
		if (req->location.empty())
			req->location = name;
		else if (!(req->location.size() >= name_len &&
			req->location.compare(req->location.size() - name_len, name_len, name) == 0))
		{
			const char last = req->location.back();
			if (last != '/' && last != '\\') req->location += '/';
			req->location += name;
		}
	}
	if (!req->location.empty())
		SDL_SetStringProperty(req->props, SDL_PROP_FILE_DIALOG_LOCATION_STRING,
			req->location.c_str());

	if (!is_folder && desc && desc->filters && desc->filter_count > 0)
	{
		const int n = desc->filter_count;
		req->filter_names.reserve((size_t)n);
		req->filter_patterns.reserve((size_t)n);
		for (int i = 0; i < n; ++i)
		{
			const char* name = desc->filters[i].name;
			req->filter_names.emplace_back((name && *name) ? name : "Files");
			req->filter_patterns.push_back(sdl3_pattern(desc->filters[i].pattern));
		}
		req->filters.reserve((size_t)n);
		for (int i = 0; i < n; ++i)
			req->filters.push_back({ req->filter_names[(size_t)i].c_str(),
				req->filter_patterns[(size_t)i].c_str() });
		SDL_SetNumberProperty(req->props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, n);
		SDL_SetPointerProperty(req->props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER,
			req->filters.data());
	}

	if (!is_save && desc && desc->allow_many)
		SDL_SetBooleanProperty(req->props, SDL_PROP_FILE_DIALOG_MANY_BOOLEAN, true);

	SDL_Window* parent = sdl3_dialog_parent();
	if (parent)
		SDL_SetPointerProperty(req->props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, parent);

	const SDL_FileDialogType type = is_folder ? SDL_FILEDIALOG_OPENFOLDER
		: (is_save ? SDL_FILEDIALOG_SAVEFILE : SDL_FILEDIALOG_OPENFILE);

	// The dialog owns itself once launched: the request is released by the
	// callback, including when the platform aborts with a NULL file list.
	const SDL_PropertiesID props = req->props;
	SDL_ShowFileDialogWithProperties(type, sdl3_dialog_done, req.release(), props);
	return 1;
}

} // namespace

// Called from app_mgr's constructor.  Unlike the clipboard bridge this cannot
// rely on a static initialiser alone: a static library only pulls in objects
// that some referenced translation unit needs, and nothing calls the dialog
// code directly -- the core reaches it through the provider table.
void vgui_sdl3_install_file_dialog_provider()
{
	vgui_file_dialog_set_provider(sdl3_show_dialog, nullptr);
}
