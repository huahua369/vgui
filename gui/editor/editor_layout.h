#pragma once
/*
 * editor_layout.h - persistent editor shell layout.
 *
 * Layout is deliberately separate from scene/document data: the host can
 * store it next to user preferences and restoring a scene never overwrites the
 * user's panel arrangement.  The JSON format carries a version so older files
 * can be rejected cleanly in favour of the built-in default layout.
 */
#include <string>
#include <vector>

namespace vgui_editor_kit {

	struct editor_layout
	{
		static constexpr int current_version = 1;

		int version = current_version;

		/* Ratios of the three shell splitters.  They are stored as fractions
		 * of the main axis and clamped to 0..1 during restore. */
		float body_ratio = 0.75f;
		float workspace_ratio = 0.20f;
		float center_ratio = 0.70f;

		bool outliner_visible = true;
		bool viewport_visible = true;
		bool properties_visible = true;
		bool bottom_visible = true;

		/* Titles are used as stable panel identities for the shell's fixed
		 * bottom tabs.  Unknown titles are ignored during restore. */
		std::vector<std::string> bottom_tab_order;
		std::string bottom_active_tab;
	};

	std::string editor_layout_to_json(const editor_layout& layout);
	bool editor_layout_from_json(const std::string& text, editor_layout& out,
		std::string* error = nullptr);

	bool editor_layout_load_file(const std::string& path, editor_layout& out,
		std::string* error = nullptr);
	bool editor_layout_save_file(const std::string& path, const editor_layout& layout,
		std::string* error = nullptr);

} /* namespace vgui_editor_kit */
