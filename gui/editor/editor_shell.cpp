#include "editor_shell.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace vgui_editor_kit {

	namespace {
		constexpr vgui_color kRootBg = 0xFF14171C;
		constexpr vgui_color kHeaderBg = 0xFF1E232B;
		constexpr vgui_color kSectionBg = 0xFF1B1E24;
		constexpr vgui_color kBorder = 0xFF2E3440;
		constexpr vgui_color kTitleText = 0xFFB8C4D6;
		constexpr vgui_color kMutedText = 0xFF7F8B9C;
		constexpr vgui_color kStatusText = 0xFF9CC7A4;

		vgui_widget* make_section(vgui_widget* parent, const char* title,
			float width, float height, float grow)
		{
			vgui_widget* section = vgui_widget_add(parent, VGUI_PANEL, nullptr);
			vgui_widget_set_size(section, width, height);
			vgui_widget_set_grow(section, grow);
			vgui_widget_set_align_self(section, VGUI_ALIGN_STRETCH);
			vgui_widget_set_layout(section, VGUI_LAYOUT_COLUMN);
			vgui_widget_set_padding(section, 0, 0, 0, 0);
			vgui_widget_set_gap(section, 0);
			vgui_widget_set_background(section, kSectionBg);
			vgui_widget_set_border(section, kBorder, 1);
			if (width > 0.0f)
				vgui_widget_set_min_size(section, width, 0);

			vgui_widget* heading = vgui_widget_add(section, VGUI_LABEL, title);
			vgui_widget_set_size(heading, 0, 24);
			vgui_widget_set_align_self(heading, VGUI_ALIGN_STRETCH);
			vgui_widget_set_font_size(heading, 11);
			vgui_widget_set_text_color(heading, kMutedText);
			vgui_widget_set_text_align(heading, 0.0f, 0.5f);
			vgui_widget_set_padding(heading, 8, 0, 8, 0);

			vgui_widget* body = vgui_widget_add(section, VGUI_PANEL, nullptr);
			vgui_widget_set_grow(body, 1.0f);
			vgui_widget_set_layout(body, VGUI_LAYOUT_COLUMN);
			vgui_widget_set_padding(body, 6, 6, 6, 6);
			vgui_widget_set_gap(body, 4);
			return body;
		}
	}

	editor_shell::editor_shell(editor_context& context)
		: context_(context)
	{
	}

	void editor_shell::build()
	{
		if (root_)
			return;

		vgui_widget* host = context_.root();
		if (!host)
			return;

		vgui_widget_set_layout(host, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_padding(host, 0, 0, 0, 0);
		vgui_widget_set_gap(host, 0);
		vgui_widget_set_align_items(host, VGUI_ALIGN_STRETCH);

		root_ = vgui_widget_add(host, VGUI_PANEL, nullptr);
		vgui_widget_set_grow(root_, 1.0f);
		vgui_widget_set_layout(root_, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_padding(root_, 4, 4, 4, 4);
		vgui_widget_set_gap(root_, 4);
		vgui_widget_set_background(root_, kRootBg);

		vgui_widget* header = vgui_widget_add(root_, VGUI_PANEL, nullptr);
		vgui_widget_set_size(header, 0, 34);
		vgui_widget_set_align_self(header, VGUI_ALIGN_STRETCH);
		vgui_widget_set_layout(header, VGUI_LAYOUT_ROW);
		vgui_widget_set_padding(header, 8, 0, 8, 0);
		vgui_widget_set_gap(header, 8);
		vgui_widget_set_background(header, kHeaderBg);
		vgui_widget_set_border(header, kBorder, 1);

		vgui_widget* title = vgui_widget_add(header, VGUI_LABEL,
			"vgui_editor_kit  ·  MVP skeleton");
		vgui_widget_set_grow(title, 1.0f);
		vgui_widget_set_size(title, 0, 24);
		vgui_widget_set_font_size(title, 14);
		vgui_widget_set_text_color(title, kTitleText);
		vgui_widget_set_text_align(title, 0.0f, 0.5f);

		vgui_widget* undo = vgui_widget_add(header, VGUI_BUTTON, "Undo");
		vgui_widget_set_size(undo, 58, 24);
		vgui_widget_set_font_size(undo, 12);
		vgui_widget_on_event(undo, &editor_shell::on_undo, this);

		vgui_widget* redo = vgui_widget_add(header, VGUI_BUTTON, "Redo");
		vgui_widget_set_size(redo, 58, 24);
		vgui_widget_set_font_size(redo, 12);
		vgui_widget_on_event(redo, &editor_shell::on_redo, this);

		status_ = vgui_widget_add(header, VGUI_LABEL, "ready");
		vgui_widget_set_size(status_, 220, 24);
		vgui_widget_set_font_size(status_, 12);
		vgui_widget_set_text_color(status_, kStatusText);
		vgui_widget_set_text_align(status_, 1.0f, 0.5f);

		body_splitter_ = vgui_widget_add(root_, VGUI_SPLITTER, nullptr);
		vgui_widget_set_grow(body_splitter_, 1.0f);
		vgui_widget_set_align_self(body_splitter_, VGUI_ALIGN_STRETCH);
		vgui_splitter_set_vertical(body_splitter_, 1);
		vgui_splitter_set_ratio(body_splitter_, 0.75f);
		vgui_splitter_set_sash_size(body_splitter_, 4.0f);

		vgui_widget* main = vgui_widget_add(body_splitter_, VGUI_PANEL, nullptr);
		vgui_widget_set_min_size(main, 0, 200);
		vgui_widget_set_layout(main, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_padding(main, 0, 0, 0, 0);
		vgui_widget_set_gap(main, 0);
		vgui_widget_set_background(main, kRootBg);

		workspace_splitter_ = vgui_widget_add(main, VGUI_SPLITTER, nullptr);
		vgui_widget_set_grow(workspace_splitter_, 1.0f);
		vgui_splitter_set_vertical(workspace_splitter_, 0);
		vgui_splitter_set_ratio(workspace_splitter_, 0.2f);
		vgui_splitter_set_sash_size(workspace_splitter_, 4.0f);

		vgui_widget* left_body = make_section(workspace_splitter_, "OUTLINER", 230, 0, 0);
		outliner_panel_ = vgui_widget_get_parent(left_body);

		center_splitter_ = vgui_widget_add(workspace_splitter_, VGUI_SPLITTER, nullptr);
		vgui_widget_set_min_size(center_splitter_, 400, 0);
		vgui_splitter_set_vertical(center_splitter_, 0);
		vgui_splitter_set_ratio(center_splitter_, 0.7f);
		vgui_splitter_set_sash_size(center_splitter_, 4.0f);
		vgui_widget* center_body = make_section(center_splitter_, "VIEWPORT", 0, 0, 1);
		viewport_panel_ = vgui_widget_get_parent(center_body);
		vgui_widget* right_body = make_section(center_splitter_, "PROPERTIES", 300, 0, 0);
		properties_panel_ = vgui_widget_get_parent(right_body);

		vgui_widget* bottom_body = make_section(body_splitter_, "PANELS", 0, 180, 0);
		bottom_panel_ = vgui_widget_get_parent(bottom_body);
		vgui_widget_set_min_size(bottom_panel_, 0, 120);

		bottom_tabs_ = vgui_widget_add(bottom_body, VGUI_TABVIEW, nullptr);
		vgui_widget_set_grow(bottom_tabs_, 1.0f);
		vgui_widget_set_min_size(bottom_tabs_, 0, 80);
		vgui_tab_set_tab_height(bottom_tabs_, 26.0f);

		vgui_widget* assets_page = vgui_widget_add(bottom_tabs_, VGUI_PANEL, nullptr);
		vgui_widget_set_layout(assets_page, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_padding(assets_page, 6, 6, 6, 6);
		assets_.build(assets_page);
		vgui_item_id assets_tab = vgui_tab_add(bottom_tabs_, "ASSETS", assets_page);
		vgui_tab_set_closeable(bottom_tabs_, assets_tab, 0);

		vgui_widget* console_page = vgui_widget_add(bottom_tabs_, VGUI_PANEL, nullptr);
		vgui_widget_set_layout(console_page, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_padding(console_page, 8, 8, 8, 8);
		vgui_widget* console_text = vgui_widget_add(console_page, VGUI_LABEL, "No messages.");
		vgui_widget_set_font_size(console_text, 12);
		vgui_widget_set_text_color(console_text, kMutedText);
		vgui_item_id console_tab = vgui_tab_add(bottom_tabs_, "CONSOLE", console_page);
		vgui_tab_set_closeable(bottom_tabs_, console_tab, 0);

		scene_.build(context_, left_body);
		viewport_.build(center_body);
		properties_.build(right_body);

		default_layout_ = capture_layout();
	}

	bool editor_shell::built() const noexcept
	{
		return root_ != nullptr;
	}

	vgui_widget* editor_shell::widget() const noexcept
	{
		return root_;
	}

	vgui_widget* editor_shell::body_splitter() const noexcept
	{
		return body_splitter_;
	}

	vgui_widget* editor_shell::workspace_splitter() const noexcept
	{
		return workspace_splitter_;
	}

	vgui_widget* editor_shell::center_splitter() const noexcept
	{
		return center_splitter_;
	}

	vgui_widget* editor_shell::bottom_tabs() const noexcept
	{
		return bottom_tabs_;
	}

	vgui_widget* editor_shell::panel_widget(editor_panel panel) const noexcept
	{
		switch (panel)
		{
		case editor_panel::outliner: return outliner_panel_;
		case editor_panel::viewport: return viewport_panel_;
		case editor_panel::properties: return properties_panel_;
		case editor_panel::bottom: return bottom_panel_;
		}
		return nullptr;
	}

	void editor_shell::set_panel_visible(editor_panel panel, bool visible)
	{
		vgui_widget* widget = panel_widget(panel);
		if (!widget)
			return;
		vgui_widget_set_visible(widget, visible ? 1 : 0);
	}

	bool editor_shell::panel_visible(editor_panel panel) const
	{
		vgui_widget* widget = panel_widget(panel);
		return widget ? vgui_widget_get_visible(widget) != 0 : false;
	}

	editor_layout editor_shell::capture_layout() const
	{
		editor_layout layout;
		if (!built())
			return layout;

		layout.body_ratio = vgui_splitter_get_ratio(body_splitter_);
		layout.workspace_ratio = vgui_splitter_get_ratio(workspace_splitter_);
		layout.center_ratio = vgui_splitter_get_ratio(center_splitter_);
		layout.outliner_visible = panel_visible(editor_panel::outliner);
		layout.viewport_visible = panel_visible(editor_panel::viewport);
		layout.properties_visible = panel_visible(editor_panel::properties);
		layout.bottom_visible = panel_visible(editor_panel::bottom);

		if (bottom_tabs_)
		{
			int count = vgui_tab_get_count(bottom_tabs_);
			for (int i = 0; i < count; ++i)
			{
				vgui_item_id item = vgui_tab_get_at(bottom_tabs_, i);
				layout.bottom_tab_order.emplace_back(vgui_tab_get_title(bottom_tabs_, item));
			}
			vgui_item_id active = vgui_tab_get_active(bottom_tabs_);
			if (active)
				layout.bottom_active_tab = vgui_tab_get_title(bottom_tabs_, active);
		}
		return layout;
	}

	bool editor_shell::apply_layout(const editor_layout& layout, std::string* error)
	{
		if (!built())
		{
			if (error) *error = "editor shell is not built";
			return false;
		}
		if (layout.version != editor_layout::current_version)
		{
			if (error) *error = "unsupported layout version";
			return false;
		}

		auto ratio = [](float value, float fallback) {
			return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : fallback;
		};
		vgui_splitter_set_ratio(body_splitter_,
			ratio(layout.body_ratio, default_layout_.body_ratio));
		vgui_splitter_set_ratio(workspace_splitter_,
			ratio(layout.workspace_ratio, default_layout_.workspace_ratio));
		vgui_splitter_set_ratio(center_splitter_,
			ratio(layout.center_ratio, default_layout_.center_ratio));

		set_panel_visible(editor_panel::outliner, layout.outliner_visible);
		set_panel_visible(editor_panel::viewport, layout.viewport_visible);
		set_panel_visible(editor_panel::properties, layout.properties_visible);
		set_panel_visible(editor_panel::bottom, layout.bottom_visible);

		if (bottom_tabs_)
		{
			int count = vgui_tab_get_count(bottom_tabs_);
			std::vector<vgui_item_id> items((size_t)count, 0);
			std::vector<char> used((size_t)count, 0);
			for (int i = 0; i < count; ++i)
				items[(size_t)i] = vgui_tab_get_at(bottom_tabs_, i);

			std::vector<vgui_item_id> desired;
			desired.reserve((size_t)count);
			for (const std::string& title : layout.bottom_tab_order)
			{
				for (int i = 0; i < count; ++i)
				{
					if (used[(size_t)i]) continue;
					if (title != vgui_tab_get_title(bottom_tabs_, items[(size_t)i])) continue;
					desired.push_back(items[(size_t)i]);
					used[(size_t)i] = 1;
					break;
				}
			}
			for (size_t i = 0; i < desired.size(); ++i)
				vgui_tab_move(bottom_tabs_, desired[i], (int)i);

			if (!layout.bottom_active_tab.empty())
			{
				count = vgui_tab_get_count(bottom_tabs_);
				for (int i = 0; i < count; ++i)
				{
					vgui_item_id item = vgui_tab_get_at(bottom_tabs_, i);
					if (layout.bottom_active_tab != vgui_tab_get_title(bottom_tabs_, item))
						continue;
					vgui_tab_set_active(bottom_tabs_, item);
					break;
				}
			}
		}

		if (context_.ui())
			vgui_layout(context_.ui());
		return true;
	}

	bool editor_shell::save_layout(const std::string& path, std::string* error) const
	{
		if (!built())
		{
			if (error) *error = "editor shell is not built";
			return false;
		}
		return editor_layout_save_file(path, capture_layout(), error);
	}

	bool editor_shell::load_layout(const std::string& path, std::string* error)
	{
		if (!built())
		{
			if (error) *error = "editor shell is not built";
			return false;
		}

		editor_layout layout;
		std::string reason;
		if (!editor_layout_load_file(path, layout, &reason))
		{
			reset_layout();
			if (error) *error = reason;
			return false;
		}
		if (!apply_layout(layout, &reason))
		{
			reset_layout();
			if (error) *error = reason;
			return false;
		}
		if (error) error->clear();
		return true;
	}

	bool editor_shell::reset_layout()
	{
		if (!built())
			return false;
		std::string error;
		return apply_layout(default_layout_, &error);
	}

	outliner_view& editor_shell::scene() noexcept
	{
		return scene_;
	}

	property_grid& editor_shell::properties() noexcept
	{
		return properties_;
	}

	asset_browser& editor_shell::assets() noexcept
	{
		return assets_;
	}

	viewport_view& editor_shell::viewport() noexcept
	{
		return viewport_;
	}

	void editor_shell::set_status(std::string text)
	{
		if (status_)
			vgui_widget_set_text(status_, text.c_str());
	}

	void editor_shell::on_undo(vgui_widget* widget, const vgui_event* event, void* user)
	{
		(void)widget;
		if (event && event->type == VGUI_EVENT_CLICK)
		{
			auto* self = static_cast<editor_shell*>(user);
			self->set_status(self->context_.commands().undo() ? "undo" : "nothing to undo");
		}
	}

	void editor_shell::on_redo(vgui_widget* widget, const vgui_event* event, void* user)
	{
		(void)widget;
		if (event && event->type == VGUI_EVENT_CLICK)
		{
			auto* self = static_cast<editor_shell*>(user);
			self->set_status(self->context_.commands().redo() ? "redo" : "nothing to redo");
		}
	}

} /* namespace vgui_editor_kit */
